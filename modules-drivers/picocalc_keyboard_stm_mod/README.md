# PicoCalc 键盘驱动设计说明

## 概述

本驱动配合定制固件 v2.0（BIOSVERSION 0x20）工作，固件配置 `CFG_USE_MODS=0`（关闭固件内部的 shift/ctrl/alt/capslock 状态机，发送原始 scancode），`CFG_REPORT_MODS=1`（修饰键以独立 scancode 上报）。

### 设计思想

将 R-shift 视为特殊的 **Fn 键**，用于触发 F1~F10、Home/End、PageUp/PageDown 等编辑功能键，同时兼作为普通按键的 shift 修饰。L-shift 维持透传。从而实现 `L-shift + R-shift + F1 = Shift + F6` 的效果。

### 三层架构

| 层                       | 职责                                               |
| ------------------------ | -------------------------------------------------- |
| Layer 1: Normalization   | 固件 scancode → `enum phys_key`（稳定的物理键 ID） |
| Layer 2: State machine   | R-shift/Fn 状态机，决定每个按键的 `emit_mode`      |
| Layer 3: Input reporting | `input_report_key/rel` + `input_sync`              |

---

## 1. R-shift 状态机设计

### 1.1 核心原则

**R-shift 的 down/up 事件从不直接上报给 Linux input 子系统。** 驱动内部维护 `RSH` 状态，仅在具体按键 down 时根据该键的性质决定是否补发 shift 事件。

```
R-shift down/up → 仅更新内部 RSH 标志，不产生任何 input 事件
```

这是相比旧驱动的核心改进：旧驱动透传 R-shift 给 Linux，触发 Fn 时需要 toggle shift up/down，导致粘滞和时序问题。新方案彻底消除了 toggle/sync 问题。

### 1.2 按键分组

所有按键在 R-shift 按住时分为两组：

**Fn 组**：R-shift 按住时触发替换 keycode（如 1→F1, A→Home, ↑→PageUp）。
- 这些键的 down/up 直接使用 Fn keycode，不带 shift。
- 例子：`R-shift + 1` → `KEY_F1`（不是 `!`）

**普通组**：R-shift 按住时表现为 shift 效果（如 1→!）。
- down 时：若 `RSH=true` 且 `rshift_reported=false`，补发 `KEY_RIGHTSHIFT down`，然后发基础 keycode。
- up 时：发基础 keycode up，并根据 RSH 状态决定是否补发 `KEY_RIGHTSHIFT up`。

### 1.3 释放追踪（策略 2）

每个物理键在按下时锁定 `emit_mode`，release 时按记录配对：

```c
struct {
    u8  emit_mode;   /* EMIT_FN / EMIT_SHIFTED / EMIT_NORMAL / EMIT_MOUSE */
    u16 keycode;     /* 实际上报的 keycode（0=未发射） */
} key_state[256];    /* index = phys_key */
```

**R-shift 在基础键按住期间提前松开，不影响该键的行为。** down 时锁定的 emit_mode 一直有效，直到该键 up。

### 1.4 事件流示例

#### Fn 组（1→F1）

```
R-shift↓  →  RSH=true（不上报）
1↓        →  emit_mode=EMIT_FN, KEY_F1=1
1↑        →  KEY_F1=0          ← 查 key_state，发 F1 up
R-shift↑  →  RSH=false（不上报）
```

#### 普通组（1→!）

```
R-shift↓  →  RSH=true（不上报）
1↓        →  emit_mode=EMIT_SHIFTED
             KEY_RIGHTSHIFT=1（补发）, KEY_1=1   → 输出 '!'
1↑        →  KEY_1=0
             RSH=true → 不补发 KEY_RIGHTSHIFT up
R-shift↑  →  RSH=false
             KEY_RIGHTSHIFT=0（补发 up）
```

#### 普通组 — R-shift 提前松开

```
R-shift↓  →  RSH=true
1↓        →  EMIT_SHIFTED, KEY_RIGHTSHIFT=1, KEY_1=1
R-shift↑  →  RSH=false, KEY_RIGHTSHIFT=0（补发 up）
1↑        →  KEY_1=0          ← RSH=false，不重复发 shift up
```

