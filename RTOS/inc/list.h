//
// Created by lx on 2026/7/30.
//

#ifndef RTOS_LIST_H
#define RTOS_LIST_H

#include "stdint.h"

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
