// SPDX-License-Identifier: GPL-2.0-only
/*
** Keyboard Driver for PicoCalc
** Complete rewrite: three-layer architecture (normalization → state machine → input).
** R-shift is never reported to Linux; it's purely a driver-internal Fn modifier.
**
** Design doc: modules-drivers/picocalc_keyboard/REDESIGN.md
*/

#include <linux/version.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include "picocalc_kbd_code.h"
#include "debug_levels.h"

/* ================================================================ */
/* Register definitions (from firmware reg.h)                       */
/* ================================================================ */

#define REG_ID_VER   0x01
#define REG_ID_CFG   0x02
#define REG_ID_INT   0x03
#define REG_ID_KEY   0x04
#define REG_ID_BKL   0x05  /* screen backlight */
#define REG_ID_FIF   0x09  /* key FIFO */
#define REG_ID_BK2   0x0A  /* keyboard backlight */
#define REG_ID_BAT   0x0B
#define REG_ID_OFF   0x0E  /* power off (delay in seconds, min 6) */
#define REG_ID_LED   0x0F  /* LED control */

#define CFG_KEY_INT      (1 << 4)
#define CFG_REPORT_MODS  (1 << 6)
#define CFG_USE_MODS     (1 << 7)

#define PICOCALC_WRITE_MASK (1 << 7)

/* REG_ID_LED bit definitions */
#define LED_DRIVER_CTRL  (1 << 7)
#define LED_ORANGE_SHIFT 1
#define LED_ORANGE_MASK  (0x7 << LED_ORANGE_SHIFT)
#define LED_GREEN_MASK   (1 << 0)
#define LED_ORANGE_OFF    (0 << LED_ORANGE_SHIFT)
#define LED_ORANGE_ON     (1 << LED_ORANGE_SHIFT)
#define LED_ORANGE_BLINK1 (2 << LED_ORANGE_SHIFT)
#define LED_ORANGE_BLINK4 (3 << LED_ORANGE_SHIFT)
#define LED_ORANGE_AUTO   (4 << LED_ORANGE_SHIFT)

/* ================================================================ */
/* Firmware scancode constants                                      */
/* ================================================================ */

#define SCAN_LALT    0xA1
#define SCAN_LSHIFT  0xA2
#define SCAN_RSHIFT  0xA3
#define SCAN_LCTRL   0xA5

/* ================================================================ */
/* Mouse movement direction flags                                   */
/* ================================================================ */

#define MOUSE_DIR_LEFT  (1 << 0)
#define MOUSE_DIR_RIGHT (1 << 1)
#define MOUSE_DIR_UP    (1 << 2)
#define MOUSE_DIR_DOWN  (1 << 3)

/* ================================================================ */
/* Constants                                                        */
/* ================================================================ */

#define KBD_FIFO_SIZE       31
#define KBD_BUS_TYPE        BUS_I2C
#define KBD_VENDOR_ID       0x0001
#define KBD_PRODUCT_ID      0x0001
#define KBD_VERSION_ID      0x0001

/* Backlight flash duration on mouse-mode toggle (ms) */
#define BL_FLASH_MS         500
/* Keyboard backlight max value for flash */
#define KBD_BL_FLASH_VAL    240

/* Screen backlight step (matches firmware LCD_BACKLIGHT_STEP) */
#define SCREEN_BL_STEP      16
#define SCREEN_BL_MIN       16
#define SCREEN_BL_MAX       240

/* Keyboard backlight cycle levels (4 levels: off/low/med/high) */
#define KBD_BL_LEVELS       4
static const uint8_t kbd_bl_levels[KBD_BL_LEVELS] = { 0, 80, 140, 200 };

/* Mouse timer: fixed interval for smooth movement */
#define MOUSE_TIMER_INTERVAL_MS  10   /* 100Hz update rate */

/* ================================================================ */
/* Firmware key state enum (mirrors firmware)                       */
/* ================================================================ */

enum pico_key_state {
	KEY_STATE_IDLE     = 0,
	KEY_STATE_PRESSED  = 1,
	KEY_STATE_HOLD     = 2,
	KEY_STATE_RELEASED = 3,
};

struct key_fifo_item {
	uint8_t _ : 4;
	enum pico_key_state state : 4;
	uint8_t scancode;
};

/* ================================================================ */
/* Layer 1: Normalization — physical key IDs                        */
/* ================================================================ */

enum phys_key {
	PK_NONE = 0,
	/* Letters (firmware sends uppercase) */
	PK_A, PK_B, PK_C, PK_D, PK_E, PK_F, PK_G, PK_H, PK_I, PK_J,
	PK_K, PK_L, PK_M, PK_N, PK_O, PK_P, PK_Q, PK_R, PK_S, PK_T,
	PK_U, PK_V, PK_W, PK_X, PK_Y, PK_Z,
	/* Digits */
	PK_0, PK_1, PK_2, PK_3, PK_4, PK_5, PK_6, PK_7, PK_8, PK_9,
	/* Symbols */
	PK_SPACE, PK_ENTER, PK_TAB, PK_ESC, PK_BACKSPACE,
	PK_DOT, PK_COMMA, PK_SEMICOLON, PK_APOSTROPHE,
	PK_MINUS, PK_EQUAL, PK_SLASH, PK_BACKSLASH,
	PK_LBRACKET, PK_RBRACKET, PK_GRAVE,
	/* Function keys */
	PK_F1, PK_F2, PK_F3, PK_F4, PK_F5, PK_F6, PK_F7, PK_F8, PK_F9, PK_F10,
	/* Navigation */
	PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT,
	PK_HOME, PK_END, PK_INSERT, PK_DELETE,
	PK_PAGEUP, PK_PAGEDOWN, PK_PAUSE,
	PK_CAPSLOCK,
	/* Modifiers */
	PK_LSHIFT, PK_RSHIFT, PK_LALT, PK_LCTRL,
	/* Special */
	PK_POWER,
	PK_MAX,
};

