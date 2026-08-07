/*
 * Goldfish virtual timer device
 *
 * Baseado no registrador documentado em docs/GOLDFISH-VIRTUAL-HARDWARE.TXT
 * (secao III, "Goldfish timer") do fork da AOSP. Diferente do goldfish_rtc
 * (que ja existe upstream e usa tempo de parede/host), este dispositivo
 * usa o QEMU_CLOCK_VIRTUAL (equivalente ao antigo vm_clock) e e a fonte
 * de tick do scheduler do kernel Android nas placas goldfish ARM/MIPS
 * antigas. Sem ele, o kernel nao tem heartbeat e trava logo apos o boot.
 *
 * Registradores (offsets de 32 bits):
 *   0x00 TIME_LOW          R: le os 32 bits baixos do tempo atual (ns);
 *                             tambem "trava" os 32 bits altos para a
 *                             proxima leitura de TIME_HIGH.
 *   0x04 TIME_HIGH         R: le os 32 bits altos travados na ultima
 *                             leitura de TIME_LOW.
 *   0x08 ALARM_LOW         W: define os 32 bits baixos do alarme.
 *   0x0c ALARM_HIGH        W: define os 32 bits altos do alarme e o arma
 *                             (deve ser escrito ANTES de ALARM_LOW,
 *                             conforme o spec original).
 *   0x10 CLEAR_INTERRUPT   W: abaixa a IRQ do dispositivo.
 *   0x14 CLEAR_ALARM       W: desarma o alarme, se houver um pendente.
 *
 * STATUS: escrito a partir do spec, ainda NAO testado contra um kernel
 * goldfish real booting. A ordem de escrita ALARM_HIGH -> ALARM_LOW
 * (inversa a do goldfish_rtc) e a parte mais fragil e merece atencao
 * extra ao debugar.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/timer/goldfish_timer.h"

#define GOLDFISH_TIMER_TIME_LOW        0x00
#define GOLDFISH_TIMER_TIME_HIGH       0x04
#define GOLDFISH_TIMER_ALARM_LOW       0x08
#define GOLDFISH_TIMER_ALARM_HIGH      0x0c
#define GOLDFISH_TIMER_CLEAR_INTERRUPT 0x10
#define GOLDFISH_TIMER_CLEAR_ALARM     0x14

static void goldfish_timer_update_irq(GoldfishTimerState *s)
{
    qemu_set_irq(s->irq, s->irq_pending);
}

static void goldfish_timer_rearm(GoldfishTimerState *s)
{
    if (s->alarm_running) {
        timer_mod(s->timer, s->alarm_next);
    } else {
        timer_del(s->timer);
    }
}

static void goldfish_timer_tick(void *opaque)
{
    GoldfishTimerState *s = GOLDFISH_TIMER(opaque);

    s->alarm_running = false;
    s->irq_pending = 1;
    goldfish_timer_update_irq(s);
}

static uint64_t goldfish_timer_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    GoldfishTimerState *s = GOLDFISH_TIMER(opaque);
    uint64_t now;

    switch (offset) {
    case GOLDFISH_TIMER_TIME_LOW:
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->time_high_latch = (uint32_t)(now >> 32);
        return (uint32_t)now;

    case GOLDFISH_TIMER_TIME_HIGH:
        return s->time_high_latch;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "goldfish_timer: leitura invalida em 0x%" HWADDR_PRIx,
                      offset);
        return 0;
    }
}

static void goldfish_timer_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    GoldfishTimerState *s = GOLDFISH_TIMER(opaque);

    switch (offset) {
    case GOLDFISH_TIMER_ALARM_HIGH:
        /* Deve ser escrito ANTES de ALARM_LOW (spec original). */
        s->alarm_high_pending = (uint32_t)value;
        break;

    case GOLDFISH_TIMER_ALARM_LOW:
        s->alarm_next = ((uint64_t)s->alarm_high_pending << 32) |
                         (uint32_t)value;
        s->alarm_running = true;
        goldfish_timer_rearm(s);
        /* Se o alarme ja esta no passado, dispara imediatamente. */
        if (s->alarm_next <= qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
            goldfish_timer_tick(s);
        }
        break;

    case GOLDFISH_TIMER_CLEAR_INTERRUPT:
        s->irq_pending = 0;
        goldfish_timer_update_irq(s);
        break;

    case GOLDFISH_TIMER_CLEAR_ALARM:
        s->alarm_running = false;
        goldfish_timer_rearm(s);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "goldfish_timer: escrita invalida em 0x%" HWADDR_PRIx,
                      offset);
        break;
    }
}

static const MemoryRegionOps goldfish_timer_ops = {
    .read = goldfish_timer_read,
    .write = goldfish_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void goldfish_timer_reset(DeviceState *dev)
{
    GoldfishTimerState *s = GOLDFISH_TIMER(dev);

    s->time_high_latch = 0;
    s->alarm_high_pending = 0;
    s->alarm_next = 0;
    s->alarm_running = false;
    s->irq_pending = 0;
    timer_del(s->timer);
}

static void goldfish_timer_realize(DeviceState *dev, Error **errp)
{
    GoldfishTimerState *s = GOLDFISH_TIMER(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, goldfish_timer_tick, s);
    memory_region_init_io(&s->iomem, OBJECT(dev), &goldfish_timer_ops, s,
                           TYPE_GOLDFISH_TIMER, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription goldfish_timer_vmstate = {
    .name = TYPE_GOLDFISH_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(alarm_next, GoldfishTimerState),
        VMSTATE_BOOL(alarm_running, GoldfishTimerState),
        VMSTATE_UINT32(irq_pending, GoldfishTimerState),
        VMSTATE_UINT32(time_high_latch, GoldfishTimerState),
        VMSTATE_UINT32(alarm_high_pending, GoldfishTimerState),
        VMSTATE_TIMER_PTR(timer, GoldfishTimerState),
        VMSTATE_END_OF_LIST()
    }
};

static void goldfish_timer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = goldfish_timer_realize;
    dc->reset = goldfish_timer_reset;
    dc->vmsd = &goldfish_timer_vmstate;
}

static const TypeInfo goldfish_timer_info = {
    .name = TYPE_GOLDFISH_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GoldfishTimerState),
    .class_init = goldfish_timer_class_init,
};

static void goldfish_timer_register(void)
{
    type_register_static(&goldfish_timer_info);
}

type_init(goldfish_timer_register)