#### 普通组 — 基础键 down 时无 R-shift

```
1↓        →  emit_mode=EMIT_NORMAL, KEY_1=1     → 输出 '1'
1↑        →  KEY_1=0
```

### 1.5 已知限制

**R-shift 必须先于基础键按下才有效。** 先按住基础键不放，再按 R-shift，不会改变已按住键的 repeat 行为（因为 R-shift down 不上报，不改变 Linux 的 shift 状态）。

这是"R-shift down 不上报"方案的固有限制，换取的是零 toggle、零时序问题的简洁状态机。

### 1.6 L-shift 透传

L-shift 直接透传给 Linux：

```
L-shift↓  →  KEY_LEFTSHIFT=1
L-shift↑  →  KEY_LEFTSHIFT=0
```

L-shift 与 R-shift 补发互不干扰（Linux input 子系统中两个 shift 是独立的位，叠加结果仍为 shift）。普通组的补发逻辑无需检查 L-shift 状态，R-shift 照常补发。

---

## 2. Fn 键位定义

### 2.1 分组总则

R-shift 按住时，每个物理键属于以下三类之一：

| 类别           | 行为                                     | emit_mode                      |
| -------------- | ---------------------------------------- | ------------------------------ |
| **Fn-keycode** | 替换为 Fn keycode（如 1→F1），不带 shift | `EMIT_FN`                      |
| **Fn-action**  | 触发特殊操作（背光调节），不上报 input   | `EMIT_FN`（action 标记）       |
| **普通**       | 表现为 shift 效果（补发 RSHIFT）         | `EMIT_SHIFTED` / `EMIT_NORMAL` |

### 2.2 4 状态矩阵

> R-shift 视为 Fn 层触发键；L-shift 维持 shift 透传，叠加在当前层之上。状态在按下瞬间锁定。

| 物理按键         | 无 shift  | 仅 L-shift（shift 透传） | 仅 R-shift（Fn 层） | L+R shift（Fn 层 + shift 透传） |
| ---------------- | --------- | ------------------------ | ------------------- | ------------------------------- |
| F1 / F6          | F1        | Shift+F1                 | F6                  | Shift+F6                        |
| F2 / F7          | F2        | Shift+F2                 | F7                  | Shift+F7                        |
| F3 / F8          | F3        | Shift+F3                 | F8                  | Shift+F8                        |
| F4 / F9          | F4        | Shift+F4                 | F9                  | Shift+F9                        |
| F5 / F10         | F5        | Shift+F5                 | F10                 | Shift+F10                       |
| ESC / PauseBreak | ESC       | Shift+ESC                | PauseBreak          | Shift+PauseBreak                |
| Tab / Home       | Tab       | Shift+Tab                | Home                | Shift+Home                      |
| Capslock         | Capslock  | Shift+Capslock           | 切换鼠标模式        | 切换鼠标模式                    |
| Del / End        | Del       | Shift+Del                | End                 | Shift+End                       |
| Backspace        | Backspace | Shift+Backspace          | F11                 | Shift+F11                       |
| ↑                | ↑         | Shift+↑                  | PageUp              | Shift+PageUp                    |
| ↓                | ↓         | Shift+↓                  | PageDown            | Shift+PageDown                  |
| ←                | ←         | Shift+←                  | Home                | Shift+Home                      |
| →                | →         | Shift+→                  | End                 | Shift+End                       |
| `i` / `I`        | i         | I                        | Insert              | Shift+Insert                    |
| `,` / `<`        | `,`       | `<`                      | 屏幕背光-           | —                               |
| `.` / `>`        | `.`       | `>`                      | 屏幕背光+           | —                               |
| Space            | Space     | Shift+Space              | 循环5档调节键盘背光 | —                               |

### 2.3 非鼠标模式 Fn-keycode 映射

