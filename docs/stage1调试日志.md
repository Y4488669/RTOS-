stage1调试日志：

> 注：文中截图是本地 Typora 图床，未随仓库上传；按文字描述也能还原当时的现象。



将程序烧写到把那子上没有发现我想要的现象，于是启用了硬件调试





调试之后发现呢，程序会死在

![image-20260929130326962](/media/y/Windows-SSD/Users/lx/AppData/Roaming/Typora/typora-user-images/image-20260929130326962.png)

这里，应该是因为程序跑飞的原因，

 





进一步打断点排查发现，跑飞发生在svc的中断服务历程里面





然后进入一步一步的运行，在执行

![image-20260929130839338](/media/y/Windows-SSD/Users/lx/AppData/Roaming/Typora/typora-user-images/image-20260929130839338.png)

这个指令的时候跑飞了，查看这个时刻的寄存器r0的值发现，

![image-20260929130929321](/media/y/Windows-SSD/Users/lx/AppData/Roaming/Typora/typora-user-images/image-20260929130929321.png)

r0为 2,然后从这个地址开始出栈，栈已经跑出去内存里面，内存地址已经成负数了，肯定就跑飞了，





然后呢，经过核查，发现任务优先级的取值没有问题，链表也是正常的，

后续排查时发现， 链表head地址为200011a0,当前的任务数量为一，也就是说此刻head的next prev应该指向同一个地方，但是，实际上是

![image-20260929134659945](/media/y/Windows-SSD/Users/lx/AppData/Roaming/Typora/typora-user-images/image-20260929134659945.png)

next与prev并没有指向同一个地址，说明链表操作出现了问题，



会看链表操作，发现list_insert_tail函数有问题

```
void list_insert_head(list_t *l,list_node_t *n)
{
    l->head.next = n;
    l->head.next->prev = n;
    n->next = l->head.next;
    n->prev = &l->head;

    n->container = l;
    l->count++;
}


改为
void list_insert_head(list_t *l,list_node_t *n)
{
    n->next = l->head.next;
    n->prev = &l->head;
    l->head.next->prev = n;
    l->head.next = n;
    n->container = l;
    l->count++;
}


void list_insert_tail(list_t *l,list_node_t *n)
{
    l->head.prev = n;
    l->head.prev->next = n;
    n->prev = l->head.prev;
    n->next = &l->head;

    n->container = l;
    l->count++;
}

改为
void list_insert_tail(list_t *l,list_node_t *n)
{
    n->prev = l->head.prev;
    n->next = &l->head;
    l->head.prev->next = n;
    l->head.prev = n;
    n->container = l;
    l->count++;
}

```

经过这个之后呢，发现svc能够运行了，但是还是又问题，那就是

pendsv出现了问题，当程序中只有一个任务的时候没啥问题，但是两个就出现了问题

出现在，bx lr这一步，当前状态是

![image-20260930182543725](/media/y/Windows-SSD/Users/lx/AppData/Roaming/Typora/typora-user-images
/image-20260930182543725.png)

返回后会从psp指针开始出栈，当前psp为 0x20000458



当前地址数据为

![image-20260930182724545](/media/y/Windows-SSD/Users/lx/AppData/Roaming/Typora/typora-user-images/image-20260930182724545.png)

| **0x20000458** | `00 00 00 00` | **R0**   | `0x00000000` |
| -------------- | ------------- | -------- | ------------ |
| **0x2000045C** | `01 01 01 01` | **R1**   | `0x01010101` |
| **0x20000460** | `02 02 02 02` | **R2**   | `0x02020202` |
| **0x20000464** | `03 03 03 03` | **R3**   | `0x03030303` |
| **0x20000468** | `12 12 12 12` | **R12**  | `0x12121212` |
| **0x2000046C** | `81 04 00 08` | **LR**   | `0x08000481` |
| **0x20000470** | `d9 02 00 08` | **PC**   | `0x080002d9` |
| **0x20000474** | `00 00 00 01` | **xPSR** | `0x01000000` |





pc地址及是下一次指令地址的位置,到这一步骤，查了半天也没用，一看代码，发现

#define REG_PENDSV_PRIO   (*(volatile uint32_t *)0xE000ED22)
#define REG_SYS_PRIO      (*(volatile uint32_t *)0xE000ED23)


这里一个寄存器对应的优先级应该是8位，一个字节才对

改完之后确实能跑了，但当时只是"改对了"，并不知道为什么。后来又带着调试器把两个任务的版本盯了一遍，才把这条链彻底想通 —— 一共三层，值得单独记一笔。

**先说结论**：`bx lr` 不是"跳到 LR 里那个地址"，它是**一次"申请返回线程模式"的异常返回动作**。硬件在这一刻检查的不只是帧和 LR 的值，还有一条帧里看不见的前提：

> **当前这个异常，有没有资格回到线程模式？**

