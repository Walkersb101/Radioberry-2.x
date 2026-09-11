// SPDX-License-Identifier: GPL-2.0-only
/*
 * Linux and RP1 glue for the deliberately small Radioberry TX-only driver.
 *
 * The data-path policy lives in tx_core.cpp.  This C file has the jobs that
 * should remain C in a Linux module: SPI binding, file operations, safe user
 * copies, locking and calls into the external RP1 PIO API.  Keeping this ABI
 * narrow also prevents C++ name mangling from leaking into kernel interfaces.
 */
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pio_rp1.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>

#include "bridge.h"
#include "tx_ioctl.h"

#define RB_MAX_BITS 4096U

static DEFINE_MUTEX(rb_lock);
static struct spi_device *rb_spi;
static struct rp1_pio_client *rb_pio;
static bool rb_opened;

/* ---------- Bridge functions called only by tx_core.cpp ---------- */

int rb_hw_open(void)
{
	if (rb_pio)
		return -EBUSY;
	rb_pio = rp1_pio_open();
	if (IS_ERR(rb_pio)) {
		int error = PTR_ERR(rb_pio);
		rb_pio = NULL;
		return error;
	}
	return rb_pio ? 0 : -ENODEV;
}

int rb_hw_claim(void)
{
	struct rp1_pio_sm_claim_args args = { .mask = 0 };

	return rb_pio ? rp1_pio_sm_claim(rb_pio, &args) : -ENODEV;
}

int rb_hw_program(const unsigned short *words, unsigned int count,
		  unsigned int origin)
{
	struct rp1_pio_add_program_args args = { .origin = origin };

	if (!rb_pio)
		return -ENODEV;
	if (!words || !count || count > ARRAY_SIZE(args.instrs))
		return -EINVAL;
	args.num_instrs = count;
	memcpy(args.instrs, words, count * sizeof(args.instrs[0]));
	/* On success the API returns the actual instruction-memory origin. */
	return rp1_pio_add_program(rb_pio, &args);
}

int rb_hw_pin(unsigned int pin, int output, int pulldown, unsigned int sm)
{
	struct rp1_gpio_init_args init = { .gpio = pin };
	struct rp1_gpio_set_function_args function = {
		.gpio = pin, .fn = GPIO_FUNC_PIO
	};
	struct rp1_gpio_set_pulls_args pulls = {
		.gpio = pin, .up = false, .down = !!pulldown
	};
	struct rp1_gpio_set_args input = { .gpio = pin, .value = !output };
	struct rp1_pio_sm_set_pindirs_args direction = {
		.sm = sm,
		.dirs = output ? BIT(pin) : 0,
		.mask = BIT(pin),
	};
	struct rp1_pio_sm_set_pins_args low = {
		.sm = sm, .values = 0, .mask = BIT(pin)
	};
	int rc;

	if (!rb_pio || pin >= RP1_PIO_GPIO_COUNT || sm >= NUM_PIO_STATE_MACHINES)
		return -EINVAL;
	rc = rp1_pio_gpio_init(rb_pio, &init);
	if (!rc)
		rc = rp1_pio_gpio_set_function(rb_pio, &function);
	if (!rc)
		rc = rp1_pio_gpio_set_pulls(rb_pio, &pulls);
	if (!rc)
		rc = rp1_pio_gpio_set_input_enabled(rb_pio, &input);
	if (!rc)
		rc = rp1_pio_sm_set_pindirs(rb_pio, &direction);
	if (!rc && output)
		rc = rp1_pio_sm_set_pins(rb_pio, &low);
	return rc;
}

void rb_hw_unpin(unsigned int pin)
{
	struct rp1_gpio_set_function_args function = {
		.gpio = pin, .fn = GPIO_FUNC_NULL
	};
	struct rp1_gpio_set_pulls_args pulls = {
		.gpio = pin, .up = false, .down = false
	};

	if (!rb_pio || pin >= RP1_PIO_GPIO_COUNT)
		return;
	/* Best effort on module removal: disconnect PIO, then remove our pull. */
	rp1_pio_gpio_set_function(rb_pio, &function);
	rp1_pio_gpio_set_pulls(rb_pio, &pulls);
}

int rb_hw_config(unsigned int sm, const struct rb_pio_config *config)
{
	struct rp1_pio_sm_init_args args;
	struct rp1_pio_sm_clear_fifos_args clear = { .sm = sm };
	int rc;

	if (!rb_pio || !config)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.sm = sm;
	args.initial_pc = 0;
	args.config.clkdiv = config->clkdiv;
	args.config.execctrl = config->execctrl;
	args.config.shiftctrl = config->shiftctrl;
	args.config.pinctrl = config->pinctrl;
	rc = rp1_pio_sm_init(rb_pio, &args);
	if (!rc)
		rc = rp1_pio_sm_clear_fifos(rb_pio, &clear);
	return rc;
}

int rb_hw_enable(unsigned int sm, int enabled)
{
	struct rp1_pio_sm_set_enabled_args args = {
		.mask = BIT(sm), .enable = !!enabled
	};

	return rb_pio ? rp1_pio_sm_set_enabled(rb_pio, &args) : -ENODEV;
}

