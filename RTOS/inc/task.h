//
// Created by lx on 2026/7/30.
//

#ifndef RTOS_TASK_H
#define RTOS_TASK_H

#include "list.h"
#include "kernel_cfg.h"
#include <stdint.h>

/* 注:寄存器相关的宏(ICSR / SysTick / SHPR3)都在 port.h。
 * 这个头文件只关心"任务是什么",不关心它跑在哪颗芯片上。*/

/*                 任务状态
 * 就绪态
 * 运行态
 * 阻塞态
 * 挂起，人为挂起
 */
typedef enum
{
    TASK_READY = 0,     /* 可以跑,躺在**就绪队列**里排队 */
    TASK_RUNNING,       /* 正在 CPU 上跑 —— ★ 它**不在**就绪队列里 */
    TASK_BLOCKED,       /* 在等某件事(信号量 / 队列 / 时间),躺在别的链上 */
    TASK_SUSPENDED,     /* 被人为挂起,谁来都不理 */
    TASK_DELETED  ,
} task_state_t;

typedef enum
{
    TASK_WAIT_OK = 0,   /* 等到了 */
    TASK_WAIT_TIMEOUT,
}task_wait_result_t;



/* ---- 等待时长的三种约定(stage3 起)----
 *
 *     timeout_ticks == 0      → 不等,试一次就走(由 sem_take/queue_send 处理)
 *     0 < timeout < FOREVER   → 最多等这么多 tick,到点自动醒
 *     TASK_WAIT_FOREVER       → 一直等,只能被别人叫醒
 *
 * ★ 为什么不能拿"一个很大的数"当无限等?
 *   因为 tick 会回绕:`wake_tick = tick_get() + 0xFFFFFFFF` 算出来其实是
 *   `tick_get() - 1`,也就是"已经过期了" —— "无限等"会当场变成立刻超时。
 *   所以无限等必须是一个**被专门判断**的常量,而不是一个很大的数字。
 *   这一类"大数溢出成了负数"的坑,在和时间打交道的代码里到处都是。*/
#define TASK_WAIT_FOREVER  0xFFFFFFFFu





/* 任务本身
 * 栈指针
 * 栈顶
 * 入口函数
 * 退出函数
 * 基础优先级
 * 当前优先级
 * 状态
 * 节点
 * 名字，调试用
 * 栈空间
 */
typedef struct task
{
    uint32_t *sp;


    uint32_t *stack_top;

    void      (*entry)(void);   /* 任务函数,必须自己死循环不返回 */
    void      (*exit)(void);    /* 万一把 entry 跑完了(return 了),落到这里 */

    uint32_t    base_prio;      /* 创建时定的优先级,永不变 */
    uint32_t    cur_prio;       /* 当前生效优先级。stage3 的优先级继承会临时抬高它,
                                 * 所以"排队排哪条"必须看 cur_prio 而不是 base_prio */
    task_state_t state;


    list_node_t node;
    list_node_t  wait_node;

    uint32_t wake_tick ;
    task_wait_result_t wait_result;

    const char* name;

    uint32_t stack[CFG_STACK_WORDS];


}task_t;





task_t *task_create(const char *name, void (*entry)(void), uint32_t prio);

task_t *task_self(void);

void task_delay(uint32_t ticks);


void task_block_current(uint32_t timeout_ticks);

void task_unblock(task_t *t);



void task_suspend(task_t *t);
void task_resume(task_t *t);

void task_reclaim_deleted(void);


void task_delete(task_t *t);

void task_exit_hook(void);






void task_init(void);



#endif //RTOS_TASK_H
