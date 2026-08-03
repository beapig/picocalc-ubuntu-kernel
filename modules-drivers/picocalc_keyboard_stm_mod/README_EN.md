# PicoCalc Keyboard Driver

## Overview

This driver works with custom firmware v2.0 (BIOSVERSION 0x20). The firmware is configured with `CFG_USE_MODS=0` (disables the firmware's internal shift/ctrl/alt/capslock state machine, sending raw scancodes) and `CFG_REPORT_MODS=1` (modifiers reported as independent scancodes).

### Design Philosophy

R-shift is treated as a special **Fn key** for triggering F1-F10, Home/End, PageUp/PageDown and other editing keys, while also serving as a shift modifier for normal keys. L-shift is passed through directly. This enables `L-shift + R-shift + F1 = Shift + F6`.

### Three-Layer Architecture

| Layer | Responsibility |
|-------|---------------|
| Layer 1: Normalization | Firmware scancode → `enum phys_key` (stable physical key ID) |
| Layer 2: State machine | R-shift/Fn state machine, determines `emit_mode` per key |
| Layer 3: Input reporting | `input_report_key/rel` + `input_sync` |

---

## 1. R-shift State Machine

### 1.1 Core Principle

**R-shift down/up events are never directly reported to the Linux input subsystem.** The driver maintains an internal `RSH` flag and only supplements shift events when a specific key is pressed, based on that key's classification.

```
R-shift down/up → only updates internal RSH flag, produces no input events
```

This is the core improvement over the old driver: the old driver passed R-shift through to Linux, requiring toggle shift up/down when triggering Fn, causing key sticking and timing issues. The new approach eliminates all toggle/sync problems.

### 1.2 Key Groups

All keys are divided into two groups when R-shift is held:

**Fn group**: R-shift triggers keycode replacement (e.g., 1→F1, A→Home, ↑→PageUp).
- down/up uses the Fn keycode directly, no shift.
- Example: `R-shift + 1` → `KEY_F1` (not `!`)

**Normal group**: R-shift produces a shift effect (e.g., 1→!).
- On down: if `RSH=true` and `rshift_reported=false`, supplement `KEY_RIGHTSHIFT down`, then emit base keycode.
- On up: emit base keycode up, then conditionally supplement `KEY_RIGHTSHIFT up` based on RSH state.

### 1.3 Release Tracking (Strategy 2)

Each physical key locks its `emit_mode` at key-down time; release pairs with the recorded mode:

```c
struct {
    u8  emit_mode;   /* EMIT_FN / EMIT_SHIFTED / EMIT_NORMAL / EMIT_MOUSE */
    u16 keycode;     /* actual keycode reported (0 = not emitted) */
} key_state[256];    /* index = phys_key */
```

**R-shift released early while a base key is held does not affect that key's behavior.** The emit_mode locked at down time remains valid until the key is released.

### 1.4 Event Flow Examples

#### Fn Group (1→F1)

```
R-shift↓  →  RSH=true (not reported)
1↓        →  emit_mode=EMIT_FN, KEY_F1=1
1↑        →  KEY_F1=0          ← lookup key_state, emit F1 up
R-shift↑  →  RSH=false (not reported)
```

#### Normal Group (1→!)

```
R-shift↓  →  RSH=true (not reported)
1↓        →  emit_mode=EMIT_SHIFTED
             KEY_RIGHTSHIFT=1 (supplemented), KEY_1=1  → outputs '!'
1↑        →  KEY_1=0
             RSH=true → don't supplement KEY_RIGHTSHIFT up
R-shift↑  →  RSH=false
             KEY_RIGHTSHIFT=0 (supplement up)
```

#### Normal Group — R-shift Released Early

```
R-shift↓  →  RSH=true
1↓        →  EMIT_SHIFTED, KEY_RIGHTSHIFT=1, KEY_1=1
R-shift↑  →  RSH=false, KEY_RIGHTSHIFT=0 (supplement up)
1↑        →  KEY_1=0          ← RSH=false, don't re-send shift up
```

#### Normal Group — No R-shift at Key Down

```
1↓        →  emit_mode=EMIT_NORMAL, KEY_1=1  → outputs '1'
1↑        →  KEY_1=0
```

### 1.5 Known Limitation

**R-shift must be pressed before the base key to take effect.** Pressing a base key first and then pressing R-shift will not change the held key's repeat behavior (since R-shift down is not reported, Linux's shift state is unchanged).

