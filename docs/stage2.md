# stage2 —— 任务状态机与阻塞延时

> 状态：已完成（2026-10）
> 这一版结束时的内核：任务能"停下来等"了 —— 睡一会儿、被挂起、被删掉，
> CPU 让给别的任务，**不再需要 `busy_wait()` 空转**。

---

## 1. 任务结构体：TCB 多了三样东西

### 1.1 状态枚举

```c
typedef enum
{
    TASK_READY = 0,     /* 可以跑，躺在**就绪队列**里排队 */
    TASK_RUNNING,       /* 正在 CPU 上跑 —— ★ 它**不在**就绪队列里 */
    TASK_BLOCKED,       /* 在等某件事（信号量 / 队列 / 时间），躺在别的链上 */
    TASK_SUSPENDED,     /* 被人为挂起，谁来都不理 */
    TASK_DELETED,       /* 已经删掉，再也不会被调度 */
} task_state_t;
```

一句小抄：**RUNNING 的任务在所有链上都找不到** —— 它正占着 CPU。

### 1.2 唤醒原因（stage3 才有人读）

```c
typedef enum
{
    TASK_WAIT_OK = 0,   /* 等到了 */
    TASK_WAIT_TIMEOUT,  /* 等超时了 */
} task_wait_result_t;
```

带超时的等待有**两种结局**，必须能分清，否则上层会把"没等到"当成"等到了"。
stage2 里它只写不读（`task_delay` 不关心自己是怎么醒的）；stage3 的"最多等 100ms 的信号量"才会读它。

### 1.3 两个节点 + 两个新字段

```c
    list_node_t node;        /* 状态链：就绪队列 / 延时链表 */
    list_node_t wait_node;   /* 事件等待队列：信号量 / 队列（stage3 用）*/

    uint32_t    wake_tick;           /* 睡到哪个 tick（存的是**绝对时刻**）*/
    task_wait_result_t wait_result;  /* 醒来的原因 */
```

**为什么非得是两个节点**：一个任务"最多等 100 个 tick 的信号量"，它既要躺在信号量的
等待队列里（等着被 give），又要躺在延时链表里（等着超时）。**一个节点只能挂一条链**，
所以必须有两个 —— 这就是 FreeRTOS 里 `xStateListItem` / `xEventListItem` 的由来。

**为什么存"绝对时刻"而不是"还要睡多久"**：存绝对时刻，每个 tick 只要比较一次；
存剩余时间，就得每个 tick 把所有任务的重载值都减一（FreeRTOS 的 `vTaskDelayUntil` 那一套）。

---

## 2. 三条链 + 一条不变量

```
                     CPU
                      │ 正在跑（RUNNING，不在任何链上）
      sched_ready ┌───┴────────────────┐ task_block_current
        ┌─────────▼──────────────┐  ┌──▼────────────────────────────┐
        │  就绪队列 ready_list[]  │  │  延时链表 delay_list（tick.c） │
        │  下标 = 优先级          │  │  按 wake_tick **升序**         │
        │  + ready_bitmap 位图    │  └──────────────┬────────────────┘
        └────────────────────────┘                 │ 每 1ms
                                       tick_wake_expired() 看队头 → sched_ready()
        （SUSPENDED / DELETED：不在任何链上）
```

stage1 那条不变量的延伸：

- **RUNNING 不在任何链上**；
- **BLOCKED 在延时链表上**（stage3 起还会**同时**挂在某个对象的等待队列上）；
- **SUSPENDED / DELETED 不在任何链上**（挂起是"人为按暂停"，不是"在等什么"）。

---

## 3. 时基与延时链表（tick.c）

### 3.1 延时链表在哪

```c
static volatile uint32_t tick_count = 0;
static list_t delay_list;              /* ★ 延时链表，文件私有 */
```

它在 `tick.c` 里，而且是 `static` —— 所以**跨文件搜不到**，`tick.h` 里只露出一个
`tick_delay_insert()` 接口。为什么不放 `sched.c`：`sched.c` 管"谁该跑"（就绪队列），
`tick.c` 管"时间"（计数 + 谁该醒了）；睡着的任务属于时间那条线。

### 3.2 升序链表：插入时花一点，每个 tick 都省

