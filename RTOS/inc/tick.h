#ifndef  RTOS_TICK_H
#define  RTOS_TICK_H


#include "task.h"
#include <stdint.h>

void tick_init(void);

uint32_t tick_get(void);


void tick_increment(void);

uint32_t tick_ms_to_ticks(uint32_t ms);


void tick_delay_insert(task_t *t);

#endif  //RTOS_TICK_H