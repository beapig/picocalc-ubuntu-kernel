// SPDX-License-Identifier: GPL-2.0+
/*
 * ILI9488 register debug module — co-exists with the DRM driver
 *
 * This module does NOT claim the SPI device.  It finds the SPI device
 * already bound by the DRM driver and borrows it for register writes.
 * DRM continues refreshing the display normally.
 *
 * Usage:
 *   insmod ili9488_dbg.ko
 *   echo "c5 0a" > /sys/kernel/ili9488_dbg/write
 *   echo "11"    > /sys/kernel/ili9488_dbg/write   # sleep out
 *   echo "29"    > /sys/kernel/ili9488_dbg/write   # display on
 *   echo "init"  > /sys/kernel/ili9488_dbg/write   # full init seq
 *   rmmod ili9488_dbg
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/device.h>

#define DRV_NAME "ili9488_dbg"

static struct spi_device *g_spi;
static int g_dc_gpio = -ENOENT;
static struct kobject *g_kobj;

/* ------------------------------------------------------------------ */
/*  Low-level register write                                          */
/*  First byte = command (DC=LOW), rest = data (DC=HIGH)              */
/* ------------------------------------------------------------------ */
static int lcd_write(const u8 *buf, size_t len)
{
	struct spi_transfer xfer_cmd = {0};
	struct spi_transfer xfer_data = {0};
	struct spi_message msg;
	int ret;

	if (!g_spi || len == 0)
		return -EINVAL;

	spi_bus_lock(g_spi->controller);

	/* Command byte: DC = LOW */
	if (gpio_is_valid(g_dc_gpio))
		gpio_set_value(g_dc_gpio, 0);

	xfer_cmd.tx_buf = buf;
	xfer_cmd.len = 1;
	xfer_cmd.speed_hz = g_spi->max_speed_hz;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer_cmd, &msg);
	ret = spi_sync_locked(g_spi, &msg);
	if (ret)
		goto out;

	/* Data bytes: DC = HIGH */
	if (len > 1) {
		if (gpio_is_valid(g_dc_gpio))
			gpio_set_value(g_dc_gpio, 1);

		xfer_data.tx_buf = buf + 1;
		xfer_data.len = len - 1;
		xfer_data.speed_hz = g_spi->max_speed_hz;

		spi_message_init(&msg);
		spi_message_add_tail(&xfer_data, &msg);
		ret = spi_sync_locked(g_spi, &msg);
	}

out:
	spi_bus_unlock(g_spi->controller);
	return ret;
}

/* ------------------------------------------------------------------ */
/*  Full init sequence (mirrors the DRM driver)                        */
/* ------------------------------------------------------------------ */
static void lcd_init_sequence(void)
{
	msleep(120);

	lcd_write((u8[]){0xE0, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A,
			 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08,
			 0x16, 0x1A, 0x0F}, 16);
	lcd_write((u8[]){0xE1, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05,
			 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D,
			 0x35, 0x37, 0x0F}, 16);
	lcd_write((u8[]){0xC0, 0x17, 0x15}, 3);
	lcd_write((u8[]){0xC1, 0x41}, 2);
	lcd_write((u8[]){0xC5, 0x00, 0x12, 0x80}, 4);
	lcd_write((u8[]){0x36, 0x48}, 2);
	lcd_write((u8[]){0x3A, 0x55}, 2);
	lcd_write((u8[]){0xB0, 0x00}, 2);
	lcd_write((u8[]){0xB1, 0xA0}, 2);
	lcd_write((u8[]){0x21}, 1);
	lcd_write((u8[]){0xB4, 0x02}, 2);
	lcd_write((u8[]){0xB6, 0x02, 0x02, 0x3B}, 4);
	lcd_write((u8[]){0xB7, 0xC6, 0xE9, 0x00}, 4);
	lcd_write((u8[]){0xF7, 0xA9, 0x51, 0x2C, 0x82}, 5);
	lcd_write((u8[]){0x11}, 1);
	msleep(100);
	lcd_write((u8[]){0x29}, 1);
	msleep(50);
}

