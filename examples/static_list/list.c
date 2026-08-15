/*
 * static_list —— 静态双向链表实现
 *
 * 环形结构:
 *     sentinel <-> node1 <-> node2 <-> ... <-> sentinel
 * 哨兵节点永远是链表的一部分,只是"逻辑上的末尾"。
 */

#include "list.h"
#include <limits.h>   /* INT32_MAX */

/* 在 pos 节点之前插入一个节点 */
static void list_insert_before(ListItem_t *pos, ListItem_t *item)
{
    item->pxNext          = pos;
    item->pxPrevious      = pos->pxPrevious;
    pos->pxPrevious->pxNext = item;
    pos->pxPrevious       = item;
    item->pxContainer     = NULL;  /* 调用方随后设置 */
}

void List_Init(List_t *list)
{
    list->count              = 0;
    list->sentinel.value     = INT32_MAX;  /* 有序插入时,遍历到它一定停下 */
    list->sentinel.owner     = NULL;
    list->sentinel.pxContainer = NULL;
    list->sentinel.pxNext    = &list->sentinel;
    list->sentinel.pxPrevious= &list->sentinel;
}

ListItem_t *List_InsertOrdered(List_t *list, ListItem_t *item)
{
    ListItem_t *pos;

    /* 从头往后扫,找到第一个 value >= item->value 的节点,插在它前面 */
    for (pos = list->sentinel.pxNext; pos != &list->sentinel; pos = pos->pxNext)
    {
        if (item->value < pos->value)
            break;
    }
    list_insert_before(pos, item);
    item->pxContainer = list;
    list->count++;
    return item;
}

void List_Append(List_t *list, ListItem_t *item)
{
    list_insert_before(&list->sentinel, item);  /* 插到哨兵前 = 末尾 */
    item->pxContainer = list;
    list->count++;
}

void List_Remove(ListItem_t *item)
{
    List_t *list = item->pxContainer;   /* 从节点记录里找回它所在的链表 */

    /* 越过当前节点,把前后两个节点直接接起来 */
    item->pxPrevious->pxNext = item->pxNext;
    item->pxNext->pxPrevious = item->pxPrevious;

    /* 清空节点指针,避免悬垂 */
    item->pxNext     = NULL;
    item->pxPrevious = NULL;
    item->pxContainer = NULL;

    if (list != NULL)
        list->count--;
}

ListItem_t *List_PeekHead(List_t *list)
{
    /* 哨兵的下一个若还是哨兵,说明链表为空 */
    if (list->sentinel.pxNext == &list->sentinel)
        return NULL;
    return list->sentinel.pxNext;
}

ListItem_t *List_PopHead(List_t *list)
{
    ListItem_t *head = List_PeekHead(list);
    if (head != NULL)
        List_Remove(head);
    return head;
}

uint32_t List_Count(const List_t *list)
{
    return list->count;
}

int List_IsEmpty(const List_t *list)
{
    return (list->count == 0);
}
