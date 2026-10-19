#include "queue.h"
#include "kernel_cfg.h"
#include "port.h"
#include "tick.h"
#include <string.h>
#include "sync.h"
#include "task.h"
#include "sched.h"
/* 信号量与队列是不一样的
* 1.传递的参数，信号量是一个状态，而队列既可以存放状态，也可以存放具体的数据
* 2.内部机制才是具体两者不能直接替代的原因
*       信号量能够“转手”，队列不能，
*       信号量只有一个等待链表，所有信号共用，所以，sem_give直接通知，叫醒就结束了
*       但是队列不行，队列的发送与接收是分开的，发送方的数据只能自己拷进去，接收方不能越界
*       队列的发送与接收必须有一个中转的过程才行，类似与快递。发送与接收都需要去驿站拿，两者不需要见面
*/

static void *queue_slot(const queue_t *q, uint32_t i)
{
    return q->buf + (uint32_t)(i * q->item_size);
}

static uint32_t remaining_wait(uint32_t deadline, uint32_t timeout_ticks)
{
    if(timeout_ticks == TASK_WAIT_FOREVER)
    {
        return TASK_WAIT_FOREVER;
    }

    int32_t left = (int32_t)(deadline - tick_get());

    return (left < 0) ? 0 : (uint32_t)left;
}


void queue_init(queue_t *q, void *buf, uint32_t item_size, uint32_t depth)
{
    KERNEL_ASSERT(buf != 0);
    KERNEL_ASSERT(item_size > 0);
    KERNEL_ASSERT(depth > 0);
    q->buf = (uint8_t *)buf;
    q->item_size = item_size;
    q->depth = depth;
    q->head = 0;
    q->count = 0;

    list_init(&q->send_wait);
    list_init(&q->recv_wait);
}



int queue_send(queue_t *q, const void *item, uint32_t timeout_ticks)
{
    uint32_t deadline = tick_get() + timeout_ticks;

    for(;;)
    {
        port_enter_critical();
        /* 有位置  */
        if(q->count < q->depth )
        {
            uint32_t tail = (q->head + q->count) % q->depth;
            memcpy(queue_slot(q,tail), item, q->item_size);
            q->count++;

            if(!list_is_empty(&q->recv_wait))
            {
                task_unblock(list_first_entry(&q->recv_wait, task_t, wait_node));
            }
            port_exit_critical();
            return SYNC_OK;
        }
        /* 满了 */

        uint32_t wait = remaining_wait(deadline, timeout_ticks);

        if(wait == 0)
        {
            port_exit_critical();
            return SYNC_TIMEOUT;
        }

        list_insert_tail(&q->send_wait, &current_task->wait_node);

        task_block_current(wait);  //其实这里用wait并不合适，因为这二个wait的时间会偏长
        port_exit_critical();

        sched_yield();


        if(current_task->wait_result != TASK_WAIT_OK)
        {
            return SYNC_TIMEOUT;
        }





    }
}




int queue_recv(queue_t *q, void *item, uint32_t timeout_ticks)
{
    uint32_t deadline = tick_get() + timeout_ticks;

    for(;;)
    {
        port_enter_critical();
        /* 有数据：读走 */
        if(q->count > 0)
        {
            memcpy(item, queue_slot(q, q->head),q->item_size);
            q->head = (q->head + 1) % q->depth;
            q->count--;
            /* 位置空了，唤醒等待的任务 */
            if(!list_is_empty(&q->send_wait))
            {
                task_unblock(list_first_entry(&q->send_wait, task_t, wait_node));
            }
            port_exit_critical();
            return SYNC_OK;

        }
        /* 没数据，等待 */

        uint32_t wait = remaining_wait(deadline, timeout_ticks);

        if(wait == 0)
        {
            port_exit_critical();
            return SYNC_TIMEOUT;
        }

        list_insert_tail(&q->recv_wait, &current_task->wait_node);
        task_block_current(wait);
        port_exit_critical();


        sched_yield();

        if(current_task-> wait_result != TASK_WAIT_OK)
        {
            return SYNC_TIMEOUT;
        }

    }
}






void mailbox_init(mailbox_t *m, void* buf, uint32_t item_size)
{
    queue_init(&m->q, buf, item_size, 1);
}

int mailbox_put(mailbox_t *m, const void *item)
{
    port_enter_critical();
    memcpy(queue_slot(&m->q, 0), item, m->q.item_size);

    if(m->q.count == 0 )
    {
        m->q.count = 1;
        if(!list_is_empty(&m->q.recv_wait))
        {
            task_unblock(list_first_entry(&m->q.recv_wait, task_t, wait_node));
        }
    }
    port_exit_critical();
    return SYNC_OK;


}
int mailbox_get(mailbox_t *m, void *item, uint32_t timeout_ticks)
{
    return queue_recv(&m->q, item, timeout_ticks);
}

/* queue.h 里声明了它,但之前一直没实现 —— 给 demo/调试用,只读 count。*/
uint32_t queue_count(const queue_t *q)
{
    return q->count;
}
