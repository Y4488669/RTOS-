//
// Created by lx on 2026/9/21.
//



#include "./inc/sched.h"
#include "timer.h"
#include "port.h"
#include "./inc/task.h"
#include "./inc/list.h"
#include "tick.h"
/*
* 调度器初始化，目前只有就绪链表，清零即可
* 将任务加入就绪链表
* 任务切换
* 判断任务是否需要切换
* 任务主动让出CPU;
* 调度开始
*/
task_t * volatile current_task = NULL;

static list_t ready_list[CFG_PRIO_LEVELS];

static uint32_t ready_bitmap  = 0;
/*
* 链表初始化
* 清空就绪链表，位图清零
*/
void sched_init(void)
{
    timer_sys_init();
    task_init();
    for (uint32_t i = 0; i < CFG_PRIO_LEVELS ; i++ )
    {
        list_init(&ready_list[i]);
    }
    ready_bitmap = 0;
}

/* 将任务t插入对应优先级的链表尾部
* 位图使能
*/
void sched_ready(task_t *t)
{
    t->state = TASK_READY;
    list_insert_tail(&ready_list[t->cur_prio], &t->node);

    ready_bitmap |= (1u << t->cur_prio);   /* 幂等,不用先判断链表原来空不空 */
}


/* 位图里最高的那一档 —— 也就是"现在最该跑哪个优先级的任务" */
static uint32_t ready_top_prio(void)
{
    return 31u - (uint32_t)__builtin_clz(ready_bitmap);
}
static void pick_next(void)
{
    KERNEL_ASSERT(ready_bitmap != 0);   /* 至少有空闲任务兜底,不该为空 */
    uint32_t p =  ready_top_prio();
    task_t * t = list_first_entry(&ready_list[p],task_t, node);
    list_remove(&t->node);   /* ★ 出队:它不再"排队"了 */
    if(list_is_empty(&ready_list[p]))
    {
        ready_bitmap &= ~(1u << p);                 /* 这条队空了,把灯熄掉 */
    }
    t->state = TASK_RUNNING;
    current_task = t;

}

/*
* 将当前任务的状态改为就绪态，并插入就绪链表尾部
* 选择下一个任务
*/

void sched_switch(void)
{
    debug_stack_check(current_task);
    if (current_task->state == TASK_RUNNING)
    {
        sched_ready(current_task);
    }
    pick_next();
}







int sched_need_switch(void);

/* 只是"挂号",不是"立刻切"。原因见 port.c 里 PendSV 的说明:
 * 等所有中断处理完、PendSV 轮到自己时才真正切换。
 */

void sched_yield(void)
{
    port_pendsv_set();
}

/* ---- 空闲任务 ----
 * 为什么必须有:
 *   1) 就绪队列可能空(所有任务都在睡),那就没人可跑了 —— 空闲任务兜底;
 *   2) 它有正经用途:WFI 省电、栈溢出检查、回收已删除任务的栈。
 * 为什么优先级是 0:它是"实在没别的活干时才跑"的那个。*/

static void idle_task(void)
{
    for(;;)
    {
        /* 空闲任务的第三个职责(stage4 新增):回收别人丢掉的东西。
         *
         * 为什么是它来做?因为它用的是**自己的栈**,可以安全地去拆别人的栈 ——
         * 而被删掉的那个任务自己刚刚还在用那块栈(见 task.c 的长注释)。*/
        task_reclaim_deleted();

        /* WFI(Wait For Interrupt):把 CPU 挂起,直到下一个中断来。
         * SysTick 每 1ms 会叫醒它一次,所以最坏只睡 1ms;
         * 但它让 CPU 在"空转"这件事上不再耗电 —— 空闲任务存在的第一个理由。*/
        __asm volatile ("wfi");
    }
}


/*
* 1.创建空闲任务
* 2.选择当前任务，也就是给当前任务赋值
* 3.配置异常优先级，在开时基
* 4.首次启动:执行svc指令
* 5.死循环兜底，理论上不会跑到这里
*/
void sched_start()
{
    tick_init();
    task_create("idle", idle_task, 0 );
    pick_next();
    port_set_exception_priorities();
    port_systick_start();
    port_first_start();

    for (;;)
    {

    }

}


void sched_dequeue(task_t *t)
  {
      if (t->node.container == &ready_list[t->cur_prio])   /* 确实在就绪队列里 */
      {
          list_remove(&t->node);
          if (list_is_empty(&ready_list[t->cur_prio]))
          {
              ready_bitmap &= ~(1u << t->cur_prio);        /* 熄灯 */
          }
      }
      else
      {
          list_remove(&t->node);                            /* 在别的链上 / 没挂 */
      }
  }

void sched_set_priority(task_t *t ,uint32_t new_prio)
{
    KERNEL_ASSERT(new_prio < CFG_PRIO_LEVELS);
    if(t->cur_prio == new_prio)
    {
        return ;
    }
    if(t->state == TASK_READY)
    {
        list_remove(&t->node);
        if(list_is_empty(&ready_list[t->cur_prio]))
        {
            ready_bitmap &= ~(1u << t->cur_prio);
        }

        t->cur_prio = new_prio;
        sched_ready(t);
    }
    else 
        t->cur_prio = new_prio;
}


