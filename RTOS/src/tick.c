#include "tick.h"

#include "list.h"
#include "task.h"
#include "sched.h"
#include "timer.h"

static volatile uint32_t tick_count = 0;

static list_t delay_list;


void tick_init(void)
{

    list_init(&delay_list);
}

uint32_t tick_get(void)
{
    return tick_count;
}

static void  tick_wake_expired(void)
{
    while (!list_is_empty(&delay_list)) {
        task_t *t = list_first_entry(&delay_list, task_t, node); 
        /* 判断"到点没有"用**有符号差值**,不要直接比大小:tick_count 是 32 位的,
         * 总会回绕(1kHz 下约 49.7 天归零一次),回绕那一刻直接比较会判错。*/
        if ((int32_t)(tick_count - t->wake_tick) < 0)
        {
            break;      /* 队头还没到点,后面(更晚的)更加没到 —— 收工 */
        }

        list_remove(&t->node);        /* 从延时链表上摘下来 */
        list_remove(&t->wait_node);   /* 可能还挂在某个对象的等待队列上(幂等) */
        sched_ready(t);               /* 放回就绪队列;wait_result 保持预设的"超时" */
    }
}
void tick_increment(void)
{
    tick_count++;
    tick_wake_expired();

    timer_check_expired();
}

uint32_t tick_ms_to_ticks(uint32_t ms)
{
    return (uint32_t)(((uint64_t)ms * CFG_TICK_HZ) / 1000u);
}


void tick_delay_insert(task_t *t)
{
    list_node_t *pos = delay_list.head.next;
    while (pos != &delay_list.head) {
        task_t *other = container_of(pos, task_t, node);
        /* 排序键也用有符号差值:链表按"谁先到点"升序,回绕时也不能乱序。*/
        if ((int32_t)(other->wake_tick - t->wake_tick) > 0)
        {
            break;
        }
        pos = pos->next;
    }
    list_insert_before(pos, &t->node);
}