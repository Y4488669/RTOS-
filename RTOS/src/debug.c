/**
 * debug.c —— 断言、栈溢出检测、HardFault 现场解析
 * ---------------------------------------------------------------------------
 * 这个文件不参与内核功能,但它决定你"调试一个内核"要花多久。
 * 三件事各讲一段。
 */
#include "debug.h"
#include "kernel_cfg.h"
#include "port.h"

/* 现场快照。volatile:它被异常处理程序写、被调试器读,
 * 而"调试器读"这件事编译器完全不知道 —— 不写 volatile 它会把整个结构体
 * 优化掉(毕竟没有任何"普通代码"读它)。这个坑很常见:优化一开,变量就没了。*/
volatile debug_info_t debug_info = { 0 };

/* ===========================================================================
 * 一、断言
 * ===========================================================================
 * 和"死循环"相比,多做的事情只有一件:把位置存进全局变量。
 * 但就是这一件,让调试从"看调用栈猜"变成"直接读 debug_info.assert_line"。
 *
 * 注意 assert 只该用来抓**程序员写错了**:参数越界、状态机自相矛盾、
 * 链表的 container 是空……这些"本来就不该发生"的事。
 * 不要用它处理运行期错误(比如"队列满了"),那是返回值该干的事。
 */
void debug_assert_failed(const char *file, uint32_t line)
{
    debug_info.assert_file = file;
    debug_info.assert_line = line;

    /* 停下。在调试器里在这里下断点,或者直接 Attach 看上面的变量。
     * 生产代码里这里通常改成"记录 + 复位",而不是死等。*/
    for (;;)
    {
    }
}

/* ===========================================================================
 * 二、栈溢出检测
 * ===========================================================================
 * 原理非常土,但极其有效:
 *
 *   创建任务时:整块栈灌成 0xA5A5A5A5
 *   每次换人时:看栈底(低地址那头)那几个字还在不在
 *
 *   栈是往下长的,所以"踩到底"一定先从**低地址**那一头开始 ——
 *   如果那里还有填充值,说明这个任务最深也没到过那里。
 *
 * 为什么在"换人的时候"检查:那时候这个任务刚刚跑过一轮,
 * 如果真溢出了,现场最接近;而且它已经不占 CPU 了,检查它很安全。
 *
 * 局限:
 *   - 只能告诉你"溢出了",不能告诉你"哪一行溢出的"(要那个得上 MPU);
 *   - 如果任务踩过界之后又退回来了,你只能看到"填充值被改了"这个既成事实;
 *   - 检查的粒度是"那几个字",所以轻微的越界可能刚好落在检查范围之外。
 * 但它足够便宜,便宜到可以每次都做 —— 这就是它值得存在的理由。
 */
void debug_stack_fill(task_t *t)
{
    if (!CFG_STACK_CHECK)
    {
        return;
    }

    for (uint32_t i = 0; i < CFG_STACK_WORDS; i++)
    {
        t->stack[i] = CFG_STACK_FILL_PATTERN;
    }
}

void debug_stack_check(task_t *t)
{
    if (!CFG_STACK_CHECK)
    {
        return;
    }

    for (uint32_t i = 0; i < CFG_STACK_CHECK_WORDS; i++)
    {
        if (t->stack[i] != CFG_STACK_FILL_PATTERN)
        {
            /* 栈底被改过 = 栈曾经长到这里 = 溢出。
             * 注意这里停下时,看 debug_info 是看不到"哪个任务"的 ——
             * 想更精确,可以在这里把 t->name 也存进 debug_info。*/
            KERNEL_ASSERT(0);
            return;
        }
    }
}

