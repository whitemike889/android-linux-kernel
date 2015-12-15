/*
 * Copyright (C) 2015 BlackBerry Limited
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/input/matrix_keypad.h>
#include <linux/regulator/consumer.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/version.h>
#include <linux/random.h>
#ifdef CONFIG_STMPE_KEYPAD_DDT
#include <misc/ddt.h>
#include <misc/lw_event_types.h>

#define LW_EVENT_KEYPAD_UNEXPECTED_RESET	LW_EVENT_KEYPAD_00002525
#endif
#define REG_CHIP_ID				0x00
#define REG_INT_CTRL				0x04
#define REG_INT_EN_MASK			0x06
#define REG_INT_STA				0x08
#define REG_GPIO_SET_LOW			0x10
#define REG_GPIO_SET_DIR_LOW			0x19
#define REG_KPC_ROW				0x30
#define REG_KPC_COL				0x31
#define REG_KPC_CTRL_LOW			0x33
#define REG_KPC_CTRL_MID			0x34
#define REG_KPC_CTRL_HIGH			0x35
#define REG_KPC_CMD				0x36
#define REG_KPC_DATA				0x3a

#define CHIP_ID				0xc1
#define INT_I0_WAKEUP				0x01
#define INT_I1_KEYPAD				0x02
#define INT_I2_KEYPAD_OVERFLOW			0x04
#define INT_I3_GPIO				0x08
#define INT_I4_COMBO_KEY			0x10
#define INT_CTRL_MASK				0x01
#define KPC_SCAN_FREQ_60			0x00
#define KPC_SCAN_FREQ_30			0x01
#define KPC_SCAN_FREQ_15			0x02
#define KPC_SCAN_FREQ_275			0x03
#define KPC_CMD_SCAN_ENABLE			0x01
#define KPC_DATA_LENGTH			5
#define KPC_DATA_NO_KEY_MASK			0x78
#define KEYPAD_ROW_SHIFT			3
#define KEYPAD_MAX_ROWS				4
#define KEYPAD_MAX_COLS				10
#define KEYPAD_KEYMAP_ROWS			8
#define KEYPAD_KEYMAP_COLS			8
#define KEYPAD_KEYMAP_SIZE	\
	(KEYPAD_KEYMAP_ROWS * KEYPAD_KEYMAP_COLS)
#define KEYPAD_MAX_KEY_BUFFER		3
#define KEYPAD_STATS_TIMER		5000
#define KEYPAD_STUCK_KEY_TIME		10
#ifdef CONFIG_STMPE_KEYPAD_DDT
#define MAX_RESET_DDT_SEND		50
#endif
#define KEYPAD_MAX_ADJACENT		6
#define KEYPAD_MAX_KEYS			255
#define KEYPAD_EVENT_LOG_SIZE		100
#define KEYPAD_MAX_KEY_WIDTH		4
#define KEY_EVENT_FIFO_SIZE	10

/* TEMP */
#define debug(str, args...) /* dev_err(&keypad->i2c_client->dev, "%s: " str "\n", __func__, ##args)*/
#define info(str, args...) dev_err(&keypad->i2c_client->dev, "%s: " str "\n", __func__, ##args)
#define warn(str, args...) dev_err(&keypad->i2c_client->dev, "%s: " str "\n", __func__, ##args)
#define error(str, args...) dev_err(&keypad->i2c_client->dev, "%s: " str "\n", __func__, ##args)

struct key_data {
	uint8_t key;
	unsigned int code;
	uint8_t adjacent[KEYPAD_MAX_ADJACENT];
	int num_adjacent;
	bool up;
	struct timeval down_time;
	struct timeval up_time;
	int count;
	bool send_count;
	bool inadv_check;
	bool modifier;
	int adjacent_detected;
	int inadvertent;
};

typedef enum {
	KEY_EVENT_UP,
	KEY_EVENT_DOWN
} key_event_type_t;

struct key_event_data {
	key_event_type_t type;
	uint8_t row;
	uint8_t col;
	uint8_t code;
	uint8_t key;
};

struct log_entry {
	uint8_t key;
	key_event_type_t type;
	struct timeval time;
};

struct stmpe_keypad {
	struct i2c_client *i2c_client;
	struct input_dev *input_dev;
	int ctrl_irq;
	int reset_irq;
	struct {
		int reset_count;
		int interrupts;
		int keys_pressed;
		int stuck_keys;
		int multi_key;
		int extra_key;
		long key_time;
		long low_key_time;
		long high_key_time;
	} counters;
	struct log_entry event_log[KEYPAD_EVENT_LOG_SIZE];
	int log_idx;
	bool reset_detect;
	struct timeval last_keypress;
	struct regulator *regulator_vdd;
	unsigned short keymap[KEYPAD_KEYMAP_SIZE];
	struct key_data keys[KEYPAD_MAX_KEYS];
	int down_keys;
	struct mutex keypad_mutex;
	struct workqueue_struct *workqueue;
	struct work_struct input_work;
	struct timer_list timer;
	struct timer_list inadv_timer;
	int inadv_timeout;
	uint8_t pending_inadv_key;
	uint8_t key_table[KEYPAD_MAX_ROWS][KEYPAD_MAX_COLS];
	bool slide_open;
	bool slide_transitioning;
	bool irq_enabled;
	bool keypad_enabled;
	struct {
		u8 row_mask;
		u16 column_mask;
		int reset_det_gpio;
		int scan_count;
		int debounce;
		int scan_frequency;
		int rst_int_gpio;
		int kp_int_gpio;
		int reset_gpio;
		int interrupt_polarity;
		bool edge_interrupt;
	} config;
};



const static char *keypad_name[] = {
	"stmpe_keypad",
	"stmpe_azerty_keypad",
	"stmpe_qwertz_keypad",
};


static const struct input_device_id input_event_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_SWBIT,
		.evbit = { BIT_MASK(EV_SW) },
		.swbit = { [BIT_WORD(SW_KEYPAD_SLIDE)] =
					BIT_MASK(SW_KEYPAD_SLIDE) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_SWBIT,
		.evbit = { BIT_MASK(EV_SW) },
		.swbit = { [BIT_WORD(SW_KEYPAD_TRANSITION)] =
					BIT_MASK(SW_KEYPAD_TRANSITION) },
	},
	{ },			/* Terminating zero entry */
};

MODULE_DEVICE_TABLE(input, evdev_ids);

static ssize_t stmpe_keypad_store_kl(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count);
static ssize_t stmpe_keypad_show_event_log(struct device *dev,
				struct device_attribute *attr, char *buf);
