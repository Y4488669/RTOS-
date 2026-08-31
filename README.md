# RTOS —— 从零到一写一个 RTOS 内核(STM32F407)

> 一个学习项目:不依赖 FreeRTOS 等现成内核,**从零手写一个可运行的 RTOS 内核**,逐步掌握任务调度、上下文切换、同步互斥、IPC 等操作系统核心机制。

目标芯片:**STM32F407ZGTx**(Cortex-M4F,1MB Flash / 128K SRAM / 64K CCM RAM)。

---

## 当前进度(2026-08)

### ✅ 已实现:2 任务时间片轮转调度器(最小可用内核)

| 模块 | 文件 | 说明 |
|---|---|---|
| TCB | `RTOS/kernel.h` | 任务控制块,当前仅含 `sp` 栈指针 |
| 任务栈初始化 | `RTOS/kernel.c` → `init_statck()` | 伪造一个"已被中断过"的栈:硬件帧 8 字 + 软件帧 9 字 |
| 首次启动 | `RTOS/kernel.c` → `SVC_Handler` | 用 SVC 异常进入,走"异常返回"路径启动第一个任务 |
| 上下文切换 | `RTOS/kernel.c` → `PendSV_Handler` | 保存/恢复 R4~R11 + EXC_RETURN,`bx r14` 硬件弹栈 |
| 时基 | `RTOS/kernel.c` → `SysTick_Handler` | 每 1s 置 PendSV 挂起位,触发切换 |
| 调度策略 | `RTOS/kernel.c` → `select_next_stask()` | 目前是 0↔1 双任务来回切换(轮转雏形) |
| 临界区 | `RTOS/kernel.c` | 用 `BASEPRI` 屏蔽 ≤0xFF 的中断,保护调度临界区 |
| 演示 | `main.c` | 两个任务轮流拉高/拉低 PF11/PF12(寄存器直写 BSRR,无 HAL) |

当前调度链路:`SysTick`(1s) → 置 `PENDSVSET` → `PendSV`(低优先级,不打断 ISR)→ 保存旧任务现场 → 选下一个任务 → 恢复新任务现场 → 返回。

### ✅ 工程瘦身:CubeMX 工程 → 最小裸机(无 HAL)

原工程由 STM32CubeMX 生成,HAL 库 + Core 目录共 6.7MB、上百个文件,而真正被用到的只有 GPIO 点灯。现已裁剪为最小裸机结构:

- **删除**:`Drivers/`(HAL 全部 + CMSIS 多余头文件)、`Core/`(CubeMX 生成代码)、`cmake/`
- **保留**:`CMSIS/` 仅 8 个必要头文件 + `system_stm32f4xx.c`、启动汇编、链接脚本
- **重写**:`main.c` 改为寄存器直写(时钟用复位默认 HSI 16MHz,原 SystemClock_Config 本就是空操作);构建脚本全部内联进 `CMakeLists.txt`
- 裁剪后全工程 ~15 个文件,固件 **FLASH 1.5KB / RAM 2.6KB**
- `RTOS.ioc` 仅作留档,不再用于 CubeMX 重新生成
- ⚠️ 注意:编译用 `-mfloat-abi=hard`,但内核上下文切换**尚未保存 FPU 寄存器**——任务里先别用 float,做 M5/M6 时再补 FPU 现场保存

### 🚧 骨架 / 待接入

- `RTOS/inc/task.h` + `RTOS/src/task.c`:`Task_Struct`(名字/状态/栈大小)已定义,`Task_Init()` 尚为空,**任务状态机未接入调度器**。
- `RTOS/inc/list.h` + `RTOS/src/list.c`:就绪队列的链表数据结构骨架(`List_add/Delete/Init`),目前整文件处于注释状态,**未接入内核**。这是下一步的关键。

---

## 目录结构(裁剪后,~15 个文件)

```
RTOS/
├── CMakeLists.txt            # 唯一构建脚本:工具链+编译选项+链接,全部内联
├── CMakePresets.json         # Debug / Release 构建预设
├── main.c                    # 裸机入口:寄存器直写 GPIO,创建任务并启动调度
├── startup_stm32f407xx.s     # 启动汇编:向量表 + 复位处理(.data拷贝/.bss清零)
├── STM32F407xx_FLASH.ld      # 链接脚本(内存分区:FLASH/RAM/CCM)
├── CMSIS/                    # 仅保留必要的 8 个头文件 + SystemInit
│   ├── stm32f407xx.h         # F407 全部寄存器定义(核心)
│   ├── stm32f4xx.h / system_stm32f4xx.h
│   ├── core_cm4.h / cmsis_compiler.h / cmsis_gcc.h / cmsis_version.h / mpu_armv7.h
│   └── system_stm32f4xx.c    # SystemInit(仅设 VTOR,不配时钟)
├── RTOS/                     # ★ 自研内核源码(重点学习区域)
│   ├── kernel.h / kernel.c   # 调度器核心:栈初始化、SVC/PendSV/SysTick、临界区
│   ├── inc/task.h            # 任务结构体与状态枚举(骨架)
│   ├── src/task.c            # 任务管理(骨架)
│   ├── inc/list.h            # 就绪队列链表(骨架)
│   └── src/list.c            # 链表实现(尚未启用)
├── examples/                 # PC 上可跑的学习示例(不参与固件编译)
└── RTOS.ioc                  # CubeMX 工程留档(已弃用,不再生成代码)
```

---

## 环境与构建

### 所需工具

