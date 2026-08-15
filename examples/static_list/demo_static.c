/*
 * demo_static.c —— 嵌入式风格:静态就绪队列(不用 malloc)
 * ---------------------------------------------------------------
 * 演示:
 *   1. 任务对象全部静态声明(全局数组),内存来自编译期;
 *   2. 链表节点内嵌在任务结构体里,由链表串起来;
 *   3. 按优先级有序入队 -> 模拟调度器每次取最高优先级任务;
 *   4. 中途演示"任务阻塞":把一个任务从就绪队列摘走,再放回来。
 *
 * 在 PC 上直接编译运行:
 *   gcc -o demo_static demo_static.c list.c && ./demo_static
 */

#include <stdio.h>
#include <string.h>
#include "list.h"

/* ---- 任务对象:节点内嵌在结构体里 ---- */
typedef struct
{
    ListItem_t  xItem;      /* 就绪队列节点(内存来自下面的静态数组,不是 malloc) */
    char        name[16];
    uint8_t     priority;
    /* 后面加 RTOS 字段时直接往这里塞:sp、status、stack[] 等 */
} Task_t;

/* ---- 静态任务池:4 个任务,编译期就占好内存,永不释放 ---- */
static Task_t task_pool[4];

static void task_setup(int index, const char *name, uint8_t priority)
{
    strcpy(task_pool[index].name, name);
    task_pool[index].priority  = priority;
    task_pool[index].xItem.value = priority;  /* 排序键 = 优先级 */
    task_pool[index].xItem.owner  = &task_pool[index]; /* 从节点找回任务 */
}

int main(void)
{
    List_t ready_queue;
    List_Init(&ready_queue);

    /* "创建"任务 —— 这一步就相当于你以前调 malloc(4 次),
     * 但这里只是给静态数组里的元素填值,内存不用释放。 */
    task_setup(0, "idle",    1);
    task_setup(1, "display", 2);
    task_setup(2, "uart",    3);
    task_setup(3, "key",     4);

    /* 就绪队列初始化好,把任务节点依次插进去 */
    for (int i = 0; i < 4; i++)
        List_InsertOrdered(&ready_queue, &task_pool[i].xItem);

    printf("=== 静态就绪队列(共 %u 个任务)===\n", List_Count(&ready_queue));

    /* 调度器视角:每次从队头取"优先级最高"的任务运行 */
    printf("\n[1] 模拟调度,按优先级依次运行:\n");
    while (!List_IsEmpty(&ready_queue))
    {
        ListItem_t *item = List_PopHead(&ready_queue);
        Task_t     *t    = (Task_t *)item->owner;
        printf("    运行 %-8s prio=%u\n", t->name, t->priority);
    }

    /* 重新入队,演示"任务阻塞/恢复"(从就绪队列摘走、再放回来) */
    for (int i = 0; i < 4; i++)
        List_InsertOrdered(&ready_queue, &task_pool[i].xItem);

    printf("\n[2] uart 任务阻塞(被从就绪队列摘除):\n");
    Task_t *uart = NULL;
    for (int i = 0; i < 4; i++)
        if (strcmp(task_pool[i].name, "uart") == 0)
            uart = &task_pool[i];
    List_Remove(&uart->xItem);          /* 阻塞 = 摘出就绪队列 */
    printf("    当前就绪队列还剩 %u 个任务\n", List_Count(&ready_queue));

    printf("[3] uart 等到事件,重新就绪:\n");
    List_InsertOrdered(&ready_queue, &uart->xItem);
    printf("    就绪队列恢复为 %u 个任务\n", List_Count(&ready_queue));

    printf("\n[4] 最终调度顺序:\n");
    while (!List_IsEmpty(&ready_queue))
    {
        ListItem_t *item = List_PopHead(&ready_queue);
        Task_t     *t    = (Task_t *)item->owner;
        printf("    运行 %-8s prio=%u\n", t->name, t->priority);
    }
    return 0;
}
