/*
 * Goldfish platform device bus (0xff001000, IRQ 1)
 *
 * E o mecanismo pelo qual o kernel Android goldfish enumera os
 * dispositivos "virtuais" que nao tem enumeracao propria (fb, audio,
 * events, nand, pipe, ...): em vez de PCI ou device-tree, o guest faz
 * polling num pequeno conjunto de registradores para descobrir nome,
 * id, base MMIO, tamanho e IRQ de cada dispositivo, um de cada vez.
 *
 * Protocolo CONFERIDO contra o driver real do kernel
 * (arch/arm/mach-goldfish/pdev_bus.c / drivers/platform/goldfish/pdev_bus.c,
 * dependendo da versao -- ambos usam os mesmos offsets):
 *
 *   0x00 PDEV_BUS_OP           R: op da entry corrente (0x00=DONE,
 *                                  0x04=REMOVE_DEV, 0x08=ADD_DEV).
 *                                  Cada LEITURA ja avanca para a
 *                                  proxima entry da fila (nao ha ack
 *                                  por escrita separado -- o driver
 *                                  roda um while(1) so lendo OP ate
 *                                  ver DONE).
 *                               W: o driver escreve PDEV_BUS_OP_INIT
 *                                  (=0) uma vez, no probe do driver.
 *   0x04 PDEV_BUS_GET_NAME     W: endereco do buffer, no guest, onde
 *                                  o bus deve escrever o nome do
 *                                  dispositivo corrente (NAME_LEN
 *                                  bytes, sem terminador). ATENCAO:
 *                                  o driver escreve aqui um PONTEIRO
 *                                  DE KERNEL (endereco virtual), nao
 *                                  necessariamente um endereco fisico
 *                                  -- ver disclaimer mais abaixo.
 *   0x08 PDEV_BUS_NAME_LEN     R: tamanho em bytes do nome.
 *   0x0c PDEV_BUS_ID           R: id do dispositivo corrente.
 *   0x10 PDEV_BUS_IO_BASE      R: base MMIO do dispositivo corrente.
 *                                  Tambem usado para IDENTIFICAR qual
 *                                  dispositivo remover quando
 *                                  OP == REMOVE_DEV.
 *   0x14 PDEV_BUS_IO_SIZE      R: tamanho da regiao MMIO.
 *   0x18 PDEV_BUS_IRQ          R: primeira linha de IRQ.
 *   0x1c PDEV_BUS_IRQ_COUNT    R: quantidade de linhas de IRQ.
 *
 * DISCLAIMER SOBRE PDEV_BUS_GET_NAME (o ponto mais fragil deste
 * arquivo): no driver real, o buffer de destino e alocado com
 * kzalloc() e o ponteiro passado para writel() e um endereco VIRTUAL
 * de kernel, nao fisico. Kernels ARM linha goldfish/2.6-3.x tipicos
 * mapeiam a RAM baixa linearmente (PAGE_OFFSET, normalmente
 * 0xc0000000, com a RAM fisica comecando em 0x00000000), entao a
 * conversao seria fisico = virtual - PAGE_OFFSET. Esse valor NAO esta
 * disponivel aqui (depende da config do kernel guest, nao do QEMU), e
 * por isso a implementacao abaixo usa cpu_get_phys_page_debug() sobre
 * a CPU corrente para traduzir o endereco via a MMU/tabela de paginas
 * ja configurada pelo guest no momento da escrita -- isso deve
 * funcionar independente do valor exato de PAGE_OFFSET, mas so foi
 * validado em teoria, nao contra um boot real. Se os nomes dos
 * dispositivos aparecerem corrompidos ou o guest crashar ali, este e
 * o primeiro lugar a investigar.
 *
 * STATUS: registradores e maquina de estados conferidos contra o
 * fonte do driver do kernel; a parte de traducao de endereco em
 * PDEV_BUS_GET_NAME continua nao testada contra um boot real.
 * REMOVE_DEV esta implementado (remove por PDEV_BUS_IO_BASE, igual ao
 * driver real).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/arm/goldfish_pdev_bus.h"
#include "exec/address-spaces.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define PDEV_BUS_OP_DONE         0x00
#define PDEV_BUS_OP_REMOVE_DEV   0x04
#define PDEV_BUS_OP_ADD_DEV      0x08
#define PDEV_BUS_OP_INIT         0x00

#define PDEV_BUS_OP              0x00
#define PDEV_BUS_GET_NAME        0x04
#define PDEV_BUS_NAME_LEN        0x08
#define PDEV_BUS_ID              0x0c
#define PDEV_BUS_IO_BASE         0x10
#define PDEV_BUS_IO_SIZE         0x14
#define PDEV_BUS_IRQ             0x18
#define PDEV_BUS_IRQ_COUNT       0x1c

static void goldfish_pdev_bus_free_entry(GoldfishPdevEntry *e)
{
    if (e) {
        g_free(e->name);
        g_free(e);
    }
}

static void goldfish_pdev_bus_update_irq(GoldfishPdevBusState *s)
{
    qemu_set_irq(s->irq, s->current != NULL || !QSIMPLEQ_EMPTY(&s->pending));
}

/*
 * Escreve 'len' bytes de 'buf' no endereco VIRTUAL 'guest_vaddr' do
 * guest corrente, traduzindo pagina a pagina via a MMU do guest (o
 * driver kzalloc() o buffer, entao ele pode nao ser fisicamente
 * contiguo alem de uma pagina -- na pratica, para nomes curtos de
 * dispositivo, cabe numa unica pagina, mas o loop cobre o caso geral
 * mesmo assim).
 */
