//
// Created by lx on 2026/8/5.
//

#ifndef RTOS_KERNEL_H
#define RTOS_KERNEL_H

//
// Created by lx on 2026/7/30.
//
#include "stdint.h"
#define STACK_SIZE (128)


typedef struct
{
    uint32_t* sp; /* 栈指针 */
} TCB;



void init_statck(TCB* tcb, uint32_t *stack,void (*entry)(void));   /* 用于初始化栈，伪造出栈的数据 */
void Register(TCB* tcb1,TCB* tcb2);





void start_scheduler(void);



#endif //RTOS_KERNEL_H
