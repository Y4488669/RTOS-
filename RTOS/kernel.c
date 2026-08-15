//
// Created by lx on 2026/7/30.
//
#include "stdint.h"
#include "stm32f4xx_it.h"
#include "kernel.h"

#define NVIC_SYSPRI3        (*((volatile uint32_t *)0xE000ED20))  /* 系统异常优先级寄存器 3 (PendSV + SysTick) */
#define NVIC_SYSTICK_CTRL         (*((volatile uint32_t *)0xE000E010) ) /* SysTick 控制与状态 */
#define NVIC_SYSTICK_LOAD         (*((volatile uint32_t *)0xE000E014))  /* SysTick 重装载值 */
#define NVIC_SYSTICK_VAL          (*((volatile uint32_t *)0xE000E018))  /* SysTick 当前值 */


/* SysTick Ctrl 寄存器位定义 */
#define NVIC_SYSTICK_CTRL_ENABLE      (1 << 0)  /* SysTick 使能 */
#define NVIC_SYSTICK_CTRL_TICKINT     (1 << 1)  /* SysTick 中断使能 */
#define NVIC_SYSTICK_CTRL_CLKSOURCE   (1 << 2)  /* 时钟源选择: 1=处理器时钟, 0=外部参考时钟 */

#define NVIC_INT_ICSR        (*((volatile uint32_t *)0xE000ED04))  /* 中断控制及状态寄存器ICSR */
#define PENDSV_SET_BIT       (1<<28)
#define PENDSV_STCLR_BIT     (1<<28)

/* BASEPRI — 屏蔽所有优先级 ≤ 该值的中断 */
#define BASEPRI_MAX         (0xFF)    /* 屏蔽所有可屏蔽中断 */







static void task_exit(void)
{
    while (1);
}
/*
 *  硬件保存8个寄存器
 *  xPSR
 *  PC
 *  LR
 *  R12
 *  R3
 *  R2
 *  R1
 *  R0
 *  软件自己保存
 *   R14 这个寄存器在异常进入后被硬件"征用"了，装的不再是任务的返回地址，而是 EXC_RETURN。task 原来的 LR已经在硬件帧里了，不需要你操心
 *   [EXC_RETURN]  ← 软件保存的 9 个字 (PendSV 的 stmdb {r4-r11, r14})，就是R14，理解EXC_RETURN
 *   [R11]
 *   [R10]
 *   [R9]
 *   [R8]
 *   [R7]
 *   [R6]
 *   [R5]
 *   [R4]          ← TCB.sp 指向这里
 */

void init_statck(TCB* tcb, uint32_t *stack,void (*entry)(void))   /* 用于初始化栈，伪造出栈的数据 */
{
    uint32_t *sp = &stack[STACK_SIZE - 1];
    /* 对齐偏移 (Cortex-M3 要求 8 字节栈对齐) */
    sp--;
    //硬件自动保存寄存器
    *sp-- = 0x01000000;  /* xPSR   -bit24=1 代表Thumb状态 */
    *sp-- = (uint32_t)entry;  /* PC */
    *sp-- = (uint32_t)task_exit;  /* LR */
    *sp-- = 0xAAAAAAAA;
    *sp-- = 0xBBBBBBBB;
    *sp-- = 0xCCCCCCCC;
    *sp-- = 0xDDDDDDDD;
    *sp-- = 0xEEEEEEEE;

    //软件自己保存
    *sp-- = 0xFFFFFFFD;       /* R14 = EXC_RETURN   告诉退出异常后进入哪一个状态，
                                *   EXC_RETURN般都是硬件自己写的，这里写的原因是因为需要伪造一个初始状态
                                *   0xFFFFFFFD: 返回线程模式 + 使用 PSP
                                */

    *sp-- = 0xAAAAAAAA;
    *sp-- = 0xBBBBBBBB;
    *sp-- = 0xCCCCCCCC;
    *sp-- = 0xDDDDDDDD;
    *sp-- = 0xEEEEEEEE;
    *sp-- = 0xCCCCCCCC;
    *sp-- = 0xDDDDDDDD;
    *sp   = 0xEEEEEEEE;     /* R4 <- sp现在指向这里 */

    tcb->sp = sp;    //栈指针本身

}