/* scancode → phys_key mapping table */
static const u8 scancode_to_phys[256] = {
	[0x08] = PK_BACKSPACE,
	[0x09] = PK_TAB,
	[0x0A] = PK_ENTER,
	[0x20] = PK_SPACE,
	/* Digits */
	['0'] = PK_0, ['1'] = PK_1, ['2'] = PK_2, ['3'] = PK_3,
	['4'] = PK_4, ['5'] = PK_5, ['6'] = PK_6, ['7'] = PK_7,
	['8'] = PK_8, ['9'] = PK_9,
	/* Letters (firmware always sends uppercase with CFG_USE_MODS=0) */
	['A'] = PK_A, ['B'] = PK_B, ['C'] = PK_C, ['D'] = PK_D,
	['E'] = PK_E, ['F'] = PK_F, ['G'] = PK_G, ['H'] = PK_H,
	['I'] = PK_I, ['J'] = PK_J, ['K'] = PK_K, ['L'] = PK_L,
	['M'] = PK_M, ['N'] = PK_N, ['O'] = PK_O, ['P'] = PK_P,
	['Q'] = PK_Q, ['R'] = PK_R, ['S'] = PK_S, ['T'] = PK_T,
	['U'] = PK_U, ['V'] = PK_V, ['W'] = PK_W, ['X'] = PK_X,
	['Y'] = PK_Y, ['Z'] = PK_Z,
	/* Symbols */
	['='] = PK_EQUAL, ['-'] = PK_MINUS,
	['['] = PK_LBRACKET, [']'] = PK_RBRACKET,
	[';'] = PK_SEMICOLON, ['\''] = PK_APOSTROPHE,
	[','] = PK_COMMA, ['.'] = PK_DOT, ['/'] = PK_SLASH,
	['`'] = PK_GRAVE, ['\\'] = PK_BACKSLASH,
	/* Function keys */
	[0x81] = PK_F1, [0x82] = PK_F2, [0x83] = PK_F3, [0x84] = PK_F4,
	[0x85] = PK_F5, [0x86] = PK_F6, [0x87] = PK_F7, [0x88] = PK_F8,
	[0x89] = PK_F9, [0x90] = PK_F10,
	/* Navigation */
	[0xB1] = PK_ESC, [0xC1] = PK_CAPSLOCK,
	[0xB5] = PK_UP, [0xB6] = PK_DOWN,
	[0xB4] = PK_LEFT, [0xB7] = PK_RIGHT,
	[0xD1] = PK_INSERT, [0xD2] = PK_HOME,
	[0xD4] = PK_DELETE, [0xD5] = PK_END,
	[0xD0] = PK_PAUSE,
	/* Modifiers */
	[0xA1] = PK_LALT, [0xA2] = PK_LSHIFT,
	[0xA3] = PK_RSHIFT, [0xA5] = PK_LCTRL,
	/* Power */
	[0x91] = PK_POWER,
};

/* ================================================================ */
/* Layer 2: Fn-layer mapping tables                                  */
/* ================================================================ */

/* Emit mode — locked at key-down, used for release pairing */
enum emit_mode {
	EMIT_NONE    = 0,
	EMIT_NORMAL  = 1,  /* base keycode, no shift needed */
	EMIT_SHIFTED = 2,  /* base keycode + RSHIFT supplement */
	EMIT_FN      = 3,  /* Fn keycode (no shift) */
	EMIT_MOUSE   = 4,  /* mouse event (REL/BTN) */
};

/* Fn-keycode table: phys_key → Fn keycode (used in both modes) */
struct fn_keymap {
	u8  phys;
	u16 fn_keycode;
};

static const struct fn_keymap fn_key_table[] = {
	{ PK_F1, KEY_F6 }, { PK_F2, KEY_F7 }, { PK_F3, KEY_F8 },
	{ PK_F4, KEY_F9 }, { PK_F5, KEY_F10 },
	{ PK_ESC, KEY_PAUSE }, { PK_TAB, KEY_HOME },
	{ PK_DELETE, KEY_END }, { PK_BACKSPACE, KEY_F11 },
	{ PK_UP, KEY_PAGEUP }, { PK_DOWN, KEY_PAGEDOWN },
	{ PK_LEFT, KEY_HOME }, { PK_RIGHT, KEY_END },
	{ PK_I, KEY_INSERT },
};

#define FN_KEY_TABLE_SIZE (sizeof(fn_key_table) / sizeof(fn_key_table[0]))

/* Mouse-mode Fn-keycode table: R-shift in mouse mode restores original keys */
static const struct fn_keymap mouse_fn_key_table[] = {
	{ PK_UP, KEY_UP }, { PK_DOWN, KEY_DOWN },
	{ PK_LEFT, KEY_LEFT }, { PK_RIGHT, KEY_RIGHT },
	{ PK_LBRACKET, KEY_LEFTBRACE }, { PK_RBRACKET, KEY_RIGHTBRACE },
	{ PK_GRAVE, KEY_PAGEUP }, { PK_SLASH, KEY_PAGEDOWN },
};

#define MOUSE_FN_KEY_TABLE_SIZE (sizeof(mouse_fn_key_table) / sizeof(mouse_fn_key_table[0]))

/* Fn-action table: phys_key → action (backlight etc.) */
enum fn_action {
	FN_ACTION_SCREEN_BKL_DOWN,
	FN_ACTION_SCREEN_BKL_UP,
	FN_ACTION_KBD_BKL_CYCLE,
};

struct fn_action_map {
	u8 phys;
	enum fn_action action;
};

static const struct fn_action_map fn_action_table[] = {
	{ PK_COMMA, FN_ACTION_SCREEN_BKL_DOWN },
	{ PK_DOT,   FN_ACTION_SCREEN_BKL_UP   },
	{ PK_SPACE, FN_ACTION_KBD_BKL_CYCLE   },
};

#define FN_ACTION_TABLE_SIZE (sizeof(fn_action_table) / sizeof(fn_action_table[0]))

/* phys_key → base keycode table */
static u16 phys_to_base_keycode[PK_MAX] = {
	[PK_A] = KEY_A, [PK_B] = KEY_B, [PK_C] = KEY_C, [PK_D] = KEY_D,
	[PK_E] = KEY_E, [PK_F] = KEY_F, [PK_G] = KEY_G, [PK_H] = KEY_H,
	[PK_I] = KEY_I, [PK_J] = KEY_J, [PK_K] = KEY_K, [PK_L] = KEY_L,
	[PK_M] = KEY_M, [PK_N] = KEY_N, [PK_O] = KEY_O, [PK_P] = KEY_P,
	[PK_Q] = KEY_Q, [PK_R] = KEY_R, [PK_S] = KEY_S, [PK_T] = KEY_T,
	[PK_U] = KEY_U, [PK_V] = KEY_V, [PK_W] = KEY_W, [PK_X] = KEY_X,
	[PK_Y] = KEY_Y, [PK_Z] = KEY_Z,
	[PK_0] = KEY_0, [PK_1] = KEY_1, [PK_2] = KEY_2, [PK_3] = KEY_3,
	[PK_4] = KEY_4, [PK_5] = KEY_5, [PK_6] = KEY_6, [PK_7] = KEY_7,
	[PK_8] = KEY_8, [PK_9] = KEY_9,
	[PK_SPACE] = KEY_SPACE, [PK_ENTER] = KEY_ENTER,
	[PK_TAB] = KEY_TAB, [PK_ESC] = KEY_ESC,
	[PK_BACKSPACE] = KEY_BACKSPACE,
	[PK_DOT] = KEY_DOT, [PK_COMMA] = KEY_COMMA,
	[PK_SEMICOLON] = KEY_SEMICOLON, [PK_APOSTROPHE] = KEY_APOSTROPHE,
	[PK_MINUS] = KEY_MINUS, [PK_EQUAL] = KEY_EQUAL,
	[PK_SLASH] = KEY_SLASH, [PK_BACKSLASH] = KEY_BACKSLASH,
	[PK_LBRACKET] = KEY_LEFTBRACE, [PK_RBRACKET] = KEY_RIGHTBRACE,
	[PK_GRAVE] = KEY_GRAVE,
	[PK_F1] = KEY_F1, [PK_F2] = KEY_F2, [PK_F3] = KEY_F3,
	[PK_F4] = KEY_F4, [PK_F5] = KEY_F5, [PK_F6] = KEY_F6,
	[PK_F7] = KEY_F7, [PK_F8] = KEY_F8, [PK_F9] = KEY_F9,
	[PK_F10] = KEY_F10,
	[PK_UP] = KEY_UP, [PK_DOWN] = KEY_DOWN,
	[PK_LEFT] = KEY_LEFT, [PK_RIGHT] = KEY_RIGHT,
	[PK_HOME] = KEY_HOME, [PK_END] = KEY_END,
	[PK_INSERT] = KEY_INSERT, [PK_DELETE] = KEY_DELETE,
	[PK_PAGEUP] = KEY_PAGEUP, [PK_PAGEDOWN] = KEY_PAGEDOWN,
	[PK_PAUSE] = KEY_PAUSE,
	[PK_CAPSLOCK] = KEY_CAPSLOCK,
};

