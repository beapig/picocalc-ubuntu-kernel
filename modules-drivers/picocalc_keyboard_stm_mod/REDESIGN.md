# PicoCalc 键盘驱动重设计方案

## 1. R-shift 状态机设计

### 1.1 核心原则

**R-shift 的 down/up 事件从不直接上报给 Linux input 子系统。** 驱动内部维护 `RSH` 状态，仅在具体按键 down 时根据该键的性质决定是否补发 shift 事件。

```
R-shift down/up → 仅更新内部 RSH 标志，不产生任何 input 事件
```

### 1.2 按键分组

所有按键在 R-shift 按住时分为两组：

**Fn 组**：R-shift 按住时触发替换 keycode（如 1→F1, A→Home, ↑→PageUp）。
- 这些键的 down/up 直接使用 Fn keycode，不带 shift。
- 例子：`R-shift + 1` → `KEY_F1`（不是 `!`）

**普通组**：R-shift 按住时表现为 shift 效果（如 1→!）。
- down 时：若 `RSH=true` 且无其他 shift 透传中，补发 `KEY_RIGHTSHIFT down`，然后发基础 keycode。
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
| **Fn-action**  | 触发特殊操作（背光调节），不上报 input   | `EMIT_ACTION`                  |
| **普通**       | 表现为 shift 效果（补发 RSHIFT）         | `EMIT_SHIFTED` / `EMIT_NORMAL` |

### 2.2 非鼠标模式 Fn 表

#### Fn-keycode 映射

| 物理键    | scancode | 基础 keycode  | Fn keycode                                                     |
| --------- | -------- | ------------- | -------------------------------------------------------------- |
| F1        | 0x81     | KEY_F1        | KEY_F6                                                         |
| F2        | 0x82     | KEY_F2        | KEY_F7                                                         |
| F3        | 0x83     | KEY_F3        | KEY_F8                                                         |
| F4        | 0x84     | KEY_F4        | KEY_F9                                                         |
| F5        | 0x85     | KEY_F5        | KEY_F10                                                        |
| ESC       | 0xB1     | KEY_ESC       | KEY_PAUSE                                                      |
| Tab       | 0x09     | KEY_TAB       | KEY_HOME                                                       |
| Delete    | 0xD4     | KEY_DELETE    | KEY_END                                                        |
| Backspace | 0x08     | KEY_BACKSPACE | KEY_F11                                                        |
| ↑         | 0xB5     | KEY_UP        | KEY_PAGEUP                                                     |
| ↓         | 0xB6     | KEY_DOWN      | KEY_PAGEDOWN                                                   |
| ←         | 0xB4     | KEY_LEFT      | KEY_HOME                                                       |
| →         | 0xB7     | KEY_RIGHT     | KEY_END                                                        |
| I         | 0x49     | KEY_I         | KEY_INSERT                                                     |
| Capslock  | 0x1A     | KEY_CAPSLOCK  | **特殊功能：切换鼠标模式（down就触发，up时不会上报scancode）** |

#### Fn-action 映射（不上报 input，I2C 写寄存器）

| 物理键 | scancode | 动作         |
| ------ | -------- | ------------ |
| ,      | 0x2C     | 屏幕背光 -   |
| .      | 0x2E     | 屏幕背光 +   |
| Space  | 0x20     | 键盘背光循环 |

### 2.3 鼠标模式 Fn 表

鼠标模式下，方向键和 `[]` 的**基础功能**变为鼠标操作。R-shift 按住时恢复为正常按键（Fn 行为）。

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

### 2.4 非 Fn 表中的按键

不在上述任何 Fn 表中的物理键（字母 A-H, J-N, P-U, W-Z, 数字 0-9, 符号 `-=[]\;'` 等），R-shift 按住时归为**普通组**，表现为 shift 效果。

---

## 3. 鼠标模式

### 3.1 鼠标模式进出

`R-shift + Capslock` 切换鼠标模式。进出时需清理状态：
- 取消鼠标移动 timer
- 清零 `mouse_dir`
- 释放所有被吞掉的 Fn 修饰状态

### 3.2 鼠标移动：内核 timer 方案

不依赖固件 HOLD 事件，改用 `delayed_work` 定时驱动鼠标移动：

```c
struct mouse_state {
    u8   dir;            /* MOUSE_DIR_UP|DOWN|LEFT|RIGHT 组合 */
    struct delayed_work dw;
    u32  interval_ms;    /* 当前间隔，加速度递减 */
};
```

**方向键 down（鼠标模式，无 R-shift）**：
1. `mouse_dir |= dir`
2. 立即发一帧 `REL_X`/`REL_Y`
3. 重置 `interval_ms` 为初始值（如 100ms）
4. 启动 `delayed_work`

**timer 回调**：
1. 发一帧 `REL_X`/`REL_Y`
2. 加速度：`interval_ms = max(interval_ms - 10, 20)`
3. 重新调度 `delayed_work`

**方向键 up**：
1. `mouse_dir &= ~dir`
2. 若 `mouse_dir == 0`，`cancel_delayed_work()`

### 3.3 固件 HOLD 事件处理

所有固件 HOLD 事件**一律吞掉**，不做任何处理。鼠标移动由内核 timer 驱动，普通按键由 Linux autorepeat 驱动。

```c
if (state == KEY_STATE_HOLD)
    return; /* 吞掉所有 HOLD */
```

### 3.4 鼠标模式下的 emit_mode 锁定

按策略 2，emit_mode 在 down 时锁定，HOLD 和 up 都查它：

| 场景 | 事件流 |
|------|--------|
| 方向键先按（鼠标移动），再按 R-shift | emit_mode=EMIT_MOUSE，R-shift 无影响，timer 继续移动 |
| R-shift 先按，方向键 down（KEY_UP），R-shift 松开 | emit_mode=EMIT_FN，KEY_UP 保持，直到方向键 up |

### 3.5 鼠标按键（BTN_LEFT/RIGHT）

`[` `]` 在鼠标模式下无 R-shift 时为鼠标左右键。固件只发 down/up（无 HOLD），直接上报 `BTN_LEFT`/`BTN_RIGHT`。