TCB *current_task;

__attribute__((naked)) void SVC_Handler( void )
{
    __asm volatile (
        "ldr r3, =current_task   \n"     /* r3 = &current_task */
        "ldr r1, [r3]            \n"     /* r1 = current_task(TCB指针) */
        "ldr r0, [r1]            \n"     /* r0 = current_task->sp(栈顶) */

        "ldmia r0!, {r4-r11, r14}\n"      /* 弹出软件保存的 R4~R11 和 R14(EXC_RETURN) r0 存的是"从哪个内存地址开始读*/
        "msr psp, r0             \n"      /* PSP = 栈指针（指向硬件栈起点） */
        "isb                     \n"      /* 让新的 PSP 在异常返回前立即生效 */
        "mov r0, #0              \n"
        "msr basepri, r0         \n"      /* 开中断（清零BASEPRI） */
        "bx r14                  \n"     /* 异常返回 -> 硬件弹出xPSR/PC/LR/R12/R3-R0 */
        ".ltorg                  \n"     /* naked 函数不自动生成字面量池，需手动放置  */
    );
}

static TCB *task_table[2];
static int current_task_index = 0;

 void select_next_stask()
{
    current_task_index = !current_task_index;      /* 0 ↔ 1 来回切换 */
    current_task = task_table[current_task_index];
}

__attribute__((naked)) void PendSV_Handler(void)
{
    __asm volatile (
        /* 保存当前任务现场 */
        "mrs r0, psp               \n"
        "isb                       \n"   /* 清空流水线 */

        "stmdb r0!, {r4-r11, r14}       \n"

        /* 更新当前任务的栈指针 */
        "ldr r3, =current_task     \n"
        "ldr r1, [r3]              \n"
        "str r0, [r1]              \n"
        /* 进行调度函数 */
        "mov r4, lr                \n"     /* 保存 EXC_RETURN (因为 bl 会覆盖 lr) */
        "mov r0, %0                \n"
        "msr basepri, r0           \n"     /* 关中断: 屏蔽所有低于 configMAX 的中断 */
        "bl select_next_stask      \n"
        "mov r0, #0                \n"
        "msr basepri, r0           \n"     /* 开中断 */
        "mov lr, r4                \n"     /* 恢复 EXC_RETURN */
        /* 还原新的任务现场 */
        "ldr r3, =current_task     \n"
        "ldr r1, [r3]              \n"
        "ldr r0, [r1]              \n"


        "ldmia r0!, {r4-r11, r14}       \n"


        "msr psp, r0               \n"
        "isb                       \n"
        "bx r14                    \n"    /* 异常返回 → 硬件自动弹出剩余寄存器 */
        ".ltorg                    \n"    /* naked 函数不自动生成字面量池，需手动放置 */

        :
        :"i"(BASEPRI_MAX)                             /* %0 = 中断屏蔽优先级(给%0赋值) */
     );
}

void SysTick_Handler(void)
{
    NVIC_INT_ICSR |= PENDSV_SET_BIT;
}


void start_scheduler(void)
{
     NVIC_SYSPRI3 = (0xFF << 24) | (0xFF << 16);
    /* 配置 SysTick: 1s 切换一次。SYSCLK = HSI = 16MHz, CLKSOURCE=1 直接用 HCLK。
     * LOAD = 16MHz/1Hz - 1 = 15999999 (24 位重装载, 上限 16777215, 放得下) */
    NVIC_SYSTICK_LOAD = (16000000 / 1) - 1;
    NVIC_SYSTICK_VAL  = 0;
    NVIC_SYSTICK_CTRL = NVIC_SYSTICK_CTRL_ENABLE | NVIC_SYSTICK_CTRL_TICKINT | NVIC_SYSTICK_CTRL_CLKSOURCE;

    /* 设置当前任务 */


    current_task_index = 0;
    current_task = task_table[current_task_index];

    /* 开启svc系统调用 */
    __asm volatile ("svc 0");

}
void Register(TCB* tcb1,TCB* tcb2)
{
    task_table[0] = tcb1;
     task_table[1] = tcb2;
}

