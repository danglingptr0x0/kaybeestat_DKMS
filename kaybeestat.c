#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/input.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simon Slamka of Ongakken Corp.");
MODULE_DESCRIPTION("KayBeeStat: a keyboard input event stat module for enthusiasts (not for long-term use!!)");
MODULE_VERSION("0.1");

#define KAYBEESTAT_MAX_EVENTS 32

static int kaybeestat_connect(struct input_handler *handler, struct input_dev *dev, const struct input_device_id *id);
static void kaybeestat_disconnect(struct input_handle *handle);
static void kaybeestat_event(struct input_handle *handle, unsigned int type, unsigned int code, int value);

typedef struct
{
    uint8_t keycode;
    uint64_t timestamp;
    uint8_t pressed;
    uint8_t released;
    char padding[54];
} __attribute__((aligned(64))) keystroke_event;

static keystroke_event kaybeestat_events[KAYBEESTAT_MAX_EVENTS];
static size_t kaybeestat_idx = 0;

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
    if (unlikely(type != EV_KEY || value == 2)) { return; }

    spin_lock(&kaybeestat_lock);

    kaybeestat_events[kaybeestat_idx].keycode = code;
    kaybeestat_events[kaybeestat_idx].timestamp = ktime_get_ns();
    kaybeestat_events[kaybeestat_idx].pressed = (value == 1);
    kaybeestat_events[kaybeestat_idx].released = (value == 0);
    memset(kaybeestat_events[kaybeestat_idx].padding, 0, sizeof(kaybeestat_events[kaybeestat_idx].padding));
    printk(KERN_INFO "KayBeeStat: key %s: %u (at %llu ns)\n", value ? "pressed" : "released", code, kaybeestat_events[kaybeestat_idx].timestamp);
    kaybeestat_idx = (kaybeestat_idx + 1) % KAYBEESTAT_MAX_EVENTS;

    spin_unlock(&kaybeestat_lock);
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