/* ===========================================================================
 * 三、HardFault 现场解析
 * ===========================================================================
 * 进来的时候,port.c 的裸汇编已经把两件事做完了:
 *   frame      —— 指向硬件压好的那 8 个字(R0,R1,R2,R3,R12,LR,PC,xPSR)
 *   exc_return —— 出错时用的是 MSP 还是 PSP(由此知道是在中断里还是任务里)
 *
 * 第一件要做对的事:**读 CFSR,而不是盯着 PC 发呆**。
 *
 *   HFSR.FORCED = 1 表示"HardFault 是被升级上来的" —— 真正的错误原因
 *   记在 CFSR 里。下面这张表是查表用的,建议抄进你的备忘录:
 *
 *   ┌─ CFSR 的 MMFSR(低 8 位):内存管理错误 ─────────────────────┐
 *   │ bit0 IACCVIOL   取指越权(跳到了不该执行的地方,XN 区域)      │
 *   │ bit1 DACCVIOL   数据访问越权                                │
 *   │ bit7 MMARVALID  1 表示 MMFAR 里的地址有效 —— 就是出事的地址 │
 *   ├─ CFSR 的 BFSR(第 8~15 位):总线错误 ───────────────────────┐
 *   │ bit0 IBUSERR    取指总线错误                                │
 *   │ bit1 PRECISERR  精确数据总线错误 ← **最常见**,BFAR 里是地址 │
 *   │ bit2 IMPRECISERR 不精确(写缓冲延迟报出来的,PC 已经不准了) │
 *   │ bit3 UNSTKERR   异常返回时出栈出错                          │
 *   │ bit4 STKERR     异常入栈出错 ← **栈溢出的典型症状**         │
 *   │ bit7 BFARVALID  1 表示 BFAR 里的地址有效                    │
 *   ├─ CFSR 的 UFSR(第 16~31 位):用法错误 ─────────────────────┐
 *   │ bit0 UNDEFINSTR 未定义指令                                  │
 *   │ bit1 INVSTATE   非法状态 ← 跳到**偶数地址**最常报这个      │
 *   │                 (Cortex-M 只有 Thumb,函数指针最低位必须是 1)│
 *   │ bit2 INVPC      非法入栈 PC                                 │
 *   │ bit3 NOCP       用了没使能的协处理器(比如 FPU 没开)        │
 *   │ bit8 UNALIGNED  非对齐访问(LDRD/STRD 这类)                 │
 *   │ bit9 DIVBYZERO  除零                                        │
 *   ├─ HFSR ────────────────────────────────────────────────────┐
 *   │ bit1 VECTTBL    取向量表出错(向量表地址没设对)             │
 *   │ bit30 FORCED    1 = 被升级上来的,真正原因看 CFSR           │
 *   │ bit31 DEBUGEVT  调试事件                                    │
 *   └────────────────────────────────────────────────────────────┘
 *
 * 第二件要做对的事:**把 PC 翻译成人话**。拿到 fault_pc 之后:
 *
 *     arm-none-eabi-addr2line -f -e build/Debug/RTOS.elf 0x08000abc
 *
 * 它会告诉你这个地址落在哪个函数、哪一行。(注意:开了优化之后地址可能
 * 对不齐,所以调试时用 -O0 最省事 —— 这也是本工程 Debug 预设用 -O0 的原因。)
 */
void debug_hardfault_report(uint32_t *frame, uint32_t exc_return)
{
    /* 硬件帧的固定顺序,index 就是"第几个被压进去的":
     *   [0]=R0 [1]=R1 [2]=R2 [3]=R3 [4]=R12 [5]=LR [6]=PC [7]=xPSR */
    debug_info.fault_pc  = frame[6];
    debug_info.fault_lr  = frame[5];
    debug_info.fault_psr = frame[7];

    /* 出错那一刻的栈指针:帧是压在当时的 SP **下面**的,所以 SP = frame + 32 */
    debug_info.fault_sp  = (uint32_t)frame + 32u;

    /* EXC_RETURN 的 bit2:1 = 当时用 PSP(在任务里),0 = 用 MSP(在中断里)*/
    debug_info.fault_from = (exc_return & 0x4u) ? FAULT_FROM_TASK : FAULT_FROM_ISR;

    /* 真正的原因大多在这里 —— 具体每一位见上面那张表 */
    debug_info.fault_cfsr = REG_CFSR;
    debug_info.fault_hfsr = REG_HFSR;

    /* 停下,保留现场。
     *
     * ❌ 不要在这里试图"修好再跑回去":HardFault 说明 CPU 遇到了它处理不了的
     *    错误,此时系统的状态是**不可信**的 —— 可能已经有别的数据结构被改坏了。
     *    正确的做法是保留现场 → 复位 → 事后分析。
     *    (真要"续命",那属于看门狗 + 复位的那一套,不是在这里恢复。) */
    for (;;)
    {
    }
}
