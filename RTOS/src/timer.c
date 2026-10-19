/**
 * timer.c —— 软件定时器
 * ---------------------------------------------------------------------------
 * 实现只有一条链表,而且和 tick.c 的延时链表是**同一个套路**:
 *
 *   一条按"到期时刻"升序排的链表
 *   + 每次 tick 看队头
 *   + 到点就从队头取下来处理
 *
 * 这不是巧合:凡是"在未来的某个时刻做某事"的机制,数据结构都长这样。
 * 你把这一条吃透了,延时、定时器、超时重传、协议状态机的超时……全是它。
 */
#include "timer.h"
#include "tick.h"
#include "port.h"

/* 按 expire 升序的定时器链表 */
static list_t timer_list;

void timer_sys_init(void)
{
    list_init(&timer_list);
}

/* 有序插入:找到第一个"比我晚到期"的,插到它前面。
 * 和 tick.c 里 tick_delay_insert() 是同一段逻辑 —— 有意没有抽成公共函数,
 * 你可以对照着看一遍,体会"这个模式在哪儿都能用"。*/
static void timer_insert(timer_h *t)
{
    list_node_t *pos = timer_list.head.next;

    while (pos != &timer_list.head)
    {
        timer_h *other = container_of(pos, timer_h, node);

        if ((int32_t)(other->expire - t->expire) > 0)
        {
            break;
        }

        pos = pos->next;
    }

    list_insert_before(pos, &t->node);
}

void timer_init(timer_h *t, timer_cb_t cb, void *arg, uint32_t period_ticks, int periodic)
{
    KERNEL_ASSERT(cb != 0);
    KERNEL_ASSERT(period_ticks > 0);

    t->cb       = cb;
    t->arg      = arg;
    t->period   = period_ticks;
    t->periodic = periodic ? 1 : 0;
    t->expire   = 0;
    t->active   = 0;
    t->node.container = 0;      /* 还没挂在任何链上 */
}

void timer_start(timer_h *t)
{
    port_enter_critical();
    {
        /* 已经在跑的话先摘下来。不然同一个节点会在链上挂两次,
         * 链表立刻被搞坏 —— 这种"重复 start"在真实代码里很常见。*/
        if (t->active)
        {
            list_remove(&t->node);
        }

        t->expire = tick_get() + t->period;    /* 从现在开始算 */
        t->active = 1;
        timer_insert(t);
    }
    port_exit_critical();
}

void timer_stop(timer_h *t)
{
    port_enter_critical();
    {
        list_remove(&t->node);      /* 幂等:不在链上时什么都不做 */
        t->active = 0;
    }
    port_exit_critical();
}

void timer_restart(timer_h *t)
{
    timer_stop(t);
    timer_start(t);
}

int timer_is_active(const timer_h *t)
{
    return t->active;
}

/* 被 tick_increment() 调用。★ 本函数运行在 **tick 中断**里 ——
 * 回调也是在这里跑的,所以回调的约束(短、不能阻塞)是硬约束,不是建议。*/
void timer_check_expired(void)
{
    while (!list_is_empty(&timer_list))
    {
        timer_h *t = container_of(timer_list.head.next, timer_h, node);

        /* 队头还没到点 → 后面更没到(链表是升序的)*/
        if ((int32_t)(tick_get() - t->expire) < 0)
        {
            break;
        }

        if (t->periodic)
        {
            /* ★ 周期定时器**先重新入队,再跑回调**。两个理由:
             *
             *   1) 回调里可能会 stop / restart 自己,先把链表状态摆正最省心;
             *   2) 重新入队用 `expire += period`,而不是 `tick_get() + period`。
             *      后者会把"跑回调花掉的那点时间"一点点累加进去,
             *      于是周期会越跑越长 —— 这叫**漂移**。
             *      按"上次该到点的时刻"往后推,就不会漂。
             *      (同样的道理:task_delay 是相对延时,会漂;FreeRTOS 为此
             *       另做了一个 vTaskDelayUntil,按绝对时刻推迟。
             *       本内核没做那个 API —— 你已经知道它为什么要存在了。)*/
            t->expire += t->period;
            list_remove(&t->node);
            timer_insert(t);

            /* ⚠️ 副作用:如果回调耗时超过一个周期,这里会连着触发好几次
             *    "把欠的补上"。要不要"跳过补触发"是个策略选择(FreeRTOS 也是照触发)。
             *    但正常情况下你不该有这个问题 —— 有,就说明回调写得太长了。*/
        }
        else
        {
            list_remove(&t->node);
            t->active = 0;
        }

        /* 跑回调。此刻在中断上下文里,而且用的是 MSP(中断栈)——
         * 所以回调里的局部变量不占任务的栈。*/
        t->cb(t->arg);
    }
}