static ssize_t stmpe_keypad_show_counters(struct device *dev,
				struct device_attribute *attr, char *buf);
static ssize_t stmpe_keypad_store_enabled(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count);
static ssize_t stmpe_keypad_show_enabled(struct device *dev,
				struct device_attribute *attr, char *buf);
static ssize_t stmpe_keypad_show_keys(struct device *dev,
				struct device_attribute *attr, char *buf);
static ssize_t stmpe_keypad_show_reset_count(struct device *dev,
				struct device_attribute *attr, char *buf);
static ssize_t stmpe_keypad_show_status(struct device *dev,
				struct device_attribute *attr, char *buf);
static void stmpe_keypad_reset_counters(struct stmpe_keypad *keypad);
static void stmpe_input_event(struct input_handle *handle,
				unsigned int type,
				unsigned int code, int value);
static int stmpe_input_event_connect(struct input_handler *handler,
					struct input_dev *dev,
					const struct input_device_id *id);
static void stmpe_input_event_disconnect(struct input_handle *handle);
int stmpe_keypad_check_status(struct stmpe_keypad *keypad);
static irqreturn_t stmpe_keypad_irq_handler(int irq, void *dev);
static irqreturn_t stmpe_reset_irq_handler(int irq, void *dev);
static int stmpe_enable_keypad(struct stmpe_keypad *keypad, bool enable);
static int stmpe_keypad_status(struct stmpe_keypad *keypad);

static struct input_handler input_event_handler = {
	.event		= stmpe_input_event,
	.connect	= stmpe_input_event_connect,
	.disconnect	= stmpe_input_event_disconnect,
	.minor		= 0,
	.name		= "stmpe_keypad",
	.id_table	= input_event_ids,
};

static DEVICE_ATTR(kl, S_IWUSR | S_IWGRP, NULL, stmpe_keypad_store_kl);
static DEVICE_ATTR(keys, S_IRUSR | S_IRGRP, stmpe_keypad_show_keys, NULL);
static DEVICE_ATTR(status, S_IRUSR | S_IRGRP, stmpe_keypad_show_status, NULL);
static DEVICE_ATTR(reset_count, S_IRUSR | S_IRGRP,
					stmpe_keypad_show_reset_count, NULL);
static DEVICE_ATTR(enabled, S_IRUSR | S_IRGRP | S_IWUSR | S_IWGRP,
					stmpe_keypad_show_enabled,
					stmpe_keypad_store_enabled);
static DEVICE_ATTR(keypad_counters, S_IRUSR | S_IRGRP,
					stmpe_keypad_show_counters, NULL);
static DEVICE_ATTR(event_log, S_IRUSR | S_IRGRP,
					stmpe_keypad_show_event_log, NULL);

static struct attribute *stmpe_keypad_attrs[] = {
	&dev_attr_kl.attr,
	&dev_attr_enabled.attr,
	&dev_attr_keys.attr,
	&dev_attr_reset_count.attr,
	&dev_attr_status.attr,
	&dev_attr_keypad_counters.attr,
	&dev_attr_event_log.attr,
	NULL,
};

static struct attribute_group stmpe_keypad_attr_group = {
	.attrs = stmpe_keypad_attrs,
};

static ssize_t stmpe_keypad_store_kl(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct stmpe_keypad *keypad = dev_get_drvdata(dev);
	uint32_t kl = 0;
	uint8_t max_num = (sizeof(keypad_name)/sizeof(char *));
	int ret;

	ret = sscanf(buf, "%u *", &kl);
	if (ret >= 1)
		keypad->input_dev->name = ((kl > 0) && (kl <= max_num)) ?
					keypad_name[kl-1] : keypad_name[0];
	else
		dev_err(&keypad->input_dev->dev, "sscanf returned %d\n", ret);

	return count;
}

static ssize_t stmpe_keypad_store_enabled(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct stmpe_keypad *keypad = dev_get_drvdata(dev);
	int enable;

	if (sscanf(buf, "%u", &enable) != 1)
		return -EINVAL;

	if (stmpe_enable_keypad(keypad, enable) == -1)
		return -EINVAL;

	return count;
}

static ssize_t stmpe_keypad_show_enabled(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct stmpe_keypad *keypad = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", keypad->keypad_enabled);
}

static ssize_t stmpe_keypad_show_keys(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	ssize_t  size = 0;
	int i;
	struct stmpe_keypad *keypad = dev_get_drvdata(dev);

	for (i = 0; i < KEYPAD_MAX_KEYS; i++) {
		if (!keypad->keys[i].up)
			size += snprintf(buf + size, PAGE_SIZE - size, "0x%X\n", i);
	}

	return size;
}

static ssize_t stmpe_keypad_show_reset_count(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct stmpe_keypad *keypad = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", keypad->counters.reset_count);
}

static ssize_t stmpe_keypad_show_status(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct stmpe_keypad *keypad = dev_get_drvdata(dev);

	if (keypad->keypad_enabled)
		return snprintf(buf, PAGE_SIZE, "%d\n", stmpe_keypad_status(keypad));
	else
		return snprintf(buf, PAGE_SIZE, "%d\n", 0);
}