int rb_hw_try_put(unsigned int sm, unsigned int word)
{
	struct rp1_pio_sm_fifo_state_args state = { .sm = sm, .tx = true };
	struct rp1_pio_sm_put_args put = {
		.sm = sm, .blocking = false, .data = word
	};
	int rc;

	if (!rb_pio)
		return -ENODEV;
	rc = rp1_pio_sm_fifo_state(rb_pio, &state);
	if (rc < 0)
		return rc;
	if (rc != sizeof(state))
		return -EIO;
	if (state.full)
		return 1; /* Special bridge result: retry after a short sleep. */
	rc = rp1_pio_sm_put(rb_pio, &put);
	return rc < 0 ? rc : 0;
}

int rb_hw_pause(void)
{
	if (signal_pending(current))
		return -ERESTARTSYS;
	usleep_range(1000, 2000);
	return signal_pending(current) ? -ERESTARTSYS : 0;
}

void rb_hw_close(void)
{
	if (rb_pio)
		rp1_pio_close(rb_pio);
	rb_pio = NULL;
}

/* ---------- /dev/radioberry-tx file operations ---------- */

static int rb_file_open(struct inode *inode, struct file *file)
{
	int rc = 0;

	mutex_lock(&rb_lock);
	if (!rb_spi)
		rc = -ENODEV;
	else if (rb_opened)
		rc = -EBUSY;
	else
		rb_opened = true;
	mutex_unlock(&rb_lock);
	return rc ? rc : nonseekable_open(inode, file);
}

static int rb_file_release(struct inode *inode, struct file *file)
{
	mutex_lock(&rb_lock);
	rb_opened = false;
	mutex_unlock(&rb_lock);
	return 0;
}

static ssize_t rb_file_write(struct file *file, const char __user *user,
			     size_t count, loff_t *position)
{
	char *bits;
	long rc;

	if (!count)
		return 0;
	if (count > RB_MAX_BITS)
		return -EINVAL;
	bits = memdup_user(user, count);
	if (IS_ERR(bits))
		return PTR_ERR(bits);
	if (mutex_lock_interruptible(&rb_lock)) {
		kfree(bits);
		return -ERESTARTSYS;
	}
	rc = rb_spi ? rb_core_write(bits, count) : -ENODEV;
	mutex_unlock(&rb_lock);
	kfree(bits);
	return rc;
}

static long rb_file_ioctl(struct file *file, unsigned int command,
			  unsigned long argument)
{
	struct rb_tx_control request, reply;
	struct spi_transfer transfer = {
		.tx_buf = &request,
		.rx_buf = &reply,
		.len = sizeof(request),
	};
	int rc;

	if (command != RB_TX_CONTROL)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
		return -EFAULT;
	if (mutex_lock_interruptible(&rb_lock))
		return -ERESTARTSYS;
	rc = rb_spi ? spi_sync_transfer(rb_spi, &transfer, 1) : -ENODEV;
	mutex_unlock(&rb_lock);
	if (!rc && copy_to_user((void __user *)argument, &reply, sizeof(reply)))
		rc = -EFAULT;
	return rc;
}

static const struct file_operations rb_fops = {
	.owner = THIS_MODULE,
	.open = rb_file_open,
	.release = rb_file_release,
	.write = rb_file_write,
	.unlocked_ioctl = rb_file_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.llseek = no_llseek,
};

static struct miscdevice rb_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "radioberry-tx",
	.fops = &rb_fops,
	.mode = 0600,
};

/* ---------- SPI device lifetime ---------- */

static int rb_probe(struct spi_device *spi)
{
	int rc;

	mutex_lock(&rb_lock);
	if (rb_spi || rb_opened) {
		rc = -EBUSY;
		goto out;
	}
	spi->mode = SPI_MODE_3;
	spi->bits_per_word = 8;
	rc = spi_setup(spi);
	if (rc)
		goto out;
	rb_spi = spi;
	rc = rb_core_start();
	if (rc)
		goto clear_spi;
	rb_misc.parent = &spi->dev;
	rc = misc_register(&rb_misc);
	if (rc)
		goto stop_core;
	dev_info(&spi->dev, "TX-only C++ driver created /dev/%s\n", rb_misc.name);
	mutex_unlock(&rb_lock);
	return 0;

stop_core:
	rb_core_stop();
clear_spi:
	rb_spi = NULL;
out:
	mutex_unlock(&rb_lock);
	return rc;
}

static void rb_remove(struct spi_device *spi)
{
	misc_deregister(&rb_misc);
	mutex_lock(&rb_lock);
	rb_core_stop();
	rb_spi = NULL;
	mutex_unlock(&rb_lock);
}

static const struct of_device_id rb_of_match[] = {
	{ .compatible = "radioberry,tx-only-cpp" },
	{ }
};
MODULE_DEVICE_TABLE(of, rb_of_match);

static const struct spi_device_id rb_spi_ids[] = {
	{ "radioberry_tx_cpp", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, rb_spi_ids);

static struct spi_driver rb_driver = {
	.driver = {
		.name = "radioberry_tx_cpp",
		.of_match_table = rb_of_match,
	},
	.probe = rb_probe,
	.remove = rb_remove,
	.id_table = rb_spi_ids,
};
module_spi_driver(rb_driver);

MODULE_DESCRIPTION("Minimal Raspberry Pi 5 Radioberry TX-only C++ kernel driver");
MODULE_AUTHOR("Radioberry contributors");
MODULE_LICENSE("GPL");
