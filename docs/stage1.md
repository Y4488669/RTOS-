# stage1 —— 就绪队列与优先级调度

> 状态：**已完成，已上板跑通**（2026-10）
> 这一版结束时的内核：能把 N 个任务按优先级排好队、按规则轮转着跑，
> 但**还没有任何办法让一个任务"停下来等"**。

---

## 1. 链表 list —— 内核里唯一的"排队"工具

内核里几乎所有东西都是"排队"：任务排队等 CPU（就绪队列）、排队等时间（延时链表）、
排队等信号量（等待队列）…… 这些全用这一个结构，所以先把机制讲透。

```c
struct list_node
{
	list_node_t *next;
	list_node_t *prev;
	list_t      *container;   /* 我在哪条链上 */
};

struct list
{
	list_node_t head;         /* 哨兵：不装数据，只当天花板 */
	uint32_t    count;
};
```

**三个关键设计**

1. **哨兵节点（head）**：链表自己带一个不装数据的节点，首尾相接成一个环。
   插入/删除**永远不用特判"是不是第一个/最后一个"**；空链表就是"哨兵自己指向自己"。
2. **节点内嵌在宿主对象里**：`task_t` 里直接放一个 `list_node_t`，而不是另外 malloc 一个节点
   指向任务。链表只把本来就存在的对象"串起来"，不负责对象的生死 —— 这正是不用 malloc 也能建链表的原因。
3. **`container` 字段**：节点自己记着"我在哪条链上"。于是 `list_remove(node)` 不需要被告知
   链表是谁，O(1) 摘除；而且节点**压根不在任何链上时，它可以安全地什么都不做**
   （`container == NULL` 直接返回）—— stage2 的"超时唤醒"全靠它才写得干净。

**四个操作（每个都只改 3~4 个指针，全是 O(1)）**

```c
void list_insert_tail(list_t *l, list_node_t *n)
{
    n->prev            = l->head.prev;   /* ① 先记录：我前面是谁（原来的尾） */
    n->next            = &l->head;       /* ② 我后面是哨兵 */
    l->head.prev->next = n;              /* ③ 让原来的尾指向我 */
    l->head.prev       = n;              /* ④ 哨兵的前驱改成我 */
    n->container       = l;
    l->count++;
}
```

> ★ **顺序不能随便改，这里踩过坑**：如果先写 `l->head.prev = n`，那么下一步的
> `l->head.prev->next` 就变成了 `n->next`，会把自己写成自环，同时 `head.next` 根本没被更新。
> 现象是：链表 `count = 1` 看着"有一项"，但 `head.next` 还指着 `head` 自己 —— 调度器取队头
> 取到一个假的 TCB，栈指针是垃圾值，一路跑飞。详见 `stage1调试日志.md` 的第一段。

其余三个同理，只是插的位置不同：

```c
void list_insert_head(list_t *l, list_node_t *n);  /* 插到 head.next（本版没用上） */
void list_remove(list_node_t *n);                  /* container 为 NULL 时空操作 */
list_node_t *list_pop_head(list_t *l);             /* 取队头并摘除（本版没用上） */
int      list_is_empty(const list_t *l);           /* 看 count */
uint32_t list_count(const list_t *l);
```

配套的两个宏（`list.h`）：`container_of` 从节点反推出宿主对象；`list_first_entry` 取队头元素。
**空链表不要调 `list_first_entry`** —— 它会返回"哨兵自己"，那是个合法地址但不是任务。

---

## 2. 任务单元 —— TCB 与 task_create

