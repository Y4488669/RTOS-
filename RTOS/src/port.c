//
// Created by lx on 2026/9/21.
//


#include "../inc/port.h"
#include "./inc/kernel_cfg.h"
#include "tick.h"
/* ===========================================================================
 * 四、SVC_Handler:第一次进入任务
 * ===========================================================================
 * 触发方式:sched_start() 里执行 `svc 0`。
 *
 * 为什么用 SVC 而不是别的?因为要"进一次异常"才能走异常返回路径。SVC 是
 * 唯一由软件主动触发的异常,正好干这个。
 *
 * ★ 这里有一处**必须**的设计,也是新手最常漏的一行:
 *
 *   执行 `svc 0` 时,CPU 处于**线程模式 + MSP**(main 还在跑,CONTROL.SPSEL=0),
 *   硬件据此把 LR 设成 0xFFFFFFF9 —— 意思是"返回线程模式,继续用 MSP"。
 *   如果我们老老实实 `bx lr` 回去,硬件会从 **MSP** 上弹那 8 个字,于是
 *   CPU 又回到 main 的 while(1) 里接着空转,任务根本没开始跑。
 *
 *   所以要把 LR 改写成 0xFFFFFFFD:
 *       `orr r14, #0x0d`  →  0xFFFFFFF9 | 0xD = 0xFFFFFFFD
 *   (用 OR 而不是 MOV,是因为 0xFFFFFFFD 这种立即数没法一条指令装进去;
 *    而 OR 上 0xD 刚好把 bit2 置 1 —— bit2 = "用 PSP",bit3 本来就是 1 ——
 *    "返回线程模式",两步并一步。)
 *
 *   改完之后,硬件就按 PSP 去弹那 8 个字,任务栈里伪造的现场被一次性生效:
 *   PC 变成任务入口,r4~r11 变成我们摆的值,任务开始在自己的栈上跑。
 *
 *   你 RTOS/src/task.c 里那版 SVC_Handler 缺的正是这一行(`bx r14` 直接用
 *   了硬件的 LR),后果不只是"第一个任务晚 1 秒才跑":那次 PendSV 还会把
 *   0xFFFFFFF9 存进被切走任务的 EXC_RETURN 槽,等它再被恢复时就会跑回 MSP 上。
 *   一条指令的事,机制不清就会一直以为是"调度器有 bug"。
 */







static uint32_t critical_nest = 0;

void port_enter_critical(void)
{
    if (critical_nest == 0)
    {
        /* 只在最外层真去动 BASEPRI,里面几层只是记账 */
        __asm volatile ("msr basepri, %0" : : "r" ((uint32_t)CFG_BASEPRI_MASK) : "memory");
    }
    critical_nest++;
}

void port_exit_critical(void)
{
    critical_nest--;

    if (critical_nest == 0)
    {
        __asm volatile ("msr basepri, %0" : : "r" (0u) : "memory");
    }
}






__attribute__((naked)) void SVC_Handler(void)
{
    __asm volatile (
        "ldr  r3, =current_task        \n"
        "ldr  r1, [r3]                 \n"   /* r1 = current_task(指向 TCB)*/
        "ldr  r0, [r1]                 \n"   /* r0 = current_task->sp(栈顶锚点)*/

        "ldmia r0!, {r4-r11, r14}      \n"   /* 弹出软件帧:r4~r11 + r14 */

        "msr  psp, r0                  \n"   /* PSP 指向硬件帧起点(R0 那一格)*/
        "isb                           \n"   /* 让 PSP 在异常返回之前生效 */


        "orr  r14, #0x0d               \n"   /* ★ 强制 0xFFFFFFFD:线程模式 + PSP */
        "bx   r14                      \n"   /* 异常返回,硬件弹掉那 8 个字 */

        ".ltorg                        \n"   /* naked 函数不自动生成字面量池,
                                              * `ldr r3, =current_task` 需要它 */
    );
    /* 说到就走,不返回。SVC 自己压进 MSP 的那 8 个字就留那了(32 字节,
     * 一次性开销,不用管)。*/
}