static ssize_t stmpe_keypad_show_counters(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct stmpe_keypad *keypad = dev_get_drvdata(dev);
	ssize_t  size = 0;
	int i;
	int tot_adjacent = 0;
	int tot_inadvertent = 0;

	buf[0] = 0;
	size += snprintf(buf, PAGE_SIZE, "reset count = %d\n",
						keypad->counters.reset_count);
	size += snprintf(buf + size, PAGE_SIZE - size,
			"stuck keys = %d\n", keypad->counters.stuck_keys);
	size += snprintf(buf + size, PAGE_SIZE - size,
			"total keys pressed = %d\n",
						keypad->counters.keys_pressed);
	size += snprintf(buf + size, PAGE_SIZE - size,
			"total multi-key = %d\n", keypad->counters.multi_key);
	size += snprintf(buf + size, PAGE_SIZE - size,
				"total extra_keys = %d\n",
						keypad->counters.extra_key);
	size += snprintf(buf + size, PAGE_SIZE - size,
			"total down keys = %d\n", keypad->down_keys);
	size += snprintf(buf + size, PAGE_SIZE - size,
			"total interrupts = %d\n",
						keypad->counters.interrupts);
	size += snprintf(buf + size, PAGE_SIZE - size,
			"longest key press = %ld\n",
					keypad->counters.high_key_time);
	size += snprintf(buf + size, PAGE_SIZE - size,
			"shortest key press = %ld\n",
						keypad->counters.low_key_time);
	size += snprintf(buf + size, PAGE_SIZE - size, "avg key press = %ld\n",
		(keypad->counters.key_time / keypad->counters.keys_pressed));
	size += snprintf(buf + size, PAGE_SIZE - size,
			"total key time = %ld\n", keypad->counters.key_time);

	for (i = 0; i < KEYPAD_MAX_KEYS; i++) {
		tot_adjacent += keypad->keys[i].adjacent_detected;
		tot_inadvertent += keypad->keys[i].inadvertent;
	}

	size += snprintf(buf + size, PAGE_SIZE - size,
			"total adjacent key = %d\n", tot_adjacent);
	size += snprintf(buf + size, PAGE_SIZE - size,
			"total inadvertent = %d\n", tot_inadvertent);

	for (i = 0; i < KEYPAD_MAX_KEYS; i++) {
		if (keypad->keys[i].inadv_check) {
			size += snprintf(buf + size, PAGE_SIZE - size,
					"total inadv key 0x%X detected = %d\n",
					i, keypad->keys[i].adjacent_detected);
			size += snprintf(buf + size, PAGE_SIZE - size,
					"total inadv key 0x%X blocked = %d\n",
					i, keypad->keys[i].inadvertent);
		}
	}

	for (i = 0; i < KEYPAD_MAX_KEYS; i++)
		if (keypad->keys[i].send_count)
			size += snprintf(buf + size, PAGE_SIZE - size,
					"total 0x%X key pressed = %d\n",
					i, keypad->keys[i].count);

	stmpe_keypad_reset_counters(keypad);

	return size;
}

static void stmpe_keypad_reset_counters(struct stmpe_keypad *keypad)
{
	int i;

	for (i = 0; i < KEYPAD_MAX_KEYS; i++) {
		keypad->keys[i].count = 0;
		keypad->keys[i].adjacent_detected = 0;
		keypad->keys[i].inadvertent = 0;
	}
	keypad->counters.interrupts = 0;
	keypad->counters.keys_pressed = 0;
	keypad->counters.multi_key = 0;
	keypad->counters.reset_count = 0;
	keypad->counters.stuck_keys = 0;
	keypad->counters.extra_key = 0;
	keypad->counters.high_key_time = 0;
	keypad->counters.low_key_time = 0;
	keypad->counters.key_time = 0;

}

void stmpe_update_key_counters(struct stmpe_keypad *keypad, uint8_t key,
				key_event_type_t type)
{
	long key_duration;

	if (type == KEY_EVENT_DOWN) {
		keypad->keys[key].count++;
		keypad->down_keys++;
		keypad->counters.keys_pressed++;
		do_gettimeofday(&keypad->keys[key].down_time);
	} else {
		keypad->down_keys--;
		do_gettimeofday(&keypad->keys[key].up_time);
		key_duration = ((keypad->keys[key].up_time.tv_sec -
				keypad->keys[key].down_time.tv_sec) * 1000) +
			((keypad->keys[key].up_time.tv_usec -
				keypad->keys[key].down_time.tv_usec) / 1000);

		keypad->counters.key_time += key_duration;

		if (key_duration > keypad->counters.high_key_time)
			keypad->counters.high_key_time = key_duration;

		if ((key_duration < keypad->counters.low_key_time) ||
				(keypad->counters.low_key_time == 0))
			keypad->counters.low_key_time = key_duration;

	}
}

static ssize_t stmpe_keypad_show_event_log(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct stmpe_keypad *keypad = dev_get_drvdata(dev);
	ssize_t  size = 0;
	int i, n;
	static uint8_t rand;

	if (!rand)
		get_random_bytes(&rand, sizeof(uint8_t));

	buf[0] = 0;
	for (n = 0; n < KEYPAD_EVENT_LOG_SIZE; n++) {
		i = (n + keypad->log_idx) % KEYPAD_EVENT_LOG_SIZE;

		if (keypad->event_log[i].key != 0)
			size += snprintf(buf + size, PAGE_SIZE - size,
				"0x%04X %s %ld.%06ld\n",
				(keypad->event_log[i].key + rand),
				(keypad->event_log[i].type ==
					KEY_EVENT_DOWN ? "DOWN" : "UP"),
				keypad->event_log[i].time.tv_sec,
				keypad->event_log[i].time.tv_usec);
	}

	return size;
}

void stmpe_log_key_event(struct stmpe_keypad *keypad, uint8_t key,
					key_event_type_t type)
{
	struct log_entry *entry = &keypad->event_log[keypad->log_idx];

	entry->key = key;
	entry->type = type;
	if (type == KEY_EVENT_DOWN)
		memcpy(&entry->time, &keypad->keys[key].down_time,
			 sizeof(struct timeval));
	else
		memcpy(&entry->time, &keypad->keys[key].up_time,
			 sizeof(struct timeval));

	keypad->log_idx = (keypad->log_idx + 1) % KEYPAD_EVENT_LOG_SIZE;
}

int stmpe_store_adjacent(struct stmpe_keypad *keypad)
{
	int row, col;
	uint8_t key;
	struct key_data *key_data;

	for (row = 0; row < KEYPAD_MAX_ROWS; row++) {
		for (col = 0; col < KEYPAD_MAX_COLS; col++) {
			key = keypad->key_table[row][col];
			key_data = &keypad->keys[key];
			if (row > 0)
				key_data->adjacent[key_data->num_adjacent++] =
						keypad->key_table[row-1][col];

			if (row < (KEYPAD_MAX_ROWS-1))
				key_data->adjacent[key_data->num_adjacent++] =
						keypad->key_table[row+1][col];

			if (col > 0 && (keypad->key_table[row][col-1] != key))
				key_data->adjacent[key_data->num_adjacent++] =
						keypad->key_table[row][col-1];

			if (col < (KEYPAD_MAX_COLS-1) &&
					(keypad->key_table[row][col+1] != key))
				key_data->adjacent[key_data->num_adjacent++] =
						keypad->key_table[row][col+1];
		}
	}

	return 0;
}
uint8_t stmpe_adjacent_down(struct stmpe_keypad *keypad, uint8_t key)
{
	int i;

	for (i = 0; i < keypad->keys[key].num_adjacent; i++)
		if (!keypad->keys[keypad->keys[key].adjacent[i]].up)
			return keypad->keys[key].adjacent[i];

	return 0;
}