```c
typedef struct task
{
    /* —— 上下文切换直接打交道的字段 —— */
    uint32_t   *sp;             /* ★ 栈指针锚点：保存/恢复现场从这个地址开始。
                                 *   必须是结构体的第一个成员 —— PendSV 里
                                 *   `ldr r1, [tcb]` 直接取它。*/
    uint32_t   *stack_top;      /* 栈的天花板（高地址端）。栈溢出检查用（本版没用上） */

    /* —— 任务是干什么的 —— */
    void      (*entry)(void);   /* 任务函数，必须自己死循环不返回 */
    void      (*exit)(void);    /* 万一 entry return 了，落到这里（= task_exit_hook） */

    /* —— 调度信息 —— */
    uint32_t    base_prio;      /* 创建时定的优先级，永不变 */
    uint32_t    cur_prio;       /* 当前生效优先级。stage3 的优先级继承会临时抬高它，
                                 * 所以"排哪条队"必须看 cur_prio 而不是 base_prio */
    task_state_t state;         /* READY / RUNNING / BLOCKED / SUSPENDED */

    list_node_t node;           /* 排队用的钩子：挂到当前所在那条链上 */

    const char *name;           /* 调试用 */
    uint32_t    stack[CFG_STACK_WORDS];   /* 栈直接内嵌：任务 = 一块内存，里面就有它自己的栈 */
} task_t;
```

**两个之前没想明白、现在有答案的点：**

- **`exit` 是干什么的？**：它是"任务函数万一 return 了"的兜底。硬件不会阻止一个函数返回，
  所以 `port_init_stack()` 把 `task_exit_hook` 填进了栈帧的 LR 槽 —— 任务函数一 `return`，
  就落到 `task_exit_hook()` 里，那里是 `KERNEL_ASSERT(0)` + 死循环（**当场停住**，方便在调试器里看是谁跑完的）。
  正常情况下永远走不到；等 stage2 有了"任务删除"，它会改成"把自己删掉"，
  于是"任务自然结束"就成了一条正规退出路径。**不是没用，是"现在只当断言用"。**

- **`exit` 之外，`base_prio`/`cur_prio` 为什么是两个字段？**：本版它们永远相等。
  分开是为了 stage3 的优先级继承 —— 那时"排队排哪条"看 `cur_prio`，"还回去"看 `base_prio`。

**task_create：一次创建就三步**

```c
task_t *task_create(void (*entry)(void), uint32_t prio, const char *name)
{
    KERNEL_ASSERT(task_used < CFG_MAX_TASKS);
    KERNEL_ASSERT(prio < CFG_PRIO_LEVELS);
    KERNEL_ASSERT(entry != 0);

    task_t *t = &task_pool[task_used++];   /* ① 从静态池里拿一块（永不失败、不会碎片） */
    t->name = name;
    t->entry = entry;
    t->exit  = task_exit_hook;
    t->base_prio = t->cur_prio = prio;
    t->state = TASK_READY;

    port_init_stack(t);                    /* ② 伪造"它被中断过"的现场 —— 见 §5 */

    port_enter_critical();                 /* ③ 入就绪队列（要动位图和链表，进临界区） */
    sched_ready(t);
    port_exit_critical();

    return t;
}
```

> 注意**参数顺序是 `(entry, prio, name)`**，name 在最后。写笔记时别记反。

★ **注意这里没有"创建完看看要不要抢占"这一步**：`task_create()` 返回后，新任务要等
下一个 tick 才可能被切到。`main.c` 里 `task_creator` 那段"创建即抢占"的注释，
和当前实现是对不上的（要接上它，得在临界区里判断"新任务优先级是否更高 → 置 PendSV"）。

---

## 3. 调度器 —— 就绪队列 + 位图

核心数据结构只有两个：

```c
static list_t   ready_list[CFG_PRIO_LEVELS];   /* 一条优先级一条链表，下标就是优先级 */
static uint32_t ready_bitmap;                  /* bit N = 1 表示 ready_list[N] 非空 */
task_t * volatile current_task = NULL;         /* ★ volatile 修饰的是"指针本身"，汇编要直接读它 */
```

### 3.1 为什么是"N 条链表 + 位图"，而不是"一条按优先级排序的链表"

- 按优先级插入是 O(n)，每次插入都要比较排序键；
- 分成 N 条链表后，插入永远是 O(1)（直接扔到自己那条的队尾），
  "该跑谁"变成"哪条链表非空"—— 位图 + `CLZ` **一条指令**就够：

```c
static uint32_t ready_top_prio(void)
{
    return 31u - (uint32_t)__builtin_clz(ready_bitmap);   /* CLZ = 数前导零 */
}
```

`__builtin_clz(0)` 是未定义行为，所以调用前必须保证位图非 0（`pick_next` 里用 `KERNEL_ASSERT` 挡）。

