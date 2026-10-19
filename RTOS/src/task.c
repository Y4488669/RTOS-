//
// Created by lx on 2026/7/30.
//
#include "../inc/task.h"
#include "../inc/port.h"
#include "../inc/sched.h"
#include "tick.h"
static task_t task_pool[CFG_MAX_TASKS];

static uint32_t slot_bitmap = 0;        /* bit i = 1 表示第 i 个槽正在用。
                                         * (位图宽度是 32,所以 CFG_MAX_TASKS 别超 32)*/



static list_t deleted_list;                                         




void task_init(void)
{
    list_init(&deleted_list);
}

static task_t *slot_alloc(void)
{
    KERNEL_ASSERT(slot_bitmap != ((1u << CFG_MAX_TASKS) - 1u));   /* 池子满了 */

    uint32_t i = 0;
    while (slot_bitmap & (1u << i))
    {
        i++;
    }

    slot_bitmap |= (1u << i);
    return &task_pool[i];
}

void task_exit_hook(void)
{
    /* 任务函数"跑完了"(return)会落到这里 —— 这是"任务自然结束"的正规退出路径。
     * 删掉自己之后,调度器就不会再理它(sched_switch 看到 DELETED 不会放回就绪队列)。*/
    task_delete(task_self());

    /* 兜底:正常情况下 PendSV 会在这一步之后立刻抢进来(task_delete 内部已经 sched_yield),
     * 下面这个循环实际跑不到。*/
    for (;;)
    {
    }
}

task_t *task_create(const char *name, void (*entry)(void), uint32_t prio)
{
    KERNEL_ASSERT(prio < CFG_PRIO_LEVELS);
    KERNEL_ASSERT(entry != 0);
    task_t *t = slot_alloc();
    t->name = name;
    t->entry = entry;
    t->exit = task_exit_hook;
    t->base_prio = prio;
    t->cur_prio = prio;
    t->state = TASK_READY;


    /* 先把整块栈灌成填充模式。后面每次切换都会检查栈底那几个字有没有被改过,
     * 改过了就说明栈溢出 —— 这是最便宜的栈溢出检测手段(见 debug.c)。*/
    debug_stack_fill(t);

    /* 再伪造初始栈帧(硬件知识,见 port.c 的栈布局图)。
     * 顺序:先灌填充,再摆现场 —— 现场只占栈顶那一小段。*/
    port_init_stack(t);

    /* 入就绪队列,并判断要不要立刻抢占 —— 抢占的"全部代码量"就是下面这几行 */
    port_enter_critical();
    {
        sched_ready(t);

        /* 新任务若比正在跑的优先级高,现在就该切过去。
         * 这里只是"挂号":PendSV 优先级最低,正被临界区的 BASEPRI 挡着,
         * 等退出临界区、所有中断都处理完之后才真正执行。*/
    }
    port_exit_critical();

    return t;
}



task_t *task_self(void)
{
    return current_task;
}






void task_delay(uint32_t ticks)
{
    if (ticks == 0)
    {
        /* delay(0) 的语义:不睡,只让出一下。
         * (FreeRTOS 的 vTaskDelay(0) 会真的等到**下一个 tick** 才回来;
         *  这是两个内核的一个细微差别,写代码时要知道自己在用哪个语义。)*/
        sched_yield();
        return;
    }

    task_block_current(ticks);   /* 挂起自己(内部进临界区)*/
    sched_yield();  
}


void task_block_current(uint32_t timeout_ticks)
{
    KERNEL_ASSERT(current_task != 0);   /* 调度器还没启动,谈不上"阻塞" */
    port_enter_critical();
    {
        task_t *t = current_task;

        t->state = TASK_BLOCKED;
        /* "不等"(0)是调用方自己处理的分支,不该走到这里 */
        KERNEL_ASSERT(timeout_ticks != 0);

        if (timeout_ticks != TASK_WAIT_FOREVER)
        {

            t->wake_tick   = tick_get() + timeout_ticks;
            t->wait_result = TASK_WAIT_TIMEOUT;   /* 预设:没等到东西就是超时 */
            tick_delay_insert(t);
        }
        else
        {
            /* 无限等:只有"等到了"这一种结局 */
            t->wait_result = TASK_WAIT_OK;
        }

    }
    port_exit_critical();
}

void task_unblock(task_t *t)
{
    port_enter_critical();
    {
        list_remove(&t->node);
        list_remove(&t->wait_node);

        t->wait_result = TASK_WAIT_OK;   /* 是被叫醒的,不是超时 */
        sched_ready(t);
    }
    port_exit_critical();
}