| 物理键    | scancode | 基础 keycode  | Fn keycode             |
| --------- | -------- | ------------- | ---------------------- |
| F1        | 0x81     | KEY_F1        | KEY_F6                 |
| F2        | 0x82     | KEY_F2        | KEY_F7                 |
| F3        | 0x83     | KEY_F3        | KEY_F8                 |
| F4        | 0x84     | KEY_F4        | KEY_F9                 |
| F5        | 0x85     | KEY_F5        | KEY_F10                |
| ESC       | 0xB1     | KEY_ESC       | KEY_PAUSE              |
| Tab       | 0x09     | KEY_TAB       | KEY_HOME               |
| Delete    | 0xD4     | KEY_DELETE    | KEY_END                |
| Backspace | 0x08     | KEY_BACKSPACE | KEY_F11                |
| ↑         | 0xB5     | KEY_UP        | KEY_PAGEUP             |
| ↓         | 0xB6     | KEY_DOWN      | KEY_PAGEDOWN           |
| ←         | 0xB4     | KEY_LEFT      | KEY_HOME               |
| →         | 0xB7     | KEY_RIGHT     | KEY_END                |
| I         | 0x49     | KEY_I         | KEY_INSERT             |
| `         | 0x60     | KEY_GRAVE     | KEY_PAGEUP             |
| /         | 0x2F     | KEY_SLASH     | KEY_PAGEDOWN           |
| Capslock  | 0x1A     | KEY_CAPSLOCK  | **特殊：切换鼠标模式** |

### 2.4 Fn-action 映射（不上报 input，I2C 写寄存器）

| 物理键 | scancode | 动作         |
| ------ | -------- | ------------ |
| ,      | 0x2C     | 屏幕背光 -   |
| .      | 0x2E     | 屏幕背光 +   |
| Space  | 0x20     | 键盘背光循环 |

### 2.5 鼠标模式 Fn 表

鼠标模式下，方向键和 `[]` 的**基础功能**变为鼠标操作。R-shift 按住时恢复为正常按键（Fn 行为）。鼠标模式下的 Fn 查找先查鼠标专用表，未命中再查通用 Fn 表（F1→F6 等仍然生效）。

#### 鼠标模式基础功能（无 R-shift）

| 物理键 | 基础功能（替代鼠标）   |
| ------ | ---------------------- |
| ↑      | REL_Y = -1（鼠标上移） |
| ↓      | REL_Y = +1（鼠标下移） |
| ←      | REL_X = -1（鼠标左移） |
| →      | REL_X = +1（鼠标右移） |
| [      | BTN_LEFT（鼠标左键）   |
| ]      | BTN_RIGHT（鼠标右键）  |

#### 鼠标模式 Fn-keycode 映射（R-shift 按住）

| 物理键 | scancode | Fn keycode     | 说明           |
| ------ | -------- | -------------- | -------------- |
| ↑      | 0xB5     | KEY_UP         | 临时恢复方向键 |
| ↓      | 0xB6     | KEY_DOWN       | 临时恢复方向键 |
| ←      | 0xB4     | KEY_LEFT       | 临时恢复方向键 |
| →      | 0xB7     | KEY_RIGHT      | 临时恢复方向键 |
| [      | 0x5B     | KEY_LEFTBRACE  | 临时恢复 `[`   |
| ]      | 0x5D     | KEY_RIGHTBRACE | 临时恢复 `]`   |
| `      | 0x60     | KEY_PAGEUP     | 鼠标模式专有   |
| /      | 0x2F     | KEY_PAGEDOWN   | 鼠标模式专有   |

### 2.6 非 Fn 表中的按键

不在上述任何 Fn 表中的物理键（字母 A-H, J-N, P-U, W-Z, 数字 0-9, 符号 `-=\;'` 等），R-shift 按住时归为**普通组**，表现为 shift 效果。

---

## 3. 鼠标模式

### 3.1 鼠标模式进出

`R-shift + Capslock` 切换鼠标模式。down 事件即触发，拦截 Capslock（不上报 scancode）。切换时追加"键盘背光闪烁"效果（最高亮度 0.5s 后恢复，计时可重入）。

