//
// Created by lx on 2026/7/30.
//
#include "../inc/task.h"


static struct list_node task_head;


void sched_init(void)
{
    list_init(&task_head);
}


/*
* struct Task
{
    uint32_t *sp;
    Task_Status status;
    list_node_t node;
};
 */
void init_stack()
{

}
void create_task(Task_t* task)
{
    task->status = RUNNING;
    list_add(&task_head, &(task->node));
    init_stack();
}

