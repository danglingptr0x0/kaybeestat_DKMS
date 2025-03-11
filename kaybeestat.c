#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/input.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simon Slamka of Ongakken");
MODULE_DESCRIPTION("KayBeeStat: a keyboard input event stat module for enthusiasts (not for long-term use!!)");
MODULE_VERSION("0.1");

static int kaybeestat_connect(struct input_handler *handler, struct input_dev *dev, const struct input_device_id *id);
static void kaybeestat_disconnect(struct input_handle *handle);
static void kaybeestat_event(struct input_handle *handle, unsigned int type, unsigned int code, int value);

static const struct input_device_id kaybeestat_ids[] =
    {
        {
            .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
            .evbit = { BIT_MASK(EV_KEY) }
        },
        { }
    };
MODULE_DEVICE_TABLE(input, kaybeestat_ids);

static struct input_handler kaybeestat_handler =
    {
        .event = kaybeestat_event,
        .connect = kaybeestat_connect,
        .disconnect = kaybeestat_disconnect,
        .name = "kaybeestat",
        .id_table = kaybeestat_ids
    };

static DEFINE_SPINLOCK(kaybeestat_lock);

static void kaybeestat_event(struct input_handle *handle, unsigned int type, unsigned int code, int value)
{
    if (unlikely(type != EV_KEY)) return;

    if (likely(value == 1))
    {
        spin_lock(&kaybeestat_lock);

        printk(KERN_INFO "KayBeeStat: key pressed: %u\n", code);

        spin_unlock(&kaybeestat_lock);
    }
}

static int kaybeestat_connect(struct input_handler *handler, struct input_dev *dev, const struct input_device_id *id)
{
    struct input_handle *handle;
    int err;

    if (!test_bit(EV_KEY, dev->evbit)) return -ENODEV;

    handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
    if (!handle) return ENOMEM;

    handle->dev = dev;
    handle->handler = handler;
    handle->name = "kaybeestat";

    err = input_register_handle(handle);
    if (err)
    {
        kfree(handle);
        return err;
    }

    err = input_open_device(handle);
    if (err)
    {
        input_unregister_handle(handle);
        kfree(handle);
    }

    printk(KERN_INFO "KayBeeStat: conn to dev: %s\n", dev->name);
    return 0;
}

static void kaybeestat_disconnect(struct input_handle *handle)
{
    printk(KERN_INFO "KayBeeStat: dc from dev: %s\n", handle->name);

    input_close_device(handle);
    input_unregister_handle(handle);
    kfree(handle);
}

static int __init kaybeestat_init(void)
{
    int err;

    printk(KERN_INFO "KayBeeStat: loading...\n");

    err = input_register_handler(&kaybeestat_handler);
    if (err)
    {
        printk(KERN_ERR "KayBeeStat: failed to reg input handler!\n");
        return err;
    }

    printk(KERN_INFO "KayBeeStat: module init!\n");
    return 0;
}

static void __exit kaybeestat_exit(void)
{
    printk(KERN_INFO "KayBeeStat: unloading...\n");

    input_unregister_handler(&kaybeestat_handler);

    printk(KERN_INFO "KayBeeStat: unloaded!\n");
}

module_init(kaybeestat_init);
module_exit(kaybeestat_exit);