进出时需清理状态：
- 取消鼠标移动 timer
- 清零 `mouse_dir`
- 释放所有被吞掉的 Fn 修饰状态

### 3.2 鼠标移动：内核 timer 方案

不依赖固件 HOLD 事件，改用 `delayed_work` 定时驱动鼠标移动（100Hz / 10ms 间隔）。

**三阶段运动模型：**

| 阶段     | 时间                                | 行为                                                                                                            |
| -------- | ----------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| 初始跳跃 | down 瞬间                           | 立即位移 `mouse_initial_step` px（默认 8px）                                                                    |
| 延迟期   | `mouse_start_delay_ms`（默认 80ms） | 不移动                                                                                                          |
| 加速移动 | 之后持续                            | 从 `mouse_base_speed`（80px/s）线性加速到 `mouse_max_speed`（320px/s），加速时间 `mouse_accel_time_ms`（400ms） |

**定点运算：** 速度单位 px/s，每 tick（10ms）增量用 8 位定点（1px = 256）累积，保证 sub-pixel 精度。

### 3.3 sysfs 可调参数

| 参数     | 默认值   | 路径                                          |
| -------- | -------- | --------------------------------------------- |
| 初始位移 | 8 px     | `/sys/firmware/picocalc/mouse_initial_step`   |
| 初始速度 | 80 px/s  | `/sys/firmware/picocalc/mouse_base_speed`     |
| 最高速度 | 320 px/s | `/sys/firmware/picocalc/mouse_max_speed`      |
| 启动延迟 | 80 ms    | `/sys/firmware/picocalc/mouse_start_delay_ms` |
| 加速时间 | 400 ms   | `/sys/firmware/picocalc/mouse_accel_time_ms`  |

### 3.4 固件 HOLD 事件处理

所有固件 HOLD 事件**一律吞掉**。鼠标移动由内核 timer 驱动，普通按键由 Linux autorepeat 驱动。

### 3.5 鼠标模式下的 emit_mode 锁定

按策略 2，emit_mode 在 down 时锁定，up 时配对：

| 场景                                              | 事件流                                               |
| ------------------------------------------------- | ---------------------------------------------------- |
| 方向键先按（鼠标移动），再按 R-shift              | emit_mode=EMIT_MOUSE，R-shift 无影响，timer 继续移动 |
| R-shift 先按，方向键 down（KEY_UP），R-shift 松开 | emit_mode=EMIT_FN，KEY_UP 保持，直到方向键 up        |

### 3.6 鼠标按键（BTN_LEFT/RIGHT）

`[` `]` 在鼠标模式下无 R-shift 时为鼠标左右键。固件只发 down/up（无 HOLD），直接上报 `BTN_LEFT`/`BTN_RIGHT`。

---

## 4. 补充说明

- Home/End 的重复映射是有意为之：键盘模式下用方向键（←/→）触发 Home/End 更方便（本机物理布局类似手柄，方向键在左上角）；鼠标模式下则用标准 Tab/Del 的 Fn 层访问 Home/End。

- 鼠标模式是一个 overlay（bool 值），非鼠标模式下的所有规则在鼠标模式下继续生效，仅与鼠标专属按键冲突时才被覆盖。

- 切换鼠标模式的判定基于 Capslock 拦截：只要 R-shift 处于按下态，Capslock 的 down 事件即被拦截为切换事件，不会再向 Linux 上报 Capslock。

- Fn-action（背光调节）在 Down 时触发（每次 down 触发一次，release 无任何操作），而且要拦截原始按键。

- 软关机：驱动注册 `reboot_notifier` + `i2c_driver.shutdown` 回调，`poweroff` 时在关机末期向 MCU 发送 `REG_ID_OFF`，MCU 切断 SoC 供电并延迟断电。

---

## 5. 固件修改说明（v2.0）

基于 PicoCalc 原厂固件 v1.6（`0x16`）修改，版本号升至 `0x20`。

### 5.1 新增功能