static int read_block(struct stmpe_keypad *keypad,
			uint8_t reg, uint8_t length, uint8_t *out)
{
	struct i2c_msg msg[] = {
		{
			.addr = keypad->i2c_client->addr,
			.flags = 0,
			.len = 1,
			.buf = &reg,
		},
		{
			.addr = keypad->i2c_client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = out,
		},
	};
	int i, ret;

	ret = i2c_transfer(keypad->i2c_client->adapter, msg, 2);
	if (ret < 0 || ret != 2) {
		error("i2c error while reading register 0x%x %d", reg, ret);
		return -EINVAL;
	}
	debug("read block %d bytes at 0x%x", length, reg);
	for (i = 0; i < length; i++)
		debug("  [0x%x] = 0x%x", reg + i, out[i]);

	return 0;
}

static int write_block(struct stmpe_keypad *keypad,
			uint8_t addr, uint8_t length, uint8_t *data)
{
	int ret;
	unsigned char buf[length + 1];
	struct i2c_msg msg[] = {
		{
			.addr = keypad->i2c_client->addr,
			.flags = 0,
			.len = sizeof(data),
			.buf = buf,
		}
	};
	buf[0] = addr;
	memcpy(&buf[1], &data[0], length);

	ret = i2c_transfer(keypad->i2c_client->adapter, msg, 1);
	if (ret < 0 || ret != 1)
		error("i2c error while writing block to adress 0x%x %d",
								addr, ret);

	return 0;
}

static int read_reg(struct stmpe_keypad *keypad, uint8_t reg)
{
	uint8_t data[1] = {0}; /* read buf */
	struct i2c_msg msg[] = {
		{
			.addr = keypad->i2c_client->addr,
			.flags = 0,
			.len = 1,
			.buf = &reg,
		},
		{
			.addr = keypad->i2c_client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = data,
		},
	};
	int ret;


	ret = i2c_transfer(keypad->i2c_client->adapter, msg, 2);
	if (ret < 0 || ret != 2) {
		error("i2c error while reading register 0x%x %d", reg, ret);
		return -EINVAL;
	}

	return data[0];
}

static int write_reg(struct stmpe_keypad *keypad, uint8_t reg, uint8_t val)
{
	uint8_t data[2] = { reg, val };
	struct i2c_msg msg[] = {
		{
			.addr = keypad->i2c_client->addr,
			.flags = 0,
			.len = sizeof(data),
			.buf = data,
		}
	};
	int ret;

	ret = i2c_transfer(keypad->i2c_client->adapter, msg, 1);
	if (ret < 0 || ret != 1)
		error("i2c error while writing register 0x%x %d", reg, ret);

	return 0;
}

static int stmpe_keypad_status(struct stmpe_keypad *keypad)
{
	int reg = 0;

	mutex_lock(&keypad->keypad_mutex);
	reg = read_reg(keypad, REG_INT_EN_MASK);
	mutex_unlock(&keypad->keypad_mutex);

	return (reg == (INT_I1_KEYPAD|INT_I2_KEYPAD_OVERFLOW)) ? 1 : 0;
}

static int stmpe_power_enable(struct stmpe_keypad *keypad, bool enable)
{
	int ret = 0;

	if (enable) {
		ret = regulator_enable(keypad->regulator_vdd);
		if (ret) {
			error("Failed to enable vcc regulator, %d", ret);
			return ret;
		}

		ret = gpio_direction_output(keypad->config.reset_gpio, 1);
		if (ret < 0) {
			error("Failed to configure reset GPIO %d", ret);
			return ret;
		}

		msleep(20);

	} else {
		ret = regulator_disable(keypad->regulator_vdd);
		if (ret) {
			error("Failed to disable vcc regulator, %d", ret);
			return ret;
		}

		ret = gpio_direction_output(keypad->config.reset_gpio, 0);
		if (ret < 0) {
			error("Failed to configure reset GPIO %d", ret);
			return ret;
		}
	}

	return ret;
}

static int stmpe_irq_enable(struct stmpe_keypad *keypad, bool enable)
{
	int ret = 0;

	if (enable) {
		ret = request_threaded_irq(keypad->ctrl_irq, NULL,
						stmpe_keypad_irq_handler,
						IRQF_TRIGGER_LOW|IRQF_ONESHOT,
						"stmpe-keypad", keypad);
		if (ret < 0) {
			error("Failed to create irq thread, %d", ret);
			return ret;
		}

		if (keypad->reset_detect == true) {
			ret = request_threaded_irq(keypad->reset_irq, NULL,
					stmpe_reset_irq_handler,
					IRQF_TRIGGER_FALLING|IRQF_ONESHOT,
					"stmpe-keypad-reset", keypad);
			if (ret < 0)
				error("Failed to attach reset int handler");
		}

		keypad->irq_enabled = true;
	} else {
		if (keypad->irq_enabled) {
			disable_irq(keypad->ctrl_irq);
			free_irq(keypad->ctrl_irq, keypad);
			if (keypad->reset_detect == true) {
				disable_irq(keypad->reset_irq);
				free_irq(keypad->reset_irq, keypad);
			}
			keypad->irq_enabled = false;
		}
	}

	return 0;
}

static void stmpe_input_event(struct input_handle *handle, unsigned int type,
						unsigned int code, int value)
{
	struct stmpe_keypad *keypad = handle->private;

	if (type == EV_SW) {
		switch (code) {
		case SW_KEYPAD_SLIDE:
			keypad->slide_open = value;
			break;
		case SW_KEYPAD_TRANSITION:
			keypad->slide_transitioning = value;
			break;
		default:
			return;
			break;
		}

		queue_work(keypad->workqueue,
				&keypad->input_work);
	}
}

static int stmpe_input_event_connect(struct input_handler *handler,
					struct input_dev *dev,
					const struct input_device_id *id)
{
	int ret;
	struct input_handle *handle;
	struct stmpe_keypad *keypad = handler->private;

	if (test_bit(SW_KEYPAD_SLIDE, dev->swbit)) {
		if (test_bit(SW_KEYPAD_SLIDE, dev->sw)) {
			keypad->slide_open = 1;
			queue_work(keypad->workqueue, &keypad->input_work);
		}
	}

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->private = handler->private;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "stmpe_keypad";

	ret = input_register_handle(handle);
	if (ret)
		goto err_input_register_handle;

	ret = input_open_device(handle);
	if (ret)
		goto err_input_open_device;

	return 0;

err_input_open_device:
	input_unregister_handle(handle);
err_input_register_handle:
	kfree(handle);
	return ret;
}