/* Helper: look up Fn keycode for a phys_key */
static u16 lookup_fn_keycode(u8 phys, bool mouse_mode)
{
	int i;

	/* In mouse mode, first check mouse-specific table (arrows/brackets) */
	if (mouse_mode) {
		for (i = 0; i < MOUSE_FN_KEY_TABLE_SIZE; i++)
			if (mouse_fn_key_table[i].phys == phys)
				return mouse_fn_key_table[i].fn_keycode;
	}

	/* Then check the common Fn table (F1→F6, ESC→PAUSE, etc.) */
	for (i = 0; i < FN_KEY_TABLE_SIZE; i++)
		if (fn_key_table[i].phys == phys)
			return fn_key_table[i].fn_keycode;

	return 0;
}

/* Helper: look up Fn action for a phys_key */
static int lookup_fn_action(u8 phys, enum fn_action *out)
{
	int i;
	for (i = 0; i < FN_ACTION_TABLE_SIZE; i++)
		if (fn_action_table[i].phys == phys) {
			*out = fn_action_table[i].action;
			return 0;
		}
	return -1;
}

/* ================================================================ */
/* Main context                                                     */
/* ================================================================ */

struct kbd_ctx {
	struct work_struct work_struct;

	struct i2c_client *i2c_client;
	struct input_dev  *input_dev;

	/* Key FIFO */
	uint8_t key_fifo_count;
	struct key_fifo_item key_fifo_data[KBD_FIFO_SIZE];

	/* Physical modifier state (internal, not reported) */
	bool phy_SHL;   /* L-shift physically held */
	bool phy_SHR;   /* R-shift physically held */
	bool phy_ALT;   /* Alt physically held */
	bool phy_CTRL;  /* Ctrl physically held */
	bool capslock;  /* Capslock state */

	/* R-shift supplement tracking */
	bool rshift_reported;  /* is KEY_RIGHTSHIFT currently down in Linux? */

	/* Per-key emit tracking (strategy 2: lock at down, pair at up) */
	struct {
		u8  emit_mode;
		u16 keycode;
	} key_state[PK_MAX];

	/* Mouse mode */
	bool    mouse_mode;
	u8      mouse_dir;        /* active directions bitmask */
	struct delayed_work mouse_dw;
	u64     mouse_start_ns;   /* ktime when movement started (for acceleration) */
	int     mouse_sub_x;
	int     mouse_sub_y;

	/* Mouse acceleration params (sysfs configurable) */
	int mouse_initial_step;      /* px: instant jump on down */
	int mouse_base_speed;        /* px/s: initial sustained speed after delay */
	int mouse_max_speed;         /* px/s: max speed after acceleration */
	int mouse_start_delay_ms;    /* ms: delay before sustained movement */
	int mouse_accel_time_ms;     /* ms: time to ramp from base to max */

	/* Keyboard backlight level index (for cycle) */
	int kbd_bl_idx;

	/* Backlight flash on mouse-mode toggle */
	struct delayed_work bl_flash_work;
	uint8_t  saved_kbd_backlight;
	bool     bl_flash_active;

	/* Last keypress time (for sysfs) */
	uint64_t last_keypress_at;
};

/* ================================================================ */
/* Global state                                                     */
/* ================================================================ */

static struct kbd_ctx *g_ctx;

/* ================================================================ */
/* I2C helpers                                                      */
/* ================================================================ */

static inline int kbd_read_i2c_u8(struct i2c_client *cli, uint8_t reg, uint8_t *dst)
{
	int v = i2c_smbus_read_byte_data(cli, reg);
	if (v < 0) {
		dev_err(&cli->dev, "%s reg 0x%02X err %d\n", __func__, reg, v);
		return v;
	}
	*dst = (uint8_t)v;
	return 0;
}

static inline int kbd_write_i2c_u8(struct i2c_client *cli, uint8_t reg, uint8_t val)
{
	int rc = i2c_smbus_write_byte_data(cli, reg | PICOCALC_WRITE_MASK, val);
	if (rc) {
		dev_err(&cli->dev, "%s reg 0x%02X err %d\n", __func__, reg, rc);
	}
	return rc;
}

static inline int kbd_read_i2c_2u8(struct i2c_client *cli, uint8_t reg, uint8_t *dst)
{
	int w = i2c_smbus_read_word_data(cli, reg);
	if (w < 0) {
		dev_err(&cli->dev, "%s reg 0x%02X err %d\n", __func__, reg, w);
		return w;
	}
	dst[0] = (uint8_t)(w & 0xFF);
	dst[1] = (uint8_t)((w >> 8) & 0xFF);
	return 0;
}

/*
 * Read a register's value (skipping the register-ID byte).
 */
static inline int kbd_read_reg(struct i2c_client *cli, uint8_t reg, uint8_t *val)
{
	uint8_t buf[2];
	int rc = kbd_read_i2c_2u8(cli, reg, buf);
	if (rc)
		return rc;
	*val = buf[1];
	return 0;
}

/* ================================================================ */
/* FIFO reading                                                     */
/* ================================================================ */

static void input_fw_read_fifo(struct kbd_ctx *ctx)
{
	uint8_t i;

	ctx->key_fifo_count = 0;
	for (i = 0; i < KBD_FIFO_SIZE; i++) {
		uint8_t data[2];
		if (kbd_read_i2c_2u8(ctx->i2c_client, REG_ID_FIF, data)) {
			dev_err(&ctx->i2c_client->dev,
				"%s FIFO read error\n", __func__);
			return;
		}
		if (data[0] == 0)
			break;

		ctx->key_fifo_data[i]._        = 0;
		ctx->key_fifo_data[i].state    = data[0];
		ctx->key_fifo_data[i].scancode = data[1];
		ctx->key_fifo_count++;

		dev_info_fe(&ctx->i2c_client->dev,
			"%s [%d] state=%d scancode=0x%02X\n",
			__func__, i, data[0], data[1]);
	}
}

/* ================================================================ */
/* Backlight helpers                                                */
/* ================================================================ */

