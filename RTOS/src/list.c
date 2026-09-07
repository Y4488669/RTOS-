//
// Created by lx on 2026/8/7.
//
#include "../inc/list.h"
#include "stddef.h"

void list_init(list_node_t* head)
{
    head->prev = head;
    head->next = head;
}

void list_add(list_node_t* list, list_node_t* new_node)
{
    if(list->next == list)
    {
        list->prev = new_node;
    }
    new_node->next = list->next;
    new_node->prev = list;
    list->next->prev = new_node;
    list->next = new_node;
}
void list_del(list_node_t* old_node)
{
    list_node_t* tmp = old_node->prev;
    tmp->next = old_node->next;
    old_node->next->prev = tmp;
}