/* ===========================================================================
 * 五、PendSV_Handler:任务切换
 * ===========================================================================
 *
 * 一、为什么不在 SysTick 中断里直接切?
 *
 *   1) **让所有中断先跑完**。PendSV 优先级最低,所有 ISR 都处理完了它才执行。
 *      于是"换任务"总是发生在"马上就回任务"的那一刻 —— 不会出现某个外设中断
 *      处理到一半,CPU 却换了任务这种情况。
 *   2) **合并**。挂起位是"或"的关系:一个 tick 内哪怕被置了 5 次,最后也只
 *      切一次。
 *   3) **和临界区天然配合**。临界区屏蔽优先级 >= 0x10 的中断,而 PendSV 是
 *      0xF0,正好在被屏蔽之列 —— 临界区里绝不会被"换人"打断。
 *
 *   所以整个内核的节奏是:SysTick(只置位)→ PendSV(真正干活)。这叫
 *   "延迟上下文切换",FreeRTOS 用的是同一套。
 *
 * 二、切换动作拆成四步(和汇编一一对应):
 *
 *      进去:  mrs r0, psp            拿到当前任务的栈指针
 *             stmdb r0!, {r4-r11,r14}  把软件帧压进**它的**栈
 *             current_task->sp = r0    记下新的栈顶,下次从这儿恢复
 *
 *      选人:  sched_switch()          纯 C 函数:更新 current_task
 *
 *      出来:  ldr r0, [current_task]  拿到**新任务**的栈顶
 *             ldmia r0!, {r4-r11,r14}  弹出它的软件帧
 *             msr psp, r0              硬件帧留给硬件
 *             bx r14                   异常返回
 *
 * 三、两个细节:
 *
 *   a) `bx r14` 里的 r14 是从**新任务的栈帧里**弹出来的 EXC_RETURN,
 *      而不是进 PendSV 时硬件给的那个。为什么能这样?因为每个任务都额外存了
 *      这一格(软件帧 9 个字,比 FreeRTOS 的 8 个字多一个)。好处是"首次启动
 *      复用同一条恢复路径"变得理所当然 —— 伪造帧里那个 0xFFFFFFFD 直接就能用。
 *      FreeRTOS 不存这一格,它把 EXC_RETURN 压在 MSP 上跨越 C 函数调用传递,
 *      也成立,只是首次启动要单独绕一下。
 *      因为这个设计,你 RTOS/ 里那两行 `mov r4, lr` / `mov lr, r4`
 *      (为了跨过 bl 而保存 EXC_RETURN)其实是多余的 —— 恢复时反正会被
 *      `ldmia` 覆盖掉。留着不影响功能,读的时候知道它为什么不需要存在就好。
 *
 *   b) 中间那句 `msr basepri` 是关中断保护"选人"这一段。其实选人只动几个
 *      全局变量和链表,单核上天然不会被"另一个 CPU"打断,而这里唯一能打断
 *      我们的就是中断 —— 如果中断里也会改就绪队列(比如 SysTick 唤醒任务),
 *      那就有必要。关一下是最省心的写法。
 */