This is an inherent limitation of the "R-shift down not reported" approach, traded for a clean state machine with zero toggle and zero timing issues.

### 1.6 L-shift Pass-through

L-shift is passed directly to Linux:

```
L-shift↓  →  KEY_LEFTSHIFT=1
L-shift↑  →  KEY_LEFTSHIFT=0
```

L-shift and R-shift supplementation do not interfere (Linux input subsystem treats both shifts as independent bits; the combined result is still shift). The normal group's supplement logic does not need to check L-shift state.

---

## 2. Fn Key Mapping

### 2.1 Classification

When R-shift is held, each physical key falls into one of three categories:

| Category | Behavior | emit_mode |
|----------|----------|-----------|
| **Fn-keycode** | Replaced with Fn keycode (e.g., 1→F1), no shift | `EMIT_FN` |
| **Fn-action** | Triggers special action (backlight), no input reported | `EMIT_FN` (action flag) |
| **Normal** | Shift effect (supplement RSHIFT) | `EMIT_SHIFTED` / `EMIT_NORMAL` |

### 2.2 Four-State Matrix

> R-shift acts as Fn layer trigger; L-shift provides shift pass-through, overlaid on the current layer. State is locked at key-down.

| Key              | No Shift  | L-shift only (pass-through) | R-shift only (Fn layer) | L+R shift (Fn + shift) |
| ---------------- | --------- | --------------------------- | ----------------------- | ---------------------- |
| F1 / F6          | F1        | Shift+F1                    | F6                      | Shift+F6               |
| F2 / F7          | F2        | Shift+F2                    | F7                      | Shift+F7               |
| F3 / F8          | F3        | Shift+F3                    | F8                      | Shift+F8               |
| F4 / F9          | F4        | Shift+F4                    | F9                      | Shift+F9               |
| F5 / F10         | F5        | Shift+F5                    | F10                     | Shift+F10              |
| ESC / PauseBreak | ESC       | Shift+ESC                   | PauseBreak              | Shift+PauseBreak       |
| Tab / Home       | Tab       | Shift+Tab                   | Home                    | Shift+Home             |
| Capslock         | Capslock  | Shift+Capslock              | Toggle Mouse Mode       | Toggle Mouse Mode      |
| Del / End        | Del       | Shift+Del                   | End                     | Shift+End              |
| Backspace        | Backspace | Shift+Backspace             | F11                     | Shift+F11              |
| ↑                | ↑         | Shift+↑                     | PageUp                  | Shift+PageUp           |
| ↓                | ↓         | Shift+↓                     | PageDown                | Shift+PageDown         |
| ←                | ←         | Shift+←                     | Home                    | Shift+Home             |
| →                | →         | Shift+→                     | End                     | Shift+End              |
| `i` / `I`        | i         | I                           | Insert                  | Shift+Insert           |
| `,` / `<`        | `,`       | `<`                         | Screen Backlight -      | —                      |
| `.` / `>`        | `.`       | `>`                         | Screen Backlight +      | —                      |
| Space            | Space     | Shift+Space                 | Cycle Keyboard Backlight| —                      |

### 2.3 Non-Mouse Mode Fn-keycode Mapping

| Key       | scancode | Base keycode  | Fn keycode             |
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
| Capslock  | 0x1A     | KEY_CAPSLOCK  | **Special: Toggle Mouse Mode** |

### 2.4 Fn-action Mapping (no input reported, I2C register write)

| Key | scancode | Action                   |
| --- | -------- | ------------------------ |
| ,   | 0x2C     | Screen backlight -       |
| .   | 0x2E     | Screen backlight +       |
| Space | 0x20   | Keyboard backlight cycle |

### 2.5 Mouse Mode Fn Table

In mouse mode, arrow keys and `[]` have mouse functions as their **base behavior**. R-shift restores them to normal keys (Fn behavior). Fn lookup in mouse mode checks the mouse-specific table first, then falls back to the common Fn table (F1→F6 etc. still work).

#### Mouse Mode Base Functions (no R-shift)

| Key | Base function (mouse) |
| --- | --------------------- |
| ↑   | REL_Y = -1 (mouse up) |
| ↓   | REL_Y = +1 (mouse down) |
| ←   | REL_X = -1 (mouse left) |
| →   | REL_X = +1 (mouse right) |
| [   | BTN_LEFT (left click)  |
| ]   | BTN_RIGHT (right click) |

