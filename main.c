/**
 * main.c —— stage4 演示:软件定时器 / 定长内存池 / 队列 / 槽回收
 * ---------------------------------------------------------------------------
 * 一个能直接跑、直接看的场景,把 stage4 的三样新东西和已有的队列/信号量串起来:
 *
 *   timer_period  250ms 周期:回调里只做计数;每 4 次(1s)给一次信号量
 *                 ★ 回调跑在 tick 中断里 —— 只"++"和 sem_give,绝不打印
 *   timer_once     2s 单次:计数后 restart 自己(演示"单次 + 重启 = 周期")
 *   producer      每 400ms:从池子拿一块 → 填 → queue_send(拷贝)→ 立刻还块
 *   consumer      每 700ms:queue_recv → 打印
 *                 (消费比生产慢 → 队列迟早会满 → producer 会睡在 send_wait 上)
 *   event         等 timer_period 给的信号量 → 打印
 *                 (演示"ISR 里 give,任务里 take"——语义上就是中断通知任务)
 *   creator       每 3s:创建一个短命任务
 *   short         干 3 步(450ms)→ task_delete(self) → 槽由空闲任务回收
 *   report        每 1s:打印 tick / 定时器次数 / 队列深度 / 池子空闲数
 *
 * 分层:main.c + board/ 属于"应用 + 板载外设";RTOS/inc + RTOS/src 只放内核。
 *
 * 输出:USART1(PA9/PA10,115200 8N1)—— 就是 MSH 控制台那个口。
 */
#include "task.h"
#include "sched.h"
#include "sync.h"
#include "queue.h"
#include "timer.h"
#include "mem.h"
#include "tick.h"
#include "board_uart.h"

/* ===========================================================================
 * 内核对象与存储(全部静态:内核不做动态分配)
 * =========================================================================*/
typedef struct
{
    uint32_t seq;
    uint32_t payload;
} msg_t;

#define Q_DEPTH     4
#define POOL_COUNT  4

static msg_t     q_storage[Q_DEPTH];
static queue_t   q;

static uint8_t     pool_blocks[POOL_COUNT * sizeof(msg_t)];
static list_node_t pool_nodes[POOL_COUNT];
static mem_pool_t  pool;

static sem_t     sem_timer;           /* 定时器回调 → 事件任务 */

static timer_h   timer_period;
static timer_h   timer_once;

/* ===========================================================================
 * 定时器回调 —— ★ 这里跑在 tick 中断里
 * =========================================================================*/
static volatile uint32_t period_count = 0;
static volatile uint32_t once_count   = 0;

static void on_period(void *arg)
{
    (void)arg;
    period_count++;

    /* 每 4 次(1s)通知一次事件任务。
     * sem_give 不阻塞;而且内核临界区用的是 BASEPRI=0x10、SysTick 是 0xF0,
     * 所以在 tick 中断里调它是安全的(见架构蓝图 §7.1)。*/
    if ((period_count & 3u) == 0u)
    {
        sem_give(&sem_timer);
    }
}

static void on_once(void *arg)
{
    (void)arg;
    once_count++;

    /* 单次定时器重启自己。回调里操作自己的定时器是允许的:
     * 周期定时器在跑回调**之前**就已经重新入队了,所以 restart 不会把链表搞乱。*/
    timer_restart(&timer_once);
}

/* ===========================================================================
 * 任务
 * =========================================================================*/
static void task_producer(void)
{
    uint32_t seq = 0;

    for (;;)
    {
        msg_t *m = (msg_t *)mem_alloc(&pool);

        if (m != 0)                 /* 池子空是正常返回,不是错误 */
        {
            m->seq     = seq++;
            m->payload = m->seq * 3u;

            uart_printf("[%5ums] producer: 拿块 → 投递 #%u\n", tick_get(), m->seq);

            /* 队列满会在这里睡。队列是拷贝语义,所以睡的时候手里也只有这一块。*/
            queue_send(&q, m, TASK_WAIT_FOREVER);

            uart_printf("[%5ums] producer: #%u 完成(队列 %u/%u,池空闲 %u)\n",
                        tick_get(), m->seq, queue_count(&q), (uint32_t)Q_DEPTH,
                        mem_free_count(&pool));

            mem_free(&pool, m);     /* 已经拷进队列了,块立刻还回去 */
        }

        task_delay(tick_ms_to_ticks(400));
    }
}

static void task_consumer(void)
{
    msg_t m;

    for (;;)
    {
        if (queue_recv(&q, &m, TASK_WAIT_FOREVER) == SYNC_OK)
        {
            uart_printf("[%5ums] consumer: 收到 #%u(payload=%u,队列 %u)\n",
                        tick_get(), m.seq, m.payload, queue_count(&q));
        }

        task_delay(tick_ms_to_ticks(700));
    }
}