static void goldfish_pdev_bus_write_guest_name(hwaddr guest_vaddr,
                                                const char *buf, size_t len)
{
    CPUState *cpu = current_cpu;
    size_t off = 0;

    if (!cpu) {
        qemu_log_mask(LOG_UNIMP,
                      "goldfish_pdev_bus: sem CPU corrente para traduzir "
                      "PDEV_BUS_GET_NAME (0x%" HWADDR_PRIx ")\n",
                      guest_vaddr);
        return;
    }

    while (off < len) {
        hwaddr page = (guest_vaddr + off) & TARGET_PAGE_MASK;
        hwaddr page_off = (guest_vaddr + off) - page;
        hwaddr chunk = TARGET_PAGE_SIZE - page_off;
        hwaddr paddr;

        if (chunk > len - off) {
            chunk = len - off;
        }

        paddr = cpu_get_phys_page_debug(cpu, page);
        if (paddr == -1) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "goldfish_pdev_bus: falha ao traduzir endereco "
                          "virtual 0x%" HWADDR_PRIx " do guest\n", page);
            return;
        }

        address_space_write(&address_space_memory, paddr + page_off,
                             MEMTXATTRS_UNSPECIFIED,
                             (const uint8_t *)buf + off, chunk);
        off += chunk;
    }
}

static uint64_t goldfish_pdev_bus_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    GoldfishPdevBusState *s = opaque;

    switch (offset) {
    case PDEV_BUS_OP:
        /*
         * Cada leitura avanca a fila: descarta a entry que ja foi
         * lida (se houver) e busca a proxima. Isso reflete o
         * while(1) { op = readl(OP); ... } do driver real, que nao
         * manda nenhum ack por escrita entre uma entry e outra.
         */
        goldfish_pdev_bus_free_entry(s->current);
        s->current = NULL;

        if (QSIMPLEQ_EMPTY(&s->pending)) {
            goldfish_pdev_bus_update_irq(s);
            return PDEV_BUS_OP_DONE;
        }
        s->current = QSIMPLEQ_FIRST(&s->pending);
        QSIMPLEQ_REMOVE_HEAD(&s->pending, next);
        goldfish_pdev_bus_update_irq(s);
        return s->current->removing ? PDEV_BUS_OP_REMOVE_DEV
                                     : PDEV_BUS_OP_ADD_DEV;

    case PDEV_BUS_NAME_LEN:
        return s->current ? strlen(s->current->name) : 0;

    case PDEV_BUS_ID:
        return s->current ? s->current->id : 0;

    case PDEV_BUS_IO_BASE:
        return s->current ? s->current->io_base : 0;

    case PDEV_BUS_IO_SIZE:
        return s->current ? s->current->io_size : 0;

    case PDEV_BUS_IRQ:
        return s->current ? s->current->irq : 0;

    case PDEV_BUS_IRQ_COUNT:
        return s->current ? s->current->irq_count : 0;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "goldfish_pdev_bus: leitura invalida em 0x%"
                      HWADDR_PRIx, offset);
        return 0;
    }
}

