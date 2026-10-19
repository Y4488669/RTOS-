# stage4 —— 软件定时器、定长内存池与健壮性

> 状态：内核完成（2026-10）
> 这一版结束时的内核：多了"**时间上的回调**"（软件定时器）和"**可控的内存**"（定长池），
> 任务可以创建、删除、**回收**；出问题时能告诉你"**在哪一行、哪条指令**"。

---

## 1. 这一版新增了什么

| 机制 | 在哪 | 一句话 |
|---|---|---|
| 软件定时器 | `timer.h` / `timer.c` | 一条按到期时刻升序的链，tick 里只看队头；回调跑在中断里 |
| 定长内存池 | `mem.h` / `mem.c` | 空闲链表 + 下标换算，O(1)、不碎片、可中断安全 |
| 任务槽回收 | `task.c` + `sched.c` | 已删除的任务挂"待回收"链，**空闲任务**来收 |
| 断言升级 | `debug.c` + `kernel_cfg.h` | `KERNEL_ASSERT` 记录 `文件:行号` 再停 |
| 栈溢出检测 | `debug.c` + `task.c` + `sched.c` | 建任务灌 `0xA5`，换人时看栈底 |
| HardFault 现场 | `debug.c` + `port.c` | 裸汇编取帧，C 里解析 CFSR / HFSR |

**调度器和上下文切换还是没动** —— 这一版新增的仍然是"对象"和"检查"，不是"调度算法"。

---

## 2. 软件定时器（`timer.c`）

### 2.1 它和延时链表是同一个套路

```
delay_list(在 tick.c)   按 wake_tick 升序   睡的是**任务**
timer_list(在 timer.c)  按 expire    升序   管的是**回调**
```

两条链是**独立**的 —— 一条睡任务，一条管回调，但它们的数据结构完全一样：
**有序链表 + 每次 tick 只看队头**，所以单次检查是 O(1)。

```c
void timer_check_expired(void)      /* 被 tick_increment() 调用 */
{
    while (!list_is_empty(&timer_list))
    {
        timer_h *t = container_of(timer_list.head.next, timer_h, node);

        if ((int32_t)(tick_get() - t->expire) < 0)
        {
            break;      /* 队头还没到点 → 后面更没到(升序)*/
        }
        ...
        t->cb(t->arg);  /* ★ 回调就在这里跑 —— 中断上下文 */
    }
}
```

### 2.2 周期 vs 单次

```c
        if (t->periodic)
        {
            t->expire += t->period;     /* ★ 用"上次该到点的时刻"往后推 */
            list_remove(&t->node);
            timer_insert(t);            /* ★ 先重新入队 */
        }
        else
        {
            list_remove(&t->node);
            t->active = 0;
        }

        t->cb(t->arg);                  /* ★ 最后才跑回调 */
```

**两个决定都值得记：**

- **先重新入队，再跑回调**：回调里可能会 `timer_stop` / `timer_restart` 自己，
  先把链表状态摆正最省心；
- **`expire += period`，而不是 `expire = tick_get() + period`**：
  后者会把"跑回调花掉的那点时间"一点点累加进去，于是周期越跑越长 —— 这叫**漂移**。
  按"上次该到点的时刻"往后推，就不会漂。
  （同样的道理：`task_delay` 是相对延时，天然会漂；FreeRTOS 为此另做了一个
  `vTaskDelayUntil`，按绝对时刻推迟。本内核没做那个 API —— 现在你知道它为什么要存在了。）

> ⚠️ 周期定时器的副作用：如果回调耗时超过一个周期，这里会**连着触发好几次**把欠的补上。
> 要不要"跳过补触发"是个策略选择；但正常情况下你不该有这个问题 ——
> 有，就说明回调写得太长了。

### 2.3 回调跑在 tick 中断里

这是硬约束，不是建议：回调里**不能阻塞、不能打印、不能等锁**。
所以 stage4 的 demo 里，定时器回调**只把计数器 +1**，打印交给一个普通任务
（`task_reporter`）—— 任务里可以放心地"该等就等"。

### 2.4 初始化的时机（这条顺序很关键）

```c
void sched_init(void)
{
    task_init();          /* 待回收链表 */
    timer_sys_init();     /* ★ 定时器链表:必须在这里,不能拖到 tick_init() */
    ...
}
```

为什么不能放在 `tick_init()`（那是 `sched_start()` 才调的）：
main 里通常会在 `sched_start()` **之前**就 `timer_init()` / `timer_start()`。
如果链表这时还没初始化（.bss 全零 → `head.next == NULL`），
`timer_insert()` 会拿 NULL 当节点顺着走 —— **在 main 里、调度器还没启动就跑飞**。
（这条是这一版真踩过的坑，见 §8.3。）