__attribute__((naked)) void PendSV_Handler(void)
{
    __asm volatile (
        /* ---- 保存当前任务现场 ---- */
        "mrs   r0, psp                 \n"
        "isb                           \n"   /* 清流水线,保证读到的是最新 PSP */
        "stmdb r0!, {r4-r11, r14}      \n"   /* 软件帧入栈(注意 r14 是 EXC_RETURN,
                                              * 也一起存进这个任务的栈)*/

        "ldr   r1, =current_task       \n"
        "ldr   r2, [r1]                \n"
        "str   r0, [r2]                \n"   /* current_task->sp = 新栈顶 */

        /* ---- 选下一个任务 ---- */




        "mov   r0, #0x10               \n"   /* 关中断(BASEPRI = 0x10)*/
        "msr   basepri, r0             \n"
        "bl    sched_switch            \n"   /* 纯 C:更新 current_task */
        "mov   r0, #0                  \n"
        "msr   basepri, r0             \n"   /* 开中断 */



        /* ---- 恢复新任务现场 ---- */
        "ldr   r1, =current_task       \n"
        "ldr   r2, [r1]                \n"
        "ldr   r0, [r2]                \n"   /* r0 = 新任务->sp */
        "ldmia r0!, {r4-r11, r14}      \n"   /* 弹出它的软件帧,r14 拿到它的 EXC_RETURN */
        "msr   psp, r0                 \n"   /* PSP 指向硬件帧起点 */
        "isb                           \n"
        "bx    r14                     \n"   /* 异常返回:硬件弹掉剩下 8 个字,
                                              * 栈指针自己会回到新任务上次被打断的地方 */

        ".ltorg                        \n"
    );
}



//栈空间初始化
void port_init_stack(task_t *t)
{
    /* 取得栈顶 */

    uint32_t *top = &t->stack[CFG_STACK_WORDS];
    top = (uint32_t *)((uint32_t)top & ~7u);   /* 向下对齐到 8 字节 */
    t->stack_top = top;
    uint32_t *sp = top;


    /* ---- 硬件帧 8 字:从高地址往低地址摆 ---- */
    /* 硬件帧的最低地址是 R0,最高地址是 xPSR;先写 xPSR 就意味着它是最高地址 */
    *(--sp) = 0x01000000u;              /* xPSR。bit24 = T 位,置 1 表示 Thumb 状态。
                                         * Cortex-M 只有 Thumb,这一位必须是 1,
                                         * 忘了它会在异常返回时直接跑飞。*/
    *(--sp) = (uint32_t)t->entry;       /* PC:异常返回后从这里开始执行 */
    *(--sp) = (uint32_t)t->exit;        /* LR:任务函数万一 return 了,就落到这里
                                         * (通常写成"自杀"钩子,见 task.c)*/
    *(--sp) = 0x12121212u;              /* R12 */
    *(--sp) = 0x03030303u;              /* R3  */
    *(--sp) = 0x02020202u;              /* R2  */
    *(--sp) = 0x01010101u;              /* R1  */
    *(--sp) = 0x00000000u;              /* R0。任务的 r0~r3/r12 是临时寄存器,
                                         * 入口函数不会去读,填什么都能跑;这里填成
                                         * 好认的花样,是为了在调试器里一眼看出
                                         * "哪个寄存器还没被恢复"。*/

    /* ---- 软件帧 9 字 ---- */
    *(--sp) = 0xFFFFFFFDu;              /* EXC_RETURN,对应 r14 —— 见上文 (c)*/
    *(--sp) = 0x11111111u;              /* R11 */
    *(--sp) = 0x10101010u;              /* R10 */
    *(--sp) = 0x09090909u;              /* R9  */
    *(--sp) = 0x08080808u;              /* R8  */
    *(--sp) = 0x07070707u;              /* R7  */
    *(--sp) = 0x06060606u;              /* R6  */
    *(--sp) = 0x05050505u;              /* R5  */
    *(--sp) = 0x04040404u;              /* R4  ← 最后一个写入,地址最低 */

    /* 存下栈顶锚点。以后每次切换,都是"从这个地址开始保存/恢复" */
    t->sp = sp;


}


//pendsv置位
void port_pendsv_set(void)
{
    REG_ICSR = ICSR_PENDSVSET;
}


