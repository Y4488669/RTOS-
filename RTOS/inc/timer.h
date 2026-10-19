#ifndef __RTOS_TIMER_H
#define __RTOS_TIMER_H

#include "list.h"
#include <stdint.h>


typedef void (*timer_cb_t)(void *arg);

typedef struct timer{
    list_node_t node;
    timer_cb_t cb;
    void *arg;
    uint32_t period;
    uint32_t expire;
    uint8_t periodic;
    uint8_t active;
}timer_h;


void timer_sys_init(void);

void timer_init(timer_h *t, timer_cb_t, void *arg, uint32_t period_ticks, int periodic);


void timer_start(timer_h *t);
void timer_stop(timer_h *t);
void timer_restart(timer_h *t);

void timer_check_expired(void);


#endif //RTOS_TIMER
