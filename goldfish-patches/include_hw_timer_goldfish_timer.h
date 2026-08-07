#ifndef HW_TIMER_GOLDFISH_TIMER_H
#define HW_TIMER_GOLDFISH_TIMER_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GOLDFISH_TIMER "goldfish_timer"
OBJECT_DECLARE_SIMPLE_TYPE(GoldfishTimerState, GOLDFISH_TIMER)

struct GoldfishTimerState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;

    uint32_t time_high_latch;
    uint32_t alarm_high_pending;
    uint64_t alarm_next;
    bool alarm_running;
    uint32_t irq_pending;
};

#endif /* HW_TIMER_GOLDFISH_TIMER_H */
