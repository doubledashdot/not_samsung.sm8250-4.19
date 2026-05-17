#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/module.h>

#define DEVICE_NAME "cpu_dma_latency"
#define CLASS_NAME "qoslat"

static dev_t dev;
static struct cdev c_dev;
static struct class *cls;

static int qoslat_open(struct inode *i, struct file *f)
{
	return 0;
}

static int qoslat_release(struct inode *i, struct file *f)
{
	return 0;
}

static ssize_t qoslat_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
	return len;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = qoslat_open,
	.release = qoslat_release,
	.write = qoslat_write,
};

static int __init qoslat_init(void)
{
	if (alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME) < 0)
		return -1;

	cls = class_create(THIS_MODULE, CLASS_NAME);
	if (!cls)
		return -1;

	cdev_init(&c_dev, &fops);
	if (cdev_add(&c_dev, dev, 1) < 0)
		return -1;

	device_create(cls, NULL, dev, NULL, DEVICE_NAME);

	return 0;
}

static void __exit qoslat_exit(void)
{
	device_destroy(cls, dev);
	class_destroy(cls);
	cdev_del(&c_dev);
	unregister_chrdev_region(dev, 1);
}

module_init(qoslat_init);
module_exit(qoslat_exit);
