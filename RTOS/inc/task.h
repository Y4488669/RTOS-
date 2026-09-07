//
// Created by lx on 2026/7/30.
//

#ifndef RTOS_TASK_H
#define RTOS_TASK_H

#include "list.h"
#include "stdint.h"

#define STACK_SIZE 256
#define STACK_NUM 4
#define SCHEDULE_INTERRUPT_PRIORITY 6 //在调度过程中禁止中断的阈值，使用basepi特出功能寄存器，其实就算不关中断也是没有任何问题的

typedef enum
{
    RUNNING = 0,
    READY ,
    STOP ,
}Task_Status;


struct TCB
{
    uint32_t *sp;
};

typedef struct Task Task_t;
struct Task
{
    struct TCB *sp;
    void (*entry)(void);
    void (*exit)(void);
    Task_Status status;
    list_node_t node;
};
struct Stack
{
    uint32_t stack[STACK_SIZE];
};
typedef struct Stack Stack;






//简易调度函数
void find_task();


void tick_config();
void sched_init(void);

void task_head_init();

void init_stack(Task_t* task);
void create_task(Task_t* task);





#endif //RTOS_TASK_H
