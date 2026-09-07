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
#define container_of(type,ptr,member) ({             \
    const typeof(((type*)0)->member) *_mptr = (ptr); \
    (type*) ((char *)_mptr -offsetof(type,member) );       })


struct list_node
{
    struct list_node* next;
    struct list_node* prev;
};
typedef struct list_node list_node_t;


void list_init(list_node_t* head);

void list_add(list_node_t* head, list_node_t* new_node);
void list_del(list_node_t* old_node);
#endif //RTOS_LIST_H
