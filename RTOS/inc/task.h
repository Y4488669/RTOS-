//
// Created by lx on 2026/7/30.
//

#ifndef RTOS_TASK_H
#define RTOS_TASK_H

#include "list.h"
#include "stdint.h"
typedef enum
{
    RUNNING = 0,
    READY ,
    STOP ,
}Task_Status;

struct Task
{
    uint32_t *sp;
    Task_Status status;
    list_node_t node;
};

typedef struct Task Task_t;


void sched_init(void);
void create_task(Task_t* task);
void init_stack();
#endif //RTOS_TASK_H
