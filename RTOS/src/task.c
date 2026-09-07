//
// Created by lx on 2026/7/30.
//
#include "../inc/task.h"


#define BASEPRI_MAX         (0xFF)    /* 屏蔽所有可屏蔽中断 */

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


static struct list_node task_head;
static Stack stacks[STACK_NUM];
static uint32_t stack_usge = 0;



static Task_t* current_task = NULL;

static struct TCB *current_tcb = NULL ;

//简易调度函数
void find_task()
{
    list_node_t *pos = current_task->node.next;    /* 下一个节点(裸节点指针) */
    if (pos != &task_head)
        current_task = container_of(Task_t,pos,node);
    else
        current_task = container_of(Task_t,pos->next,node);
    current_tcb = current_task->sp;
}


void tick_config()
{
    NVIC_SYSPRI3 = (0xFF << 24) | (0xFF << 16);
    /* 配置 SysTick: 1s 切换一次。SYSCLK = HSI = 16MHz, CLKSOURCE=1 直接用 HCLK。
     * LOAD = 16MHz/1Hz - 1 = 15999999 (24 位重装载, 上限 16777215, 放得下) */
    NVIC_SYSTICK_LOAD = (16000000 / 1) - 1;
    NVIC_SYSTICK_VAL  = 0;
    NVIC_SYSTICK_CTRL = NVIC_SYSTICK_CTRL_ENABLE | NVIC_SYSTICK_CTRL_TICKINT | NVIC_SYSTICK_CTRL_CLKSOURCE;
}

void task_head_init()
{
    list_init(&task_head);
}

void sched_init(void)
{

    current_task = container_of(Task_t,task_head.next,node);
    current_tcb = (current_task->sp);
    //开启tick中断即可
    tick_config();
    __asm volatile ("svc 0");
}


/*
* struct Task
{
    uint32_t *sp;
    void (*entry)(void);
    void (*exit)(void);
    Task_Status status;
    list_node_t node;
};
 */
void init_stack(Task_t* task)
{
    task->sp->sp = &(stacks[stack_usge++].stack[STACK_SIZE - 1]);  //指向了栈顶，栈由高地址->低地址生长；
    uint32_t *sp = task->sp->sp;
    /* 对齐偏移 (Cortex-M3 要求 8 字节栈对齐) */
    sp--;
    //硬件 8个寄存器xpsr pc lr r12 r3 r2 r1 r0
    *sp-- = 0x01000000;     //使用tumb指令集 16位
    *sp-- = (uint32_t)task->entry;   //任务入口地址
    *sp-- = (uint32_t)task->exit;    //存的是返回地址，但是当前实际的lr里面存的是状态EXC_RETURN
    //一下寄存器随意即可
    *sp-- = 0xAAAAAAAA;
    *sp-- = 0xBBBBBBBB;
    *sp-- = 0xCCCCCCCC;
    *sp-- = 0xDDDDDDDD;
    *sp-- = 0xEEEEEEEE;

    //软件，顺序其实随意即可，只要保持一致就行，有 r4-r11 ,需要存一个EXC_RETURN（代表状态，退出时是 psp ,线程模式，也就是存入lr（r14））
    /*
        |0xFFFF_FFF1|返回Handler模式|
        |0xFFFF_FFF9|返回线程模式，使用MSP主堆栈|
        |0xFFFF_FFFD|返回线程模式，使用PSP线程堆栈|
     */
    *sp-- = 0xFFFFFFFD;   //EXC_RETURN,r14
    //以下随意即可 r4-r11
    *sp-- = 0xAAAAAAAA;
    *sp-- = 0xBBBBBBBB;
    *sp-- = 0xCCCCCCCC;
    *sp-- = 0xDDDDDDDD;
    *sp-- = 0xEEEEEEEE;
    *sp-- = 0xCCCCCCCC;
    *sp-- = 0xDDDDDDDD;
    *sp   = 0xEEEEEEEE;     /* R4 <- sp现在指向这里 */
    task->sp->sp = sp;

}
void create_task(Task_t* task)
{
    task->status = RUNNING;
    list_add(&task_head, &(task->node));
    init_stack(task);
}


void SysTick_Handler(void)
{
    NVIC_INT_ICSR |= PENDSV_SET_BIT;
}
/*  SVC服务程序
 * 1.找到当前的任务栈空间 sp;
 * 2.出栈软件入栈的寄存器
 * 3.更新psp指针
 * 4,开中断
 * 5.退出
 */


__attribute__((naked)) void SVC_Handler( void )
{
    __asm volatile (
        "ldr r0,  =current_tcb \n"   // r0 = &current_tcb，相当于将地址赋予了r0，不是将current_tcb地址处的内容服务
        "ldr r1, [r0]          \n"   // r1 = current_tcb, =====是一个指向TCB的指针
        "ldr r3, [r1]          \n"   // r3 = *current_tcb = tcb->sp
        "ldmia r3!, {r4-r11, r14}\n"
        "msr psp, r3           \n"
        "isb                     \n"      /* 让新的 PSP 在异常返回前立即生效 */
        "mov r0, #0            \n"
        "msr basepri, r0       \n"    //开中断,且特殊功能寄存器必须用msr指令访问,不过，如果没有关中断的话，这里其实可以不写也没啥事
        "bx r14                \n"
        ".ltorg                  \n"     /* naked 函数不自动生成字面量池，需手动放置  */
    );
}
/* PendSV_Handler 函数
 *  保存现场，不能直接保存，因为线程模式用的是psp指针，但是到中断里面用的是msp，所以需要动一下
 *  切换,执行切换函数
 *  恢复现场
 */

__attribute__((naked)) void PendSV_Handler(void)     //优先级最低，不会抢占其他中断
{
    __asm volatile (
        "mrs r0, psp       \n"


        "isb                       \n"   /* 清空流水线 */


        "stmdb r0!, {r4-r11, r14} \n"     //因为下面需要进入一个函数里面，退出后会有lr变成函数的返回值，不是EXC_RETURN了，所以需要切换才行

        //更新当前任务的栈指针，因为刚刚入栈了一些上下文，所以需要更新栈指针
        "ldr r1, =current_tcb    \n"
        "ldr r3, [r1]            \n"
        "str r0, [r3]            \n"


        //进行任务切换
        "mov r4, lr              \n"                //因为进入find_task函数会覆盖掉原来的EXC_RETURN

        "mov r0, %0                \n"
        "msr basepri, r0           \n"     /* 关中断: 屏蔽所有低于 configMAX 的中断 */
        "bl find_task            \n"
        "mov r0, #0                \n"
        "msr basepri, r0           \n"     /* 开中断 */
        "mov lr, r4              \n"

        //恢复现场
        "ldr r3, =current_tcb    \n"
        "ldr r1, [r3]            \n"
        "ldr r0, [r1]            \n"
        "ldmia r0!, {r4-r11, r14}\n"
        "msr psp, r0              \n"

        "isb                       \n"


        "bx r14                   \n"


        ".ltorg                    \n"    /* naked 函数不自动生成字面量池，需手动放置 */

        :
        :"i"(BASEPRI_MAX)                             /* %0 = 中断屏蔽优先级(给%0赋值) */

     );
}
