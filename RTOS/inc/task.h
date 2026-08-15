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

typedef struct Task_Struct
{
    char *name;
    Task_Status status;
    uint32_t stack_size;
}Task_Struct, *Task_Struct_Ptr;


void Task_Init(Task_Struct_Ptr task);

#endif //RTOS_TASK_H
