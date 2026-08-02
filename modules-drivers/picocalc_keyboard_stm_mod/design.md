按键改进设计说明书

1，通过 CFG_USE_MODS 关掉键盘固件的原始 shift/ctrl/alt/capslock/numlock等状态机
2，对应的状态机由驱动独立实现（最重要的机制是将 l-shift 和 r-shift 拆分）
3，重新设计后的按键设计：

设计思想：将 rshift 视为特殊的 “fn键”。用于触发 F1~F5 这些编辑键的双键机制。lshift 维持透传。从而实现 lshift+rshift+F1=shift+F5 的效果。

- 保留鼠标模式的状态机。由 rshift + capslock 触发，down 事件就触发切换，如果触发切换，拦截capslock事件，而且追加一个“切换时闪烁一下键盘背光”的功能（键盘亮度调到最高维持0.5s后恢复原来的亮度）

# 鼠标模式（鼠标模式是一个“overlay”的逻辑，所以鼠标模式是bool值）

鼠标模式下特殊处理的按键（优先级更高）

上下左右=移动鼠标指针;
rshift+上下左右=上下左右；
"["=鼠标左键;
"]"=鼠标右键;
rshift+"~"=pageup；
rshift+"/"=pagedown；

鼠标指针的移动需求：

1，上下左右按键 down 的瞬间就会触发一个“初始的位移”，后面持续按住触发的位移为独立事件；
2，我想鼠标的速度随按键按住时间增加而增加，但是不会超过最大速度。
3，鼠标的移动速度参数暴露成可访问的参数（最大位移，初始位移，加速度），用户可以通过 linux 的 sysfs 接口直接读取与调整。


# 非鼠标模式下的按键映射

按键映射矩阵（4 状态状态机，横轴 = L/R shift 组合状态，纵轴 = 物理按键）

> 状态机说明：R-shift 视为 Fn 层触发键，负责切换到“上排”功能（F6~F10、Home/End/PauseBreak 等）；L-shift 维持 shift 透传，叠加在当前层之上。四个状态互斥判定（按下瞬间锁定），优先级高于鼠标模式之外的其它修饰键。

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
| ---------------- | --------  | ------------------------ | ------------------- | ------------------------------- |
| `,` / `<`        | `,`       | `<`                      | 屏幕背光-           | —                               |
| `.` / `>`        | `.`       | `>`                      | 屏幕背光+           | —                               |
| Space            | Space     | Shift+Space              | 循环5档调节键盘背光 | —                               |
| `i` / `I`        | i         | I                        | Insert              | Shift+Insert                    |

补充说明：

- R-shift 不是完全当作 Fn 键，而是兼作为 shift 修饰键。因此按下 R-shift 时会向 Linux 发送 R-shift down 事件。当发生 Fn 按键事件时再前面补发送一个 R-shift up 事件。直到 Fn 按键 release。
- 由于存在 Fn 按键未释放时提前释放 R-shift 的情况，这种状况要保持 Fn 按键的hold状态，由于触发 Fn down 的时候已经发送了 R-shift up 事件，R-shift 释放时不用再重复发送，Fn 键的基准键释放时会增加检查是否有对应的 Fn 键未释放以及 R-shift 是否按下，如果有，发送的是 fn 键的 up 事件。同时重新发送 R-shift down 事件。

- 屏幕背光加减，键盘背光切换 都是 Down 时触发（每次 down 触发一次，release 无任何操作），而且要拦截原始按键。

- 鼠标模式切换闪烁（背光最高亮度维持 0.5s 后恢复）的计时是可重入的：若 0.5s 内再次触发切换，打断当前闪烁并重新计时。

- 鼠标模式是一个 overlay（bool 值），非鼠标模式下的所有规则在鼠标模式下继续生效，仅与鼠标专属按键冲突时才被覆盖。

- Home/End 的重复映射是有意为之：键盘模式下用方向键（←/→）触发 Home/End 更方便（本机物理布局类似手柄，方向键在左上角）；鼠标模式下则用标准 Tab/Del 的 Fn 层访问 Home/End。

- 切换鼠标模式的判定基于 Capslock 拦截：只要 R-shift 处于按下态，Capslock 的 down 事件即被拦截为切换事件，不会再向 Linux 上报 Capslock。此时若 L-shift 也按下，由于 L-shift 是透传的，Linux 实际收到的是 lshift down，无 Capslock 事件。

- 总结论，除了 R-shift 的情况要特殊处理其他按键都直接透传由 Linux 处理。