static void bl_flash_restore(struct work_struct *work)
{
	struct kbd_ctx *ctx = container_of(to_delayed_work(work),
					  struct kbd_ctx, bl_flash_work);
	kbd_write_i2c_u8(ctx->i2c_client, REG_ID_BK2, ctx->saved_kbd_backlight);
	ctx->bl_flash_active = false;
}

static void trigger_backlight_flash(struct kbd_ctx *ctx)
{
	cancel_delayed_work(&ctx->bl_flash_work);

	if (!ctx->bl_flash_active)
		kbd_read_reg(ctx->i2c_client, REG_ID_BK2, &ctx->saved_kbd_backlight);

	kbd_write_i2c_u8(ctx->i2c_client, REG_ID_BK2, KBD_BL_FLASH_VAL);
	ctx->bl_flash_active = true;

	schedule_delayed_work(&ctx->bl_flash_work,
			      msecs_to_jiffies(BL_FLASH_MS));
}

static void screen_backlight_adjust(struct kbd_ctx *ctx, int delta)
{
	uint8_t val;
	if (kbd_read_reg(ctx->i2c_client, REG_ID_BKL, &val))
		return;
	val += delta;
	if (val < SCREEN_BL_MIN) val = SCREEN_BL_MIN;
	if (val > SCREEN_BL_MAX) val = SCREEN_BL_MAX;
	kbd_write_i2c_u8(ctx->i2c_client, REG_ID_BKL, val);
}

static void kbd_backlight_cycle(struct kbd_ctx *ctx)
{
	ctx->kbd_bl_idx = (ctx->kbd_bl_idx + 1) % KBD_BL_LEVELS;
	kbd_write_i2c_u8(ctx->i2c_client, REG_ID_BK2,
			 kbd_bl_levels[ctx->kbd_bl_idx]);
}

/* ================================================================ */
/* Mouse movement — kernel timer (delayed_work)                     */
/* ================================================================ */

static void mouse_emit_rel(struct work_struct *work)
{
	struct kbd_ctx *ctx = container_of(to_delayed_work(work),
					   struct kbd_ctx, mouse_dw);
	u64 now_ns;
	int elapsed_ms;
	int speed;       /* current speed in px/s */
	int inc_fp;      /* per-tick increment in fp units (1px = 256) */
	int step_px;

	if (ctx->mouse_dir == 0)
		return;

	now_ns = ktime_get_boottime_ns();
	elapsed_ms = div_u64(now_ns - ctx->mouse_start_ns, 1000000);

	/* Phase 1: within start_delay → no movement (initial jump already done) */
	if (elapsed_ms < ctx->mouse_start_delay_ms) {
		schedule_delayed_work(&ctx->mouse_dw,
				      msecs_to_jiffies(MOUSE_TIMER_INTERVAL_MS));
		return;
	}

	/* Phase 2: sustained movement with acceleration */
	{
		int accel_elapsed = elapsed_ms - ctx->mouse_start_delay_ms;
		int ramp = ctx->mouse_accel_time_ms;

		if (accel_elapsed >= ramp || ramp <= 0)
			speed = ctx->mouse_max_speed;
		else
			speed = ctx->mouse_base_speed +
				(ctx->mouse_max_speed - ctx->mouse_base_speed) *
				accel_elapsed / ramp;
	}

	/* Per-tick increment in fp units (1px = 256): speed(px/s) * interval(ms) * 256 / 1000 */
	inc_fp = speed * MOUSE_TIMER_INTERVAL_MS * 256 / 1000;

	/* X axis */
	if (ctx->mouse_dir & (MOUSE_DIR_LEFT | MOUSE_DIR_RIGHT)) {
		int dir = (ctx->mouse_dir & MOUSE_DIR_RIGHT) ? 1 : -1;
		ctx->mouse_sub_x += inc_fp;
		step_px = ctx->mouse_sub_x >> 8;
		if (step_px > 0) {
			ctx->mouse_sub_x -= step_px << 8;
			input_report_rel(ctx->input_dev, REL_X, dir * step_px);
		}
	}

	/* Y axis */
	if (ctx->mouse_dir & (MOUSE_DIR_UP | MOUSE_DIR_DOWN)) {
		int dir = (ctx->mouse_dir & MOUSE_DIR_DOWN) ? 1 : -1;
		ctx->mouse_sub_y += inc_fp;
		step_px = ctx->mouse_sub_y >> 8;
		if (step_px > 0) {
			ctx->mouse_sub_y -= step_px << 8;
			input_report_rel(ctx->input_dev, REL_Y, dir * step_px);
		}
	}

	input_sync(ctx->input_dev);

	schedule_delayed_work(&ctx->mouse_dw,
			      msecs_to_jiffies(MOUSE_TIMER_INTERVAL_MS));
}

static void mouse_start_move(struct kbd_ctx *ctx, u8 dir)
{
	/* Already moving in this direction? Ignore repeated press from firmware */
	if (ctx->mouse_dir & dir)
		return;

	ctx->mouse_dir |= dir;
	/* If first direction, emit initial jump + start timer */
	if (ctx->mouse_dir == dir) {
		ctx->mouse_start_ns = ktime_get_boottime_ns();
		ctx->mouse_sub_x = 0;
		ctx->mouse_sub_y = 0;
		/* Emit initial jump */
		if (dir & (MOUSE_DIR_LEFT | MOUSE_DIR_RIGHT)) {
			int d = (dir & MOUSE_DIR_RIGHT) ? 1 : -1;
			input_report_rel(ctx->input_dev, REL_X, d * ctx->mouse_initial_step);
		}
		if (dir & (MOUSE_DIR_UP | MOUSE_DIR_DOWN)) {
			int d = (dir & MOUSE_DIR_DOWN) ? 1 : -1;
			input_report_rel(ctx->input_dev, REL_Y, d * ctx->mouse_initial_step);
		}
		schedule_delayed_work(&ctx->mouse_dw,
				      msecs_to_jiffies(MOUSE_TIMER_INTERVAL_MS));
	}
}

static void mouse_stop_move(struct kbd_ctx *ctx, u8 dir)
{
	ctx->mouse_dir &= ~dir;
	if (ctx->mouse_dir == 0)
		cancel_delayed_work(&ctx->mouse_dw);
}

/* ================================================================ */
/* R-shift supplement helpers                                       */
/* ================================================================ */

static void rshift_supplement_down(struct kbd_ctx *ctx)
{
	if (ctx->phy_SHR && !ctx->rshift_reported) {
		input_report_key(ctx->input_dev, KEY_RIGHTSHIFT, 1);
		ctx->rshift_reported = true;
	}
}

static void rshift_supplement_up(struct kbd_ctx *ctx)
{
	if (ctx->rshift_reported) {
		input_report_key(ctx->input_dev, KEY_RIGHTSHIFT, 0);
		ctx->rshift_reported = false;
	}
}

/* ================================================================ */
/* Layer 2: Key dispatch — the core state machine                   */
/* ================================================================ */