static void stmpe_input_event_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static int stmpe_keypad_configure(struct stmpe_keypad *keypad)
{
	int ret = 0;
	u8 scan_frequency;

	mutex_lock(&keypad->keypad_mutex);

	/* Configure keypad controller */
	if (keypad->reset_detect == true) {
		ret = write_reg(keypad, REG_GPIO_SET_DIR_LOW,
					(1 << keypad->config.reset_det_gpio));
		if (ret < 0)
			goto unlock_ret;

		ret = write_reg(keypad, REG_GPIO_SET_LOW,
					(1 << keypad->config.reset_det_gpio));
		if (ret < 0)
			goto unlock_ret;
	}

	ret = write_reg(keypad, REG_KPC_ROW, keypad->config.row_mask);
	if (ret < 0)
		goto unlock_ret;

	ret = write_block(keypad, REG_KPC_COL, 2,
				(uint8_t *) &keypad->config.column_mask);
	if (ret < 0)
		goto unlock_ret;

	ret = write_reg(keypad, REG_KPC_CTRL_LOW,
				(u8) (keypad->config.scan_count << 4));
	if (ret < 0)
		goto unlock_ret;

	ret = write_reg(keypad, REG_KPC_CTRL_MID,
				(u8) (keypad->config.debounce << 1));
	if (ret < 0)
		goto unlock_ret;

	switch(keypad->config.scan_frequency) {
	case 60:
		scan_frequency = KPC_SCAN_FREQ_60;
		break;
	case 30:
		scan_frequency = KPC_SCAN_FREQ_30;
		break;
	case 15:
		scan_frequency = KPC_SCAN_FREQ_15;
		break;
	case 275:
		scan_frequency = KPC_SCAN_FREQ_275;
		break;
	default:
		scan_frequency = KPC_SCAN_FREQ_60;
		error("Unsupported keypad scan frequency %d",
					keypad->config.scan_frequency);
		break;
	}
	ret = write_reg(keypad, REG_KPC_CTRL_HIGH, scan_frequency);
	if (ret < 0)
		goto unlock_ret;

	/* Set INT_EN_MASK */
	ret = write_reg(keypad, REG_INT_EN_MASK,
				INT_I1_KEYPAD|INT_I2_KEYPAD_OVERFLOW);
	if (ret < 0)
		goto unlock_ret;

	/* Set polarity/type and enable global ints (INT_CTRL) */
	ret = write_reg(keypad, REG_INT_CTRL, INT_CTRL_MASK);
	if (ret < 0)
		goto unlock_ret;

	ret = write_reg(keypad, REG_KPC_CMD, KPC_CMD_SCAN_ENABLE);
	if (ret < 0)
		goto unlock_ret;

unlock_ret:
	mutex_unlock(&keypad->keypad_mutex);

	return ret;
}

int stmpe_inject_key(struct stmpe_keypad *keypad, uint8_t key,
						key_event_type_t type)
{
	input_event(keypad->input_dev, EV_MSC, MSC_SCAN,
					keypad->keys[key].code);

	input_report_key(keypad->input_dev, key, type);

	input_sync(keypad->input_dev);


	return 0;
}

int stmpe_handle_keypress(struct stmpe_keypad *keypad)
{
	int ret = 0;
	uint8_t data[KPC_DATA_LENGTH];
	int i, num_events;
	bool inject_key;
	int pressed = 0;
	int released = 0;
	uint8_t adjacent_key;
	key_event_type_t type;
	uint8_t row, col, code, key;
	struct key_event_data key_data[KEYPAD_MAX_KEY_BUFFER];

	do {
		num_events = 0;
		ret = read_block(keypad, REG_KPC_DATA, KPC_DATA_LENGTH, data);
		if (ret < 0) {
			error("failed to read keypad data");
			return IRQ_NONE;
		}

		for (i = 0; i < KEYPAD_MAX_KEY_BUFFER; i++) {
			if (KPC_DATA_NO_KEY_MASK !=
				(data[i] & KPC_DATA_NO_KEY_MASK)) {

				type = data[i] & 0x80 ? KEY_EVENT_UP :
							KEY_EVENT_DOWN;
				row = data[i] & 0x07;
				col = (data[i] >> 3) & 0x0f;
				code = MATRIX_SCAN_CODE(row, col,
							KEYPAD_ROW_SHIFT);
				key = keypad->keymap[code];

				/* Check if key already reported */
				if ((type == KEY_EVENT_UP) ||
					((type == KEY_EVENT_DOWN) &&
						keypad->keys[key].up)) {
					key_data[num_events].type = type;
					key_data[num_events].row = row;
					key_data[num_events].col = col;
					key_data[num_events].code = code;
					key_data[num_events].key = key;
					keypad->keys[key].up =
						(type == KEY_EVENT_UP ?
								true : false);
					num_events++;
				}
			}
		}

		for (i = 0; i < num_events; i++) {
			inject_key = true;
			adjacent_key = 0;

			debug("key:0x%X %s row:%d col:%d", key_data[i].key,
				(key_data[i].type == KEY_EVENT_UP ? "UP" : "DOWN"),
					key_data[i].row, key_data[i].col);

			if (key_data[i].type == KEY_EVENT_DOWN) {
				pressed++;
				adjacent_key =  stmpe_adjacent_down(keypad,
							key_data[i].key);

				/* Check for inadvertent key press */
				if (adjacent_key) {
					info("Adjacent key: down: 0x%X 0x%X",
							key_data[i].key, adjacent_key);
					keypad->keys[key_data[i].key].adjacent_detected++;

					if ((keypad->keys[key_data[i].key].inadv_check == true)
							&& (!keypad->keys[adjacent_key].modifier)) {
						inject_key = false;
						keypad->pending_inadv_key = key_data[i].code;
						mod_timer(&keypad->inadv_timer, (jiffies +
								msecs_to_jiffies(keypad->inadv_timeout)));
					}
				}
			} else {
				released++;
				/* Cancel inadvertent key timer on release */
				if (key_data[i].code ==
						keypad->pending_inadv_key) {
					del_timer(&keypad->inadv_timer);
					keypad->pending_inadv_key = 0;
					keypad->keys[key_data[i].key].inadvertent++;
					info("Adjacent key: inadvertent");
					inject_key = false;
				}
			}
			stmpe_log_key_event(keypad, key_data[i].key,
							key_data[i].type);

			stmpe_update_key_counters(keypad, key_data[i].key,
					key_data[i].type);

			if (inject_key) {
				stmpe_inject_key(keypad,
						key_data[i].key,
						key_data[i].type);
			}

			do_gettimeofday(&keypad->last_keypress);

			/* Start the stats timer */
			if (!timer_pending(&keypad->timer))
				mod_timer(&keypad->timer,
					(jiffies +
					msecs_to_jiffies(
					KEYPAD_STATS_TIMER)));
		}

	} while (num_events > 0);

	/* Update final counters */
	if ((pressed + released) > 1)
		keypad->counters.extra_key += ((pressed + released) - 1);

	if ((pressed > 0)  && (keypad->down_keys > 1))
		keypad->counters.multi_key++;

	return ret;
}