```c
void tick_delay_insert(task_t *t)          /* 由 task_block_current 调用（临界区内）*/
{
    list_node_t *pos = delay_list.head.next;
    while (pos != &delay_list.head) {
        task_t *other = container_of(pos, task_t, node);
        if ((int32_t)(other->wake_tick - t->wake_tick) > 0) break;  /* 它比我晚 → 插它前面 */
        pos = pos->next;
    }
    list_insert_before(pos, &t->node);     /* stage2 新加的链表操作 */
}
```

**为什么必须有序**：tick 每秒来 1000 次，每次都要回答"谁该醒了"：

- 有序：只看**队头**，队头没到点，后面更没到 —— 一次比较结束，O(1)；
- 无序：每次 tick 都要把整条链扫一遍。

用"插入时多扫几眼"换"每次 tick 都省下来"。FreeRTOS 换个折法（记一个"下一个解封时刻"，
到点才去扫），本质是同一个取舍。

```c
static void tick_wake_expired(void)
{
    while (!list_is_empty(&delay_list)) {
        task_t *t = list_first_entry(&delay_list, task_t, node);

        if ((int32_t)(tick_count - t->wake_tick) < 0)
        {
            break;      /* 队头还没到点，后面（更晚的）更加没到 —— 收工 */
        }

        list_remove(&t->node);        /* 从延时链表上摘下来 */
        list_remove(&t->wait_node);   /* 可能还挂在某个对象的等待队列上（幂等）*/
        sched_ready(t);               /* 放回就绪队列；wait_result 保持预设的"超时" */
    }
}
```

那句 `break` 是这条链表的**命门**：没有它，队头没到点就会在中断里原地死循环。

### 3.3 回绕：比较用有符号差值

`tick_count` 是 32 位的，1kHz 下约 **49.7 天**归零一次。回绕的那一刻，
`tick_count >= wake_tick` 直接比大小会判错；而 `(int32_t)(tick_count - wake_tick)`
在回绕时依然正确。**凡是"比时间戳先后"的地方都用这个写法**（排序键也一样）。

### 3.4 临界区归谁

- `tick_delay_insert()` **自己不进临界区** —— 由调用者（`task_block_current`）持着。
  这是个跨文件的约定：插入要改链表，而 tick 中断随时可能正在遍历这条链；
- `tick_wake_expired()` 跑在中断里，没有另外加锁 —— 因为 SysTick 是最低异常优先级，
  而目前系统里**只有它一个中断**在跑。（一旦将来有别的中断也碰就绪队列，
  这里和那边都得补 `port_enter_critical()`。）

### 3.5 接线

```c
void SysTick_Handler(void)
{
    tick_increment();      /* 时间 +1，并把到点的任务放回就绪队列 */
    port_pendsv_set();     /* 挂号：让 PendSV 决定要不要换人 */
}
```

`tick_increment()` 只做两件事：`tick_count++`、`tick_wake_expired()`。
"时间"和"调度"仍然是两条线：时间只负责把人叫醒（放回就绪队列），跑不跑由调度器决定。

---

## 4. 阻塞与唤醒（task.c）

### 4.1 task_delay

```c
void task_delay(uint32_t ticks)
{
    if (ticks == 0)
    {
        sched_yield();          /* delay(0) = 不睡，只让出一下 */
        return;
    }
    task_block_current(ticks);  /* 挂起自己（内部进临界区）*/
    sched_yield();              /* 让出：真正的切换在 PendSV 里发生 */
}
```

### 4.2 task_block_current：临界区里干三件事

```c
        t->state = TASK_BLOCKED;                     /* ① 状态：我不再是 RUNNING */
        t->wake_tick   = tick_get() + timeout_ticks; /* ② 记下绝对时刻 */
        t->wait_result = TASK_WAIT_TIMEOUT;          /*    预设：没等到东西就是超时 */
        tick_delay_insert(t);                        /* ③ 挂进延时链表 */
```

`wake_tick` 和插入必须在**同一个临界区**里算和做，否则"算完还没插进去就来 tick"会丢唤醒。

### 4.3 ★ sched_switch 里那个 `if` —— stage1 给它留的坑，现在填上了

```c
void sched_switch(void)
{
    if (current_task->state == TASK_RUNNING)   /* ← stage1 里这句永远成立 */
    {
        sched_ready(current_task);
    }
    pick_next();
}
```

