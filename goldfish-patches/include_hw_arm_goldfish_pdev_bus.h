#ifndef HW_ARM_GOLDFISH_PDEV_BUS_H
#define HW_ARM_GOLDFISH_PDEV_BUS_H

#include "hw/sysbus.h"
#include "qom/object.h"
#include "qemu/queue.h"

#define TYPE_GOLDFISH_PDEV_BUS "goldfish_pdev_bus"
OBJECT_DECLARE_SIMPLE_TYPE(GoldfishPdevBusState, GOLDFISH_PDEV_BUS)

typedef struct GoldfishPdevEntry {
    char *name;
    uint32_t id;
    hwaddr io_base;
    uint32_t io_size;
    uint32_t irq;
    uint32_t irq_count;
    bool removing;      /* true = entry de REMOVE_DEV, nao ADD_DEV */
    QSIMPLEQ_ENTRY(GoldfishPdevEntry) next;
} GoldfishPdevEntry;

struct GoldfishPdevBusState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    QSIMPLEQ_HEAD(, GoldfishPdevEntry) pending;
    GoldfishPdevEntry *current;
};

/*
 * Enfileira o anuncio de um dispositivo goldfish para o guest, na
 * proxima vez que ele fizer polling de PDEV_BUS_OP. Deve ser chamado
 * durante o init da maquina; quem chama continua responsavel por
 * criar/mapear o dispositivo real (fb/audio/nand/pipe) no mesmo
 * io_base/irq informados aqui -- esta funcao so cuida do anuncio, nao
 * cria o backend.
 *
 * Protocolo (registradores e maquina de estados) conferido contra o
 * driver real do kernel goldfish (arch/arm/mach-goldfish/pdev_bus.c).
 * O unico ponto ainda nao validado contra um boot real e a traducao
 * de endereco em PDEV_BUS_GET_NAME -- ver disclaimer detalhado em
 * hw/arm/goldfish_pdev_bus.c.
 */
void goldfish_pdev_bus_add(DeviceState *bus_dev, const char *name,
                            uint32_t id, hwaddr io_base, uint32_t io_size,
                            uint32_t irq, uint32_t irq_count);

/*
 * Enfileira a remocao do dispositivo cuja base MMIO e 'io_base' (e
 * exatamente assim que o driver real identifica qual remover -- nao
 * ha id separado para isso no protocolo).
 */
void goldfish_pdev_bus_remove(DeviceState *bus_dev, hwaddr io_base);

#endif /* HW_ARM_GOLDFISH_PDEV_BUS_H */