static int stmpe_keypad_process_events(struct stmpe_keypad *keypad)
{
	int status, ret = 0;

	mutex_lock(&keypad->keypad_mutex);
	/* Read the interrupt status */
	status = read_reg(keypad, REG_INT_STA);

	if (status < 0) {
		error("Failed reading interrupt status %d", status);
		goto unlock_ret;
	}
	if (0 == status) {
		warn("No interrupt source");
		goto unlock_ret;
	}

	/* Handle overflow interrupt */
	if (status & INT_I2_KEYPAD_OVERFLOW)
		error("Keypad overflow interrupt");

	/* Handle keypad interrupt */
	if (status & INT_I1_KEYPAD)
		ret = stmpe_handle_keypress(keypad);

unlock_ret:
	mutex_unlock(&keypad->keypad_mutex);

	return ret;
}

static irqreturn_t stmpe_keypad_irq_handler(int irq, void *dev)
{
	struct stmpe_keypad *keypad = dev;
	int ret;

	keypad->counters.interrupts++;

	ret = stmpe_keypad_process_events(keypad);
	if (ret < 0) {
		warn("Error handling keypad interrupt");
		return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static irqreturn_t stmpe_reset_irq_handler(int irq, void *dev)
{
	struct stmpe_keypad *keypad = dev;

	stmpe_keypad_check_status(keypad);

	return IRQ_HANDLED;
}


int stmpe_keypad_check_status(struct stmpe_keypad *keypad)
{
#ifdef CONFIG_STMPE_KEYPAD_DDT
	struct logworthy_event_details_t details;
	int ret = 0;
#endif

	if (!stmpe_keypad_status(keypad)) {
		keypad->counters.reset_count++;
		error("Keypad reset!! Count: %d",
				keypad->counters.reset_count);

#ifdef CONFIG_STMPE_KEYPAD_DDT
		if (keypad->counters.reset_count < MAX_RESET_DDT_SEND) {
			memset(&details, 0,
				sizeof(struct logworthy_event_details_t));
			details.d1 = keypad->counters.reset_count;
			details.creator_id = (char *)keypad->input_dev->name;
			ret = ddt_send(LW_EVENT_KEYPAD_UNEXPECTED_RESET, &details,
					"['logcat://main',"
					"'exec:///system/bin/dmesg']");
			error("ddt send event");
			if (ret < 0)
				error("error sending: %d", ret);
		}
#endif

		/* toggle power */
		stmpe_power_enable(keypad, false);
		msleep_interruptible(20);
		stmpe_power_enable(keypad, true);

		while (read_reg(keypad, REG_CHIP_ID) != CHIP_ID) {
			error("Unable to communicate with Keypad IC");
			msleep_interruptible(1000);
		}
		stmpe_keypad_configure(keypad);
		error("Keypad Controller - Reconfigured");
	}

	return 0;
}

static void stmpe_keypad_inadv_handler(unsigned long data)
{
	struct stmpe_keypad *keypad = (struct stmpe_keypad *) data;
	uint8_t key;

	if (keypad->pending_inadv_key) {
		key = keypad->keymap[keypad->pending_inadv_key];
		if (keypad->keys[key].inadv_check) {
			info("Adjacent key: inject key");
			stmpe_inject_key(keypad, key, KEY_EVENT_DOWN);
		}
		keypad->pending_inadv_key = 0;
	}
}

static void stmpe_keypad_timer_handler(unsigned long data)
{
	struct stmpe_keypad *keypad = (struct stmpe_keypad *) data;
	struct timeval current_time;
	int last_kp = 0;
	int inv_key_count = 0;
	int i;
	static bool stuck_key;


	for (i = 0; i < KEYPAD_MAX_KEYS; i++)
		if (keypad->keys[i].inadv_check)
			inv_key_count += keypad->keys[i].adjacent_detected;

	info("STATS: ints: %d ex:%d kp: %d mk:%d sk:%d rc: %d dc: %d ik: %d",
						keypad->counters.interrupts,
						keypad->counters.extra_key,
						keypad->counters.keys_pressed,
						keypad->counters.multi_key,
						keypad->counters.stuck_keys,
						keypad->counters.reset_count,
						keypad->down_keys,
						inv_key_count);
	do_gettimeofday(&current_time);
	last_kp = current_time.tv_sec - keypad->last_keypress.tv_sec;

	if ((last_kp < 5) || keypad->down_keys > 0)
		mod_timer(&keypad->timer, (jiffies + msecs_to_jiffies(
							KEYPAD_STATS_TIMER)));

	if (keypad->down_keys > 0) {
		if (last_kp >= KEYPAD_STUCK_KEY_TIME) {
			if (!stuck_key) {
				stuck_key = true;
				keypad->counters.stuck_keys++;
			}
			error("Key(s) down for > 10 sec.");
			for (i = 0; i < KEYPAD_MAX_KEYS; i++) {
				if (!keypad->keys[i].up)
					error("Stuck key? 0x%X", i);
			}
		}
	} else
		stuck_key = false;
}

static void stmp_keypad_input_wq(struct work_struct *work)
{
	struct stmpe_keypad *keypad =
			container_of(work, struct stmpe_keypad,
							input_work);
	stmpe_enable_keypad(keypad,
			(keypad->slide_open && !keypad->slide_transitioning));
}

static int stmpe_enable_keypad(struct stmpe_keypad *keypad, bool enable)
{
	int ret = 0;
	int i;

	if (enable) {
		if (!keypad->keypad_enabled) {
			ret = stmpe_power_enable(keypad, true);
			if (ret < 0)
				error("Failed to enable keypad power: %d\n",
									ret);

			ret = stmpe_keypad_configure(keypad);
			if (ret < 0)
				error("Failed to configure the keypad: %d\n",
									ret);

			ret = stmpe_irq_enable(keypad, true);
			if (ret < 0)
				error("Failed to enable the keypad IRQ: %d\n",
									ret);

			keypad->keypad_enabled = true;
		}
	} else {
		if (keypad->keypad_enabled) {
			if (timer_pending(&keypad->timer))
				del_timer(&keypad->timer);

			ret = stmpe_irq_enable(keypad, false);
			if (ret < 0)
				error("Failed to disable the keypad IRQ: %d\n",
									ret);

			ret = stmpe_power_enable(keypad, false);
			if (ret < 0) {
				error("Failed to disable keypad power: %d\n",
									ret);
			}
			/* Send key release for all down keys */
			for (i = 0; i < KEYPAD_MAX_KEYS; i++) {
				if (keypad->keys[i].up == false) {
					keypad->keys[i].up = true;
					stmpe_inject_key(keypad, i,
								KEY_EVENT_UP);
					stmpe_log_key_event(keypad, i,
								KEY_EVENT_UP);
					stmpe_update_key_counters(keypad, i,
								KEY_EVENT_UP);
				}
			}

			keypad->keypad_enabled = false;
		}
	}
	return ret;
}
static int stmpe_keypad_parse_dt(struct device *dev,
					struct stmpe_keypad *keypad)
{
	struct device_node *np = dev->of_node;
	int ret;
	const __be32 *prop_data;
	unsigned int prop_data_len;
	int i, size;
	int row, col;
	unsigned int key;

	/* Retrieve device tree data */
	ret = of_property_read_u8_array(np, "st,row-mask",
					&keypad->config.row_mask, 1);
	if (ret) {
		error("Failed to retrieve row-mask, %d", ret);
		return -EINVAL;
	}

	ret = of_property_read_u16(np, "st,column-mask",
					&keypad->config.column_mask);
	if (ret) {
		error("Failed to retrieve column-mask, %d", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "st,scan-count",
					&keypad->config.scan_count);
	if (ret) {
		error("Failed to retrieve scan-count, %d", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "st,debounce",
					&keypad->config.debounce);
	if (ret) {
		error("Failed to retrieve debounce, %d", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "st,scan-frequency",
					&keypad->config.scan_frequency);
	if (ret) {
		error("Failed to retrieve scan-frequency, %d", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "st,reset_det_gpio",
					&keypad->config.reset_det_gpio);
	if (!ret) {
		keypad->reset_detect = true;

		keypad->config.rst_int_gpio =
				of_get_named_gpio(np, "st,keypad-rst-int", 0);
		if (!gpio_is_valid(keypad->config.rst_int_gpio)) {
			error("Reset GPIO is invalid");
			keypad->reset_detect = false;
		}
	} else
		keypad->reset_detect = false;


	keypad->config.kp_int_gpio =
			of_get_named_gpio(np, "st,keypad-kp-int", 0);
	if (!gpio_is_valid(keypad->config.kp_int_gpio)) {
		error("Interrupt GPIO is invalid");
		return -EINVAL;
	}

	keypad->config.reset_gpio = of_get_named_gpio(np, "st,keypad-reset", 0);
	if (!gpio_is_valid(keypad->config.reset_gpio)) {
		error("Reset GPIO is invalid");
		return -EINVAL;
	}

	keypad->regulator_vdd =
			regulator_get(&keypad->i2c_client->dev, "st,vdd");
	if (IS_ERR(keypad->regulator_vdd)) {
		error("Failed to get regulator %d",
					(int)PTR_ERR(keypad->regulator_vdd));
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "st,inadv-timeout",
						&keypad->inadv_timeout);
	if (ret)
		keypad->inadv_timeout = 200;

	prop_data = of_get_property(np, "st,physical_keymap", &prop_data_len);
	if (prop_data != NULL) {

		if (prop_data_len % sizeof(u32)) {
			error("Malformed physical key table");
			return -EINVAL;
		}

		size = prop_data_len / sizeof(u32);
		if (size > KEYPAD_MAX_KEYS) {
			error("physical key table overflow");
			return -EINVAL;
		}

		for (i = 0; i < size; i++) {
			key = be32_to_cpup(prop_data + i);
			row = (KEY_ROW(key) - 1);
			col = (KEY_COL(key) - 1);
			keypad->key_table[row][col] = KEY_VAL(key);
		}
		stmpe_store_adjacent(keypad);
	} else {
		info("No physical key table. Adj. key check unavailable");
	}

	prop_data = of_get_property(np, "st,check-inadv", &prop_data_len);
	if (prop_data != NULL) {
		if (prop_data_len % sizeof(u32)) {
			error("Malformed inadvertent key table");
			return -EINVAL;
		}
		size = prop_data_len / sizeof(u32);
		for (i = 0; i < size; i++) {
			key = be32_to_cpup(prop_data + i);
			keypad->keys[key].inadv_check = true;
		}

	}

	prop_data = of_get_property(np, "st,modifier-keys", &prop_data_len);
	if (prop_data != NULL) {
		if (prop_data_len % sizeof(u32)) {
			error("Malformed modifier key table");
			return -EINVAL;
		}
		size = prop_data_len / sizeof(u32);
		for (i = 0; i < size; i++) {
			key = be32_to_cpup(prop_data + i);
			keypad->keys[key].modifier = true;
		}

	}

	prop_data = of_get_property(np, "st,key-count", &prop_data_len);
	if (prop_data != NULL) {
		if (prop_data_len % sizeof(u32)) {
			error("Malformed inadvertent key table");
			return -EINVAL;
		}
		size = prop_data_len / sizeof(u32);
		for (i = 0; i < size; i++) {
			key = be32_to_cpup(prop_data + i);
			keypad->keys[key].send_count = true;
		}
	}

	return 0;
}

static int stmpe_keypad_probe(struct i2c_client *i2c,
		const struct i2c_device_id *dev_id)
{
	struct stmpe_keypad *keypad;
	int ret;
	int i, n;

	/* Initialize the keypad structure */
	keypad = kzalloc(sizeof(*keypad), GFP_KERNEL);
	if (NULL == keypad) {
		error("Failed to allocate memory for keypad device");
		ret = -ENOMEM;
		goto fail;
	}

	keypad->slide_transitioning = 0;
	keypad->i2c_client = i2c;
	keypad->keypad_enabled = false;
	keypad->down_keys = 0;
	for (i = 0; i < KEYPAD_MAX_ROWS; i++)
		for (n = 0; n < KEYPAD_MAX_COLS; n++)
			keypad->key_table[i][n] = 0;

	for (i = 0; i < KEYPAD_MAX_KEYS; i++) {
		keypad->keys[i].key = 0;
		keypad->keys[i].up = true;
		keypad->keys[i].inadv_check = false;
		keypad->keys[i].modifier = false;
		keypad->keys[i].adjacent_detected = 0;
		keypad->keys[i].inadvertent = 0;
		keypad->keys[i].num_adjacent = 0;
		keypad->keys[i].send_count = false;
		keypad->keys[i].count = 0;
	}

	for (i = 0; i < KEYPAD_EVENT_LOG_SIZE; i++)
		keypad->event_log[i].key = 0;

	keypad->log_idx = 0;

	stmpe_keypad_reset_counters(keypad);

	mutex_init(&keypad->keypad_mutex);

	dev_set_drvdata(&i2c->dev, keypad);

	ret = stmpe_keypad_parse_dt(&i2c->dev, keypad);
	if (ret < 0) {
		error("Failed to parse device tree parameters %d\n", ret);
		goto fail_free;
	}

	ret = gpio_request(keypad->config.reset_gpio, "keypad-rst");
	if (ret < 0) {
		error("Failed to request reset GPIO %d, error %d\n",
				keypad->config.reset_gpio, ret);
		goto fail_free;
	}

	/* Set up irq gpio */
	ret = gpio_request(keypad->config.kp_int_gpio, "keypad-int");
	if (ret < 0) {
		error("Failed to request interrupt GPIO %d, error %d\n",
					keypad->config.kp_int_gpio, ret);
		goto fail_free;
	}

	keypad->ctrl_irq = gpio_to_irq(keypad->config.kp_int_gpio);

	/* Set up the reset detect interrupt */
	if (keypad->reset_detect == true) {
		ret = gpio_request(keypad->config.rst_int_gpio, "reset-int");
		if (ret < 0) {
			error("Failed to request reset int GPIO %d, err %d\n",
					keypad->config.rst_int_gpio, ret);
		}

		keypad->reset_irq = gpio_to_irq(keypad->config.rst_int_gpio);
	}

	/* Enable keypad power */
	ret = stmpe_power_enable(keypad, true);
	if (ret < 0) {
		error("Failed to initialize the keypad controller: %d\n", ret);
		goto fail_free;
	}
	/* Read out the chip ID. Verify version. */
	ret = read_reg(keypad, REG_CHIP_ID);
	if (ret < 0) {
		error("Unable to communicate with IC %d", ret);
		ret = -EAGAIN;
		goto fail_free;
	}
	if (CHIP_ID != ret) {
		error("Found unknown IC (chip ID = 0x%x)", ret);
		ret = -ENODEV;
		goto fail_free;
	}

	/* Disable power */
	ret = stmpe_power_enable(keypad, false);
	if (ret < 0) {
		error("Failed to disable power: %d\n", ret);
		goto fail_free;
	}

	/* Create sysfs entry for keypad language */
	ret = sysfs_create_group(&i2c->dev.kobj, &stmpe_keypad_attr_group);
	if (ret)
		dev_err(&i2c->dev, "Unable to create group. error: %d\n", ret);

	ret = sysfs_create_link(i2c->dev.kobj.parent->parent->parent->parent,
						&i2c->dev.kobj, "keypad");
	if (ret)
		dev_err(&i2c->dev, "Unable to create link, rc=%d\n", ret);

	/* Register as an input device */
	keypad->input_dev = input_allocate_device();
	if (NULL == keypad->input_dev) {
		error("Unable to register keypad as input device");
		ret = -ENOMEM;
		goto fail_free;;
	}

	keypad->input_dev->name = keypad_name[0];
	keypad->input_dev->id.bustype = BUS_I2C;
	keypad->input_dev->dev.parent = &i2c->dev;

	input_set_capability(keypad->input_dev, EV_MSC, MSC_SCAN);

	ret = input_register_device(keypad->input_dev);
	if (ret) {
		error("Unable to register input device : %d", ret);
		goto fail_input_free;
	}

	/* Build keypad matrix using device tree data */
	ret = matrix_keypad_build_keymap(NULL, "linux,keymap",
					KEYPAD_KEYMAP_ROWS,
					KEYPAD_KEYMAP_COLS,
					keypad->keymap, keypad->input_dev);
	if (ret)
		error("Unable build keymap : %d", ret);

	for (i = 0; i < KEYPAD_KEYMAP_SIZE; i++)
		keypad->keys[keypad->keymap[i]].code = i;

	/* Create input handler for slide events */
	input_event_handler.private = keypad;
	ret = input_register_handler(&input_event_handler);
	if (ret)
		dev_err(&i2c->dev,
			"Unable to register with input, rc=%d\n", ret);

	/* Set up work queue for input events */
	keypad->workqueue =
			create_singlethread_workqueue("background_workqueue");
	INIT_WORK(&keypad->input_work, stmp_keypad_input_wq);

	init_timer(&keypad->timer);
	keypad->timer.function = stmpe_keypad_timer_handler;
	keypad->timer.data = (unsigned long) keypad;

	init_timer(&keypad->inadv_timer);
	keypad->inadv_timer.function = stmpe_keypad_inadv_handler;
	keypad->inadv_timer.data = (unsigned long) keypad;

	return 0;

fail_input_free:
	input_free_device(keypad->input_dev);
fail_free:
	kfree(keypad);
fail:
	return ret;
}

static int stmpe_keypad_remove(struct i2c_client *client)
{
	dev_err(&client->dev, "%s: exiting stmpe-keypad!\n", __func__);
	return 0;
}

static const struct i2c_device_id stmpe_id_table[] = {
	{"stmpe-keypad", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, stmpe_id_table);

#ifdef CONFIG_OF
static struct of_device_id stmpe_match_table[] = {
	{ .compatible = "st,stmpe-keypad",},
	{ },
};
#else
#define stmpe_match_table NULL
#endif

static struct i2c_driver stmpe_keypad_driver = {
	.driver.name    = "stmpe-keypad",
	.driver.owner   = THIS_MODULE,
	.driver.of_match_table = stmpe_match_table,
	.probe      = stmpe_keypad_probe,
	.remove     = stmpe_keypad_remove,
	.id_table   = stmpe_id_table,
};

static int __init stmpe_kp_init(void)
{
	return i2c_add_driver(&stmpe_keypad_driver);
}

static void __exit stmpe_kp_exit(void)
{
	i2c_del_driver(&stmpe_keypad_driver);
}

module_init(stmpe_kp_init);
module_exit(stmpe_kp_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("STMPE keypad driver");
MODULE_AUTHOR("BlackBerry Limited");