PendSV 这次是**内层**异常（外面还压着 SysTick），没资格 —— 所以这条申请在 `bx lr` 这一步就被判掉了，PC 直接飞出去。**帧内容是对的、LR 的值也是合法的，都救不了它**：被检查的是"资格"（异常嵌套状态），不是数据。

**一、先把"帧"的嫌疑排除掉**

把 PSP 指着的那块栈 dump 出来（上面那张表）：R0~R3、R12 正是我在 port_init_stack 里填的花样值，PC 是任务入口，LR 是 exit hook，xPSR 的 T 位也是 1 —— 这一帧完全正常。所以当时卡住的点是：**帧是对的，为什么 `bx lr` 会跑飞？**

`bx r14` 里的 EXC_RETURN 确实同时决定两件事：

1. 去哪块栈弹那 8 个字（PSP 还是 MSP）；
2. 返回到哪个模式（线程模式还是 handler 模式）。

但这两条都只是"我申请怎么做"。**还有第三个前提，不写在这个值里：谁有资格提出这次返回。**

**二、`bx lr` 这一步到底在检查什么**

异常嵌套是一个栈，合法出口只有两条，而且顺序是固定的：

```
线程模式（任务在跑）
  └─ SysTick 中断        ← 外层
       └─ PendSV 中断     ← 内层（被写歪的优先级弄进来的）

内层 ──(LR = 0xFFFFFFF1，回外层 handler)──> SysTick 继续跑完
                                            └──(LR = 0xFFFFFFFD，回线程模式)──> 任务
```

- **内层异常**只能回**外层 handler**；
- **"返回线程模式"这个动作，只有最外层异常才能做**。

我的 PendSV 是内层，却提交了"回线程模式"（LR = `0xFFFFFFFD`）—— 这一级跨不过去。硬件不是"先弹栈、再发现不对"，而是**在执行这次返回动作的当口就把申请判掉**，所以看起来就是 PC 从 `bx lr` 直接飞掉。

**三、调试器里看到的，正好是一个闭环**

- **进 PendSV 时 LR = `0xFFFFFFF1`** → 它确实是从 handler 模式进来的（被嵌套了），这一半和预期一致；
- **`bx lr` 时 LR = `0xFFFFFFFD`** → 被恢复的是**另一个任务**的帧（它上次保存的 EXC_RETURN 当然写着"回线程模式"，值是合法的）；
- **然后死在 `bx lr` 这一条上** → 说明问题不在"LR 是哪个值"，而在"这次返回的资格"。

同一条指令、同一个 PSP、帧都合法，唯一的差别是 LR 声明要回哪儿 —— 差别在**资格**，不在数据。

**四、失败的样子（架构上这条是 UNPREDICTABLE）**

具体怎么死，芯片实现二选一，但两种都表现为"跑飞"：

- **当场报 fault**：UsageFault 的 `INVPC`（非法异常返回）→ 而 `UsageFault` 复位后是关着的 → **升级成 HardFault** → 跳进 `HardFault_Handler`（我这儿就是 `Default_Handler` 的 `for(;;)` 死循环）；
- **用错的状态做返回**：硬件改用自己存档的返回状态（handler + MSP）而不是 EXC_RETURN 说的那套 → PC 落到不相干的残留地址上，然后一路烂下去。

崩了之后可以读这三个，确认是哪一种：

| 地址 | 看哪一位 | 说明 |
|---|---|---|
| `0xE000ED2C` HFSR | bit30 `FORCED` | =1 → 是别的 fault 升级上来的 |
| `0xE000ED2A` UFSR | bit2 `INVPC` | =1 → 就是"非法异常返回" |
| `0xE000ED2A` UFSR | bit1 `INVSTATE` | 如果只有它，是取指/状态问题 |

顺手再看一眼 **MSP**：应该比它该在的位置**深 32 字节**（0x20）—— 那是被跨过去的 SysTick 留下的栈帧（它的 handler 再也没机会跑完）。

**五、为什么只崩两个任务**

这是这次最值钱的一点。

- **一个任务**时，`pick_next()` 选回来的**就是刚刚被切走的那个任务**，恢复的是同一份帧 —— 帧里的 r14 就是刚存进去的 `0xFFFFFFF1`，"内层 → 外层"是**合法**出口，那一步过得去；CPU 回到 `SysTick_Handler` 里接着跑完，再由它做"最外层 → 线程模式"的合法返回。**"切了个寂寞"，反而把非法的那一步绕开了。**
- **两个任务**时，恢复的是另一个任务的帧（LR = `0xFFFFFFFD`），这一次 `bx lr` 真的要回线程模式 —— **非法，死在这儿。**

所以不是"两个任务引入了新 bug"，而是：**一个任务走的是合法出口，两个任务走的是非法出口**；区别在"被恢复的帧的 LR 属于谁"，不在任务数量本身。