/* Check if phys_key is a mouse-mode special key (arrows, brackets) */
static bool is_mouse_key(u8 phys)
{
	return (phys == PK_UP || phys == PK_DOWN ||
		phys == PK_LEFT || phys == PK_RIGHT ||
		phys == PK_LBRACKET || phys == PK_RBRACKET);
}

static void key_dispatch(struct kbd_ctx *ctx, u8 phys, enum pico_key_state state)
{
	bool press   = (state == KEY_STATE_PRESSED);
	bool release = (state == KEY_STATE_RELEASED);

	if (phys == PK_NONE || phys >= PK_MAX)
		return;

	/* Drop power key events */
	if (phys == PK_POWER)
		return;

	/* Swallow all HOLD events early — they must not reset modifier state */
	if (state == KEY_STATE_HOLD)
		return;

	/* --- Modifiers (always processed first) --- */

	if (phys == PK_LSHIFT) {
		ctx->phy_SHL = press;
		if (press)   input_report_key(ctx->input_dev, KEY_LEFTSHIFT, 1);
		if (release) input_report_key(ctx->input_dev, KEY_LEFTSHIFT, 0);
		ctx->last_keypress_at = ktime_get_boottime_ns();
		return;
	}

	if (phys == PK_RSHIFT) {
		/* R-shift: internal only, never reported directly */
		ctx->phy_SHR = press;
		/* If R-shift released and supplement was active, clean up */
		if (release)
			rshift_supplement_up(ctx);
		return;
	}

	if (phys == PK_LALT) {
		ctx->phy_ALT = press;
		if (press)   input_report_key(ctx->input_dev, KEY_LEFTALT, 1);
		if (release) input_report_key(ctx->input_dev, KEY_LEFTALT, 0);
		ctx->last_keypress_at = ktime_get_boottime_ns();
		return;
	}

	if (phys == PK_LCTRL) {
		ctx->phy_CTRL = press;
		if (press)   input_report_key(ctx->input_dev, KEY_LEFTCTRL, 1);
		if (release) input_report_key(ctx->input_dev, KEY_LEFTCTRL, 0);
		ctx->last_keypress_at = ktime_get_boottime_ns();
		return;
	}

	/* --- Capslock: R-shift+Capslock = mouse toggle --- */

	if (phys == PK_CAPSLOCK) {
		if (press && ctx->phy_SHR) {
			/* Toggle mouse mode */
			ctx->mouse_mode = !ctx->mouse_mode;
			ctx->mouse_dir = 0;
			cancel_delayed_work(&ctx->mouse_dw);
			rshift_supplement_up(ctx);
			trigger_backlight_flash(ctx);
			dev_info_fe(&ctx->i2c_client->dev,
				    "mouse mode %s\n",
				    ctx->mouse_mode ? "ON" : "OFF");
			return;
		}
		/* Normal capslock */
		if (press || release) {
			u16 kc = phys_to_base_keycode[PK_CAPSLOCK];
			input_report_key(ctx->input_dev, kc, press);
			if (press)
				ctx->capslock = !ctx->capslock;
		}
		ctx->last_keypress_at = ktime_get_boottime_ns();
		return;
	}

	/* ================================================ */
	/* Key RELEASE: look up emit_mode and pair the up   */
	/* ================================================ */
	if (release) {
		u8 em = ctx->key_state[phys].emit_mode;
		u16 kc = ctx->key_state[phys].keycode;

		switch (em) {
		case EMIT_NORMAL:
		case EMIT_SHIFTED:
		case EMIT_FN:
			input_report_key(ctx->input_dev, kc, 0);
			break;
		case EMIT_MOUSE:
			if (is_mouse_key(phys) && phys >= PK_UP && phys <= PK_RIGHT) {
				/* Arrow release: stop mouse movement */
				switch (phys) {
				case PK_UP:    mouse_stop_move(ctx, MOUSE_DIR_UP);    break;
				case PK_DOWN:  mouse_stop_move(ctx, MOUSE_DIR_DOWN);  break;
				case PK_LEFT:  mouse_stop_move(ctx, MOUSE_DIR_LEFT);  break;
				case PK_RIGHT: mouse_stop_move(ctx, MOUSE_DIR_RIGHT); break;
				}
			}
			/* Bracket release: BTN_LEFT/BTN_RIGHT up */
			if (phys == PK_LBRACKET)
				input_report_key(ctx->input_dev, BTN_LEFT, 0);
			if (phys == PK_RBRACKET)
				input_report_key(ctx->input_dev, BTN_RIGHT, 0);
			break;
		default:
			break;
		}
		ctx->key_state[phys].emit_mode = EMIT_NONE;
		ctx->key_state[phys].keycode   = 0;
		ctx->last_keypress_at = ktime_get_boottime_ns();
		return;
	}

	/* ================================================ */
	/* Key PRESS: decide emit_mode and emit             */
	/* ================================================ */
	if (press) {
		/* Ignore repeated PRESSED events from firmware (acts like HOLD) */
		if (ctx->key_state[phys].emit_mode != EMIT_NONE)
			return;

		/* Mouse mode + no R-shift: mouse actions for arrows/brackets */
		if (ctx->mouse_mode && !ctx->phy_SHR && is_mouse_key(phys)) {
			if (phys >= PK_UP && phys <= PK_RIGHT) {
				u8 dir = 0;
				switch (phys) {
				case PK_UP:    dir = MOUSE_DIR_UP;    break;
				case PK_DOWN:  dir = MOUSE_DIR_DOWN;  break;
				case PK_LEFT:  dir = MOUSE_DIR_LEFT;  break;
				case PK_RIGHT: dir = MOUSE_DIR_RIGHT; break;
				}
				ctx->key_state[phys].emit_mode = EMIT_MOUSE;
				ctx->key_state[phys].keycode   = 0;
				mouse_start_move(ctx, dir);
				ctx->last_keypress_at = ktime_get_boottime_ns();
				return;
			}
			if (phys == PK_LBRACKET) {
				ctx->key_state[phys].emit_mode = EMIT_MOUSE;
				ctx->key_state[phys].keycode   = BTN_LEFT;
				input_report_key(ctx->input_dev, BTN_LEFT, 1);
				ctx->last_keypress_at = ktime_get_boottime_ns();
				return;
			}
			if (phys == PK_RBRACKET) {
				ctx->key_state[phys].emit_mode = EMIT_MOUSE;
				ctx->key_state[phys].keycode   = BTN_RIGHT;
				input_report_key(ctx->input_dev, BTN_RIGHT, 1);
				ctx->last_keypress_at = ktime_get_boottime_ns();
				return;
			}
		}

		/* R-shift held: check Fn tables */
		if (ctx->phy_SHR) {
			/* Fn-action keys (backlight etc.) */
			enum fn_action act;
			if (lookup_fn_action(phys, &act) == 0) {
				switch (act) {
				case FN_ACTION_SCREEN_BKL_DOWN:
					screen_backlight_adjust(ctx, -SCREEN_BL_STEP);
					break;
				case FN_ACTION_SCREEN_BKL_UP:
					screen_backlight_adjust(ctx, SCREEN_BL_STEP);
					break;
				case FN_ACTION_KBD_BKL_CYCLE:
					kbd_backlight_cycle(ctx);
					break;
				}
				/* Fn-action: set emit_mode to block firmware repeats */
				ctx->key_state[phys].emit_mode = EMIT_FN;
				ctx->key_state[phys].keycode   = 0;
				ctx->last_keypress_at = ktime_get_boottime_ns();
				return;
			}

			/* Fn-keycode keys (mouse_mode selects mouse_fn_key_table) */
			u16 fn_kc = lookup_fn_keycode(phys, ctx->mouse_mode);
			if (fn_kc) {
				ctx->key_state[phys].emit_mode = EMIT_FN;
				ctx->key_state[phys].keycode   = fn_kc;
				input_report_key(ctx->input_dev, fn_kc, 1);
				ctx->last_keypress_at = ktime_get_boottime_ns();
				return;
			}
		}

		/* Normal key: base keycode + R-shift supplement if needed */
		u16 base_kc = phys_to_base_keycode[phys];
		if (base_kc == 0) {
			dev_warn(&ctx->i2c_client->dev,
				 "unmapped phys_key %d\n", phys);
			return;
		}

		if (ctx->phy_SHR) {
			rshift_supplement_down(ctx);
			ctx->key_state[phys].emit_mode = EMIT_SHIFTED;
		} else {
			ctx->key_state[phys].emit_mode = EMIT_NORMAL;
		}
		ctx->key_state[phys].keycode = base_kc;
		input_report_key(ctx->input_dev, base_kc, 1);
		ctx->last_keypress_at = ktime_get_boottime_ns();
		return;
	}
}

