/**
 * debug.h —— 出问题了,怎么知道出了什么问题
 * ---------------------------------------------------------------------------
 * 这一版给内核加了三样"保命"的东西:
 *
 *   1) 断言:条件不成立时**记录现场再停下**(不是简单死循环,而是把
 *      "哪个文件哪一行"存进全局变量,等你 Attach 上去看);
 *   2) 栈溢出检测:创建任务时把整块栈灌成固定模式,切换时看栈底那几个字
 *      有没有被改过 —— 改过了就是溢出了;
 *   3) HardFault 现场解析:程序跑飞之后,把**出错的那条指令地址**挖出来。
 *
 * 这三样加起来,把"单片机死机了,只能靠猜"变成"死在哪一行,一看就知道"。
 * 这份代码里最"工程"的部分就是这里,但也是最省时间的一部分。
 */
#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>

/* 前向声明,避免 debug.h ←→ task.h 循环包含:
 * kernel_cfg.h 包含 debug.h,task.h 又包含 kernel_cfg.h,
 * 若这里再包含 task.h,轮到 task.h 时它的 include guard 已置位,
 * task_t 还没定义就被使用了。*/
typedef struct task task_t;

/* ===========================================================================
 * 一、断言
 * =========================================================================*/

/* 被 KERNEL_ASSERT 宏调用(见 kernel_cfg.h)。记录位置后停住,不返回。*/
void debug_assert_failed(const char *file, uint32_t line);

/* ===========================================================================
 * 二、栈溢出检测
 * =========================================================================*/

/* 把任务的整块栈灌成 CFG_STACK_FILL_PATTERN。创建任务时调用。*/
void debug_stack_fill(task_t *t);

/* 检查栈底那 CFG_STACK_CHECK_WORDS 个字还是不是填充模式。
 * 被 sched_switch() 在每次换人时调用 —— 检查越频繁,越接近"第一现场"。*/
void debug_stack_check(task_t *t);

/* ===========================================================================
 * 三、HardFault 现场
 * =========================================================================*/

/* 被 port.c 的 HardFault_Handler 调用(它是个 naked 汇编函数,
 * 因为普通的 C 函数进门就会改 SP、现场就找不回来了)。不返回。*/
void debug_hardfault_report(uint32_t *frame, uint32_t exc_return);

/* 现场是从哪来的 */
#define FAULT_FROM_TASK  0      /* 任务里出的事(出错时用 PSP)*/
#define FAULT_FROM_ISR   1      /* 中断里出的事(出错时用 MSP)*/

/* 现场快照。调试器 Attach 上来,直接看这个结构体就够了。*/
typedef struct
{
    /* —— 断言 —— */
    const char *assert_file;    /* 哪个文件 */
    uint32_t    assert_line;    /* 哪一行 */

    /* —— HardFault —— */
    uint32_t    fault_pc;       /* ★ 出错的那条指令地址:最想知道的就是它 */
    uint32_t    fault_lr;       /* 谁调用它的(返回地址)*/
    uint32_t    fault_psr;      /* 出错时的 xPSR(看 IPSR 就知道在不在中断里)*/
    uint32_t    fault_sp;       /* 出错时的栈指针 */
    uint32_t    fault_from;     /* FAULT_FROM_TASK / FAULT_FROM_ISR */
    uint32_t    fault_cfsr;     /* 可配置错误状态:真正的原因大多在这里 */
    uint32_t    fault_hfsr;     /* 硬错误状态:HFSR.FORCED=1 说明是"被升级"上来的 */
} debug_info_t;

extern volatile debug_info_t debug_info;

#endif /* DEBUG_H */