#### Mouse Mode Fn-keycode Mapping (R-shift held)

| Key | scancode | Fn keycode     | Description           |
| --- | -------- | -------------- | --------------------- |
| ↑   | 0xB5     | KEY_UP         | Restore arrow key     |
| ↓   | 0xB6     | KEY_DOWN       | Restore arrow key     |
| ←   | 0xB4     | KEY_LEFT       | Restore arrow key     |
| →   | 0xB7     | KEY_RIGHT      | Restore arrow key     |
| [   | 0x5B     | KEY_LEFTBRACE  | Restore `[`           |
| ]   | 0x5D     | KEY_RIGHTBRACE | Restore `]`           |
| `   | 0x60     | KEY_PAGEUP     | Mouse-mode exclusive  |
| /   | 0x2F     | KEY_PAGEDOWN   | Mouse-mode exclusive  |

### 2.6 Keys Not in Fn Tables

Physical keys not in any Fn table (letters A-H, J-N, P-U, W-Z, digits 0-9, symbols `-=\;'` etc.) are classified as **normal group** when R-shift is held, producing shift effect.

---

## 3. Mouse Mode

### 3.1 Mouse Mode Toggle

`R-shift + Capslock` toggles mouse mode. Triggered on down event; Capslock is intercepted (no scancode reported). A keyboard backlight flash effect is added on toggle (max brightness for 0.5s then restore, re-entrant timer).

State cleanup on toggle:
- Cancel mouse movement timer
- Clear `mouse_dir`
- Release all swallowed Fn modifier states

### 3.2 Mouse Movement: Kernel Timer

Does not depend on firmware HOLD events. Uses `delayed_work` for mouse movement (100Hz / 10ms interval).

**Three-phase motion model:**

| Phase | Duration | Behavior |
|-------|----------|----------|
| Initial jump | At down instant | Immediate displacement of `mouse_initial_step` px (default 8px) |
| Delay period | `mouse_start_delay_ms` (default 80ms) | No movement |
| Acceleration | Continuous after delay | Linear ramp from `mouse_base_speed` (80px/s) to `mouse_max_speed` (320px/s) over `mouse_accel_time_ms` (400ms) |

**Fixed-point arithmetic:** Speed in px/s, per-tick (10ms) increment accumulated with 8-bit fixed-point (1px = 256), ensuring sub-pixel precision.

### 3.3 sysfs Tunable Parameters

| Parameter | Default | Path |
|-----------|---------|------|
| Initial step | 8 px | `/sys/firmware/picocalc/mouse_initial_step` |
| Base speed | 80 px/s | `/sys/firmware/picocalc/mouse_base_speed` |
| Max speed | 320 px/s | `/sys/firmware/picocalc/mouse_max_speed` |
| Start delay | 80 ms | `/sys/firmware/picocalc/mouse_start_delay_ms` |
| Acceleration time | 400 ms | `/sys/firmware/picocalc/mouse_accel_time_ms` |

### 3.4 Firmware HOLD Event Handling

All firmware HOLD events are **swallowed**. Mouse movement is driven by the kernel timer; normal key repeat is handled by Linux autorepeat.

### 3.5 emit_mode Locking in Mouse Mode

Per Strategy 2, emit_mode is locked at key-down and paired at release:

| Scenario | Event flow |
|----------|------------|
| Arrow key pressed first (mouse moving), then R-shift | emit_mode=EMIT_MOUSE, R-shift has no effect, timer continues |
| R-shift first, arrow key down (KEY_UP), R-shift released | emit_mode=EMIT_FN, KEY_UP held until arrow key up |

### 3.6 Mouse Buttons (BTN_LEFT/RIGHT)

`[` `]` in mouse mode without R-shift act as mouse left/right buttons. Firmware sends only down/up (no HOLD), directly reported as `BTN_LEFT`/`BTN_RIGHT`.

---

## 4. Notes

- Duplicate Home/End mapping is intentional: arrow keys (←/→) trigger Home/End in keyboard mode (physical layout resembles a gamepad with arrow keys at top-left); mouse mode uses standard Tab/Del Fn layer for Home/End access.

- Mouse mode is an overlay (boolean). All non-mouse-mode rules remain active in mouse mode, overridden only when conflicting with mouse-specific keys.

- Mouse mode toggle is based on Capslock interception: whenever R-shift is held, Capslock's down event is intercepted as a toggle event and not reported to Linux as Capslock.