### 3.2 ★ 贯穿全篇的一条不变量

> **正在跑的任务（RUNNING）不在就绪队列里。**

它被"选出来"的那一刻就从队头摘掉了；等它被切走时再放回**队尾**。
于是"同优先级轮转"只需要两行：**切出去插队尾，选人取队头**。
如果把 RUNNING 也留在队列里，你就得在每个地方特判"别选到自己"。

```c
/* 选下一个：从最高优先级的队头取，取完就不在队列里了 */
static void pick_next(void)
{
    KERNEL_ASSERT(ready_bitmap != 0);              /* 有空闲任务兜底，不该为空 */
    uint32_t p = ready_top_prio();
    task_t  *t = list_first_entry(&ready_list[p], task_t, node);

    list_remove(&t->node);                          /* ★ 出队 */
    if (list_is_empty(&ready_list[p]))              /* 这条队空了，把位图上的灯熄掉 */
        ready_bitmap &= ~(1u << p);

    t->state     = TASK_RUNNING;
    current_task = t;
}

/* 把当前任务放回队尾，再选下一个 —— 被 PendSV 调用 */
void sched_switch(void)
{
    if (current_task->state == TASK_RUNNING)        /* 只有"还在跑"的才回队列 */
        sched_ready(current_task);                  /* = 置 READY + 插队尾 + 置位图 */
    pick_next();
}
```

`sched_switch` 里那个 `if` 是为 stage2 留的：等任务会"睡着"（状态变成 BLOCKED，
躺在延时链表上）时，它就不能被捞回就绪队列 —— 这一版还没有任务会走到那个分支。

### 3.3 一个 tick 里的完整链路

```
SysTick 中断（1ms 一次）        只做一件事：置 PendSV 挂起位
        ↓
PendSV（最低优先级，等所有中断都处理完）
        ├─ 保存当前任务现场（汇编）
        ├─ bl sched_switch()      ← 切出去插队尾 + pick_next 选下一个
        └─ 恢复新任务现场（汇编）→ bx r14 回到任务
```

### 3.4 sched_start：五步

```c
void sched_start(void)
{
    task_create(idle_task, 0, "idle");      /* ① 兜底任务，优先级 0 */
    pick_next();                            /* ② 先选出 current_task —— SVC 要读它 */
    port_set_exception_priorities();        /* ③ 先把 PendSV/SysTick 配成最低 */
    port_systick_start();                   /* ④ 再开时基 */
    port_first_start();                     /* ⑤ svc 0：以"异常返回"的方式跳进第一个任务 */

    for (;;) { }                            /* 理论上永远到不了这里 */
}
```

- **② 为什么要在 SVC 之前先选人**：SVC_Handler 和 PendSV_Handler 都是"读 `current_task`，
  从它的栈里恢复现场"，必须有人先把 `current_task` 指对。第一个任务不是"切"进去的，是"选"出来的。
- **③ 为什么必须在开中断之前**：`PendSV`/`SysTick` 的优先级寄存器复位值是 0（= 最高）。
  如果先开 SysTick 再配优先级，中间有一段窗口期，tick 会以"最高优先级"闯进来抢任何东西。

### 3.5 空闲任务

优先级 0（最低），函数体只有一个 `wfi`：

```c
static void idle_task(void)
{
    for (;;)
        __asm volatile ("wfi");   /* 挂起 CPU 直到下一个中断，最坏睡 1 个 tick */
}
```

存在的理由：① 就绪队列可能空（所有任务都在睡），得有人兜底；
② 它以后是干"顺手活"的地方 —— WFI 省电、栈溢出检查、回收已删除任务的栈。
任务级的那行 `busy_wait()` 和这里的 `wfi` 是两回事：前者是"我不知道怎么阻塞，只能空转"，
后者是"真的没事干，让 CPU 睡下"。

### 3.6 sched_need_switch() 与 sched_yield()

