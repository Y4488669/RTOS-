// //
// // Created by lx on 2026/8/7.
// //
// #include "../inc/list.h"
// #include "stddef.h"
// static Node node;
//
// /**
// * typedef struct
// {
//     uint8_t priority ;
//     struct List *pxNext;
//     struct List *pxPrevious;
//     TCB_t pxTCB;
// }List, *List_t;
//  */
// void List_Init()
// {
//     node.count = 0;
//     node.list = NULL;
// }
// void List_add(List_t p_List)
// {
//     List_t plist;
//     plist = node.list;
//     node.count++;
//     node.list = p_List;
//     p_List->pxNext = plist;
// }
// void List_delete(List_t List)
// {
//
// }