| 工具 | 说明 |
|---|---|
| `arm-none-eabi-gcc` | 交叉编译工具链(本机位于 STM32CubeCLT 的 `GNU-tools-for-STM32`,需在 PATH) |
| CMake ≥ 3.22 + Ninja | 构建系统 |
| CLion(可选) | IDE,CMake 原生支持 |
| STM32CubeProgrammer / OpenOCD | 烧录调试 |

### 命令行编译

```bash
cmake --preset Debug          # 配置,产物目录 build/Debug
cmake --build --preset Debug  # 编译
```

产物:`build/Debug/RTOS.elf`(调试)与 `build/Debug/RTOS.bin`(烧录镜像,构建后自动生成并打印内存占用)。

### 烧录

```bash
STM32_Programmer_CLI -c port=SWD -w build/Debug/RTOS.bin 0x08000000 -RST
```

### 调试

CLion 配置 arm-none-eabi-gdb + OpenOCD 即可单步;重点观察的现场:每个任务自己的 `sp`、进入 PendSV 前后 `psp`/`r4-r11` 的变化、`current_task` 的指向。

---

## 内核设计笔记

### 1. 上下文切换 = 换栈 + 换寄存器现场

Cortex-M 上,**一次任务切换就是"把 A 的寄存器现场存进 A 的栈,再把 B 的栈里的现场弹回寄存器"**。切分两半:

- **硬件自动保存(进异常时入栈,8 字)**:`xPSR / PC / LR / R12 / R3 / R2 / R1 / R0`
- **软件手动保存(9 字)**:`R4~R11` + `R14`

注意 `R14(LR)` 的奥妙:异常进入后硬件把 LR 装成 `EXC_RETURN`,所以 PendSV 保存的 R14 是 `EXC_RETURN` 而非任务自己的返回地址——任务的返回地址已经在硬件帧的 `PC` 里了。恢复时先软件弹回 R4~R11+R14,再用 `bx r14` 触发**硬件弹出**剩余 8 字,一条指令完成"软硬分界"。

### 2. EXC_RETURN:异常怎么回去的

`init_statck()` 里给 R14 填了 `0xFFFFFFFD`:

| 值 | 含义 |
|---|---|
| `0xFFFFFFF1` | 返回**处理器模式**(Handler Mode),用 **MSP** |
| `0xFFFFFFF9` | 返回**线程模式**,用 **MSP** |
| `0xFFFFFFFD` | 返回**线程模式**,用 **PSP** ← 任务跑 PSP,因此伪造初始现场填这个 |

首次启动用 SVC 异常走"返回路径",是为了**复用同一套弹栈逻辑**:启动第一个任务看起来就像"从中断里回来",只是现场是 `init_statck` 预先伪造的。

### 3. 临界区:三种武器,别混用

| 手段 | 屏蔽范围 | 用途 |
|---|---|---|
| `PRIMASK`(`cpsid i`) | **全部**可屏蔽中断 | 短临界区,代价高 |
| `BASEPRI = x` | 优先级 ≤ x 的中断 | **任务调度临界区首选**(本工程用 `0xFF`) |
| 关抢占(如仅关 SysTick) | 单个中断 | 精细控制 |

本工程在 `select_next_stask()` 前后用 `BASEPRI` 包裹,保证"选任务"这一段不被打断,又保留高优先级中断响应。

### 4. 为什么用 PendSV 而不是直接在 ISR 里切换

SysTick 中断里**只置位** `PENDSVSET`,把真正的切换推迟到 PendSV。因为 PendSV 被配置为**最低优先级**(`0xFF`),它会在所有 ISR 完成之后才执行——避免"正在处理中断时切换任务导致现场错乱"的问题。这是 RTOS 标准的"延迟上下文切换"手法。

---

## 学习路线图(Roadmap)

从当前"2 任务轮转"到完整内核,建议的里程碑顺序:

- [x] **M1 最小调度器**:任务栈构建、SVC 首启、PendSV 切换、SysTick 时基 ✅
- [ ] **M2 就绪队列**:用 `list.h` 双向链表替换固定 2 槽 `task_table`,支持 N 个任务
- [ ] **M3 任务状态机**:RUNNING / READY / BLOCKED / SUSPENDED,状态迁移入队出队
- [ ] **M4 任务延时与阻塞**:`delay`/`sleep` 挂起任务,不再 `while` 死等
- [ ] **M5 抢占式多优先级**:就绪队列按优先级排序,高优先级就绪即抢占
- [ ] **M6 同步原语**:二值/计数信号量、互斥锁(优先级继承)
- [ ] **M7 IPC**:消息队列、邮箱
- [ ] **M8 软件定时器**
- [ ] **M9 内存管理**:静态池 vs 堆分配器
- [ ] **M10 健壮性**:HardFault 定位、断言、可追踪调试
- [ ] **(可选)外设驱动**:串口、GPIO、定时器等,验证内核可用性

> 建议:**每个里程碑一个可编译运行的 commit**,并写一小节设计笔记,失败或踩坑的地方单独记录。

---

## 参考资料

- 《Cortex-M3 权威指南》(Joseph Yiu)——上下文切换/异常模型的权威解释
- FreeRTOS 源码——就绪链表、状态机、优先级继承的成熟参考
- ARMv7-M Architecture Reference Manual
- 本工程 `kernel.c` 中已实现代码本身就是最好的入门教材

---

## License

内核源码部分为个人学习代码;`Drivers/`、`Core/`(CubeMX 生成)与链接脚本版权归 ST 及原作者,详见各文件头。