stage1 里所有任务都是"跑不完的死循环"，状态永远是 RUNNING，这个 `if` 是摆设。
stage2 它才真正起作用：**睡着的任务（BLOCKED）不回就绪队列** ——
它此刻正躺在延时链表上，这里不该把它捞回来。

### 4.4 task_unblock：两条唤醒路径

```c
        list_remove(&t->node);         /* 在延时链表上就摘下来 */
        list_remove(&t->wait_node);    /* 在事件等待队列上也摘下来（幂等）*/
        t->wait_result = TASK_WAIT_OK; /* ← 与"超时"路径的唯一区别 */
        sched_ready(t);
```

- **超时**唤醒：`tick_wake_expired()` 里，只摘不问，`wait_result` 保持预设的 TIMEOUT；
- **被叫醒**（give）：走这里，把 `wait_result` 改成 OK。

两条路径都会摘、都会 `sched_ready`，而且**幂等**：`list_remove()` 在节点不在链上时
安全地什么都不做，所以"超时和被 give 同时发生"也不会把链表搞坏 —— 这是 `container` 字段的价值。

---

## 5. 挂起 / 恢复 / 删除

### 5.1 task_suspend / task_resume

挂起 = 把它从所有链上摘下来、状态改成 SUSPENDED；挂起自己时还要 `sched_yield()` 让出 CPU
（挂起的任务只能被**别人**恢复）。

**本版的简化**：如果被挂起时它正在睡延时/等信号量，那个等待就被丢掉了；恢复之后它只会
"从就绪队列重新开始"，而不是接着睡完剩下的时间。FreeRTOS 更细致（挂起不取消等待），
真要做对就得在 SUSPENDED 时再记住"它原来在哪条链上" —— 又一个"状态多了就要多记账"的例子。

### 5.2 task_delete：删掉自己 = 零成本

```c
        sched_dequeue(t);
        list_remove(&t->wait_node);
        t->state = TASK_DELETED;
    ...
    if (self) sched_yield();
```

`sched_switch()` 看到状态是 DELETED（而不是 RUNNING），就不会把它放回就绪队列 ——
它的栈从此再也不会被恢复，这个任务就"消失"了。**不需要任何新机制**：
只要状态机是干净的，调度器自然就不理它。

`task_exit_hook()` 也因此成了正规路径：任务函数一 `return`，落到那里 → `task_delete(task_self())`。

> 内存确实没回收（TCB 和栈是静态池里的），这是"全静态分配"的代价。

### 5.3 sched_dequeue：为什么出队要单独一个函数

```c
void sched_dequeue(task_t *t)
{
    if (t->node.container == &ready_list[t->cur_prio])   /* 确实在就绪队列里 */
    {
        list_remove(&t->node);
        if (list_is_empty(&ready_list[t->cur_prio]))
        {
            ready_bitmap &= ~(1u << t->cur_prio);        /* ★ 位图要跟着熄灯 */
        }
    }
    else
    {
        list_remove(&t->node);                            /* 在延时链表上 / 没挂 */
    }
}
```

**链表和位图是同一份信息的两种存法，改一个必须改另一个。** 直接 `list_remove(&t->node)`
会把节点摘掉却留下亮着的位图 —— 位图说"这条队非空"、链表却是空的，`pick_next()`
就会取到**哨兵**（当成 TCB 用），栈指针是垃圾值 → 跑飞。

这里用 `node.container` 判断"它到底挂在哪条链上"：`container` 是节点自己记的，
不需要问别人 —— 又一次是这个字段在救场。

> 到 stage3 要注意：优先级继承会**临时改 `cur_prio`**，那时 `&ready_list[t->cur_prio]`
> 这个比较就失效了（任务在队列里时优先级被抬高，就判断不出"它在就绪队列里"）。
> 到时候改成从 `container` 反推优先级。

---

## 6. 一个 tick 里的完整链路

```
task_delay(200)
  └─ task_block_current         进临界区
       ├─ state = BLOCKED
       ├─ wake_tick = now + 200
       └─ tick_delay_insert      按 wake_tick 升序插进延时链表
  └─ sched_yield() → PendSV → sched_switch()
       └─ state != RUNNING → 不回就绪队列 → 它"躺在延时链表上"被切走

     ...每 1ms:
SysTick 中断
  ├─ tick_increment()
  │    └─ tick_wake_expired()   看队头：没到点就 break；到点就摘下来 → sched_ready()
  └─ port_pendsv_set()
       ↓（中断返回后）
     PendSV → sched_switch() → pick_next() → 换现场
       └─ 被唤醒的任务回到就绪队列，按优先级参与竞争
```

