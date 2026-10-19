# stage3 —— 同步原语与 IPC

> 状态：机制层完成（2026-10）
> 这一版结束时的内核：任务之间终于能"配合"了 —— 能**通知**（信号量）、
> 能**互斥**（互斥量 + 优先级继承）、能**传数据**（队列 / 邮箱）。

---

## 1. 这一版新增了什么

| 机制 | 在哪 | 一句话 |
|---|---|---|
| 信号量 | `sync.c` | 计数 + 一条等待队列；give 时"有人等就转手，没人等才计数" |
| 互斥量 | `sync.c` | 把计数换成"持有者"，外加**优先级继承** |
| `sched_set_priority()` | `sched.c` | 改"当前生效优先级"；**只在 READY 时才换队列** |
| `TASK_WAIT_FOREVER` | `task.h` | 三种等待约定的第三种，必须被**专门判断** |
| 消息队列 | `queue.c` | 环形缓冲 + **两条**等待队列，拷贝语义，**醒来必重试** |
| 邮箱 | `queue.c` | 深度 1 的队列 + "覆盖"语义 |

**调度器和上下文切换一行没改** —— 优先级继承说到底只是"改了一个数字 + 换了一条队列"，
并不需要调度器知道有锁这回事。这是这一版最值得体会的一点。

---

## 2. 同步对象的统一形状

```
             一个"状态"                +        一条/两条"等待队列"
信号量        count / max_count                 wait_list
互斥量        owner（谁是持有者）               wait_list
队列          环形缓冲（buf/head/count/depth）  send_wait + recv_wait
```

两个"为什么"：

- **为什么队列要两条链，信号量只要一条？**
  信号量只有一种"缺"：没货。队列有两种：收的人怕**空**（`recv_wait`）、
  发的人怕**满**（`send_wait`）。所以队列要两条。
  注意：是**队列需要两条链**，不是任务需要多挂一条链 —— 一个任务同一时刻只在一头等着。

- **为什么互斥量不需要 count？**
  它的"货"就是"谁持有"这一个事实，所以状态字段是 `owner` 而不是计数。

---

## 3. 三种"等"的约定（`task.h`）

```c
#define TASK_WAIT_FOREVER  0xFFFFFFFFu
```

| 传进去的值 | 含义 | 谁处理 |
|---|---|---|
| `0` | 不等，试一次就走 | `sem_take` / `queue_send` 自己处理（`task_block_current` 里有断言拦着） |
| `0 < timeout < FOREVER` | 最多等这么多 tick，到点自动醒 | `task_block_current` → 挂进延时链表 |
| `TASK_WAIT_FOREVER` | 一直等，**只能被别人叫醒** | `task_block_current` → **不挂**延时链表 |

**★ 为什么不能拿"一个很大的数"当无限等？**

因为 tick 会回绕：`wake_tick = tick_get() + 0xFFFFFFFF` 算出来其实是 `tick_get() - 1`，
也就是"已经过期了" —— "无限等"会当场变成立刻超时。
所以无限等必须是一个**被专门判断**的常量。这类"大数溢出成负数"的坑，
在和时间打交道的代码里到处都是。

---

## 4. 信号量（`sync.c`）

### 4.1 sem_take：三条路

```c
int sem_take(sem_t *s, uint32_t timeout_ticks)
{
    port_enter_critical();
    /* ① 有货：直接拿走 */
    if (s->count > 0) { s->count--; port_exit_critical(); return SYNC_OK; }
    /* ② 没货，而且不等 */
    if (timeout_ticks == 0) { port_exit_critical(); return SYNC_TIMEOUT; }
    /* ③ 没货，要等：挂等待链 → 阻塞（带超时的话挂延时链）→ 让出 */
    list_insert_tail(&s->wait_list, &current_task->wait_node);
    task_block_current(timeout_ticks);
    port_exit_critical();

    sched_yield();
    return (current_task->wait_result == TASK_WAIT_OK) ? SYNC_OK : SYNC_TIMEOUT;
}
```

三条路的顺序就是"能拿到就别排队"：先试一次 → 不能等就走 → 最后才排队睡。
醒来之后只看 `wait_result` 就够了 —— 因为**两条唤醒路径的语义已经分好了**：
被 give 转手叫醒的是 OK，被 tick 判超时的是 TIMEOUT。

### 4.2 sem_give：两种命运，而且**只能选一个**

```c
void sem_give(sem_t *s)
{
    port_enter_critical();
    if (!list_is_empty(&s->wait_list))          /* 有人等着 → 转手 */
    {
        task_unblock(list_first_entry(&s->wait_list, task_t, wait_node));
    }
    else if (s->count < s->max_count)           /* 没人等 → 存起来 */
    {
        s->count++;
    }
    /* 满了而且没人等：这次 give 直接丢弃 */
    port_exit_critical();
}
```

