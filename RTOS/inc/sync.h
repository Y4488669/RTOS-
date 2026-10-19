#ifndef __RTOS_SYNC_H
#define __RTOS_SYNC_H


#include"list.h"
#include<stdint.h>
#include"task.h"


/* 同步接口的返回码 */
#define SYNC_OK              0
#define SYNC_TIMEOUT         (-1)


typedef struct 
{
    uint32_t count;            /* 现在还有多少货  */
    uint32_t max_count ;       /* 上限，写1就是二值信号量 */
    list_t wait_list ;
}sem_t;

void sem_init(sem_t *s, uint32_t max_count, uint32_t initial_count);



int sem_take(sem_t *s ,uint32_t timeout_ticks);

void sem_give(sem_t *s);



typedef struct 
{
    task_t *owner;
    list_t wait_list;
}mutex_t;

void mutex_init(mutex_t *m);


int mutex_lock(mutex_t *m, uint32_t timeout_ticks);

void mutex_unlock(mutex_t *m);



#endif //RTOS_SYNC_H

