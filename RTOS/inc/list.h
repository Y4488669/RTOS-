//
// Created by lx on 2026/7/30.
//

#ifndef RTOS_LIST_H
#define RTOS_LIST_H

#include "stdint.h"
#include "stm32f4xx_it.h"


typedef struct
{
    uint32_t *sp ;
}TCB ,*TCB_t ;





typedef struct
{
    uint8_t priority ;
    struct List *pxNext;
    struct List *pxPrevious;
    TCB_t pxTCB;
}List, *List_t;

typedef  struct
{
    uint8_t count;
    List_t list;
}Node,*p_Node;


void List_Init();
void List_add(List_t List);
void List_delete(List_t List);
#endif //RTOS_LIST_H
