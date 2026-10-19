//
// Created by lx on 2026/9/21.
//

#ifndef RTOS_SCHED_H
#define RTOS_SCHED_H

#endif //RTOS_SCHED_H

#include "../inc/task.h"

/*
* 调度器初始化，目前只有就绪链表，清零即可
* 将任务加入就绪链表
* 任务切换
* 判断任务是否需要切换
* 任务主动让出CPU;
* 调度开始
*/
extern  task_t * volatile current_task;

/*
* 链表初始化
* 清空就绪链表，位图清零
*/
void sched_init(void);

/* 将任务t插入对应优先级的链表尾部
* 位图使能
*/
void sched_ready(task_t *t);


/*
* 将当前任务的状态改为就绪态，并插入就绪链表尾部
* 选择下一个任务
*/

void sched_switch(void);





int sched_need_switch(void);

/* 只是"挂号",不是"立刻切"。原因见 port.c 里 PendSV 的说明:
 * 等所有中断处理完、PendSV 轮到自己时才真正切换。
 */

void sched_yield(void);


/*
* 1.创建空闲任务
* 2.选择当前任务，也就是给当前任务赋值
* 3.配置异常优先级，在开时基
* 4.首次启动:执行svc指令
* 5.死循环兜底，理论上不会跑到这里
*/

void sched_start();





void sched_set_priority(task_t *t ,uint32_t new_prio);


void sched_dequeue(task_t *t);
