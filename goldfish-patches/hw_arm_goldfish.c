/*
 * Goldfish ARM machine (Android emulator board, 2.3.x/4.x era)
 *
 * Porta mínima para a árvore moderna do QEMU (qemu-wasm), reaproveitando
 * os dispositivos hw/intc/goldfish_pic.c e hw/char/goldfish_tty.c que já
 * existem upstream (adicionados originalmente para a m68k "virt" board),
 * porque o layout de registradores é o mesmo documentado em
 * docs/GOLDFISH-VIRTUAL-HARDWARE.TXT do fork da AOSP.
 *
 * Endereços físicos confirmados a partir de
 * arch/arm/mach-goldfish/include/mach/hardware.h (kernel goldfish original):
 *
 *   0xff000000  goldfish interrupt controller (goldfish_pic)
 *   0xff001000  goldfish platform bus (device.c / pdev_bus)  -- AINDA NAO PORTADO
 *   0xff002000  goldfish tty #0 (console)
 *   0xff003000  goldfish timer                                -- AINDA NAO PORTADO
 *
 * STATUS DESTE ARQUIVO: builda uma placa com CPU + RAM + PIC + console
 * serial. NAO inclui ainda o goldfish platform bus (necessario para o
 * kernel enumerar fb/audio/events/nand/pipe) nem o goldfish timer
 * (necessario pro scheduler ter uma fonte de tempo). Sem esses dois, o
 * kernel do Android trava logo depois de imprimir as primeiras linhas
 * de boot no console. Isso ainda precisa ser testado num boot real
 * antes de confiar nele.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/boards.h"
#include "hw/intc/goldfish_pic.h"
#include "hw/char/goldfish_tty.h"
#include "hw/timer/goldfish_timer.h"
#include "hw/sysbus.h"
#include "exec/address-spaces.h"
#include "cpu.h"

#define GOLDFISH_IO_BASE        0xff000000
#define GOLDFISH_PIC_OFFSET     0x00000
#define GOLDFISH_PDEV_OFFSET    0x01000   /* TODO: ainda nao implementado */
#define GOLDFISH_TTY_OFFSET     0x02000
#define GOLDFISH_TIMER_OFFSET   0x03000   /* TODO: ainda nao implementado */

/* IRQ de entrada unica do goldfish_pic no CPU ARM926 (IRQ, nao FIQ) */
#define GOLDFISH_PIC_CPU_IRQ    0

/*
 * Linha de IRQ do console dentro do goldfish_pic. O board-goldfish.c
 * original nao hard-codava isso (vinha do platform bus), entao esse
 * valor e uma estimativa que precisa ser confirmada contra um boot
 * real (procurar por "goldfish_tty.0: probe" ou similar no dmesg).
 */
#define GOLDFISH_TTY0_PIC_IRQ   4

/* IRQ_TIMER = 3 no hardware.h original — essa e a linha confirmada
 * (diferente da do TTY, que e estimativa). */
#define GOLDFISH_TIMER_PIC_IRQ  3

static void goldfish_arm_init(MachineState *machine)
{
    ARMCPU *cpu;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    DeviceState *pic_dev;
    DeviceState *tty_dev;

    cpu = ARM_CPU(cpu_create(machine->cpu_type));

    memory_region_init_ram(ram, NULL, "goldfish.ram",
                            machine->ram_size, &error_fatal);
    memory_region_add_subregion(sysmem, 0x00000000, ram);

    /* Interrupt controller: ja existe upstream, so precisa ser mapeado
     * no offset correto e cascateado na linha de IRQ da CPU. */
    pic_dev = sysbus_create_varargs(TYPE_GOLDFISH_PIC,
                                     GOLDFISH_IO_BASE + GOLDFISH_PIC_OFFSET,
                                     qdev_get_gpio_in(DEVICE(cpu),
                                                       GOLDFISH_PIC_CPU_IRQ),
                                     NULL);

    /* Console: idem, ja existe upstream (goldfish_tty.c). */
    tty_dev = qdev_new(TYPE_GOLDFISH_TTY);
    qdev_prop_set_chr(tty_dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(tty_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(tty_dev), 0,
                     GOLDFISH_IO_BASE + GOLDFISH_TTY_OFFSET);
    sysbus_connect_irq(SYS_BUS_DEVICE(tty_dev), 0,
                        qdev_get_gpio_in(pic_dev, GOLDFISH_TTY0_PIC_IRQ));

    /* Timer: escrito do zero (nao existe upstream), fonte de clock do
     * scheduler do kernel. Sem ele o boot trava logo apos o console. */
    DeviceState *timer_dev = qdev_new(TYPE_GOLDFISH_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer_dev), 0,
                     GOLDFISH_IO_BASE + GOLDFISH_TIMER_OFFSET);
    sysbus_connect_irq(SYS_BUS_DEVICE(timer_dev), 0,
                        qdev_get_gpio_in(pic_dev, GOLDFISH_TIMER_PIC_IRQ));

    /* TODO: goldfish platform bus (0xff001000, IRQ 1) — precisa ser
     * escrito do zero, nao existe upstream. Sem ele o kernel nao
     * enumera fb/audio/events/nand/pipe (mas ja da pra ver o kernel
     * "andar" no console com timer + pic + tty presentes). */

    if (machine->kernel_filename) {
        struct arm_boot_info *boot_info = g_new0(struct arm_boot_info, 1);
        boot_info->ram_size = machine->ram_size;
        boot_info->board_id = 1441; /* MACH_GOLDFISH, ver arch/arm/tools/mach-types */
        boot_info->kernel_filename = machine->kernel_filename;
        boot_info->kernel_cmdline = machine->kernel_cmdline;
        boot_info->initrd_filename = machine->initrd_filename;
        arm_load_kernel(cpu, machine, boot_info);
    }
}

static void goldfish_arm_machine_init(MachineClass *mc)
{
    mc->desc = "Android Goldfish ARM (2.3.x/4.x, WIP)";
    mc->init = goldfish_arm_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    mc->max_cpus = 1;
    mc->default_ram_size = 128 * 1024 * 1024;
}

DEFINE_MACHINE("goldfish", goldfish_arm_machine_init)
