//
// Created by lx on 2026/9/21.
//

#ifndef RTOS_PORT_H
#define RTOS_PORT_H
#include "../inc/list.h"
#include "../inc/kernel_cfg.h"
#include "../inc/task.h"

/**
 * port.h —— 硬件相关的一切:寄存器地址、特殊寄存器、异常入口
 * --------------------------------------------------------------------------- *
 * 两个约定
 *
 * 1) **内存映射寄存器**用 volatile 指针直接读写(ICSR、SHPR3、SysTick 那三个)。
 *
 * 2) **核心特殊寄存器**(BASEPRI、PRIMASK、PSP、CONTROL 等)**没有内存地址**,
 *    只能靠 MSR/MRS 指令碰它们。别写成 `*(volatile uint32_t *)0xE000ED22`,
 *    那是个流传很广的错误写法 —— 0xE000ED20 是 SHPR3,它的低 16 位是保留位,
 *    根本不对应 BASEPRI。本文件用内联汇编 MSR 访问。
 */



/* 中断控制与状态寄存器(ICSR)。这里只用它做一件事:把 PendSV 挂起来。*/
#define REG_ICSR            (*(volatile uint32_t *)0xE000ED04u)
#define ICSR_PENDSVSET      (1u << 28)  /* 写 1:PendSV 挂起(请求一次任务切换)*/
#define ICSR_PENDSVCLR      (1u << 27)  /* 写 1:取消挂起 —— 注意是**相邻的另一个位**,
* 写错成 (1u<<28) 就会变成"越清越挂"。*/


#define REG_PENDSV_PRIO   (*(volatile uint8_t *)0xE000ED22)
#define REG_SYS_PRIO      (*(volatile uint8_t *)0xE000ED23)




/* SysTick 三件套 */
#define REG_SYST_CSR        (*(volatile uint32_t *)0xE000E010u)  /* 控制与状态 */
#define REG_SYST_RVR        (*(volatile uint32_t *)0xE000E014u)  /* 重装载值(决定周期)*/
#define REG_SYST_CVR        (*(volatile uint32_t *)0xE000E018u)  /* 当前计数值 */

#define SYST_CSR_ENABLE       (1u << 0)    /* 启动计数 */
#define SYST_CSR_TICKINT      (1u << 1)    /* 数到0产生中断 */
#define SYST_CSR_CLKSOURCE  (1u << 2)   /* 1 = 用处理器时钟(我们走这条路) */
#define SYST_CSR_COUNTFLAG  (1u << 16)  /* 读:上次读之后是否数到过 0 */

/* 错误状态寄存器。只在 HardFault 现场解析里用(见 src/debug.c),
 * 但它们是"硬件状态",所以地址定义归 port.h 管。
 * 具体每一位什么意思,debug.c 里的那张表写得很细。*/
#define REG_CFSR            (*(volatile uint32_t *)0xE000ED28u)  /* 可配置错误状态 */
#define REG_HFSR            (*(volatile uint32_t *)0xE000ED2Cu)  /* 硬错误状态 */


//栈空间初始化
void port_init_stack(task_t *t);



//首次启动，执行svc
void port_first_start(void);




//pendsv置位
void port_pendsv_set(void);

/* 把 PendSV / SysTick 配成最低优先级。必须在开中断之前调。*/
void port_set_exception_priorities(void);


void port_systick_start(void);

void port_systick_stop(void);



/* 临界区:进出必须成对,允许嵌套。见 src/port.c 的使用约定。*/
void port_enter_critical(void);
void port_exit_critical(void);

#endif //RTOS_PORT_H