**为什么两件事只能做一件？** 假设两个都做：先 `count++` 再叫醒等待者 ——
等待者拿到的是"我转手给你的货"，而 `count` 里又凭空多出一个令牌。
一个 give 变出两个令牌，信号量的计数就彻底不可信了。
所以：**有人等，就把这次 give 直接交到那个人手里（count 不动）；没人等，才记账。**

### 4.3 "转手"带来的一条重要性质

被转手叫醒的 `sem_take` **醒来即是成功**，不需要回到循环重试、
也不需要再判断 count —— 因为在 give 的临界区里，货和唤醒是**同时**完成的。

这跟队列是两回事（队列不行的原因见 §6.3）—— 这个差别是理解信号量/队列分家的关键。

---

## 5. 互斥量与优先级继承

### 5.1 优先级反转是怎么发生的

```
L(低优先级) 拿到锁，进临界区
M(中优先级) 不碰锁，但它就绪了 → 抢占 L（L 跑不动了）
H(高优先级) 想拿同一把锁 → 被 L 持有 → H 只能等
            ↓
     结果：H 在等 L，而 L 在等 M 让出 CPU —— H 被 M 拖着，
     明明 M 不碰锁，却成了整个链条上的"隐形阻塞者"。
```

L 被 M 抢占这件事本身没错（优先级调度就该这样），错的是**"持有锁的 L 被压住"会连累 H**。
解法：**谁拿着锁挡了我，就把谁的优先级临时抬到我这一档** —— 这样 M 就抢不走 L 了。

### 5.2 继承改的是 `cur_prio`，不是 `base_prio`

```c
    if (m->owner->cur_prio < current_task->cur_prio)   /* 数字越大优先级越高 */
    {
        sched_set_priority(m->owner, current_task->cur_prio);   /* 抬到"我这一档" */
    }
```

- 判断的是 `cur_prio`（当前生效优先级）—— 因为**等待者自己也可能正被别人的锁抬着**，
  用 `base_prio` 算就低了一档；
- 改的也是 `cur_prio`；`base_prio` 是"还回去"时的目标值，从头到尾不动。

### 5.3 ★ sched_set_priority：只在 READY 时才换队列

```c
void sched_set_priority(task_t *t, uint32_t new_prio)
{
    if (t->cur_prio == new_prio) return;

    if (t->state == TASK_READY)          /* ← 关键 */
    {
        list_remove(&t->node);
        if (list_is_empty(&ready_list[t->cur_prio])) ready_bitmap &= ~(1u << t->cur_prio);
        t->cur_prio = new_prio;
        sched_ready(t);                  /* 挪到新优先级那条队尾 */
    }
    else
    {
        t->cur_prio = new_prio;          /* RUNNING / BLOCKED / SUSPENDED：只改数字 */
    }
}
```

**这个 `if` 是这一版最容易写错的地方。** 如果无脑 `sched_dequeue(t); cur_prio = new; sched_ready(t);`：

- 对 **RUNNING** 的任务（`mutex_unlock` 里降优先级时就是这种），它**不在**就绪队列里，
  `sched_ready` 会把"正在跑的任务"塞进就绪队列 → 破坏 stage1 那条不变量
  （RUNNING 不在任何链上）→ 下一次 `sched_switch` 又把它插一遍 → **链表被插两次** → 撕裂。

所以：**排队中的才需要换队，跑着/睡着/挂着的只改数字。**

### 5.4 mutex_unlock：先交锁，再降优先级

```c
    m->owner = 0;
    if (!list_is_empty(&m->wait_list))
    {
        task_t *t = list_first_entry(&m->wait_list, task_t, wait_node);
        m->owner = t;                    /* ★ 直接把锁交给队首那个等待者 */
        task_unblock(t);
    }
    uint32_t base = current_task->base_prio;
    port_exit_critical();

    if (current_task->cur_prio != base)  /* 被抬过 → 还回去 */
    {
        sched_set_priority(current_task, base);
    }
```

两个决定：

- **直接把锁交给等待者**，而不是"先释放、让它们再抢一次"。在当前这个临界区里没有竞争，
  交出去更简单、也更公平（先来的先得，不会因为优先级高低被打乱顺序）；
- **顺序是"先交锁、再降优先级"**。反过来的话会有一小段"锁已经没人持有了，
  但我还占着被抬高的优先级"的窗口 —— 不致命，但没必要。

### 5.5 本版的两处简化（知道就行）

1. **不做传递继承**：如果锁的持有者自己也在等另一把锁（被更里层的人挡着），
   优先级不会沿着链条继续传下去。FreeRTOS 的完整实现会顺着等下去。