/* ================================================================ */
/* Workqueue & timer                                                */
/* ================================================================ */

static void input_workqueue_handler(struct work_struct *ws)
{
	struct kbd_ctx *ctx = container_of(ws, struct kbd_ctx, work_struct);
	uint8_t i;

	input_fw_read_fifo(ctx);

	for (i = 0; i < ctx->key_fifo_count; i++) {
		const struct key_fifo_item *ev = &ctx->key_fifo_data[i];
		u8 phys = scancode_to_phys[ev->scancode];

		dev_info_ld(&ctx->i2c_client->dev,
			    "dispatch phys=%d state=%d scancode=0x%02X\n",
			    phys, ev->state, ev->scancode);

		key_dispatch(ctx, phys, ev->state);
	}

	ctx->key_fifo_count = 0;
	input_sync(ctx->input_dev);
}

static void kbd_timer_function(struct timer_list *t);
static DEFINE_TIMER(g_kbd_timer, kbd_timer_function);

static void kbd_timer_function(struct timer_list *t)
{
	schedule_work(&g_ctx->work_struct);
	mod_timer(&g_kbd_timer, jiffies + HZ / 128);
}

/* ================================================================ */
/* Sysfs: parsing helpers                                           */
/* ================================================================ */

static inline int parse_u8(char const *buf)
{
	int rc, result;
	if ((rc = kstrtoint(buf, 10, &result)) || result < 0 || result > 0xff)
		return -EINVAL;
	return result;
}

static int parse_and_write_i2c_u8(char const *buf, size_t count, uint8_t reg)
{
	int parsed;
	if ((parsed = parse_u8(buf)) < 0)
		return -EINVAL;
	if (g_ctx && g_ctx->i2c_client)
		kbd_write_i2c_u8(g_ctx->i2c_client, reg, (uint8_t)parsed);
	return count;
}

/* ================================================================ */
/* Sysfs: existing entries                                          */
/* ================================================================ */

/* Battery percent */
static ssize_t battery_percent_show(struct kobject *k,
				    struct kobj_attribute *a, char *buf)
{
	uint8_t pct[2];
	int rc;
	if (!g_ctx || !g_ctx->i2c_client)
		return -EINVAL;
	if ((rc = kbd_read_i2c_2u8(g_ctx->i2c_client, REG_ID_BAT, pct)) < 0)
		return rc;
	return sprintf(buf, "%d\n", pct[1]);
}
static struct kobj_attribute battery_percent_attr =
	__ATTR(battery_percent, 0444, battery_percent_show, NULL);

/* Keyboard backlight (raw) */
static ssize_t keyboard_backlight_store(struct kobject *k,
					struct kobj_attribute *a,
					char const *buf, size_t count)
{
	return parse_and_write_i2c_u8(buf, count, REG_ID_BK2);
}
static struct kobj_attribute keyboard_backlight_attr =
	__ATTR(keyboard_backlight, 0220, NULL, keyboard_backlight_store);

/* Screen backlight (raw) */
static ssize_t screen_backlight_store(struct kobject *k,
				      struct kobj_attribute *a,
				      char const *buf, size_t count)
{
	return parse_and_write_i2c_u8(buf, count, REG_ID_BKL);
}
static struct kobj_attribute screen_backlight_attr =
	__ATTR(screen_backlight, 0220, NULL, screen_backlight_store);

/* Last keypress ms */
static ssize_t last_keypress_show(struct kobject *k,
				  struct kobj_attribute *a, char *buf)
{
	uint64_t now;
	if (!g_ctx)
		return sprintf(buf, "-1\n");
	now = ktime_get_boottime_ns();
	if (g_ctx->last_keypress_at >= now)
		return sprintf(buf, "-1\n");
	now -= g_ctx->last_keypress_at;
	now = div_u64(now, 1000000ULL);
	return sprintf(buf, "%lld\n", now);
}
static struct kobj_attribute last_keypress_attr =
	__ATTR(last_keypress, 0444, last_keypress_show, NULL);

/* ================================================================ */
/* Sysfs: mouse params                                              */
/* ================================================================ */

static ssize_t mouse_initial_step_show(struct kobject *k,
				       struct kobj_attribute *a, char *buf)
{
	return sprintf(buf, "%d\n", g_ctx ? g_ctx->mouse_initial_step : -1);
}

static ssize_t mouse_initial_step_store(struct kobject *k,
					struct kobj_attribute *a,
					char const *buf, size_t count)
{
	int v;
	if (kstrtoint(buf, 10, &v) || v < 0)
		return -EINVAL;
	if (g_ctx)
		g_ctx->mouse_initial_step = v;
	return count;
}
static struct kobj_attribute mouse_initial_step_attr =
	__ATTR(mouse_initial_step, 0644,
	       mouse_initial_step_show, mouse_initial_step_store);

static ssize_t mouse_max_speed_show(struct kobject *k,
				    struct kobj_attribute *a, char *buf)
{
	return sprintf(buf, "%d\n", g_ctx ? g_ctx->mouse_max_speed : -1);
}

static ssize_t mouse_max_speed_store(struct kobject *k,
				     struct kobj_attribute *a,
				     char const *buf, size_t count)
{
	int v;
	if (kstrtoint(buf, 10, &v) || v < 0)
		return -EINVAL;
	if (g_ctx)
		g_ctx->mouse_max_speed = v;
	return count;
}
static struct kobj_attribute mouse_max_speed_attr =
	__ATTR(mouse_max_speed, 0644,
	       mouse_max_speed_show, mouse_max_speed_store);