static void task_event(void)
{
    for (;;)
    {
        if (sem_take(&sem_timer, TASK_WAIT_FOREVER) == SYNC_OK)
        {
            uart_printf("[%5ums] event   : 收到定时器回调给的信号量\n", tick_get());
        }
    }
}

static void task_short(void)
{
    for (uint32_t i = 0; i < 3u; i++)
    {
        uart_printf("[%5ums] short   : 第 %u/3 步\n", tick_get(), i + 1u);
        task_delay(tick_ms_to_ticks(150));
    }

    uart_printf("[%5ums] short   : 干完,删掉自己 → 槽由空闲任务回收\n", tick_get());

    /* 删自己:状态置 DELETED 并让出;之后空洞任务回收槽位。
     * 正常情况下 PendSV 在这一步之后立刻抢进来,下面的循环跑不到。*/
    task_delete(task_self());

    for (;;)
    {
    }
}

static void task_creator(void)
{
    uint32_t n = 0;

    for (;;)
    {
        task_delay(tick_ms_to_ticks(3000));

        uart_printf("[%5ums] creator : 创建第 %u 个短命任务\n", tick_get(), ++n);

        /* 池子有 8 个槽:idle + 5 个常驻任务 + 1 个短命任务 = 7,还剩 1 个余量。
         * 能一直创建下去,就说明删除的槽确实被回收了。*/
        task_create("short", task_short, 1);
    }
}

static void task_report(void)
{
    for (;;)
    {
        uart_printf("[%5ums] report  : 周期=%u 单次=%u 队列=%u/%u 池空闲=%u\n",
                    tick_get(), period_count, once_count,
                    queue_count(&q), (uint32_t)Q_DEPTH, mem_free_count(&pool));

        task_delay(tick_ms_to_ticks(1000));
    }
}

/* ===========================================================================
 * main
 * =========================================================================*/
int main(void)
{
    /* 1. 板级(应用层):串口 */
    uart_init();

    /* 2. 内核:task_init + timer_sys_init + 就绪链/位图清零。
     *    ★ 必须在 timer_start / task_create 之前调用。*/
    sched_init();

    /* 3. 内核对象(此时内核对象已可用,但调度器还没跑) */
    queue_init(&q, q_storage, sizeof(msg_t), Q_DEPTH);
    sem_init(&sem_timer, 1, 0);                             /* 二值:上限 1,初始 0 */
    mem_pool_init(&pool, pool_blocks, pool_nodes, sizeof(msg_t), POOL_COUNT);

    timer_init(&timer_period, on_period, 0, tick_ms_to_ticks(250), 1);   /* 周期 */
    timer_init(&timer_once,   on_once,   0, tick_ms_to_ticks(2000), 0);  /* 单次 */
    timer_start(&timer_period);
    timer_start(&timer_once);

    /* 4. 任务(数字越大优先级越高;0 留给 idle)
     *    常驻 5 个 + idle = 6 个槽,给短命任务留 2 个余量。*/
    task_create("consumer", task_consumer, 1);
    task_create("creator",  task_creator,  1);
    task_create("report",   task_report,   2);
    task_create("producer", task_producer, 3);
    task_create("event",    task_event,    4);

    uart_printf("\n=== stage4:定时器 / 内存池 / 队列 / 槽回收 ===\n");

    /* 5. 启动:建 idle → 选第一个任务 → 配异常优先级 → 启动 SysTick → svc 0
     *    不会返回。*/
    sched_start();
}

/* ===========================================================================
 * 动手实验(做完这两次,你对"死机"的恐惧会少一大半)
 * ===========================================================================
 *
 * 实验一:栈溢出
 *   把 CFG_STACK_WORDS 改成 32,再加一个任务:
 *       static void task_boom(void)
 *       {
 *           volatile uint32_t hog[64];      // 256 字节,比整块栈还大
 *           for (;;) { hog[0]++; }
 *       }
 *   跑起来会停在 debug_assert_failed() 里 —— 调试器里看 debug_info:
 *       assert_file / assert_line   ← debug.c 里 KERNEL_ASSERT(0) 的那一行
 *
 * 实验二:HardFault
 *   在任意任务里加一行:
 *       *(volatile uint32_t *)0x12345678u = 1;   // 往一个不存在的地址写
 *   跑起来会停在 debug_hardfault_report() 里 —— 调试器里看:
 *       fault_pc    出错的那条指令地址
 *       fault_lr    谁调用它的
 *       fault_cfsr  真正的原因(BFSR.PRECISERR 表示精确数据总线错误)
 *   然后:
 *       arm-none-eabi-addr2line -f -e build/Debug/RTOS.elf <fault_pc>
 *   死机就从玄学变成一道填空题。
 */