### 2.5 两条使用约束

- `timer_init()` 只用于**还没在跑**的定时器。对一个 `active` 的定时器重新 init，
  会把一个仍在链上的节点 `container` 清成 0 —— 之后 `list_remove` 就摘不掉它了；
- `timer_start()` 对已经在跑的定时器是安全的（先摘再插），重复 start 不会在链上挂两次。

---

## 3. 定长内存池（`mem.c`）

### 3.1 结构

```
      blocks  ┌────────┬────────┬────────┬────────┐
              │ 块 0   │ 块 1   │ 块 2   │ 块 3   │   ← 调用方给的大数组
              └────────┴────────┴────────┴────────┘
      nodes   ┌────────┬────────┬────────┬────────┐
              │ node 0 │ node 1 │ node 2 │ node 3 │   ← 与块一一对应的节点
              └────────┴────────┴────────┴────────┘
      free_list: 哨兵 → node 1 → node 3 → 哨兵   (哪些块还空闲)
```

分配 = 从 `free_list` 摘一个节点，用它的**下标**算块地址；
释放 = 用块地址算下标，把对应节点挂回 `free_list`。

```c
/* 块地址 = blocks + idx * block_size
 * idx    = (块地址 - blocks) / block_size   ← 两个方向都是一次减法/乘除 */
uint32_t idx = (uint32_t)(n - p->nodes);        /* 指针相减 = 下标 */
blk = p->blocks + (idx * p->block_size);
```

### 3.2 为什么不用 malloc

| | malloc | 定长块池 |
|---|---|---|
| 分配/释放耗时 | 不定（要搜空闲块、可能拆分/合并） | **O(1)**（摘链表头 / 挂回） |
| 碎片 | 会 | 不会（块等大） |
| 中断安全 | 一般不能 | 能（进临界区就行） |
| 内存利用率 | 高 | 差一点（块大小固定） |

实时系统最在意的是**最坏情况可预测**。"这次 3us、下次可能 300us"是不能接受的。
代价是：你得**预先知道最大同时要几块、每块多大**。

### 3.3 两个设计选择

- **节点单独一个数组**，而不是塞进块里（intrusive free list）。
  塞进块里更省内存，但 `block_size` 得 ≥ 8，而且代码里到处是强转。
  单独一个数组多占 `block_count × 8` 字节，换来"**节点下标 == 块下标**"，查起来就是一次减法；
- **进出都进临界区**。之所以"中断安全"，不是因为算法天然安全，
  而是因为它足够**短** —— 关中断几十个时钟周期就完成了。

### 3.4 使用约束

- `mem_free()` **必须传当初 `mem_alloc()` 返回的那个指针**。
  传块中间的地址进去，算出来的下标是错的 —— 所以里面用断言把
  "越界"和"不在块边界上"都拦住了；
- **重复 free 不做检测**（那要把同一个节点挂两次 → 两次 alloc 拿到同一块内存，
  两个主人互相踩）。真要做，得给每块加"使用中"标志；本版靠约定 + 边界断言。

> 和队列的关系：队列是**拷贝语义**，所以队列根本不需要这块池子
> （数据在 `queue_send` 里就拷进队列的缓冲区了）。池子是给"比队列槽更大的数据"
> 或"要活得更久的对象"用的。demo 里用它只是为了把池子演示一遍。

---

## 4. 任务槽回收（`task.c` + `sched.c`）

### 4.1 槽位图

```c
static uint32_t slot_bitmap = 0;   /* bit i = 1 → 第 i 个槽正在用 */

static task_t *slot_alloc(void)
{
    KERNEL_ASSERT(slot_bitmap != ((1u << CFG_MAX_TASKS) - 1u));   /* 池子满了 */
    uint32_t i = 0;
    while (slot_bitmap & (1u << i)) i++;      /* 从低位找第一个 0 */
    slot_bitmap |= (1u << i);
    return &task_pool[i];
}
```

和 `sched.c` 里用 `CLZ` 从**高位**找第一个 1 是同一招的镜像 ——
只是这里"空"比"有"更常见，不值得为它换指令。

### 4.2 ★ 为什么删除不能当场回收

