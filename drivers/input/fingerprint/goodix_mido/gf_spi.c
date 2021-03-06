/*Simple synchronous userspace interface to SPI devices
 *
 * Copyright (C) 2006 SWAPP
 *     Andrea Paterniani <a.paterniani@swapp-eng.it>
 * Copyright (C) 2007 David Brownell (simplification, cleanup)
 * Copyright (C) 2017 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <asm/uaccess.h>
#include <linux/ktime.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>
#include <linux/timer.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/pm_qos.h>
#include <linux/cpufreq.h>
#include "gf_spi.h"

#include <linux/platform_device.h>

#define GF_SPIDEV_NAME "goodix,fingerprint"
/*device name after register in charater*/
#define GF_DEV_NAME "goodix_fp"
#define GF_INPUT_NAME "gf3208" /*"goodix_fp" */

#define CHRD_DRIVER_NAME "goodix_fp_spi"
#define CLASS_NAME "goodix_fp"
#define SPIDEV_MAJOR 225 /* assigned */
#define N_SPI_MINORS 32 /* ... up to 256 */

struct gf_key_map key_map[] = {
	{ "POWER", KEY_POWER }, { "HOME", KEY_HOME },     { "MENU", KEY_MENU },
	{ "BACK", KEY_BACK },   { "UP", KEY_UP },	 { "DOWN", KEY_DOWN },
	{ "LEFT", KEY_LEFT },   { "RIGHT", KEY_RIGHT },   { "FORCE", KEY_F9 },
	{ "CLICK", KEY_F19 },   { "CAMERA", KEY_CAMERA },
};

/*Global variables*/
/*static MODE g_mode = GF_IMAGE_MODE;*/
static DECLARE_BITMAP(minors, N_SPI_MINORS);
static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);
static struct gf_dev gf;
static struct class *gf_class;
static struct wakeup_source ttw_wl;
static int driver_init_partial(struct gf_dev *gf_dev);

static void gf_enable_irq(struct gf_dev *gf_dev)
{
	if (gf_dev->irq_enabled)
		pr_warn("IRQ has been enabled.\n");
	else {
		enable_irq_wake(gf_dev->irq);
		gf_dev->irq_enabled = 1;
	}
}

static void gf_disable_irq(struct gf_dev *gf_dev)
{
	if (gf_dev->irq_enabled) {
		gf_dev->irq_enabled = 0;
		disable_irq_wake(gf_dev->irq);
	} else
		pr_warn("IRQ has been disabled.\n");
}

