/**
 * mem.c —— 定长块内存池
 * ---------------------------------------------------------------------------
 * 结构图(block_count = 4):
 *
 *      blocks  ┌────────┬────────┬────────┬────────┐
 *              │ 块 0   │ 块 1   │ 块 2   │ 块 3   │   ← 调用方给的大数组
 *              └────────┴────────┴────────┴────────┘
 *      nodes   ┌────────┬────────┬────────┬────────┐
 *              │ node 0 │ node 1 │ node 2 │ node 3 │   ← 与块一一对应的节点
 *              └────────┴────────┴────────┴────────┘
 *
 *      free_list: 哨兵 → node 1 → node 3 → 哨兵   (哪些块还是空闲的)
 *
 * 分配 = 从 free_list 摘一个节点、用它的**下标**算出块地址;
 * 释放 = 用块地址算出下标、把对应的节点挂回 free_list。
 *
 * ★ 下标和块的对应关系就是本文件唯一的"技巧":
 *     块地址 = blocks + idx * block_size
 *     idx    = (块地址 - blocks) / block_size
 *   两个方向都是一次减法 + 一次乘除,常数时间。
 *
 * 这也解释了一个使用约束:mem_free() 必须传**当初 mem_alloc() 返回的那个指针**。
 * 传一个块中间的地址进去,算出来的 idx 是错的 —— 所以下面用断言把它拦住。
 */
#include "mem.h"
#include "port.h"

void mem_pool_init(mem_pool_t *p, void *blocks, list_node_t *nodes,
                   uint32_t block_size, uint32_t block_count)
{
    KERNEL_ASSERT(blocks != 0);
    KERNEL_ASSERT(nodes != 0);
    KERNEL_ASSERT(block_size >= 4);     /* 太小就没意义了 */
    KERNEL_ASSERT(block_count > 0);

    p->blocks      = (uint8_t *)blocks;
    p->nodes       = nodes;
    p->block_size  = block_size;
    p->block_count = block_count;
    p->used        = 0;

    list_init(&p->free_list);

    /* 一上来所有块都是空闲的:逐个挂进空闲链表。
     * 初始化是 O(n),但只做一次,和后面每次都是 O(1) 的分配不冲突。*/
    for (uint32_t i = 0; i < block_count; i++)
    {
        list_insert_tail(&p->free_list, &nodes[i]);
    }
}

void *mem_alloc(mem_pool_t *p)
{
    void *blk = 0;

    /* 进临界区:空闲链表可能被别的任务(甚至中断里的回调)同时动。
     * 定长池之所以"中断安全",不是因为它的算法天然安全,
     * 而是因为它足够**短** —— 关中断几十个时钟周期就能完成。*/
    port_enter_critical();
    {
        list_node_t *n = list_pop_head(&p->free_list);

        if (n != 0)
        {
            uint32_t idx = (uint32_t)(n - p->nodes);    /* 指针相减 = 下标 */
            blk = p->blocks + (idx * p->block_size);
            p->used++;
        }
        /* 没货就返回 NULL —— 定长池的"失败"是正常返回值,不是异常。*/
    }
    port_exit_critical();

    return blk;
}

void mem_free(mem_pool_t *p, void *block)
{
    if (block == 0)
    {
        return;             /* 允许 free(NULL),和标准库的习惯保持一致 */
    }

    uint8_t *b   = (uint8_t *)block;
    uint32_t off = (uint32_t)(b - p->blocks);

    /* 拦住最经典的两种误用:
     *   1) 传进来的不是这个池子的块;
     *   2) 传进来的是块中间的地址(不在块边界上)。*/
    KERNEL_ASSERT(off < (p->block_count * p->block_size));
    KERNEL_ASSERT((off % p->block_size) == 0);

    port_enter_critical();
    {
        uint32_t idx = off / p->block_size;

        /* 注意这里**没有**检查"这块是不是已经空闲了"。
         * 重复 free 会把同一个节点挂进链表两次 → 以后两次 alloc 拿到同一块内存,
         * 两个"主人"互相踩 —— 这是最阴险的一类 bug。
         * 真要防,可以给每块加个"在使用中"标志(代价是每块多一个字节,
         * 而且要维护它)。本教学版选择"靠约定 + 断言边界",不做重复 free 检测。*/
        list_insert_tail(&p->free_list, &p->nodes[idx]);
        p->used--;
    }
    port_exit_critical();
}

void mem_pool_reset(mem_pool_t *p)
{
    port_enter_critical();
    {
        list_init(&p->free_list);
        for (uint32_t i = 0; i < p->block_count; i++)
        {
            list_insert_tail(&p->free_list, &p->nodes[i]);
        }
        p->used = 0;
    }
    port_exit_critical();
}

uint32_t mem_block_size(const mem_pool_t *p)
{
    return p->block_size;
}

uint32_t mem_free_count(const mem_pool_t *p)
{
    return p->block_count - p->used;
}
