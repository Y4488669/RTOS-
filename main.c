/**
 * main.c —— 最小裸机入口(无 HAL)
 * ---------------------------------------------------------------------------
 * 与自研调度器(RTOS/)的协作契约:
 *   1. 每个任务 = 一块 Task_t + 一块 TCB(静态声明,不用堆)
 *   2. task.sp 指向自己的 TCB(调度器靠 task->sp->sp 存取栈顶)
 *   3. task.entry/exit 必须在 create_task 前填好
 *   4. 栈内存由 RTOS 内部 stacks[STACK_NUM] 池提供,main 不需要管
 *   5. 所有任务 create_task 后,sched_init() 启动调度,不再返回
 *
 * 硬件: LED_B = PF11(蓝), LED_R = PF12(红),时钟复位默认 HSI 16MHz。
 */
#include "stm32f4xx.h"
#include "task.h"

/* LED 引脚:PF11 + PF12 */
#define LED_PINS   ((1u << 11) | (1u << 12))

/* 把 PF11/PF12 配成推挽输出:开 GPIOF 时钟 → MODER 置输出 → 初始低 */
static void led_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;

    GPIOF->MODER &= ~(0x3u << (11 * 2));
    GPIOF->MODER |=  (0x1u << (11 * 2));
    GPIOF->MODER &= ~(0x3u << (12 * 2));
    GPIOF->MODER |=  (0x1u << (12 * 2));

    GPIOF->BSRR = LED_PINS << 16;          /* 初始输出低 */
}

/* 任务若意外从 entry 返回,停死在这里(永不返回,兜底) */
static void task_exit(void)
{
    while (1)
    {
    }
}

/* 任务 1:每轮把灯拉高 */
void task1_entry(void)
{
    while (1)
    {
        GPIOF->BSRR = LED_PINS;            /* BSRR 低 16 位:置位 */
    }
}

/* 任务 2:每轮把灯拉低 —— 两任务轮转,肉眼看到 LED 每秒翻转 */
void task2_entry(void)
{
    while (1)
    {
        GPIOF->BSRR = LED_PINS << 16;      /* BSRR 高 16 位:清位 */
    }
}

/* 任务资源:各一块 TCB(存栈顶)+ 一块 Task_t */
static struct TCB tcb1;
static struct TCB tcb2;
static Task_t task1;
static Task_t task2;

int main(void)
{
    task_head_init();
    led_init();

    /* 任务 1:填好 sp(→自己的 TCB)、entry、exit,再注册 */
    task1.sp    = &tcb1;
    task1.entry = task1_entry;
    task1.exit  = task_exit;
    create_task(&task1);

    /* 任务 2 同上 */
    task2.sp    = &tcb2;
    task2.entry = task2_entry;
    task2.exit  = task_exit;
    create_task(&task2);

    /* 启动调度器:内部 svc 0,CPU 从此跑在任务里,不再回到这里 */
    sched_init();

    while (1)
    {
    }
}