static void goldfish_pdev_bus_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    GoldfishPdevBusState *s = opaque;

    switch (offset) {
    case PDEV_BUS_GET_NAME:
        if (s->current) {
            goldfish_pdev_bus_write_guest_name(value, s->current->name,
                                                strlen(s->current->name));
        }
        break;

    case PDEV_BUS_OP:
        /* PDEV_BUS_OP_INIT (=0), escrito uma vez no probe do driver.
         * Nao ha estado de fila para resetar alem do que ja e feito
         * em reset/realize; tratado aqui so para nao logar como
         * escrita invalida. */
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "goldfish_pdev_bus: escrita invalida em 0x%"
                      HWADDR_PRIx, offset);
        break;
    }
}

static const MemoryRegionOps goldfish_pdev_bus_ops = {
    .read = goldfish_pdev_bus_read,
    .write = goldfish_pdev_bus_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

void goldfish_pdev_bus_add(DeviceState *bus_dev, const char *name,
                            uint32_t id, hwaddr io_base, uint32_t io_size,
                            uint32_t irq, uint32_t irq_count)
{
    GoldfishPdevBusState *s = GOLDFISH_PDEV_BUS(bus_dev);
    GoldfishPdevEntry *e = g_new0(GoldfishPdevEntry, 1);

    e->name = g_strdup(name);
    e->id = id;
    e->io_base = io_base;
    e->io_size = io_size;
    e->irq = irq;
    e->irq_count = irq_count;
    e->removing = false;

    QSIMPLEQ_INSERT_TAIL(&s->pending, e, next);
    goldfish_pdev_bus_update_irq(s);
}

void goldfish_pdev_bus_remove(DeviceState *bus_dev, hwaddr io_base)
{
    GoldfishPdevBusState *s = GOLDFISH_PDEV_BUS(bus_dev);
    GoldfishPdevEntry *e = g_new0(GoldfishPdevEntry, 1);

    /* O driver real identifica o dispositivo a remover exclusivamente
     * pelo valor de PDEV_BUS_IO_BASE, entao so precisamos propagar
     * isso -- os demais campos nao sao lidos pelo driver num
     * REMOVE_DEV. */
    e->name = g_strdup("");
    e->io_base = io_base;
    e->removing = true;

    QSIMPLEQ_INSERT_TAIL(&s->pending, e, next);
    goldfish_pdev_bus_update_irq(s);
}

static void goldfish_pdev_bus_reset(DeviceState *dev)
{
    GoldfishPdevBusState *s = GOLDFISH_PDEV_BUS(dev);
    GoldfishPdevEntry *e;

    while ((e = QSIMPLEQ_FIRST(&s->pending))) {
        QSIMPLEQ_REMOVE_HEAD(&s->pending, next);
        goldfish_pdev_bus_free_entry(e);
    }
    goldfish_pdev_bus_free_entry(s->current);
    s->current = NULL;
}

static void goldfish_pdev_bus_realize(DeviceState *dev, Error **errp)
{
    GoldfishPdevBusState *s = GOLDFISH_PDEV_BUS(dev);

    QSIMPLEQ_INIT(&s->pending);
    memory_region_init_io(&s->iomem, OBJECT(dev), &goldfish_pdev_bus_ops, s,
                           TYPE_GOLDFISH_PDEV_BUS, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void goldfish_pdev_bus_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = goldfish_pdev_bus_realize;
    dc->reset = goldfish_pdev_bus_reset;
}

static const TypeInfo goldfish_pdev_bus_info = {
    .name = TYPE_GOLDFISH_PDEV_BUS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GoldfishPdevBusState),
    .class_init = goldfish_pdev_bus_class_init,
};

static void goldfish_pdev_bus_register(void)
{
    type_register_static(&goldfish_pdev_bus_info);
}

type_init(goldfish_pdev_bus_register)
