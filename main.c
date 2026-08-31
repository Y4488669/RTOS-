/**
 * main.c —— 最小裸机入口(无 HAL)
 * ---------------------------------------------------------------------------
 * 硬件: LED_B = PF11(蓝), LED_R = PF12(红),推挽输出,低电平/高电平由任务翻转
 * 时钟: 复位后默认 HSI 16MHz 直供 SYSCLK(SystemInit 不做倍频),
 *       与 kernel.c 里 SysTick 配置(16MHz)保持一致。
 *
 * 对比原来 CubeMX 版 main.c:
 *   HAL_Init()              → 不需要(它只初始化 HAL 时基/FLASH 接口)
 *   SystemClock_Config()    → 不需要(原配置就是 HSI 16MHz,复位默认值)
 *   MX_GPIO_Init()          → led_init(),寄存器直写
 *   HAL_GPIO_WritePin()     → 直接写 BSRR 寄存器
 */
#include "stm32f4xx.h"
#include "kernel.h"

/* LED 引脚:PF11(蓝) + PF12(红),ODR/BSRR 里对应的位 */
#define LED_PINS   ((1u << 11) | (1u << 12))

/* 把 PF11/PF12 配成推挽输出:
 * RCC.AHB1ENR 打开 GPIOF 时钟 → GPIOF.MODER 置为输出模式(01) → ODR 初始为 0 */
static void led_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;      /* GPIOF 时钟使能 */

    GPIOF->MODER &= ~(0x3u << (11 * 2));      /* 清 PF11 模式位 */
    GPIOF->MODER |=  (0x1u << (11 * 2));      /* PF11 = 输出(01) */
    GPIOF->MODER &= ~(0x3u << (12 * 2));      /* 清 PF12 模式位 */
    GPIOF->MODER |=  (0x1u << (12 * 2));      /* PF12 = 输出(01) */

    GPIOF->BSRR = LED_PINS << 16;             /* 两个灯初始输出低(对应原 RESET) */
}

/* 任务 1:把灯拉低(蓝/红灭或亮取决于电路,与原 HAL 版 RESET 行为一致) */
void task1_entry(void)
{
    while (1)
    {
        GPIOF->BSRR = LED_PINS << 16;         /* BSRR 高 16 位:清位 */
    }
}

/* 任务 2:把灯拉高 */
void task2_entry(void)
{
    while (1)
    {
        GPIOF->BSRR = LED_PINS;               /* BSRR 低 16 位:置位 */
    }
}

int main(void)
{
    /* 任务资源全部静态声明:TCB + 各 128 字(512B)栈 */
    TCB task1_tcb;
    TCB task2_tcb;
    static uint32_t task1_stack[STACK_SIZE];
    static uint32_t task2_stack[STACK_SIZE];

    led_init();

    Register(&task1_tcb, &task2_tcb);
    init_statck(&task1_tcb, task1_stack, task1_entry);
    init_statck(&task2_tcb, task2_stack, task2_entry);
    start_scheduler();

    /* 正常情况下 start_scheduler 里的 svc 0 之后 CPU 就跑在任务里,到不了这里 */
    while (1)
    {
    }
}
