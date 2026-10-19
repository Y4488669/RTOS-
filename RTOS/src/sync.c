#include "sync.h"
#include "kernel_cfg.h"
#include "port.h"
#include "task.h"
#include "sched.h"

void sem_init(sem_t *s ,uint32_t max_count , uint32_t initial_count)
{
    KERNEL_ASSERT(max_count > 0);
    KERNEL_ASSERT(initial_count <= max_count);

    s->count     = initial_count;
    s->max_count = max_count;
    list_init(&s->wait_list);
}


int sem_take(sem_t *s ,uint32_t timeout_ticks)
{
    port_enter_critical();
    /* 第一种情况，直接拿走 */
    if(s->count > 0)
    {
        s->count--;
        port_exit_critical();
        return SYNC_OK;
    }
    /* 第二种情况，没有货，而且不等 */
    if(timeout_ticks == 0)
    {
        port_exit_critical();
        return SYNC_TIMEOUT;
    }
    /* 第三种情况，没货，要等 
    * 将自己挂在等待队列上
    * 让task_block_current 把自己挂进延时链表（带超时的话）
    * 让出CPU;
    */
    list_insert_tail(&s->wait_list, &current_task->wait_node);
    task_block_current(timeout_ticks);
    port_exit_critical();

    sched_yield();
    return (current_task->wait_result == TASK_WAIT_OK) ? SYNC_OK : SYNC_TIMEOUT;

}

void sem_give(sem_t *s)
{
    port_enter_critical();
    /* 1.有人等着，直接转手 */
    if(!list_is_empty(&s->wait_list))
    {
        task_t *t = list_first_entry(&s->wait_list,task_t ,wait_node);
        task_unblock(t);
    }
    /* 2.没人等，直接存起来 */
    else if(s->count < s->max_count)
    {
        s->count++;
    }
    /*  满了，而且没人等，这次give直接丢弃  */
    else
    {

    }
    port_exit_critical();
}




void mutex_init(mutex_t *m)
{
    m->owner = 0;
    list_init(&m->wait_list);
}


int mutex_lock(mutex_t *m , uint32_t timeout_ticks)
{
    port_enter_critical();
    /* 没人持有锁，直接走人 */
    if(m->owner == 0)
    { 
        m->owner = current_task;
        port_exit_critical();
        return SYNC_OK;
    }
    /* 如果已经被占用了，需要判断是否被递归使用了锁，本接口不支持递归加锁 */
    KERNEL_ASSERT(m->owner != current_task);


    if(timeout_ticks == 0)
    {
        port_exit_critical();
        return SYNC_TIMEOUT;
    }

    /* 锁被占用了，等着,如果优先级比较高的话就需要反转优先级了，但是结束了必须反转回来才行 */

    if(m->owner->cur_prio < current_task->cur_prio)
    {
        /* 优先级反转  */
        sched_set_priority(m->owner , current_task->cur_prio);
    }


    list_insert_tail(&m->wait_list, &current_task->wait_node);
    task_block_current(timeout_ticks);
    port_exit_critical();

    sched_yield();
    
    if(current_task->wait_result == TASK_WAIT_OK)
    {
        return SYNC_OK;
    }

    return SYNC_TIMEOUT;



}


void mutex_unlock(mutex_t *m)
{
    KERNEL_ASSERT(m->owner == current_task);
    port_enter_critical();
    m->owner= 0;
    if(!list_is_empty(&m->wait_list))
    {
        task_t *t = list_first_entry(&m->wait_list, task_t , wait_node);
        m->owner = t;
        task_unblock(t);
    }
    uint32_t base = current_task->base_prio;
    port_exit_critical();

    if(current_task->cur_prio != base)
    {
        port_enter_critical();
        {
            sched_set_priority(current_task , base);
        }
    }
    port_exit_critical();


}



