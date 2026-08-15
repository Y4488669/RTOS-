/*
 * demo_malloc.c —— 对照组:你熟悉的 malloc 写法
 * ---------------------------------------------------------------
 * 同一个"按优先级排序的就绪队列",这次用 malloc 动态建链表。
 * 放一起对比,结论就一句话:
 *
 *     malloc 版: 先"造节点"再"填数据",最后还要记得 free;
 *     静态版:    对象本来就在,链表只负责"把对象串起来",不用 free。
 *
 * 你已有的算法思维(malloc 版)完全可以平移到静态版,变的只是
 * "节点内存从哪来"这一件事。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- 链表节点:这次节点 == 分配出来的对象本身 ---- */
typedef struct MTask
{
    struct MTask *pxNext;      /* 单链表后继 */
    struct MTask *pxPrevious;  /* 前驱 */
    char          name[16];
    uint8_t       priority;
} MTask_t;

static MTask_t *head = NULL;   /* 链头 */

/* 按优先级升序插入(头插法 + 排序) */
static void insert_sorted(MTask_t *task)
{
    MTask_t *cur = head;
    MTask_t *prev = NULL;

    while (cur != NULL && cur->priority < task->priority)
    {
        prev = cur;
        cur  = cur->pxNext;
    }

    task->pxNext = cur;
    task->pxPrevious = prev;
    if (prev != NULL)
        prev->pxNext = task;
    else
        head = task;
    if (cur != NULL)
        cur->pxPrevious = task;
}

/* 取头(最高优先级) */
static MTask_t *pop_head(void)
{
    MTask_t *t = head;
    if (head != NULL)
    {
        head = head->pxNext;
        if (head != NULL)
            head->pxPrevious = NULL;
    }
    return t;
}

/* 从链表里摘除一个节点 */
static void remove_task(MTask_t *task)
{
    if (task->pxPrevious != NULL)
        task->pxPrevious->pxNext = task->pxNext;
    else
        head = task->pxNext;
    if (task->pxNext != NULL)
        task->pxNext->pxPrevious = task->pxPrevious;
}

int main(void)
{
    /* "创建"任务 = 每次都 malloc 一块内存,用完 free */
    MTask_t *idle    = malloc(sizeof(MTask_t)); strcpy(idle->name,    "idle");    idle->priority = 1;
    MTask_t *display = malloc(sizeof(MTask_t)); strcpy(display->name, "display"); display->priority = 2;
    MTask_t *uart    = malloc(sizeof(MTask_t)); strcpy(uart->name,    "uart");    uart->priority = 3;
    MTask_t *key     = malloc(sizeof(MTask_t)); strcpy(key->name,     "key");     key->priority = 4;

    insert_sorted(idle);
    insert_sorted(display);
    insert_sorted(uart);
    insert_sorted(key);

    printf("=== malloc 版就绪队列 ===\n");

    /* uart 阻塞(摘除),再恢复(重新插入)—— 和静态版同一套演示 */
    remove_task(uart);
    printf("    阻塞 uart 后就绪队列剩余节点按序运行:\n");
    MTask_t *t;
    while ((t = pop_head()) != NULL)
    {
        printf("    运行 %-8s prio=%u\n", t->name, t->priority);
        free(t);                /* 用完了释放 —— 静态版不需要这一步 */
    }

    /* 恢复 uart(它没被 free 过,内存还在,可以安全重新入队)。
     * 但注意:每个节点的"创建/释放/还活没活着"都得靠你手工记账,
     * 稍一疏忽就会 double-free 或悬垂指针 —— 这就是 malloc 的坑。
     * 静态版里节点由数组拥有,永远存在,这类问题从根上不存在。 */
    insert_sorted(uart);
    printf("    恢复 uart 后单独运行它:\n");
    printf("    运行 %-8s prio=%u\n", uart->name, uart->priority);
    free(uart);
    return 0;
}