void task_suspend(task_t *t)
{
    int self = (t == task_self());

    port_enter_critical();
    {

        sched_dequeue(t);
        list_remove(&t->wait_node);
        t->state = TASK_SUSPENDED;
    }
    port_exit_critical();

    if (self)
    {
        /* 挂起的是自己:得让出去,否则接下来就没有语句可以执行了。
         * 挂起的任务只能被**别人**恢复 —— 它自己已经没有 CPU 了。*/
        sched_yield();
    }
}


void task_resume(task_t *t)
{
    port_enter_critical();
    {
        if (t->state == TASK_SUSPENDED)
        {
            sched_ready(t);
        }
        /* 本版的简化:如果被挂起时它正在等信号量/睡延时,那个等待已经被
         * task_suspend 丢掉了,恢复之后它只会"从就绪队列重新开始"。
         * FreeRTOS 更细致:挂起不取消等待,恢复后接着等剩下的时间。
         * 真要做对,就得在 SUSPENDED 时再记住"它原来在哪条链上" ——
         * 又一个"状态多了就要多记账"的例子。*/
    }
    port_exit_critical();
}


void task_delete(task_t *t)
{
    int self = (t == task_self());

    port_enter_critical();
    {
        list_remove(&t->node);
        list_remove(&t->wait_node);
        t->state = TASK_DELETED;

        /* ★ 挂进"待回收"名单,而不是当场回收。
         *   因为我们可能正在删**自己** —— 而自己此刻正踩着那块栈在跑,
         *   把栈清掉等于把脚下的地板抽了。所以只能先记账,
         *   等确定它不再运行之后,由空闲任务来回收(见 task_reclaim_deleted)。*/
        list_insert_tail(&deleted_list, &t->node);
    }
    port_exit_critical();

    if (self)
    {
        /* 删自己:让出去。
         * sched_switch() 看到状态是 DELETED(而不是 RUNNING),就不会把它放回
         * 就绪队列 —— 它的栈从此再也不会被恢复,这个任务就"消失"了。
         * 换句话说,"任务自己结束"这件事**不需要任何新机制**:
         * 只要状态机是干净的,调度器自然就不理它了。
         *
         * ⚠️ 但请注意:sched_yield() 只是"挂号",PendSV 要等到当前这几条
         *    指令走完、且没有更高优先级中断在跑的时候才执行。所以
         *    "删掉自己之后的几行代码"很可能还是会执行 —— 别在那里做事。*/
        sched_yield();
    }
}





/* ===========================================================================
 * 回收:把已经"彻底不再运行"的已删除任务还回池子
 * ===========================================================================
 * 由空闲任务调用(见 sched.c 的 idle_task)。这是空闲任务存在的第三个理由
 * (前两个是:兜底"没人可跑"、WFI 省电)。
 *
 * ★ 为什么删除要绕这一圈,不能当场回收?
 *
 *   因为"删掉自己"的任务**还在用自己那块栈** —— 它此刻正踩在上面执行代码。
 *   当场把栈清掉 = 把脚下的地板抽了。所以只能:
 *       ① 先把它挂进待回收名单(它马上就会被切换走);
 *       ② 等它不再是 current_task,由空闲任务(用的是空闲任务自己的栈)回收。
 *
 *   这正是 FreeRTOS 里 vTaskDelete 最后一定要落到空闲任务上的原因 ——
 *   它文档里只说了"删除的 TCB/栈由空闲任务释放",没说为什么。
 *   现在你知道了。
 */
void task_reclaim_deleted(void)
{
    list_node_t *pos = deleted_list.head.next;

    while (pos != &deleted_list.head)
    {
        task_t *t = container_of(pos, task_t, node);

        /* 先存好下一个:下面一旦 list_remove(pos),pos->next 就被清成 NULL 了。
         * (list.h 里那句提醒在这里用上了。)*/
        list_node_t *next = pos->next;

        /* 判断"它是不是真的不跑了":
         *   state == DELETED            —— 确定被删了;
         *   t != current_task           —— 而且此刻不是它在 CPU 上。
         * 两条一起构成"这个任务已经不在任何栈上执行"的充分条件
         * (不变量:正在跑的任务**就是** current_task)。*/
        if (t->state == TASK_DELETED && t != current_task)
        {
            list_remove(&t->node);                     /* 从待回收名单上摘掉 */

            uint32_t idx = (uint32_t)(t - task_pool);
            slot_bitmap &= ~(1u << idx);              /* 槽还回池子 */
            t->name = 0;                              /* 别在调试器里留个"幽灵任务" */
        }

        pos = next;
    }
}
