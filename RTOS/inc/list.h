//
// Created by lx on 2026/7/30.
//

#ifndef RTOS_LIST_H
#define RTOS_LIST_H
#include "stdint.h"
#include <stddef.h>
//思路：把地址 0 强制转成 type*，取 member 成员地址，这个地址就是偏移。
/*
 * 不是说 “0 是结构体变量”，而是：**指针变量里面存的地址编号是 0**，假想这个指针指向地址 0 处有一个 type 结构体。
 * 等价与 type * p = 0;p指向0地址
 */
#ifndef offsetof
#define offsetof(type, member) ((size_t )&((type*)0)->member)   //算出的是偏移字节数
#endif

//这里必须这么char *强制类型转化，因为C 语言指针加减，是按「指针指向类型的大小」来偏移的。
#define container_of(ptr,type,member) ({             \
    const typeof(((type*)0)->member) *_mptr = (ptr); \
    (type*) ((char *)_mptr -offsetof(type,member) );       })

typedef struct list_node list_node_t;
typedef struct list list_t;

struct list_node
{
    list_node_t* next;
    list_node_t* prev;
    list_t *container;
};
struct list
{
    list_node_t head;
    uint32_t count;
};


/* 链表操作
 * 初始化链表
 * 头插法
 * 尾插法
 * 删除节点，从链表上移除，（作用与节点没用的场景）
 * 从链表上取出节点（该节点还有用）
 * 判断链表为空
 * 链表节点数量
 *
 */
void list_init(list_t *l);
void list_insert_head(list_t *l,list_node_t *n);
void list_insert_tail(list_t *l,list_node_t *n);
void list_remove(list_node_t *n);
list_node_t *list_pop_head(list_t *l);
int list_is_empty(const list_t *l);
uint32_t list_count(const list_t *l);


/* stage2新增 */

void list_insert_before(list_node_t *pos, list_node_t *node);

#define list_is_linked(n) ((n)->container != NULL)

/* 取队首元素的宿主对象(空链表不要调)。*/
#define list_first_entry(l, type, member) \
    container_of((l)->head.next, type, member)


#define list_for_each(pos, l) \
     for ((pos) = (l)->head.next; (pos) != &(l)->head; (pos) = (pos)->next)

#endif //RTOS_LIST_H