static long gf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct gf_dev *gf_dev = &gf;
	struct gf_key gf_key = { 0 };
	int retval = 0;
	int i;

	if (_IOC_TYPE(cmd) != GF_IOC_MAGIC)
		return -ENODEV;

	if (_IOC_DIR(cmd) & _IOC_READ)
		retval = !access_ok(VERIFY_WRITE, (void __user *)arg,
				    _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		retval = !access_ok(VERIFY_READ, (void __user *)arg,
				    _IOC_SIZE(cmd));
	if (retval)
		return -EFAULT;

	if (gf_dev->device_available == 0) {
		if ((cmd == GF_IOC_POWER_ON) || (cmd == GF_IOC_POWER_OFF) ||
		    (cmd == GF_IOC_ENABLE_GPIO))
			pr_debug("power cmd\n");
		else {
			pr_debug("Sensor is power off currently. \n");
			return -ENODEV;
		}
	}

	switch (cmd) {
	case GF_IOC_ENABLE_GPIO:
		driver_init_partial(gf_dev);
		break;
	case GF_IOC_RELEASE_GPIO:
		gf_cleanup(gf_dev);
		break;
	case GF_IOC_DISABLE_IRQ:
		gf_disable_irq(gf_dev);
		break;
	case GF_IOC_ENABLE_IRQ:
		gf_enable_irq(gf_dev);
		break;
	case GF_IOC_SETSPEED:
		break;
	case GF_IOC_RESET:
		gf_hw_reset(gf_dev, 3);
		break;
	case GF_IOC_COOLBOOT:
		gf_power_off(gf_dev);
		mdelay(5);
		gf_power_on(gf_dev);
		break;
	case GF_IOC_SENDKEY:
		if (copy_from_user(&gf_key, (struct gf_key *)arg,
				   sizeof(struct gf_key))) {
			pr_debug("Failed to copy data from user space.\n");
			retval = -EFAULT;
			break;
		}

		for (i = 0; i < ARRAY_SIZE(key_map); i++) {
			if (key_map[i].val == gf_key.key) {
				if (KEY_CAMERA == gf_key.key) {
					input_report_key(gf_dev->input,
							 KEY_SELECT,
							 gf_key.value);
					input_sync(gf_dev->input);
				} else {
					input_report_key(gf_dev->input,
							 gf_key.key,
							 gf_key.value);
					input_sync(gf_dev->input);
				}
				break;
			}
		}

		if (i == ARRAY_SIZE(key_map)) {
			pr_warn("key %d not support yet \n", gf_key.key);
			retval = -EFAULT;
		}

		break;
	case GF_IOC_CLK_READY:
		break;
	case GF_IOC_CLK_UNREADY:
		break;
	case GF_IOC_PM_FBCABCK:
		__put_user(gf_dev->fb_black, (u8 __user *)arg);
		break;
	case GF_IOC_POWER_ON:
		if (gf_dev->device_available == 1)
			pr_debug("Sensor has already powered-on.\n");
		else
			gf_power_on(gf_dev);
		gf_dev->device_available = 1;
		break;
	case GF_IOC_POWER_OFF:
		if (gf_dev->device_available == 0)
			pr_debug("Sensor has already powered-off.\n");
		else
			gf_power_off(gf_dev);
		gf_dev->device_available = 0;
		break;
	default:
		pr_debug("unsupport cmd:0x%x\n", cmd);
		break;
	}

	return retval;
}

#ifdef CONFIG_COMPAT
static long gf_compat_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	return gf_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif /*CONFIG_COMPAT*/

static irqreturn_t gf_irq(int irq, void *handle)
{
	struct gf_dev *gf_dev = &gf;
	char temp = GF_NET_EVENT_IRQ;

	if (gf_dev->fb_black)
		__pm_wakeup_event(&ttw_wl, msecs_to_jiffies(2000));

	sendnlmsg(&temp);
	return IRQ_HANDLED;
}

static int driver_init_partial(struct gf_dev *gf_dev)
{
	int ret = 0;

	pr_warn("--------driver_init_partial start.--------\n");

	gf_dev->device_available = 1;

	if (gf_parse_dts(gf_dev))
		goto error;

	gf_dev->irq = gf_irq_num(gf_dev);
	ret = request_threaded_irq(gf_dev->irq, NULL, gf_irq,
				   IRQF_TRIGGER_RISING | IRQF_ONESHOT |
					   IRQF_TH_SCHED_FIFO_HI,
				   "gf", gf_dev);
	if (ret) {
		pr_err("Could not request irq %d\n",
		       gpio_to_irq(gf_dev->irq_gpio));
		goto error;
	}
	if (!ret) {
		enable_irq_wake(gf_dev->irq);
		gf_enable_irq(gf_dev);
		gf_disable_irq(gf_dev);
	}
	gf_hw_reset(gf_dev, 360);

	return 0;

error:

	gf_cleanup(gf_dev);

	gf_dev->device_available = 0;

	return -EPERM;
}
static int gf_open(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev;
	int status = -ENXIO;

	mutex_lock(&device_list_lock);

	list_for_each_entry (gf_dev, &device_list, device_entry) {
		if (gf_dev->devt == inode->i_rdev) {
			pr_debug("Found\n");
			status = 0;
			break;
		}
	}

	if (status == 0) {
		if (status == 0) {
			gf_dev->users++;
			filp->private_data = gf_dev;
			nonseekable_open(inode, filp);
			pr_debug("Succeed to open device. irq = %d\n",
				 gf_dev->irq);
			/*power the sensor*/
			gf_dev->device_available = 1;
		}
	} else
		pr_debug("No device for minor %d\n", iminor(inode));
	mutex_unlock(&device_list_lock);
	return status;
}