2. **多把锁时直接回 `base_prio`**：一个任务同时持有两把锁、分别被抬过优先级时，
   正确做法是"还回去时取所有抬高因素里最高的那个"（或者在解锁时重新算一遍它该是多少）。
   这里直接回 `base_prio` —— 够用，但要记得它是个简化。

---

## 6. 队列与邮箱（`queue.c`）

### 6.1 环形缓冲：只有 head + count

```c
    uint32_t tail = (q->head + q->count) % q->depth;   /* 尾部是算出来的，不存字段 */
    memcpy(queue_slot(q, tail), item, q->item_size);
    q->count++;
```

`head` 是"下一条要读的下标"，`count` 是"现在有几条"，**尾部不用存**。
没有 tail 字段就少一处能写错的地方。

### 6.2 拷贝语义：传的是数据的副本，不是指针

`memcpy` 进出。代价是每次多一次拷贝，换来的是**边界干净**：
发送方把数据拷进队列就可以不管了，接收方拿到的是自己的数据。
传指针（共享内存）的前提是双方对"这块内存归谁、谁能改、谁负责释放"有共识 ——
那是另一整套复杂度。FreeRTOS 的队列也是拷贝，所以它必须知道 `item_size`。

### 6.3 ★ 醒来必重试：那个 `for (;;)` 的唯一理由

**信号量能"转手"，队列不能。**

- `sem_give` 可以**直接把货交给等待者**，所以被叫醒的 `sem_take` 醒来即成功；
- 队列不行：队列里放的是**数据**，数据必须在"有位置的那一瞬间"由**发送方自己**拷进去。
  可发送方当时正在睡 —— 接收方手里只有"位置"，没有你的 item，**谁都替不了你**。
  所以它醒来之后必须回到循环开头，把那次 `memcpy` 真正做掉。

还有一层：**醒来 ≠ 一定成功**。接收方腾出位置、叫醒了队首的我；但在真正恢复运行之前，
可能另一个正在跑的发送方先把那个位置又占了 → 我回到循环开头一看，又满了 → 重新睡回去。

所以这个循环不是"写得笨"，这件事本来就长这样（FreeRTOS 的 `xQueueGenericSend` 里是同一个循环）。
`queue_recv` 是它的镜像（空的等 `recv_wait`，读走后叫醒一个发不出去的）。

### 6.4 超时：`deadline` 只算一次

```c
    uint32_t deadline = tick_get() + timeout_ticks;          /* 进来只算这一次 */
    ...
    uint32_t wait = remaining_wait(deadline, timeout_ticks); /* 每轮只问"还剩多少" */
```

重试是要花时间的。如果每轮都拿 `timeout_ticks` 重新起算，
那"最多等 100 个 tick"理论上能等上无穷久（每醒一次就再送 100 个 tick）。
`remaining_wait` 里两个细节：`TASK_WAIT_FOREVER` 原样传下去；差值用**有符号比较**（回绕时依然正确）。

### 6.5 那四行的顺序不能换

```c
    list_insert_tail(&q->send_wait, &current_task->wait_node);  /* ① 挂上等待链 */
    task_block_current(wait);                                   /* ② 进阻塞态（内部又进一次临界区）*/
    port_exit_critical();                                       /* ③ 放开临界区 */
    sched_yield();                                              /* ④ 挂号 PendSV，真正切走 */
```

- **① ② 必须在临界区里面**：如果先放开锁再挂链，中间的窗口期里接收方可能腾出位置、
  到 `send_wait` 里找等待者 —— **找不到我**（还没挂上），于是它谁也唤醒不了；
  而我随后又睡着了 → 位置白白浪费，我**永远醒不过来**。这就是并发里最经典的**丢失唤醒**。
- **④ 放在 ③ 后面**：PendSV 是 0xF0、被 `BASEPRI = 0x10` 挡着，在临界区里挂号它不会切；
  先退出临界区，让它立刻切走。
- `task_block_current` 里**又进了一次临界区**（嵌套计数就是为这个存在的），
  所以这四行是"锁里套锁"，不是重复加锁。

### 6.6 谁叫醒，谁负责摘

成功后**看不到它从 `send_wait` 上摘自己** —— 因为：

- 被 `task_unblock()` 叫醒：那里面已经 `list_remove(&t->wait_node)`（以及延时链上的 `node`）；
- 超时被 `tick_wake_expired()` 叫醒：里面也有那两句 `list_remove`。

所以等待方只管看 `wait_result` 就够了。`list_remove` 是幂等的（`container` 为 NULL 时什么都不做），
重复摘也不会坏 —— stage2 里讲过这个字段的价值。

### 6.7 邮箱 = 深度 1 的队列 + 覆盖语义

```c
void mailbox_init(mailbox_t *m, void *buf, uint32_t item_size)
{
    queue_init(&m->q, buf, item_size, 1);   /* 深度 1，就这么多 */
}
```