```
task_delete(self)
   └─ 状态置 DELETED，并把自己挂进 deleted_list
   └─ sched_yield() → PendSV 切走
        （此刻它**还在用自己那块栈**跑最后几条指令）
   └─ 之后某个时刻，空闲任务跑起来
        └─ task_reclaim_deleted()
             ├─ state == DELETED 且 t != current_task   ← 两条都满足才算"真的不跑了"
             ├─ 从 deleted_list 摘掉
             ├─ slot_bitmap 清位        ← 槽还回池子
             └─ name = 0                ← 别在调试器里留个"幽灵任务"
```

当场回收 = 把脚下的地板抽了。所以只能先记账，等它**确定不在任何栈上执行**之后，
由空闲任务（用**空闲任务自己的栈**）去收。

这正是 FreeRTOS 里 `vTaskDelete` 最后一定要落到空闲任务上的原因 ——
它的文档只说了"由空闲任务释放",没说为什么。现在你知道了。

### 4.3 初始化

`deleted_list` 的 `list_init` 在 `task_init()` 里，而 `task_init()` 是 `sched_init()` 的**第一行** ——
必须在 main 里第一次 `task_create` 之前跑过。（这条也是踩过的坑，见 §8.1。）

---

## 5. 调试三件套（`debug.c`）

### 5.1 断言：`文件:行号` + 现场快照

```c
#define KERNEL_ASSERT(cond)                                        \
    do {                                                           \
        if (CFG_ASSERT_ENABLE && !(cond))                          \
        {                                                          \
            debug_assert_failed(__FILE__, __LINE__);               \
        }                                                          \
    } while (0)
```

`debug_assert_failed()` 只多做一件事：把 `file/line` 存进全局的 `debug_info`，然后停住。
但就是这一件，让调试从"看调用栈猜"变成"直接读 `debug_info.assert_line`"。

```c
volatile debug_info_t debug_info = { 0 };
```

**为什么是 `volatile`**：它被异常处理程序写、被调试器读，而"调试器读"这件事
编译器完全不知道 —— 不写 `volatile`，优化一开它可能把整个结构体优化掉。

**断言的边界**：只用来抓"程序员写错了"（参数越界、状态机自相矛盾、链表 container 为空……）。
运行期错误（队列满了、池子空了）用**返回值**，不要用断言。

### 5.2 栈溢出检测

```
建任务时:整块栈灌 0xA5A5A5A5          （debug_stack_fill，在 port_init_stack **之前**）
换人时:  看栈底 CFG_STACK_CHECK_WORDS 个字还是不是 0xA5…（debug_stack_check）
```

- **为什么看栈底**：栈是往下长的，"踩到底"一定先从**低地址**那头开始；
  那里还有填充值，说明这个任务最深也没到过那里；
- **为什么在 `sched_switch` 检查**：被切走的那一刻，这个任务刚跑过一轮，
  痕迹最"热"；而且它已经不占 CPU 了，检查它很安全；
- **顺序不能反**：先灌填充、再伪造栈帧 —— 现场只占栈顶那一小段，不会碰到底部。

**局限**（很重要）：它只能告诉你"**溢出了**"，不能告诉你"**哪一行**溢出的"（那个要 MPU）；
如果任务踩过界之后又退回来了，你只能看到"填充值被改了"这个既成事实；
而且只查 8 个字，轻微的越界可能刚好落在检查范围之外。
但它足够便宜，便宜到可以每次都做 —— 这就是它存在的理由。

### 5.3 HardFault 现场

**为什么必须是 naked 汇编**：普通 C 函数在序言里就会动 SP、动寄存器，现场就找不回来了。

```asm
    tst   lr, #4                 @ EXC_RETURN 的 bit2
    ite   eq
    mrseq r0, msp                @ bit2 = 0:现场在 MSP 上(中断里出的事)
    mrsne r0, psp                @ bit2 = 1:现场在 PSP 上(任务里出的事)
    mov   r1, lr                 @ 第二个参数:EXC_RETURN
    b     debug_hardfault_report @ 尾调用:不返回
```

帧的固定顺序（`frame` 指向的 8 个字）：

```
[0]=R0 [1]=R1 [2]=R2 [3]=R3 [4]=R12 [5]=LR [6]=PC [7]=xPSR
```

所以 `fault_pc = frame[6]`、`fault_lr = frame[5]`，出错时的 SP = `frame + 32`。

**进来之后先读 CFSR，而不是盯着 PC 发呆。** `HFSR.FORCED = 1` 说明是"被升级"上来的，
真正的原因在 CFSR：