- `sched_yield()`：已实现，就是置 PendSV 挂起位 —— "挂号，不立刻切"。
- `sched_need_switch()`：**只有声明（`sched.h:45`、`sched.c:90`），没有实现**。
  它的职责是"判断要不要切"：① 有更高优先级就绪 → 抢占；② 同优先级还有别人在排队 → 轮转
  （FreeRTOS 在 `xTaskIncrementTick()` 里做同样的事）。
  所以本版的任务切换只发生在 tick 上，这一点和 §2 末尾那段要连起来看。

---

## 4. 时基 —— 这一版只有 SysTick 中断

**本版没有独立的 tick 模块**（`tick.h`/`tick.c` 是后面版本才拆出来的），
SysTick 的处理直接放在 `port.c` 里，全部内容就三件套寄存器 + 一个空的中断函数：

```c
void port_systick_start(void)
{
    REG_SYST_RVR = (CFG_CPU_HZ / CFG_TICK_HZ) - 1u;   /* 16MHz / 1000Hz - 1 = 15999 */
    REG_SYST_CVR = 0;                                 /* 先清零，否则可能马上来一次 */
    REG_SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

void SysTick_Handler(void)
{
    port_pendsv_set();     /* 只挂个号，真正切换留给 PendSV */
}
```

**本版没有 tick 计数**：`SysTick_Handler` 只做"置 PendSV"，"时间"和"调度"这两条线还是分开的。

> 等接上 tick 计数，`ms → tick` 的换算有个坑：必须**先乘后除**
> （`(uint64_t)ms * CFG_TICK_HZ / 1000`）。写成 `ms / 1000 * CFG_TICK_HZ` 的话，
> 所有小于 1000ms 的延时都会被整除成 0 个 tick。

---

## 5. 硬件强相关 port —— 本版最硬核的一段

### 5.1 两个约定（写在 `port.h` 顶部，值得背下来）

1. **内存映射寄存器**（ICSR、SHPR3、SysTick 三件套）用 `volatile` 指针直接读写；
2. **内核特殊寄存器**（BASEPRI、PRIMASK、PSP、CONTROL）**没有内存地址**，只能 `MSR`/`MRS`。
   别写成 `*(volatile uint32_t *)0xE000ED22`——那是把 SHPR3 的字节寄存器当成了别的寄存器，
   这个坑本版真踩过，见 §8。

### 5.2 寄存器宏（订正版）

```c
/* 中断控制与状态寄存器：只用它做一件事 —— 把 PendSV 挂起来 */
#define REG_ICSR            (*(volatile uint32_t *)0xE000ED04u)
#define ICSR_PENDSVSET      (1u << 28)   /* 写 1：PendSV 挂起 */
#define ICSR_PENDSVCLR      (1u << 27)   /* 写 1：取消挂起（相邻的另一个位！） */

/* ★ SHPR3：PendSV / SysTick 的优先级，**按字节访问** */
#define REG_PENDSV_PRIO     (*(volatile uint8_t  *)0xE000ED22u)
#define REG_SYS_PRIO        (*(volatile uint8_t  *)0xE000ED23u)

/* SysTick 三件套 */
#define REG_SYST_CSR        (*(volatile uint32_t *)0xE000E010u)  /* 控制与状态 */
#define REG_SYST_RVR        (*(volatile uint32_t *)0xE000E014u)  /* 重装载值 */
#define REG_SYST_CVR        (*(volatile uint32_t *)0xE000E018u)  /* 当前计数值 */

#define SYST_CSR_ENABLE       (1u << 0)    /* 启动计数 */
#define SYST_CSR_TICKINT      (1u << 1)    /* 数到 0 产生中断 */
#define SYST_CSR_CLKSOURCE    (1u << 2)    /* 1 = 用处理器时钟 */
#define SYST_CSR_COUNTFLAG    (1u << 16)   /* 读：上次读之后是否数到过 0 */
```

- 宏名是 `SYST_CSR_*`（不是 `SYS_CSR_*`）；
- `CFG_PENDSV_PRIO` / `CFG_SYSTICK_PRIO` 在 `kernel_cfg.h`（名字里**没有** `_SET`）。

### 5.3 栈帧 —— 17 个字，顺序是硬性的

`port_init_stack()` 干的事就是"伪造一个刚被中断打过、但还没保存现场的任务"：

