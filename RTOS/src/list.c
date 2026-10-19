//
// Created by lx on 2026/8/7.
//
#include "../inc/list.h"
#include "stddef.h"

void list_init(list_t *l)
{
    l->head.next = &l->head;
    l->head.prev = &l->head;
    l->head.container = l;
    l->count = 0;

}
void list_insert_head(list_t *l,list_node_t *n)
{
    n->next = l->head.next;
    n->prev = &l->head;
    l->head.next->prev = n;
    l->head.next = n;
    n->container = l;
    l->count++;
}
void list_insert_tail(list_t *l,list_node_t *n)
{
    n->prev = l->head.prev;
    n->next = &l->head;
    l->head.prev->next = n;
    l->head.prev = n;
    n->container = l;
    l->count++;
}
void list_remove(list_node_t *n)
{
    if (n->container == NULL)
    {
        return;
    }
    n->next->prev = n->prev;
    n->prev->next = n->next;
    n->next = NULL;
    n->prev = NULL;
    n->container->count--;
    n->container = NULL;

}
list_node_t *list_pop_head(list_t *l)
{
    if (l->count == 0)
    {
        return NULL;
    }
    list_node_t *n = l->head.next;
    list_remove(n);
    return n;
}
int list_is_empty(const list_t *l)
{
    return l->count == 0;
}
uint32_t list_count(const list_t *l)
{
    return l->count;
}




void list_insert_before(list_node_t *pos, list_node_t *node)
{
    node->next = pos;
    node->prev = pos->prev;
    node->container = pos->container;

    pos->prev->next = node;
    pos->prev = node;


    node->container->count++;
}