static ssize_t mouse_accel_time_show(struct kobject *k,
				     struct kobj_attribute *a, char *buf)
{
	return sprintf(buf, "%d\n", g_ctx ? g_ctx->mouse_accel_time_ms : -1);
}

static ssize_t mouse_accel_time_store(struct kobject *k,
				      struct kobj_attribute *a,
				      char const *buf, size_t count)
{
	int v;
	if (kstrtoint(buf, 10, &v) || v < 0)
		return -EINVAL;
	if (g_ctx)
		g_ctx->mouse_accel_time_ms = v;
	return count;
}
static struct kobj_attribute mouse_accel_time_attr =
	__ATTR(mouse_accel_time_ms, 0644,
	       mouse_accel_time_show, mouse_accel_time_store);

static ssize_t mouse_base_speed_show(struct kobject *k,
				     struct kobj_attribute *a, char *buf)
{
	return sprintf(buf, "%d\n", g_ctx ? g_ctx->mouse_base_speed : -1);
}

static ssize_t mouse_base_speed_store(struct kobject *k,
				      struct kobj_attribute *a,
				      char const *buf, size_t count)
{
	int v;
	if (kstrtoint(buf, 10, &v) || v < 0)
		return -EINVAL;
	if (g_ctx)
		g_ctx->mouse_base_speed = v;
	return count;
}
static struct kobj_attribute mouse_base_speed_attr =
	__ATTR(mouse_base_speed, 0644,
	       mouse_base_speed_show, mouse_base_speed_store);

static ssize_t mouse_start_delay_show(struct kobject *k,
				      struct kobj_attribute *a, char *buf)
{
	return sprintf(buf, "%d\n", g_ctx ? g_ctx->mouse_start_delay_ms : -1);
}

static ssize_t mouse_start_delay_store(struct kobject *k,
				       struct kobj_attribute *a,
				       char const *buf, size_t count)
{
	int v;
	if (kstrtoint(buf, 10, &v) || v < 0)
		return -EINVAL;
	if (g_ctx)
		g_ctx->mouse_start_delay_ms = v;
	return count;
}
static struct kobj_attribute mouse_start_delay_attr =
	__ATTR(mouse_start_delay_ms, 0644,
	       mouse_start_delay_show, mouse_start_delay_store);

/* ================================================================ */
/* Sysfs: power off                                                 */
/* ================================================================ */

static ssize_t poweroff_store(struct kobject *k,
			      struct kobj_attribute *a,
			      char const *buf, size_t count)
{
	int v;
	if (kstrtoint(buf, 10, &v) || v < 6)
		return -EINVAL;
	if (g_ctx && g_ctx->i2c_client)
		kbd_write_i2c_u8(g_ctx->i2c_client, REG_ID_OFF, (uint8_t)v);
	return count;
}
static struct kobj_attribute poweroff_attr =
	__ATTR(poweroff, 0220, NULL, poweroff_store);

/* ================================================================ */
/* Sysfs: firmware modifier state machine switch                    */
/* ================================================================ */

static ssize_t fw_use_mods_show(struct kobject *k,
				struct kobj_attribute *a, char *buf)
{
	uint8_t cfg = 0;
	if (g_ctx && g_ctx->i2c_client)
		kbd_read_reg(g_ctx->i2c_client, REG_ID_CFG, &cfg);
	return sprintf(buf, "%d\n", (cfg & CFG_USE_MODS) ? 1 : 0);
}

static ssize_t fw_use_mods_store(struct kobject *k,
				 struct kobj_attribute *a,
				 char const *buf, size_t count)
{
	int v;
	uint8_t cfg = 0;
	if (kstrtoint(buf, 10, &v) || v < 0 || v > 1)
		return -EINVAL;
	if (!g_ctx || !g_ctx->i2c_client)
		return -EINVAL;
	kbd_read_reg(g_ctx->i2c_client, REG_ID_CFG, &cfg);
	if (v)
		cfg |= CFG_USE_MODS;
	else
		cfg &= ~CFG_USE_MODS;
	kbd_write_i2c_u8(g_ctx->i2c_client, REG_ID_CFG, cfg);
	return count;
}
static struct kobj_attribute fw_use_mods_attr =
	__ATTR(fw_use_mods, 0644, fw_use_mods_show, fw_use_mods_store);

/* ================================================================ */
/* Sysfs: attribute group                                           */
/* ================================================================ */

static struct attribute *picocalc_attrs[] = {
	&battery_percent_attr.attr,
	&screen_backlight_attr.attr,
	&keyboard_backlight_attr.attr,
	&last_keypress_attr.attr,
	&mouse_initial_step_attr.attr,
	&mouse_base_speed_attr.attr,
	&mouse_max_speed_attr.attr,
	&mouse_start_delay_attr.attr,
	&mouse_accel_time_attr.attr,
	&poweroff_attr.attr,
	&fw_use_mods_attr.attr,
	NULL,
};
static struct attribute_group picocalc_attr_group = {
	.attrs = picocalc_attrs,
};

static struct kobject *picocalc_kobj;

static uint32_t sysfs_gid_setting;

static void picocalc_get_ownership
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
(struct kobject *kobj, kuid_t *uid, kgid_t *gid)
#else
(struct kobject const *kobj, kuid_t *uid, kgid_t *gid)
#endif
{
	if (gid)
		gid->val = sysfs_gid_setting;
}

static struct kobj_type picocalc_ktype = {
	.get_ownership = picocalc_get_ownership,
	.sysfs_ops    = &kobj_sysfs_ops,
};

/* ================================================================ */
/* Probe & shutdown                                                */
/* ================================================================ */