```
高地址 ┌──────────────────┐ ← t->stack_top（8 字节对齐后的栈顶）
       │     ...未用...    │
       ├──────────────────┤
       │ xPSR = 0x01000000 │ ┐
       │ PC   = entry      │ │ 硬件帧 8 字
       │ LR   = exit       │ │ 异常返回时由**硬件**弹出
       │ R12  = 0x12121212 │ │ （填花样值是为了在调试器里一眼认出"还没被恢复"）
       │ R3   = 0x03030303 │ │
       │ R2   = 0x02020202 │ │
       │ R1   = 0x01010101 │ │
       │ R0   = 0           │ ┘ ← PSP 指向这里
       ├──────────────────┤
       │ LR   = 0xFFFFFFFD │ ┐
       │ R11  = 0x11111111 │ │ 软件帧 9 字
       │ ...                │ │ 切换时由 stmdb/ldmia 存取
       │ R4   = 0x04040404 │ ┘ ← t->sp 指向这里
低地址 └──────────────────┘
```

- **xPSR 的 bit24（T 位）必须是 1**：Cortex-M 只有 Thumb 状态，忘了它异常返回时直接跑飞；
- **PC = 任务入口，LR = `task_exit_hook`**：任务函数一 `return` 就落到 exit 钩子；
- **软件帧最上面那格存的是 `EXC_RETURN`（`0xFFFFFFFD` = 线程模式 + PSP）**，
  这是本设计和 FreeRTOS 的一个差别：每个任务的帧里自带自己的 EXC_RETURN，
  于是"首次启动"和"以后每次切换"可以**复用同一条恢复路径**（`ldmia ... r14` + `bx r14`）。

### 5.4 SVC_Handler —— 第一次进任务

执行 `svc 0` 时 CPU 处于线程模式 + MSP，硬件给的 LR 是 `0xFFFFFFF9`（返回线程模式 + 用 MSP）。
如果老实 `bx lr` 回去，硬件会从 **MSP** 弹那 8 个字 → 又回到 `main` 的 `while(1)` 里空转。
所以必须把它改写成 `0xFFFFFFFD`（用 PSP）：

```asm
orr   r14, #0x0d      @ 0xFFFFFFF9 | 0xD = 0xFFFFFFFD：改成"线程模式 + PSP"
bx    r14             @ 异常返回，硬件按 PSP 弹出我们伪造的那 8 个字 → 任务开始跑
```

### 5.5 PendSV_Handler —— 任务切换四步

```
进去：mrs r0, psp             拿到当前任务的栈指针（指向硬件帧）
      stmdb r0!, {r4-r11,r14} 把软件帧压进**它的**栈
      current_task->sp = r0   记下新的栈顶
选人：bl sched_switch()       纯 C：更新 current_task（外面用 BASEPRI 护住）
出来：ldr r0, [current_task]  拿新任务的栈顶
      ldmia r0!, {r4-r11,r14} 弹出它的软件帧（r14 = 它的 EXC_RETURN）
      msr psp, r0             硬件帧留给硬件
      bx r14                  异常返回：硬件弹掉剩下 8 个字，回到任务上次被打断的地方
```

**为什么不在 SysTick 里直接切？** PendSV 优先级最低（0xF0），所有 ISR 都处理完了它才执行，
于是"换任务"总发生在"马上就回任务"的那一刻；而且挂起位是"或"的关系，
一个 tick 内被置 5 次也只切一次；再加上 BASEPRI 天然屏蔽它（0xF0 ≥ 0x10），
临界区里绝不会被"换人"打断 —— 这一整套叫**延迟上下文切换**。

### 5.6 ★★ 这条设计有一个不能破的前提

> **PendSV 必须是最低优先级的异常。**

`bx r14` 不只是一条跳转，它是**一次"申请返回线程模式"的异常返回动作**。硬件在那一刻还要检查一条
不写在 LR 里的前提：**当前这个异常有没有资格回线程模式**。规则是固定的：

- **内层**异常（被嵌套的那个）只能回**外层 handler**（LR = `0xFFFFFFF1`）；
- **"返回线程模式"（LR = `0xFFFFFFFD`）只有最外层异常才能做。**

