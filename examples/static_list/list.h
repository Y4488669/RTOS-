/*
 * static_list —— 无 malloc 的嵌入式静态双向链表模板
 * ---------------------------------------------------------------
 * 设计要点:
 *   1. 节点(ListItem_t)不是独立分配的,而是"内嵌"在任务/对象结构体里。
 *      内存来自静态声明(全局数组/局部 static),不来自堆。
 *   2. 链表带哨兵节点(sentinel),构成环形双向链表,所有操作都是 O(1)
 *      的插入/删除,遍历也简单。
 *   3. 有序插入:按 value 升序,配合哨兵节点的最大 value 作为终止标记。
 *
 * 这套接口直接对标 FreeRTOS 的 list.h(list 库),写完后可平移到
 * 你的 RTOS 工程,替换掉现在 kernel.c 里的 task_table[2]。
 */

#ifndef STATIC_LIST_H
#define STATIC_LIST_H

#include <stdint.h>
#include <stddef.h>

/* 链表节点 —— 会被内嵌进任意对象(如任务结构体) */
typedef struct ListItem
{
    int32_t            value;        /* 排序键值:优先级 / 唤醒 tick / 计数等 */
    struct ListItem   *pxNext;       /* 后继节点 */
    struct ListItem   *pxPrevious;   /* 前驱节点 */
    void              *owner;        /* 指向所属对象(如 TCB)。从节点找回对象 */
    struct List       *pxContainer;  /* 指向所在链表(NULL = 不在链表中) */
} ListItem_t;

/* 链表 —— 环形结构,哨兵节点永远存在 */
typedef struct List
{
    uint32_t           count;        /* 链表中节点的个数 */
    ListItem_t         sentinel;     /* 哨兵:标记"链表末尾",value 恒为最大值 */
} List_t;

/* 初始化一个空链表 */
void List_Init(List_t *list);

/* 按 value 升序插入(数值小优先级高排前面),返回插入后的位置 */
ListItem_t *List_InsertOrdered(List_t *list, ListItem_t *item);

/* 追加到链表末尾(等价于插入到哨兵之前) */
void List_Append(List_t *list, ListItem_t *item);

/* 把节点从它所在的链表中摘下(节点不必是自己 main 里插入的) */
void List_Remove(ListItem_t *item);

/* 查看链表头节点(不取出),空链表返回 NULL */
ListItem_t *List_PeekHead(List_t *list);

/* 取出链表头节点(取出后从链表移除),空链表返回 NULL */
ListItem_t *List_PopHead(List_t *list);

/* 链表当前节点个数 / 是否为空 */
uint32_t List_Count(const List_t *list);
int      List_IsEmpty(const List_t *list);

/* 遍历宏:pos 依次指向每个真实节点(不含哨兵) */
#define List_FOR_EACH(list, pos)                                        \
    for ((pos) = (list)->sentinel.pxNext;                               \
         (pos) != &(list)->sentinel;                                    \
         (pos) = (pos)->pxNext)

#endif /* STATIC_LIST_H */