static int input_probe(struct i2c_client *cli)
{
	struct kbd_ctx *ctx;
	uint8_t cfg;
	int rc, i;

	ctx = devm_kzalloc(&cli->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->i2c_client     = cli;
	ctx->input_dev      = devm_input_allocate_device(&cli->dev);
	ctx->last_keypress_at = ktime_get_boottime_ns();

	/* Mouse defaults */
	ctx->mouse_initial_step   = 8;    /* px: instant jump on down */
	ctx->mouse_base_speed     = 80;   /* px/s: initial sustained speed */
	ctx->mouse_max_speed      = 320;  /* px/s: max speed after acceleration */
	ctx->mouse_start_delay_ms = 80;   /* ms: delay before sustained movement */
	ctx->mouse_accel_time_ms  = 400;  /* ms: time to ramp from base to max */

	if (!ctx->input_dev) {
		dev_err(&cli->dev, "could not allocate input device\n");
		return -ENOMEM;
	}

	/*
	 * Configure firmware: disable CFG_USE_MODS so firmware sends raw scancodes.
	 * Keep CFG_REPORT_MODS to receive modifier events as independent scancodes.
	 */
	if (!kbd_read_reg(cli, REG_ID_CFG, &cfg)) {
		cfg &= ~CFG_USE_MODS;
		cfg |= CFG_REPORT_MODS | CFG_KEY_INT;
		kbd_write_i2c_u8(cli, REG_ID_CFG, cfg);
	}

	/* Input device identity */
	ctx->input_dev->name       = cli->name;
	ctx->input_dev->id.bustype = KBD_BUS_TYPE;
	ctx->input_dev->id.vendor  = KBD_VENDOR_ID;
	ctx->input_dev->id.product = KBD_PRODUCT_ID;
	ctx->input_dev->id.version = KBD_VERSION_ID;

	/* Keycode table for ioctl support */
	ctx->input_dev->keycode     = keycodes;
	ctx->input_dev->keycodesize = sizeof(keycodes[0]);
	ctx->input_dev->keycodemax  = ARRAY_SIZE(keycodes);

	/* Declare all keycodes from the map */
	for (i = 0; i < NUM_KEYCODES; i++)
		__set_bit(keycodes[i], ctx->input_dev->keybit);
	__clear_bit(KEY_RESERVED, ctx->input_dev->keybit);

	/* Fn-layer keycodes not in base table */
	__set_bit(KEY_F11, ctx->input_dev->keybit);
	__set_bit(KEY_POWER, ctx->input_dev->keybit);

	/* Event types */
	__set_bit(EV_REP, ctx->input_dev->evbit);
	__set_bit(EV_KEY, ctx->input_dev->evbit);
	__set_bit(EV_REL, ctx->input_dev->evbit);

	/* Mouse capabilities */
	input_set_capability(ctx->input_dev, EV_REL, REL_X);
	input_set_capability(ctx->input_dev, EV_REL, REL_Y);
	input_set_capability(ctx->input_dev, EV_KEY, BTN_LEFT);
	input_set_capability(ctx->input_dev, EV_KEY, BTN_RIGHT);

	/* Misc */
	input_set_capability(ctx->input_dev, EV_MSC, MSC_SCAN);

	/* Delayed works */
	INIT_DELAYED_WORK(&ctx->bl_flash_work, bl_flash_restore);
	INIT_DELAYED_WORK(&ctx->mouse_dw, mouse_emit_rel);

	/* Workqueue + timer */
	INIT_WORK(&ctx->work_struct, input_workqueue_handler);
	g_kbd_timer.expires = jiffies + HZ / 128;
	add_timer(&g_kbd_timer);

	/* Register */
	dev_info(&cli->dev, "%s registering input device\n", __func__);
	if ((rc = input_register_device(ctx->input_dev))) {
		dev_err(&cli->dev, "input_register_device failed: %d\n", rc);
		return rc;
	}

	g_ctx = ctx;
	return 0;
}

static void input_shutdown(struct i2c_client *cli)
{
	del_timer_sync(&g_kbd_timer);
	cancel_delayed_work_sync(&g_ctx->bl_flash_work);
	cancel_delayed_work_sync(&g_ctx->mouse_dw);
	cancel_work_sync(&g_ctx->work_struct);
	g_ctx = NULL;
}

/* ================================================================ */
/* Sysfs probe / shutdown                                           */
/* ================================================================ */

static int sysfs_probe(struct i2c_client *cli)
{
	int rc;

	picocalc_kobj = devm_kzalloc(&cli->dev, sizeof(*picocalc_kobj),
				     GFP_KERNEL);
	if (!picocalc_kobj)
		return -ENOMEM;

	rc = kobject_init_and_add(picocalc_kobj, &picocalc_ktype,
				  firmware_kobj, "picocalc");
	if (rc) {
		kobject_put(picocalc_kobj);
		return rc;
	}

	if (sysfs_create_group(picocalc_kobj, &picocalc_attr_group)) {
		kobject_put(picocalc_kobj);
		return -ENOMEM;
	}
	return 0;
}

static void sysfs_shutdown(void)
{
	if (picocalc_kobj) {
		kobject_put(picocalc_kobj);
		picocalc_kobj = NULL;
	}
}

/* ================================================================ */
/* Power-off: hook into system poweroff                             */
/* ================================================================ */

static bool g_system_powering_off;
static struct notifier_block picocalc_reboot_nb;

static int picocalc_reboot_notify(struct notifier_block *nb,
				  unsigned long code, void *data)
{
	g_system_powering_off = (code == SYS_POWER_OFF);
	return NOTIFY_DONE;
}

static void picocalc_kbd_shutdown(struct i2c_client *cli)
{
	/* Called by kernel at the end of shutdown (after filesystems unmounted).
	 * Only act on power-off, not reboot.
	 */
	if (g_system_powering_off && g_ctx && g_ctx->i2c_client) {
		dev_info(&cli->dev, "sending power-off command to MCU\n");
		kbd_write_i2c_u8(g_ctx->i2c_client, REG_ID_OFF, 6);
	}
}

/* ================================================================ */
/* I2C driver                                                       */
/* ================================================================ */

static int picocalc_kbd_probe
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
(struct i2c_client *cli, struct i2c_device_id const *id)
#else
(struct i2c_client *cli)
#endif
{
	int rc;

	if ((rc = input_probe(cli)))
		return rc;
	if ((rc = sysfs_probe(cli)))
		return rc;
	return 0;
}

static void picocalc_kbd_remove(struct i2c_client *cli)
{
	sysfs_shutdown();
	input_shutdown(cli);
}

static const struct i2c_device_id picocalc_kbd_i2c_id[] = {
	{ "picocalc_kbd", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, picocalc_kbd_i2c_id);

static const struct of_device_id picocalc_kbd_of_id[] = {
	{ .compatible = "picocalc,picocalc-keyboard" },
	{ }
};
MODULE_DEVICE_TABLE(of, picocalc_kbd_of_id);

static struct i2c_driver picocalc_kbd_driver = {
	.driver = {
		.name = "picocalc_kbd",
		.of_match_table = picocalc_kbd_of_id,
	},
	.probe    = picocalc_kbd_probe,
	.remove   = picocalc_kbd_remove,
	.shutdown = picocalc_kbd_shutdown,
	.id_table = picocalc_kbd_i2c_id,
};

static int __init picocalc_kbd_init(void)
{
	int rc;

	picocalc_reboot_nb.notifier_call = picocalc_reboot_notify;
	register_reboot_notifier(&picocalc_reboot_nb);

	if ((rc = i2c_add_driver(&picocalc_kbd_driver))) {
		pr_err("picocalc_kbd: init failed: %d\n", rc);
		unregister_reboot_notifier(&picocalc_reboot_nb);
		return rc;
	}
	pr_info("picocalc_kbd: initialised\n");
	return 0;
}
module_init(picocalc_kbd_init);

static void __exit picocalc_kbd_exit(void)
{
	i2c_del_driver(&picocalc_kbd_driver);
	unregister_reboot_notifier(&picocalc_reboot_nb);
}
module_exit(picocalc_kbd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hiro <hiro@hiro.com>");
MODULE_DESCRIPTION("Keyboard driver for PicoCalc — v2 rewrite");
MODULE_VERSION("2.0");