PendSV 平时就是最外层，所以它的 `bx r14` 永远合法。反过来，一旦它的优先级不是最低
（比如优先级寄存器被写歪），它就会**嵌套进别的中断里** —— 这时它提交的"回线程模式"申请是
**非法**的，硬件在执行这条 `bx r14` 的当口就把申请判掉，PC 直接飞出去
（要么报 `INVPC` 升级成 HardFault，要么用错的状态做返回）。

**注意：这跟帧里存的值有没有问题无关。** 帧是好的、LR 是合法的 `0xFFFFFFFD`，一样会死在
`bx lr` 上 —— 被检查的是"资格"（异常嵌套状态），不是数据。

本版真踩过这个坑：`REG_PENDSV_PRIO` 用 32 位指针写，把优先级写歪了，
**一个任务时不崩、两个任务必崩**（单任务时"切回自己"是空操作，返回时用到的还是
"内层 → 外层"那条合法出口，恰好把非法的那一步绕开了）。完整推导写在 `stage1调试日志.md`。

### 5.7 临界区

```c
static uint32_t critical_nest = 0;

void port_enter_critical(void)
{
    if (critical_nest == 0)                       /* 只在最外层真去动 BASEPRI */
        msr basepri, CFG_BASEPRI_MASK;            /* 屏蔽优先级 >= 0x10 的中断 */
    critical_nest++;
}

void port_exit_critical(void)
{
    if (--critical_nest == 0)
        msr basepri, 0;
}
```

- 用 `BASEPRI` 而不是 `PRIMASK`：**屏蔽"优先级 ≥ 阈值"的中断，而不是全部**，
  留出优先级 0x00 的口子给真正需要"打断内核"的高优先级外设（比如电机 PWM）。
- `CFG_BASEPRI_MASK` **不能写 0xFF**：F4 只实现高 4 位，最高只能到 0xF0，
  而 BASEPRI 的规则是"≥ 阈值才屏蔽"，写 0xFF 等于**一个都挡不住**。
- 进出的**嵌套计数**是为了让临界区可以嵌套调用（`task_create` 里套 `sched_ready` 就是这么用的）。

---

## 6. 内核可调参数 kernel_cfg.h

| 宏 | 值 | 说明 |
|---|---|---|
| `CFG_PRIO_LEVELS` | 8 | 优先级层数 = 就绪链表条数。**数字越大优先级越高**（和 ARM 中断优先级相反） |
| `CFG_MAX_TASKS` | 8 | 任务池大小（静态数组，永不失败、不会碎片） |
| `CFG_STACK_WORDS` | 128 | 任务栈深度，单位**字**（= 512 字节） |
| `CFG_TICK_HZ` | 1000 | 1 tick = 1ms |
| `CFG_CPU_HZ` | 16000000 | 复位默认 HSI；改了 PLL 必须跟着改，否则节拍不准 |
| `CFG_BASEPRI_MASK` | 0x10 | 临界区屏蔽阈值，见 §5.7 |
| `CFG_PENDSV_PRIO` / `CFG_SYSTICK_PRIO` | 0xFF | 实际落成 0xF0（最低），**必须最低** |
| `CFG_ASSERT_ENABLE` | 1 | 配合 `KERNEL_ASSERT(cond)` |

`KERNEL_ASSERT` 用的是 `if (CFG_ASSERT_ENABLE && !(cond))` 而不是 `#if`：
**关掉它只是让判断永远为假，代码依然在** —— 全内核不写条件编译，
保证"你读到的就是实际会跑的东西"。

> 有三个宏目前还没有代码去读它们：`CFG_IDLE_STACK_WORDS`、`CFG_STACK_FILL_PATTERN`、
> `CFG_STACK_CHECK_WORDS` —— 看到它们别以为已经在工作了。

---

## 7. 流程

### 7.1 上电到第一个任务