`mailbox_put` **永远不阻塞**：不管原来有没有，直接覆盖 0 号槽 ——
"最新的那条最重要，旧的扔掉"就是它的语义。所以它不需要 `send_wait` 这条链。
适合"状态通知"：我关心的是**当前状态**，漏掉中间过程没关系。

取的那头 `mailbox_get` 和队列完全相同 —— 同一个环形缓冲，只是"满"的策略不同。

---

## 7. 一条完整链路（队列满 → 转手 → 重试）

| 时刻 | 发送方（`queue_send`） | 接收方（`queue_recv`） |
|---|---|---|
| 1 | 满 → 挂 `send_wait`、阻塞、退锁、让出 | （正在跑） |
| 2 | **睡着了**：躺在 `send_wait` + 延时链表上 | 进临界区 → 取走一条、`count--` → 叫醒 `send_wait` 队首 → 返回 OK |
| 3 | （被 unblock：摘链、状态 READY、进就绪队列） | 继续跑 |
| 4 | 被调度回来，从 `sched_yield()` 下一行继续 → `wait_result == OK` | |
| 5 | **回到循环开头**：再进临界区 → 有位置 → `memcpy`、`count++` → 返回 OK | |

第 2 步的"叫醒"只完成**腾位置 + 改状态**，**数据是第 5 步才进去的** ——
这就是"队列不能转手"最直观的体现。

---

## 8. 与 stage2 的对照

| | stage2 | stage3 |
|---|---|---|
| 任务能等什么 | 只能等时间（`task_delay`） | 等时间 **或** 等一个对象（信号量 / 锁 / 队列） |
| `wait_node` | 定义了但没处用 | **这就是它等对象的地方** |
| `wait_result` | 只写不读 | `sem_take` 靠它区分"等到了 / 超时" |
| 三种等待约定 | 只有 0 / 有限 | 多了 `TASK_WAIT_FOREVER`（必须专门判断） |
| `cur_prio` | 恒等于 `base_prio` | 会被优先级继承**临时抬高**，解锁时还回去 |
| 调度器 | 就绪队列 + 位图 | 多了一个 `sched_set_priority()`；**其余一行没改** |
| 新文件 | — | `inc/sync.h`、`src/sync.c`、`inc/queue.h`、`src/queue.c` |

---

## 9. 踩过的坑（调试记录）

这一版的问题全都出在**"约定对不上"**上，不再是 stage1/stage2 那种"指针顺序 / 链表撕裂" ——
层次确实在往上走。

### 9.1 `sem_init` 漏赋值（而且断言差一个等号）

```c
void sem_init(sem_t *s, uint32_t max_count, uint32_t initial_count)
{
    KERNEL_ASSERT(max_count > 0);
    KERNEL_ASSERT(initial_count < max_count);   /* ✗ 应该是 <= */
    list_init(&s->wait_list);
    /* ✗ 少了：s->count = initial_count; s->max_count = max_count; */
}
```

两个后果：信号量是全局/静态的话（.bss 全零）→ `count = max_count = 0` →
**give 全被丢弃、take 永远拿不到**，信号量整个失效；
而 `<` 会让最常见的 `sem_init(&s, 1, 1)`（二值信号量"一开始就有货"）
**当场触发断言死循环** —— 不是不生效，是上电就卡住。

### 9.2 `task_block_current` 没跟上新的等待约定

stage2 的约定是"`0` = 无限等"，stage3 变成了"`0` = 不等、`FOREVER` = 无限等"，
但 `task_block_current` 还是老的两分支 —— 于是 `TASK_WAIT_FOREVER` 走进"有限超时"那一支：
`wake_tick = tick_get() + 0xFFFFFFFF = tick_get() - 1` → **下一个 tick 立刻判超时**。
正是 §3 里警告的那个坑。（这就是"宏抄过来了、语义没同步"的典型。）

### 9.3 声明和实现两个名字

`queue.h` 里声明 `mailbox_get`，`queue.c` 里实现 `mailbox_recv`。
编译能过**只是因为没人调用**（C 允许"声明了但不定义"，只要不引用）—— 一调用就链接失败。

### 9.4 差点踩的：`sched_set_priority` 把 RUNNING 塞回就绪队列

见 §5.3。`mutex_unlock` 降优先级时，目标是**正在跑的任务** —— 它不在就绪队列里，
无脑 `sched_ready()` 就会破坏"RUNNING 不在任何链上"这条从 stage1 立起来的规矩，
而且链表会被插两次、直接撕裂。

**这次的写法是对的**：只在 `state == TASK_READY` 时才换队，其它状态只改数字。
记一笔是因为它很容易写错，而且错了以后的表现（跑飞）离原因很远。