---

## 7. 与 stage1 的对照

| | stage1 | stage2 |
|---|---|---|
| 任务怎么"慢一点" | `busy_wait()` 空转，霸着 CPU | `task_delay()` 睡进延时链表，CPU 让给别人 |
| 状态枚举 | 定义了但只用到 READY/RUNNING | 五个状态全部用上 |
| TCB | `node` 一个节点 | `node` + `wait_node`、`wake_tick`、`wait_result` |
| 链表 | 就绪队列 | 就绪队列 + 延时链表（升序） |
| 链表 API | head/tail 插入、摘除 | 加 `list_insert_before()`（有序插入要用） |
| `sched_switch` 那个 `if` | 永远成立，是摆设 | BLOCKED 的任务不回就绪队列 |
| 出队 | `list_remove` 直接摘 | `sched_dequeue()`：摘节点 **+ 熄灭位图** |

---

## 8. 踩过的坑（调试记录）

这一版一共踩了五个，其中**三个是"没接上/顺序写反"，编译器一条都不会报**。

### 8.1 延时链表从没被初始化

`delay_list` 是 `static`（.bss，全零），`head.next = NULL`。没人调 `tick_init()`，
于是第一次 `task_delay()` 里 `container_of(NULL, task_t, node)` 顺着垃圾指针走 → 跑飞。
**修**：`sched_start()` 里、开时基之前加 `tick_init();`。

### 8.2 `tick_increment()` 写好了，但没人调用

`SysTick_Handler` 里只有 `port_pendsv_set()` —— 时间这条线是断的：`tick_count` 永远是 0，
`wake_tick = 0 + timeout`，谁都不会到点。
**教训**：新写的函数不光要"实现 + 声明"，还要问一句 **"谁调用它"**（初始化？中断？）。

### 8.3 `tick_wake_expired()` 少了 `break`

队头没到点时不收工，`while` 就在**中断里**原地死循环。这条被 8.1 / 8.4 掩盖着，
把它们修好的那一刻它立刻现形 —— 有序链表的全部意义就是那句 break。

### 8.4 `list_insert_before()` 的指针顺序（第二次踩同款）

```c
/* ✗ 错的写法：pos->prev 已经被改成 node 了，这句实际是 node->next = node（自环）*/
pos->prev = node;
pos->prev->next = node;
```

后果是双向断的：`node->next` 变成自己，而 `head.next` 从头到尾没被更新。
更隐蔽的是 `count` 忘了涨（后来补了）——`list_is_empty()` 用 `count`，
于是**延时链表在别人眼里永远是空的**，任务永远醒不过来。

**正确的顺序**（已在用的版本）：

```c
void list_insert_before(list_node_t *pos, list_node_t *node)
{
    node->next      = pos;            /* ① 我的 next = pos */
    node->prev      = pos->prev;      /* ② 我的 prev = 原来的前驱 */

    pos->prev->next = node;           /* ③ ★ 原来的前驱 → 指向我（此刻 pos->prev 还没被动过）*/
    pos->prev       = node;           /* ④ 再改 pos 的前驱 */

    node->container = pos->container;
    node->container->count++;         /* ⑤ 记账 */
}
```

> **规则（第二次踩了，背下来）**：先把"我自己"的 next/prev 用**旧的**邻居填好，
> 再改邻居；改邻居的这两句里，**凡是引用 `pos->prev` 的，必须排在 `pos->prev = node` 之前**。
> 画一下 `A ↔ node ↔ B` 四根线，逐根落笔。

### 8.5 摘就绪队列节点时，位图没跟着熄灯

`task_suspend()` / `task_delete()` 一开始直接 `list_remove(&t->node)` 就完事了 ——
如果那个任务正好在就绪队列里，位图还亮着：**位图说非空、链表是空的** →
`pick_next()` 取到哨兵（当成 TCB）→ `current_task` 变成垃圾 → 恢复垃圾 `sp` → 跑飞。
**修**：`sched_dequeue()`（见 §5.3）。