```
芯片上电
 └─ 查向量表 → Reset_Handler
     ├─ 拷 .data / 清 .bss → SystemInit()（开 FPU，不配时钟）→ main()
     │    ├─ led_init()
     │    ├─ sched_init()              就绪链表清零、位图清零
     │    ├─ task_create(blue/red)     填 TCB → port_init_stack 伪造 17 字 → 入就绪队列
     │    └─ sched_start()
     │         ├─ task_create(idle)    优先级 0
     │         ├─ pick_next()          current_task = blue（此后 blue 在跑，不在队列里）
     │         ├─ port_set_exception_priorities()
     │         ├─ port_systick_start() 1ms 一次
     │         └─ svc 0 ──→ SVC_Handler 恢复 blue 的伪造帧
     └─ ★ 从此永远跑在任务里，不再回 main
```

### 7.2 之后每秒 1000 次

```
SysTick（0xF0）→ 置 PENDSVSET 位 → 返回
       ↓（等所有中断都处理完）
PendSV（0xF0，最低）→ 保存当前任务现场 → sched_switch() 选下一个 → 恢复新任务现场 → bx r14
       ↓
两个同优先级任务（blue / red）：每个 tick 轮换一次，两个灯交替闪
```

### 7.3 一句话总结这一版

**调度器每 1ms 醒一次，把"还在跑的"放回队尾，从最高优先级那条队的队头挑一个出来跑。**
它不认识"等待"，所以任务想慢一点只能空转 —— 这就是 stage2 要解决的第一个问题。

---

## 8. 踩过的坑（调试记录）

### 8.1 链表插入顺序写反（已修）

`list_insert_tail()` / `list_insert_head()` 里那几个指针的赋值顺序不能颠倒。当时的写法：

```c
l->head.prev       = n;                 /* ✗ 先动了哨兵 */
l->head.prev->next = n;                 /* 这句实际变成了 n->next = n：自环 */
n->prev            = l->head.prev;      /* 又变成 n->prev = n：指向自己 */
n->next            = &l->head;
```

现象：`count == 1` 看着"有一项"，但 `head.next` 还指着 `head` 自己
（**next 和 prev 指的都不是同一个地方**，这就是当时那个排查线索）。
于是调度器取队头取到一个"假 TCB"（本质是哨兵那块内存），`sp` 读出垃圾值，
SVC 里 `ldmia r0!, {...}` 从非法地址出栈 → 跑飞。

修法就是 §1 里那个顺序：**先把"我"自己的两个指针定下来（`n->prev` / `n->next`），
再去改邻居（`old_tail->next` / `head.prev`）。** 顺序反了，邻居就被写坏了。

### 8.2 SHPR3 用 32 位指针写（根因已定位）

```c
#define REG_PENDSV_PRIO   (*(volatile uint32_t *)0xE000ED22)    /* ✗：会崩 */
#define REG_PENDSV_PRIO   (*(volatile uint8_t  *)0xE000ED22u)   /* ✓ */
```

0xE000ED22 / 0xE000ED23 是**字节寄存器**（SHP[10] / SHP[11]），而且这两个地址本身不对齐 ——
用 `uint32_t *` 去写，就是对 PPB 空间的一次**非对齐字访问**。
M4 的 TRM 写着 PPB 空间不支持非对齐访问，所以它落在哪儿是**不可预测**的。
这次的后果是：PendSV 的优先级没被设成最低，它于是**嵌套进了 SysTick 中断**里，
硬件给出的 LR 变成 `0xFFFFFFF1`，直接破坏了 §5.6 那条前提。

**症状很有迷惑性：一个任务正常，两个任务在 PendSV 的 `bx r14` 必崩。**
因为单任务时"切回自己"是个空操作，`bx r14` 那一刻 LR 还是刚存进去的 `0xFFFFFFF1`
（内层 → 外层，**合法**），自己就接回去了；两个任务时恢复的是别人的帧、LR 是 `0xFFFFFFFD`
（内层 → 线程模式，**非法**），才露馅。完整推导 → `stage1调试日志.md`。

> 两个可以现场取证的点，以后遇到同类问题直接用：
> ① 调试器里读 `0xE000ED22` / `0xE000ED23`，正常应该都是 `0xF0`；
> ② 在 `PendSV_Handler` 第一条指令看 **LR**，应该永远是 `0xFFFFFFFD`（线程模式 + PSP）——
> 出现 `0xFFFFFFF1` / `0xFFFFFFF9` 就说明这条前提被破了。