static int gf_release(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev;

	mutex_lock(&device_list_lock);
	gf_dev = filp->private_data;
	filp->private_data = NULL;

	gf_dev->users--;
	if (!gf_dev->users) {
		pr_debug("disble_irq. irq = %d\n", gf_dev->irq);
		gf_disable_irq(gf_dev);
		/*power off the sensor*/
		gf_dev->device_available = 0;
		free_irq(gf_dev->irq, gf_dev);
		gpio_free(gf_dev->irq_gpio);
		gpio_free(gf_dev->reset_gpio);
		gf_power_off(gf_dev);
	}
	mutex_unlock(&device_list_lock);

	return 0;
}

static const struct file_operations gf_fops = {
	.owner = THIS_MODULE,
	/* REVISIT switch to aio primitives, so that userspace
	 * gets more complete API coverage.  It'll simplify things
	 * too, except for the locking.
	 */
	.unlocked_ioctl = gf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = gf_compat_ioctl,
#endif /*CONFIG_COMPAT*/
	.open = gf_open,
	.release = gf_release,
};

static void fb_state_worker(struct work_struct *work)
{
	struct gf_dev *gf_dev = container_of(work, typeof(*gf_dev), fb_work);

	if (!gf_dev->device_available)
		return;
	{
		char temp = gf_dev->fb_black ? GF_NET_EVENT_FB_BLACK :
					       GF_NET_EVENT_FB_UNBLACK;
		sendnlmsg(&temp);
	}
}