void port_systick_start(void)
{
    /* 重装载值 = 一个 tick 要数多少个时钟周期 - 1。
     * 16MHz / 1000Hz = 16000,所以写 15999。
     *
     * SysTick 是 24 位递减计数器,最大 16777215。所以在 16MHz 下,
     * 能配出的最慢节拍是 16e6 / 16777216 ≈ 0.95 Hz —— 你 RTOS/ 里那个
     * "1 秒一次"正好顶到极限附近(15999999),再慢一点就放不下了。*/
    REG_SYST_RVR = (CFG_CPU_HZ / CFG_TICK_HZ) - 1u;

    /* 清零当前值:否则可能马上就来一次中断(计数器里还留着上次的残值)。*/
    REG_SYST_CVR = 0;

    /* CLKSOURCE = 1:直接用处理器时钟,不用外面的参考时钟。
     * 用外部时钟时 SysTick 频率可能等于 HCLK/8,算出来的节拍慢 8 倍,
     * 这是"配了 1kHz 结果 125Hz"这类怪现象的常见来源。*/
    REG_SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}



void port_systick_stop(void)
{
    REG_SYST_CSR = 0;
}


/* 把 PendSV / SysTick 配成最低优先级。必须在开中断之前调。*/
void port_set_exception_priorities(void)
{
    REG_PENDSV_PRIO = CFG_PENDSV_PRIO;
    REG_SYS_PRIO = CFG_SYSTICK_PRIO;
}



void SysTick_Handler(void)
{
    tick_increment();
    port_pendsv_set();
}

//首次启动，执行svc
void port_first_start(void)
{
    __asm volatile ("svc 0");
}





/* ===========================================================================
 * 七、HardFault_Handler:程序跑飞之后,怎么查到飞在哪
 * ===========================================================================
 * 这一段是 M10 里最实用的东西 —— 有了它,你不再需要"靠猜"来定位死机。
 *
 * 一、进 HardFault 时现场在哪?
 *
 *   硬件已经自动压好了那 8 个寄存器(R0,R1,R2,R3,R12,LR,PC,xPSR),
 *   但压在哪要看**出错时用的是哪个栈**:
 *      任务里出错 → 用的是 PSP,现场在任务的栈上;
 *      中断里出错 → 用的是 MSP,现场在中断栈上。
 *   怎么分辨?看 LR(EXC_RETURN)的 **bit2**:0 表示当时用 MSP,1 表示用 PSP。
 *
 * 二、为什么这个函数必须是 naked 汇编?
 *
 *   因为一个普通的 C 函数在序言里就会动 SP、动寄存器,现场就找不回来了。
 *   所以第一步必须在汇编里把"现场在哪"这个指针**原样**取出来,再交给 C 函数。
 *   (这也解释了为什么调试器里直接看 Call Stack 经常是一堆垃圾 ——
 *    栈已经乱了,回溯自然不对。)
 *
 * 三、然后 C 函数里做什么?
 *
 *   debug_hardfault_report() 会把栈帧里的 PC(出错的那条指令)、
 *   LR(是谁调用的)、xPSR 取出来,连同 CFSR/HFSR 一起存进全局变量,
 *   然后停住 —— 你在调试器里 Attach 上去,读那几个全局变量就行。
 *   具体每一项怎么看,见 src/debug.c。
 *
 * 四、❌ 不要在 HardFault 里试图"恢复运行"。
 *
 *   HardFault 意味着"CPU 遇到了它无法处理的错误":访问了不存在的地址、
 *   除零(如果开了)、指令不对齐、栈踩到了别的段……
 *   这时候系统的状态是**不可信**的(可能有别的数据结构已经被改坏了)。
 *   正确的做法是:保留现场 → 复位 → 事后分析。
 */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile (
        "tst   lr, #4                 \n"   /* 看 EXC_RETURN 的 bit2 */
        "ite   eq                     \n"
        "mrseq r0, msp                \n"   /* bit2 = 0:现场在 MSP 上 */
        "mrsne r0, psp                \n"   /* bit2 = 1:现场在 PSP 上 */

        "mov   r1, lr                 \n"   /* 第二个参数:EXC_RETURN,用来判断
                                             * "当时是在中断里还是任务里" */
        "b     debug_hardfault_report \n"   /* 尾调用:不返回(b 而不是 bl) */
    );
}