| 位 | 名字 | 含义 |
|---|---|---|
| MMFSR bit0/1 | IACCVIOL / DACCVIOL | 取指 / 数据访问越权 |
| BFSR bit1 | **PRECISERR** | 精确数据总线错误 ← 最常见，BFAR 里就是出错的地址 |
| BFSR bit4 | STKERR | **异常入栈出错 ← 栈溢出的典型症状** |
| UFSR bit1 | **INVSTATE** | 非法状态 ← 跳到**偶数地址**最常报这个（Thumb 函数指针最低位必须是 1） |
| UFSR bit2 | INVPC | 非法异常返回（stage1 那次 `bx lr` 被拒就是这个） |

把 `fault_pc` 翻译成人话：

```bash
arm-none-eabi-addr2line -f -e build/Debug/RTOS.elf 0x08000abc
```

**❌ 不要在 HardFault 里试图"修好再跑回去"**：系统状态已经不可信
（可能有别的数据结构已经被改坏了）。正确做法是保留现场 → 复位 → 事后分析。

> **已知局限**：如果出错时用的是 **FPU 扩展帧**（`EXC_RETURN` 的 bit4 = 0，
> 帧会多 17 个字），`frame[6]` 就不是 PC 了 —— 那种情况帧起点要 `+0x48`。
> 当前内核的任务不使用浮点，所以碰不到；但要知道这条边界在哪。

---

## 6. 一个 tick 里的完整链路（stage4 版）

```
SysTick 中断
  ├─ tick_increment()
  │    ├─ tick_count++            时间往前走
  │    ├─ tick_wake_expired()     睡够的任务 → sched_ready()（看延时链表队头）
  │    └─ timer_check_expired()   到点的定时器 → 跑回调（看定时器链队头）
  └─ port_pendsv_set()
       ↓（中断返回后）
     PendSV → sched_switch()
       ├─ debug_stack_check(current_task)   顺便查一下栈底
       └─ 选人 → 换现场
```

创建/唤醒之后的切换时机：仍然是**下一个 tick** 触发（PendSV 是 tick 挂的号），
所以从"就绪"到"真正跑起来"最多差 1ms。

---

## 7. 与 stage3 的对照

| | stage3 | stage4 |
|---|---|---|
| 定时能力 | 只有 `task_delay`（睡任务） | 多了**软件定时器**（睡回调），周期/单次 |
| 内存 | 全静态（队列/池子都在编译期定好） | 多了**定长块池**（运行期按块借还，O(1) 无碎片） |
| 任务删除 | 删了就"消失"，槽永远占着 | **槽会被空闲任务回收**，可以反复创建 |
| 断言 | 死循环 | 记录 `文件:行号` 到 `debug_info` 再停 |
| 栈溢出 | 只能靠猜 | 填充模式 + 每次换人检查 |
| 跑飞 | 只能靠猜 | HardFault 现场（PC/LR/CFSR/HFSR）+ addr2line |
| 空闲任务 | 兜底 + WFI | 多了第三个职责：**回收已删除任务** |

---

## 8. 踩过的坑（调试记录）

这一版的三个坑有个共同点：**模块都写好了，但"接线"没接或者接晚了**。
和 stage2 那两条（延时链表没初始化、`tick_increment` 没人调）是同一类病。

### 8.1 `deleted_list` 没初始化，`task_delete` 也没入队

`static list_t deleted_list;` 在 .bss 里全零 → `head.next == NULL`。
而 `task_delete()` 只把状态改成 DELETED，**没有**把任务挂进 `deleted_list`。
于是空闲任务第一次调 `task_reclaim_deleted()`：

```
pos = deleted_list.head.next = NULL
pos != &head  成立
container_of(NULL, task_t, node)  →  从野地址读 t->state  →  BusFault
```

和 stage1 那个"假 TCB"是同一种死法。**修**：`task_init()` 里 `list_init`，
`sched_init()` 第一行调用；`task_delete()` 里补一句 `list_insert_tail(&deleted_list, &t->node)`。

### 8.2 栈填充/检查的两个调用点没接

`debug_stack_fill()` / `debug_stack_check()` 声明、实现都齐了，但**全工程没有调用者** ——
功能等于死代码。**修**：
`task_create()` 里在 `port_init_stack()` **之前** `debug_stack_fill(t)`；
`sched_switch()` 开头 `debug_stack_check(current_task)`。

### 8.3 定时器初始化得太晚

`timer_sys_init()` 一开始放在 `tick_init()` 里，而 `tick_init()` 是 `sched_start()` 才调的。
可 main 里会在 `sched_start()` **之前**就 `timer_start()` ——
那时 `timer_list` 的头还是 NULL，`timer_insert()` 拿 NULL 当节点走链，
**在 main 里就跑飞**。**修**：把 `timer_sys_init()` 挪进 `sched_init()`。