- Fn-actions (backlight adjustment) trigger on down (once per down event, no action on release), and the original key is intercepted.

- Soft poweroff: driver registers `reboot_notifier` + `i2c_driver.shutdown` callback. On `poweroff`, sends `REG_ID_OFF` to MCU during late shutdown phase; MCU cuts SoC power and delays full power-off.

---

## 5. Firmware Changes (v2.0)

Modified from PicoCalc factory firmware v1.6 (`0x16`), version bumped to `0x20`.

### 5.1 New Features

| Feature | Description |
|---------|-------------|
| `REG_ID_CFG` read/write | Driver can read/write CFG register at runtime via I2C |
| `CFG_USE_MODS=0` support | Disables firmware's internal shift/ctrl/alt/capslock state machine; raw scancodes sent, modifiers no longer affect key output |
| `CFG_REPORT_MODS` | Modifiers (L-shift/R-shift/Alt/Ctrl) reported as independent scancodes (0xA1-0xA5) |
| `REG_ID_LED` (0x0F) | Driver can directly control green LED (PC13) and orange charge LED; firmware stops auto-management when `LED_DRIVER_CTRL` is set |
| `REG_ID_OFF` (0x0E) | Soft poweroff register; writing triggers MCU to turn off backlight and SoC power, then execute `PMU.shutdown()` after 6s delay |
| `BIOSVERSION` detection | Driver reads version via `REG_ID_VER`; enables new features when `>= 0x20` |
| I2C bus watchdog | Detects I2C bus deadlock (SDA held low), auto-sends clock pulses to recover |

### 5.2 Register Definitions

| Register | Address | Description |
|----------|---------|-------------|
| REG_ID_VER | 0x01 | Firmware version (current 0x20) |
| REG_ID_CFG | 0x02 | Config register (bit7=CFG_USE_MODS, bit6=CFG_REPORT_MODS) |
| REG_ID_KEY | 0x04 | Key status |
| REG_ID_BKL | 0x05 | Screen backlight brightness (0-16) |
| REG_ID_BK2 | 0x0A | Keyboard backlight brightness (0-32) |
| REG_ID_OFF | 0x0E | Soft poweroff (write to trigger) |
| REG_ID_LED | 0x0F | LED control (bit7=driver takeover, bit0=green LED, bit3:1=orange LED mode) |

### 5.3 Firmware Files

| File | Description |
|------|-------------|
| `picocalc_keyboard_stm/` | Firmware source (Arduino/STM32duino) |
| `picocalc_keyboard_v2.0.bin` | Compiled firmware (40948 bytes) |

---

## 6. Usage

### 6.1 Compile Keyboard Firmware

Requires [STM32duino](https://github.com/stm32duino/Arduino_Core_STM32) 2.10.0+. See [PicoCalc build guide](https://github.com/clockworkpi/PicoCalc/wiki/Setting-Up-Arduino-Development-for-PicoCalc-keyboard).

- Board: **Generic F103R8Tx**
- Upload method: **STM32CubeProgrammer (Serial)**

Output: `picocalc_keyboard_v2.0.bin`.

### 6.2 Flash Firmware

Disassemble PicoCalc, connect USB-TTL to MCU UART pins, use `stm32flash`:

```bash
stm32flash -w picocalc_keyboard_v2.0.bin -v -g 0x08000000 /dev/ttyUSB0
```

### 6.3 Compile Driver

Requires PicoCalc Linux kernel source tree:

```bash
cd modules-drivers/picocalc_keyboard_stm_mod
make
```

Output: `picocalc_kbd.ko`.

### 6.4 Deploy Driver

```bash
# Temporary load
scp picocalc_kbd.ko lyra@<device>:/tmp/
ssh lyra@<device> "sudo rmmod picocalc_kbd; sudo insmod /tmp/picocalc_kbd.ko"

# Persistent (auto-load after reboot)
ssh lyra@<device> "sudo cp /tmp/picocalc_kbd.ko /lib/modules/\$(uname -r)/kernel/drivers/picocalc/ && sudo depmod -a"
```

### 6.5 Debug

Built-in debug logging controlled via `debug_levels.h`:

```c
#define DEBUG_LEVEL (DEBUG_LEVEL_FE | DEBUG_LEVEL_RW | DEBUG_LEVEL_LD)
```

View logs: `dmesg | grep picocalc`