| 功能 | 说明 |
|------|------|
| `REG_ID_CFG` 读写 | 驱动可在运行时通过 I2C 读写 CFG 寄存器，动态配置固件行为 |
| `CFG_USE_MODS=0` 支持 | 关闭固件内部的 shift/ctrl/alt/capslock 状态机，固件发送原始 scancode，修饰键不再改变按键输出 |
| `CFG_REPORT_MODS` | 修饰键（L-shift/R-shift/Alt/Ctrl）以独立 scancode 上报（0xA1-0xA5），驱动自行处理 |
| `REG_ID_LED` (0x0F) | 驱动可直接控制绿色 LED（PC13）和橙色充电 LED，固件不再自动管理（当 `LED_DRIVER_CTRL` 置位时） |
| `REG_ID_OFF` (0x0E) | 软关机寄存器，写入后 MCU 关闭背光和 SoC 供电，延迟 6 秒后执行 `PMU.shutdown()` 彻底断电 |
| `BIOSVERSION` 版本检测 | 驱动通过 `REG_ID_VER` 读取版本号，`>= 0x20` 时启用新特性 |
| I2C 总线看门狗 | 检测 I2C 总线死锁（SDA 被拉低），自动发送时钟脉冲恢复 |

### 5.2 寄存器定义

| 寄存器 | 地址 | 说明 |
|--------|------|------|
| REG_ID_VER | 0x01 | 固件版本（当前 0x20） |
| REG_ID_CFG | 0x02 | 配置寄存器（bit7=CFG_USE_MODS, bit6=CFG_REPORT_MODS） |
| REG_ID_KEY | 0x04 | 按键状态 |
| REG_ID_BKL | 0x05 | 屏幕背光亮度（0-16） |
| REG_ID_BK2 | 0x0A | 键盘背光亮度（0-32） |
| REG_ID_OFF | 0x0E | 软关机（写入即触发） |
| REG_ID_LED | 0x0F | LED 控制（bit7=driver接管, bit0=绿色LED, bit3:1=橙色LED模式） |

### 5.3 固件文件

| 文件 | 说明 |
|------|------|
| `picocalc_keyboard_stm/` | 固件源码（Arduino/STM32duino） |
| `picocalc_keyboard_v2.0.bin` | 编译好的固件（40948 bytes） |

---

## 6. 使用说明

### 6.1 编译键盘固件

需要 [STM32duino](https://github.com/stm32duino/Arduino_Core_STM32) 2.10.0+，参考 [PicoCalc 编译指南](https://github.com/clockworkpi/PicoCalc/wiki/Setting-Up-Arduino-Development-for-PicoCalc-keyboard)。

- Board: **Generic F103R8Tx**
- Upload method: **STM32CubeProgrammer (Serial)**

编译产物为 `picocalc_keyboard_v2.0.bin`。

### 6.2 刷写固件

PicoCalc 拆机后通过 USB-TTL 连接 MCU 的 UART 引脚，使用 `stm32flash`：

```bash
stm32flash -w picocalc_keyboard_v2.0.bin -v -g 0x08000000 /dev/ttyUSB0
```

### 6.3 编译驱动

需要 PicoCalc Linux 内核源码树：

```bash
cd modules-drivers/picocalc_keyboard_stm_mod
make
```

产物为 `picocalc_kbd.ko`。

### 6.4 部署驱动

```bash
# 临时加载
scp picocalc_kbd.ko lyra@<device>:/tmp/
ssh lyra@<device> "sudo rmmod picocalc_kbd; sudo insmod /tmp/picocalc_kbd.ko"

# 持久化（重启后自动加载）
ssh lyra@<device> "sudo cp /tmp/picocalc_kbd.ko /lib/modules/\$(uname -r)/kernel/drivers/picocalc/ && sudo depmod -a"
```

### 6.5 调试

驱动内置 debug 日志，通过 `debug_levels.h` 控制：

```c
#define DEBUG_LEVEL (DEBUG_LEVEL_FE | DEBUG_LEVEL_RW | DEBUG_LEVEL_LD)
```

查看日志：`dmesg | grep picocalc`
