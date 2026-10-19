/**
 * mem.h —— 定长块内存池
 * ---------------------------------------------------------------------------
 * 先说清楚**为什么不用 malloc**:
 *
 *   ┌────────────┬──────────────────────────┬──────────────────────────┐
 *   │            │ malloc                   │ 定长块池(本文件)         │
 *   ├────────────┼──────────────────────────┼──────────────────────────┤
 *   │ 分配耗时    │ 不定(要搜空闲块、可能拆分)│ O(1)(摘个链表头)         │
 *   │ 释放耗时    │ 不定(可能还要合并相邻块)  │ O(1)                     │
 *   │ 碎片        │ 会(长期分配释放必然碎)    │ 不会(块等大,摘下来还能用)│
 *   │ 内存利用率  │ 高                       │ 差一点(块大小固定,有浪费)│
 *   │ 中断安全    │ 一般不能               │ 能(加个临界区就行)        │
 *   └────────────┴──────────────────────────┴──────────────────────────┘
 *
 * 实时系统最在意的是前两行:**最坏情况必须是可预测的**。
 * "这次分配花了 3 微秒,下次可能 300 微秒"这种事,在硬实时里是不能接受的。
 *
 * 定长池的代价也很明确:你得**预先知道最大同时需要多少个、每个多大**。
 * 内核里这两件事通常都知道(任务数、队列深度、报文长度),所以它很合适。
 *
 * (FreeRTOS 的对应物:heap_1 只分配不释放;heap_4 支持可变大小 + 合并空闲块;
 *  本文件是第三种思路 —— 定长块池。)
 */
#ifndef MEM_H
#define MEM_H

#include <stdint.h>
#include "kernel_cfg.h"
#include "list.h"

typedef struct
{
    uint8_t     *blocks;        /* 整块内存:block_count × block_size 字节 */
    list_node_t *nodes;         /* 与块**一一对应**的节点数组(见下方说明)*/
    uint32_t     block_size;    /* 每块多少字节 */
    uint32_t     block_count;   /* 一共几块 */
    uint32_t     used;          /* 已分配几块 */
    list_t       free_list;     /* 空闲块链表 */
} mem_pool_t;

/**
 * 初始化。两个存储都由调用方提供(内核不做动态分配):
 *
 *   blocks  —— block_count × block_size 字节的大数组
 *   nodes   —— 长度 block_count 的 list_node 数组
 *
 * ★ 为什么节点要单独一个数组,而不是塞在块里?
 *
 *   塞在块里更省内存(那块内存反正闲着),做法是"把空闲块的头 8 个字节
 *   当成 next/prev 用" —— 这是很多内核的做法,叫 intrusive free list。
 *   代价是 block_size 必须 >= 8,而且代码里到处是强制类型转换,读起来费劲。
 *
 *   本版用"节点单独一个数组"的写法,代价是多占 block_count × 8 字节,
 *   换来的是**节点的下标 == 块的下标**,查块、查节点都是一次减法。
 *   教学版选可读的那个。(真实项目里,这 8 字节常常是值得省的。)
 */
void mem_pool_init(mem_pool_t *p, void *blocks, list_node_t *nodes,
                   uint32_t block_size, uint32_t block_count);

/* 拿一块。池子空了返回 NULL(注意:返回空是**正常情况**,不是错误 ——
 * 所以这里不用断言,由调用方决定怎么办)。*/
void *mem_alloc(mem_pool_t *p);

/* 还一块。传 NULL 是安全的空操作。*/
void mem_free(mem_pool_t *p, void *block);

/* 一次性把池子清空重来(复位、自检时用)。*/
void mem_pool_reset(mem_pool_t *p);

uint32_t mem_block_size(const mem_pool_t *p);
uint32_t mem_free_count(const mem_pool_t *p);

#endif /* MEM_H */