（验证：单任务的 32 位版本，在 PendSV 入口看 LR 是 `0xFFFFFFF1`，`bx lr` 时它还是 `0xFFFFFFF1` —— 过得了；两个任务时 `bx lr` 变成 `0xFFFFFFFD` —— 过不了。）

**六、根因：这两个优先级是怎么被写歪的**

`0xFFFFFFFD` = 线程模式 + PSP，是"从线程模式进入异常"时硬件给的 LR。PendSV 能拿到这个值，靠的就是一个前提：**它是最低优先级的异常**，永远在"所有中断都处理完之后、马上就要回任务"的那一刻从线程模式进入。而这个前提被下面这两个宏破坏了：

```c
#define REG_PENDSV_PRIO   (*(volatile uint32_t *)0xE000ED22)
#define REG_SYS_PRIO      (*(volatile uint32_t *)0xE000ED23)
```

0xE000ED22 / 0xE000ED23 是**字节寄存器**（SHP[10] / SHP[11]，CMSIS 里 `SCB->SHP[12]` 就是 `uint8_t` 数组），而且 0xE000ED22 不是 4 字节对齐、0xE000ED23 连 2 字节都不对齐。用 `uint32_t *` 去写，就是一次**落在 PPB 空间的非对齐字访问** —— M4 的 TRM 写得明白：PPB 空间不支持非对齐访问。也就是说这次写落在哪儿是**不可预测**的，反正没落在想要的字节上（反汇编出来是一条 `str r2,[r3]`，一次字写，把相邻字节也一起带走了）。

结果是 PendSV 的优先级没被设成最低，于是：

1. SysTick 中断进来，`SysTick_Handler` 刚执行到 `port_pendsv_set()`；
2. PendSV 比 SysTick 更急，硬件**当场抢进 SysTick 中断**里（正常的嵌套行为，优先级更高就该抢）；
3. PendSV 这次从 **handler 模式**进来 → 硬件给的 LR 是 `0xFFFFFFF1`（回 handler 模式、用 MSP）；
4. 我的 handler 不问来路，`stmdb r0!, {r4-r11, r14}` 把它原样存进了任务帧 —— 这一格就成了"它被嵌套过"的印记；
5. 然后轮到恢复别人时，就发生了 §二 那件事。

**七、顺带被带崩的还有临界区**

`port_enter_critical()` 用 `BASEPRI = 0x10` 屏蔽优先级 ≥ 0x10 的中断。优先级被写歪成 0x00 之后，SysTick 和 PendSV 都屏蔽不住了 —— 临界区静默失效。所以这个坑不只是"优先级不对"，是内核的中断保护一起没了。

**八、修法**

```c
#define REG_PENDSV_PRIO   (*(volatile uint8_t *)0xE000ED22)
#define REG_SYS_PRIO      (*(volatile uint8_t *)0xE000ED23)
```

SHP 本来就是按字节设计的，字节写才是它正确的打开方式。

要一次写两个，就用**对齐**的 SHPR3 基地址，把优先级放进对应的字节位：

```c
#define REG_SHPR3  (*(volatile uint32_t *)0xE000ED20u)

REG_SHPR3 = (0xF0u << 16)     /* PendSV  在 bits[23:16] */
          | (0xF0u << 24);    /* SysTick 在 bits[31:24] */
```

另外，那次 32 位写还会顺手把 0xE000ED26 清零，那是 SHCSR 的 MEMFAULTENA / BUSFAULTENA / USGFAULTENA 位。现在无害（复位值本来就是 0），但将来为了查 fault 打开它们时，这一行会悄悄又关掉。

**九、记几条教训**

- **"PendSV 必须是最低优先级"不是优化，是契约 —— 而且有两层含义**：① 延迟切换（等所有 ISR 跑完再换任务）；② **保证它的每一次 `bx lr` 都是"最外层 → 线程模式"这次合法返回**。优先级一歪，它变成内层，这次返回就必然非法。
- **看异常返回别只看 LR 的值，要看"它有没有资格"**：同一个 `0xFFFFFFFD`，在合法位置是"回任务"，在非法位置就是"当场被判掉"。
- **内核里凡是"我以为硬件一定会给我 X"的地方，都得能验证。** 顺手在 PendSV 入口加个护栏（`CFG_ASSERT_ENABLE` 打开时生效），LR 的低 4 位不是 0xD 就当场停住：

```asm
and   r2, lr, #0x0f
cmp   r2, #0x0d     @ 0xFFFFFFFD；将来开了 FPU 会是 0xFFFFFFED，低 4 位一样是 D
bne   .
```

- **非对齐访问是硬件里少数真正"不可预测"的坑。** 见到字节寄存器就老老实实写 uint8_t。
- 崩的地方和原因常常不在同一个地方：崩在 `bx lr`（异常返回被拒），根因在 `port_set_exception_priorities()`。