/* ------------------------------------------------------------------ */
/*  Sysfs: write                                                       */
/* ------------------------------------------------------------------ */
static ssize_t write_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	u8 bytes[64];
	int n = 0, ret;
	char tmp[4];
	const char *p = buf;
	size_t remaining = count;

	if (!g_spi)
		return -ENODEV;

	/* Special: "init" runs the full init sequence */
	if (remaining >= 4 && strncmp(buf, "init", 4) == 0) {
		dev_info(&g_spi->dev, "dbg: running full init seq\n");
		lcd_init_sequence();
		return count;
	}

	/* Parse hex bytes */
	while (remaining > 0 && n < 64) {
		while (remaining > 0 && (*p == ' ' || *p == '\t' ||
		       *p == '\n' || *p == '\r')) {
			p++;
			remaining--;
		}
		if (remaining == 0)
			break;

		tmp[0] = *p;
		tmp[1] = '\0';
		tmp[2] = '\0';
		p++;
		remaining--;

		if (remaining > 0 && *p != ' ' && *p != '\t' &&
		    *p != '\n' && *p != '\r') {
			tmp[1] = *p;
			p++;
			remaining--;
		}

		ret = kstrtou8(tmp, 16, &bytes[n]);
		if (ret)
			break;
		n++;
	}

	if (n == 0)
		return -EINVAL;

	ret = lcd_write(bytes, n);
	if (ret)
		dev_err(&g_spi->dev, "dbg: write failed: %d\n", ret);
	else
		dev_info(&g_spi->dev, "dbg: wrote %d bytes OK\n", n);

	return ret ? ret : count;
}
static struct kobj_attribute write_attr = __ATTR_WO(write);

/* ------------------------------------------------------------------ */
/*  Sysfs: help                                                        */
/* ------------------------------------------------------------------ */
static ssize_t help_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return sysfs_emit(buf,
		"ILI9488 Debug Interface (coexists with DRM)\n"
		"  echo \"CMD D1 D2...\" > write   # hex bytes\n"
		"  echo \"c5 0a\" > write          # VCOM\n"
		"  echo \"11\" > write              # sleep out\n"
		"  echo \"29\" > write              # display on\n"
		"  echo \"init\" > write            # full init sequence\n"
	);
}
static struct kobj_attribute help_attr = __ATTR_RO(help);

static struct attribute *dbg_attrs[] = {
	&write_attr.attr,
	&help_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dbg);

/* ------------------------------------------------------------------ */
/*  Match function to find our SPI device                              */
/* ------------------------------------------------------------------ */
static int match_lcd(struct device *dev, const void *data)
{
	struct spi_device *spi = to_spi_device(dev);
	return of_device_is_compatible(spi->dev.of_node, "ilitek,ili9488") ||
	       of_device_is_compatible(spi->dev.of_node, "picocalc,spilcd");
}

/* ------------------------------------------------------------------ */
/*  Module init / exit                                                 */
/* ------------------------------------------------------------------ */
static int __init ili9488_dbg_init(void)
{
	struct device *dev;
	struct device_node *np;
	int ret;

	/* Find the SPI device already bound by the DRM driver */
	dev = bus_find_device(&spi_bus_type, NULL, NULL, match_lcd);
	if (!dev) {
		pr_err("%s: no matching LCD SPI device found\n", DRV_NAME);
		return -ENODEV;
	}
	g_spi = to_spi_device(dev);
	/* bus_find_device() gives us a ref; drop it, we take our own below */
	put_device(dev);

	if (!spi_dev_get(g_spi)) {
		pr_err("%s: failed to get ref to SPI device\n", DRV_NAME);
		g_spi = NULL;
		return -ENODEV;
	}

	/* Read DC GPIO number from DT (without claiming the gpio) */
	np = g_spi->dev.of_node;
	if (np) {
		g_dc_gpio = of_get_named_gpio(np, "dc-gpios", 0);
		if (!gpio_is_valid(g_dc_gpio))
			g_dc_gpio = -ENOENT;
		else
			dev_info(&g_spi->dev, "%s: DC GPIO #%d\n",
				 DRV_NAME, g_dc_gpio);
	}

	/* Create sysfs node */
	g_kobj = kobject_create_and_add(DRV_NAME, kernel_kobj);
	if (!g_kobj) {
		spi_dev_put(g_spi);
		g_spi = NULL;
		return -ENOMEM;
	}

	ret = sysfs_create_groups(g_kobj, dbg_groups);
	if (ret) {
		kobject_put(g_kobj);
		spi_dev_put(g_spi);
		g_spi = NULL;
		return ret;
	}

	dev_info(&g_spi->dev, "%s: loaded — use /sys/kernel/%s/write\n",
		 DRV_NAME, DRV_NAME);
	return 0;
}

static void __exit ili9488_dbg_exit(void)
{
	if (g_kobj) {
		sysfs_remove_groups(g_kobj, dbg_groups);
		kobject_put(g_kobj);
	}
	if (g_spi) {
		dev_info(&g_spi->dev, "%s: unloaded\n", DRV_NAME);
		spi_dev_put(g_spi);
	}
	g_spi = NULL;
	g_dc_gpio = -ENOENT;
}

module_init(ili9488_dbg_init);
module_exit(ili9488_dbg_exit);

MODULE_DESCRIPTION("ILI9488 register debug — co-exists with DRM");
MODULE_AUTHOR("debug tool");
MODULE_LICENSE("GPL v2");