static int goodix_fb_state_chg_callback(struct notifier_block *nb,
					unsigned long val, void *data)
{
	struct gf_dev *gf_dev = container_of(nb, typeof(*gf_dev), notifier);
	struct fb_event *evdata = data;
	int *blank = evdata->data;

	if (val != FB_EARLY_EVENT_BLANK)
		return 0;
	switch (*blank) {
	case FB_BLANK_POWERDOWN:
		gf_dev->fb_black = 1;
		schedule_work(&gf_dev->fb_work);
		break;
	case FB_BLANK_UNBLANK:
		gf_dev->fb_black = 0;
		schedule_work(&gf_dev->fb_work);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block goodix_noti_block = {
	.notifier_call = goodix_fb_state_chg_callback,
};

static void gf_reg_key_kernel(struct gf_dev *gf_dev)
{
	int i;

	set_bit(EV_KEY, gf_dev->input->evbit);
	for (i = 0; i < ARRAY_SIZE(key_map); i++)
		set_bit(key_map[i].val, gf_dev->input->keybit);
	set_bit(KEY_SELECT, gf_dev->input->keybit);
	gf_dev->input->name = GF_INPUT_NAME;
	if (input_register_device(gf_dev->input))
		pr_debug("Failed to register GF as input device.\n");
}

static int gf_probe(struct platform_device *pdev)
{
	struct gf_dev *gf_dev = &gf;
	int status = -EINVAL;
	unsigned long minor;

	pr_debug("--------gf_probe start.--------\n");
	/* Initialize the driver data */
	INIT_LIST_HEAD(&gf_dev->device_entry);

	gf_dev->spi = pdev;

	gf_dev->irq_gpio = -EINVAL;
	gf_dev->reset_gpio = -EINVAL;
	gf_dev->pwr_gpio = -EINVAL;
	gf_dev->device_available = 0;
	gf_dev->fb_black = 0;
	gf_dev->irq_enabled = 0;
	gf_dev->fingerprint_pinctrl = NULL;

	/* If we can allocate a minor number, hook up this device.
	 * Reusing minors is fine so long as udev or mdev is working.
	 */
	mutex_lock(&device_list_lock);
	minor = find_first_zero_bit(minors, N_SPI_MINORS);
	if (minor < N_SPI_MINORS) {
		struct device *dev;

		gf_dev->devt = MKDEV(SPIDEV_MAJOR, minor);
		dev = device_create(gf_class, &gf_dev->spi->dev, gf_dev->devt,
				    gf_dev, GF_DEV_NAME);
		status = IS_ERR(dev) ? PTR_ERR(dev) : 0;
	} else {
		dev_dbg(&gf_dev->spi->dev, "no minor number available!\n");
		status = -ENODEV;
	}

	if (status == 0) {
		set_bit(minor, minors);
		list_add(&gf_dev->device_entry, &device_list);
	} else
		gf_dev->devt = 0;
	mutex_unlock(&device_list_lock);

	if (status == 0) {
		/*input device subsystem */
		gf_dev->input = input_allocate_device();
		if (gf_dev->input == NULL) {
			pr_debug("%s, failed to allocate input device\n",
				 __func__);
			status = -ENOMEM;
			goto error;
		}

		INIT_WORK(&gf_dev->fb_work, fb_state_worker);
		gf_dev->notifier = goodix_noti_block;
		fb_register_client(&gf_dev->notifier);
		gf_reg_key_kernel(gf_dev);

		wakeup_source_init(&ttw_wl, "goodix_ttw_wl");
	}

	pr_debug("--------gf_probe end---OK.--------\n");
	return status;

error:
	gf_cleanup(gf_dev);
	gf_dev->device_available = 0;
	if (gf_dev->devt != 0) {
		pr_debug("Err: status = %d\n", status);
		mutex_lock(&device_list_lock);
		list_del(&gf_dev->device_entry);
		device_destroy(gf_class, gf_dev->devt);
		clear_bit(MINOR(gf_dev->devt), minors);
		mutex_unlock(&device_list_lock);

		if (gf_dev->input != NULL)
			input_unregister_device(gf_dev->input);
	}

	return status;
}

static int gf_remove(struct platform_device *pdev)
{
	struct gf_dev *gf_dev = &gf;

	/* make sure ops on existing fds can abort cleanly */
	if (gf_dev->irq)
		free_irq(gf_dev->irq, gf_dev);

	if (gf_dev->input != NULL) {
		input_unregister_device(gf_dev->input);
		input_free_device(gf_dev->input);
	}

	/* prevent new opens */
	mutex_lock(&device_list_lock);
	list_del(&gf_dev->device_entry);
	device_destroy(gf_class, gf_dev->devt);
	clear_bit(MINOR(gf_dev->devt), minors);
	if (gf_dev->users == 0)
		kfree(gf_dev);

	mutex_unlock(&device_list_lock);

	wakeup_source_trash(&ttw_wl);

	return 0;
}

static int gf_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int gf_resume(struct platform_device *pdev)
{
	return 0;
}

static struct of_device_id gx_match_table[] = {
	{
		.compatible = GF_SPIDEV_NAME,
	},
	{},
};

static struct platform_driver gf_driver = {
	.driver			= {
		.name		= GF_DEV_NAME,
		.owner		= THIS_MODULE,
		.of_match_table = gx_match_table,
	},
	.probe			= gf_probe,
	.remove			= gf_remove,
	.suspend		= gf_suspend,
	.resume			= gf_resume,
};

static int __init gf_init(void)
{
	int status;

	pr_debug("--------gf_init start.--------\n");
	/* Claim our 256 reserved device numbers.  Then register a class
	 * that will key udev/mdev to add/remove /dev nodes.  Last, register
	 * the driver which manages those device numbers.
	 */

	BUILD_BUG_ON(N_SPI_MINORS > 256);
	status = register_chrdev(SPIDEV_MAJOR, CHRD_DRIVER_NAME, &gf_fops);
	if (status < 0) {
		pr_warn("Failed to register char device!\n");
		return status;
	}
	gf_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(gf_class)) {
		unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
		pr_warn("Failed to create class.\n");
		return PTR_ERR(gf_class);
	}
	status = platform_driver_register(&gf_driver);
	if (status < 0) {
		class_destroy(gf_class);
		unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
		pr_warn("Failed to register SPI driver.\n");
	}

#ifdef GF_NETLINK_ENABLE
	netlink_init();
#endif
	pr_debug(" status = 0x%x\n", status);

	pr_debug("--------gf_init end---OK.--------\n");
	return 0;
}
module_init(gf_init);

static void __exit gf_exit(void)
{
#ifdef GF_NETLINK_ENABLE
	netlink_exit();
#endif
	platform_driver_unregister(&gf_driver);
	class_destroy(gf_class);
	unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
}
module_exit(gf_exit);

MODULE_AUTHOR("Jiangtao Yi, <yijiangtao@goodix.com>");
MODULE_DESCRIPTION("User mode SPI device interface");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:gf-spi");
