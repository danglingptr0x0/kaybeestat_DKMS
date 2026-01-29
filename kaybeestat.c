#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/input.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/cred.h>
#include <linux/uidgid.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simon Slamka of Ongakken Corp.");
MODULE_DESCRIPTION("KayBeeStat: a keyboard input event stat module for enthusiasts");
MODULE_VERSION("0.7");

// constants

#define KB_KEY_MAX 768
#define KB_WINDOW_CUNT 8

#define KB_SAT_ADD32(a, b) ((uint32_t)((a) > (U32_MAX - (b)) ? U32_MAX : ((a) + (b))))
#define KB_SAT_ADD64(a, b) ((uint64_t)((a) > (U64_MAX - (b)) ? U64_MAX : ((a) + (b))))
#define KB_SECS_RING_SIZE 60
#define KB_MINS_RING_SIZE 60
#define KB_HOURS_RING_SIZE 24
#define KB_DAYS_RING_SIZE 365

// data structures

typedef struct
{
    uint32_t press_cunt;
    uint32_t release_cunt;
    uint32_t char_cunt;
    uint32_t char_del_cunt;
    uint32_t word_del_cunt;
    uint64_t hold_sum_ns;
    uint32_t hold_cunt;
    uint64_t hold_m2;
    uint64_t longest_hold_ns;
    uint64_t gap_sum_ns;
    uint32_t gap_cunt;
    uint64_t gap_m2;
    uint64_t shortest_gap_ns;
    uint64_t longest_gap_ns;
    uint32_t per_key_cunt[KB_KEY_MAX];
} kb_bucket_t;

static inline int kb_key_printable_is(unsigned int code)
{
    if (code >= KEY_1 && code <= KEY_0) { return 1; }

    if (code >= KEY_Q && code <= KEY_P) { return 1; }

    if (code >= KEY_A && code <= KEY_APOSTROPHE) { return 1; }

    if (code >= KEY_Z && code <= KEY_SLASH) { return 1; }

    if (code == KEY_SPACE || code == KEY_MINUS || code == KEY_EQUAL) { return 1; }

    if (code == KEY_LEFTBRACE || code == KEY_RIGHTBRACE || code == KEY_BACKSLASH) { return 1; }

    if (code == KEY_GRAVE) { return 1; }

    return 0;
}

typedef struct
{
    uint64_t keystroke_cunt;
    uint64_t release_cunt;
    uint64_t char_cunt;
    uint64_t char_del_cunt;
    uint64_t word_del_cunt;
    uint64_t avg_kps;
    uint64_t avg_cps;
    uint64_t peak_kps;
    uint64_t avg_hold_ns;
    uint64_t hold_var_ns;
    uint64_t longest_hold_ns;
    uint64_t avg_gap_ns;
    uint64_t gap_var_ns;
    uint64_t shortest_gap_ns;
    uint64_t longest_gap_ns;
    uint32_t per_key_cunt[KB_KEY_MAX];
} kb_window_stats_t;

typedef struct
{
    uint64_t uptime_ns;
    uint16_t last_vendor;
    uint16_t last_product;
    uint32_t pudding;
    kb_window_stats_t windows[KB_WINDOW_CUNT];
} kb_stats_t;

typedef struct
{
    uint64_t keystroke_cunt;
    uint64_t release_cunt;
    uint64_t char_cunt;
    uint64_t char_del_cunt;
    uint64_t word_del_cunt;
    uint64_t avg_kps;
    uint64_t avg_cps;
    uint64_t peak_kps;
    uint64_t avg_hold_ns;
    uint64_t hold_var_ns;
    uint64_t longest_hold_ns;
    uint64_t avg_gap_ns;
    uint64_t gap_var_ns;
    uint64_t shortest_gap_ns;
    uint64_t longest_gap_ns;
} kb_window_stats_pub_t;

typedef struct
{
    uint64_t uptime_ns;
    uint16_t last_vendor;
    uint16_t last_product;
    uint32_t pudding;
    kb_window_stats_pub_t windows[KB_WINDOW_CUNT];
} kb_stats_pub_t;

// tiered ring buffers

static kb_bucket_t kb_live;
static kb_bucket_t *kb_secs_ring = NULL;
static kb_bucket_t *kb_mins_ring = NULL;
static kb_bucket_t *kb_hours_ring = NULL;
static kb_bucket_t *kb_days_ring = NULL;

static size_t kb_secs_idx = 0;
static size_t kb_mins_idx = 0;
static size_t kb_hours_idx = 0;
static size_t kb_days_idx = 0;

// hold tracking

static uint64_t kb_key_press_ts[KB_KEY_MAX];
static uint64_t kb_last_press_ns = 0;

// modifier tracking

static int kb_ctrl_held = 0;
static int kb_alt_held = 0;

static uint16_t kb_last_vendor = 0;
static uint16_t kb_last_product = 0;

// timer

static struct timer_list kb_timer;
static uint64_t kb_tick_cunt = 0;
static uint64_t kb_init_ns = 0;

static kb_bucket_t *kb_scratch_timer = NULL;
static kb_bucket_t *kb_scratch_rd = NULL;

// synchronization

static DEFINE_SPINLOCK(kb_lock);
static int kb_shutdown = 0;

// input handler forward declarations

static int kb_connect(struct input_handler *handler, struct input_dev *dev, const struct input_device_id *id);
static void kb_disconnect(struct input_handle *handle);
static void kb_event(struct input_handle *handle, unsigned int type, unsigned int code, int val);

// bucket operations

static void kb_bucket_zero(kb_bucket_t *b)
{
    memset(b, 0, sizeof(*b));
    b->shortest_gap_ns = U64_MAX;
}

static void kb_bucket_merge(kb_bucket_t *dst, const kb_bucket_t *src, int skip_perkey)
{
    size_t idx = 0;
    uint64_t n_a = 0;
    uint64_t n_b = 0;
    uint64_t n_combined = 0;
    int64_t delta = 0;
    uint64_t mean_a = 0;
    uint64_t mean_b = 0;

    dst->press_cunt = KB_SAT_ADD32(dst->press_cunt, src->press_cunt);
    dst->release_cunt = KB_SAT_ADD32(dst->release_cunt, src->release_cunt);
    dst->char_cunt = KB_SAT_ADD32(dst->char_cunt, src->char_cunt);
    dst->char_del_cunt = KB_SAT_ADD32(dst->char_del_cunt, src->char_del_cunt);
    dst->word_del_cunt = KB_SAT_ADD32(dst->word_del_cunt, src->word_del_cunt);

    n_a = dst->hold_cunt;
    n_b = src->hold_cunt;
    n_combined = n_a + n_b;
    if (n_combined > 0)
    {
        mean_a = (n_a > 0) ? (dst->hold_sum_ns / n_a) : 0;
        mean_b = (n_b > 0) ? (src->hold_sum_ns / n_b) : 0;
        delta = (int64_t)mean_b - (int64_t)mean_a;
        dst->hold_m2 = KB_SAT_ADD64(dst->hold_m2, src->hold_m2);
        dst->hold_m2 = KB_SAT_ADD64(dst->hold_m2, (uint64_t)(delta * delta) * n_a * n_b / n_combined);
    }

    dst->hold_sum_ns = KB_SAT_ADD64(dst->hold_sum_ns, src->hold_sum_ns);
    dst->hold_cunt = KB_SAT_ADD32(dst->hold_cunt, src->hold_cunt);

    if (src->longest_hold_ns > dst->longest_hold_ns) { dst->longest_hold_ns = src->longest_hold_ns; }

    n_a = dst->gap_cunt;
    n_b = src->gap_cunt;
    n_combined = n_a + n_b;
    if (n_combined > 0)
    {
        mean_a = (n_a > 0) ? (dst->gap_sum_ns / n_a) : 0;
        mean_b = (n_b > 0) ? (src->gap_sum_ns / n_b) : 0;
        delta = (int64_t)mean_b - (int64_t)mean_a;
        dst->gap_m2 = KB_SAT_ADD64(dst->gap_m2, src->gap_m2);
        dst->gap_m2 = KB_SAT_ADD64(dst->gap_m2, (uint64_t)(delta * delta) * n_a * n_b / n_combined);
    }

    dst->gap_sum_ns = KB_SAT_ADD64(dst->gap_sum_ns, src->gap_sum_ns);
    dst->gap_cunt = KB_SAT_ADD32(dst->gap_cunt, src->gap_cunt);

    if (src->shortest_gap_ns < dst->shortest_gap_ns) { dst->shortest_gap_ns = src->shortest_gap_ns; }

    if (src->longest_gap_ns > dst->longest_gap_ns) { dst->longest_gap_ns = src->longest_gap_ns; }

    if (!skip_perkey) { for (idx = 0; idx < KB_KEY_MAX; idx++) { dst->per_key_cunt[idx] = KB_SAT_ADD32(dst->per_key_cunt[idx], src->per_key_cunt[idx]); } }
}

static void kb_window_from_ring(kb_window_stats_t *w, const kb_bucket_t *ring, size_t ring_size, size_t head, size_t cunt, const kb_bucket_t *live_bucket, kb_bucket_t *acc, int skip_perkey)
{
    size_t idx = 0;
    size_t start = 0;
    uint32_t peak = 0;
    uint64_t duration_secs = 0;

    kb_bucket_zero(acc);

    if (live_bucket) { kb_bucket_merge(acc, live_bucket, skip_perkey); }

    if (cunt > ring_size) { cunt = ring_size; }

    start = (head + ring_size - cunt) % ring_size;

    for (idx = 0; idx < cunt; idx++)
    {
        size_t pos = (start + idx) % ring_size;
        kb_bucket_merge(acc, &ring[pos], skip_perkey);

        if (ring[pos].press_cunt > peak) { peak = ring[pos].press_cunt; }
    }

    w->keystroke_cunt = acc->press_cunt;
    w->release_cunt = acc->release_cunt;
    w->char_cunt = acc->char_cunt;
    w->char_del_cunt = acc->char_del_cunt;
    w->word_del_cunt = acc->word_del_cunt;
    w->longest_hold_ns = acc->longest_hold_ns;
    w->shortest_gap_ns = (acc->shortest_gap_ns == U64_MAX) ? 0 : acc->shortest_gap_ns;
    w->longest_gap_ns = acc->longest_gap_ns;
    w->avg_hold_ns = (acc->hold_cunt > 0) ? (acc->hold_sum_ns / acc->hold_cunt) : 0;
    w->hold_var_ns = (acc->hold_cunt > 0) ? (acc->hold_m2 / acc->hold_cunt) : 0;
    w->avg_gap_ns = (acc->gap_cunt > 0) ? (acc->gap_sum_ns / acc->gap_cunt) : 0;
    w->gap_var_ns = (acc->gap_cunt > 0) ? (acc->gap_m2 / acc->gap_cunt) : 0;

    duration_secs = cunt;
    if (live_bucket) { duration_secs += 1; }

    w->avg_kps = (duration_secs > 0) ? ((uint64_t)acc->press_cunt * 1000 / duration_secs) : 0;
    w->avg_cps = (duration_secs > 0) ? ((uint64_t)acc->char_cunt * 1000 / duration_secs) : 0;
    w->peak_kps = (uint64_t)peak * 1000;

    if (!skip_perkey) { for (idx = 0; idx < KB_KEY_MAX; idx++) { w->per_key_cunt[idx] = acc->per_key_cunt[idx]; } }
}

// timer callback

static void kb_timer_cb(struct timer_list *t)
{
    unsigned long flags = 0;

    spin_lock_irqsave(&kb_lock, flags);

    if (READ_ONCE(kb_shutdown))
    {
        spin_unlock_irqrestore(&kb_lock, flags);
        return;
    }

    kb_secs_ring[kb_secs_idx] = kb_live;
    kb_secs_idx = (kb_secs_idx + 1) % KB_SECS_RING_SIZE;
    kb_bucket_zero(&kb_live);

    kb_tick_cunt++;

    if (kb_tick_cunt % 60 == 0)
    {
        size_t idx = 0;

        kb_bucket_zero(kb_scratch_timer);
        for (idx = 0; idx < KB_SECS_RING_SIZE; idx++) { kb_bucket_merge(kb_scratch_timer, &kb_secs_ring[idx], 0); }
        kb_mins_ring[kb_mins_idx] = *kb_scratch_timer;
        kb_mins_idx = (kb_mins_idx + 1) % KB_MINS_RING_SIZE;
    }

    if (kb_tick_cunt % 3600 == 0)
    {
        size_t idx = 0;

        kb_bucket_zero(kb_scratch_timer);
        for (idx = 0; idx < KB_MINS_RING_SIZE; idx++) { kb_bucket_merge(kb_scratch_timer, &kb_mins_ring[idx], 0); }
        kb_hours_ring[kb_hours_idx] = *kb_scratch_timer;
        kb_hours_idx = (kb_hours_idx + 1) % KB_HOURS_RING_SIZE;
    }

    if (kb_tick_cunt % 86400 == 0)
    {
        size_t idx = 0;

        kb_bucket_zero(kb_scratch_timer);
        for (idx = 0; idx < KB_HOURS_RING_SIZE; idx++) { kb_bucket_merge(kb_scratch_timer, &kb_hours_ring[idx], 0); }
        kb_days_ring[kb_days_idx] = *kb_scratch_timer;
        kb_days_idx = (kb_days_idx + 1) % KB_DAYS_RING_SIZE;
    }

    if (!READ_ONCE(kb_shutdown)) { mod_timer(&kb_timer, jiffies + HZ); }

    spin_unlock_irqrestore(&kb_lock, flags);
}

// character device

static int kb_dev_open(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t kb_dev_rd(struct file *file, char __user *buff, size_t len, loff_t *off)
{
    kb_stats_t *stats = NULL;
    unsigned long flags = 0;
    int is_root = uid_eq(current_uid(), GLOBAL_ROOT_UID) || uid_eq(current_euid(), GLOBAL_ROOT_UID);
    size_t out_size = is_root ? sizeof(kb_stats_t) : sizeof(kb_stats_pub_t);

    if (unlikely(*off > 0)) { return 0; }

    if (unlikely(len < out_size)) { return -EINVAL; }

    stats = kvmalloc(sizeof(kb_stats_t), GFP_KERNEL);
    if (unlikely(!stats)) { return -ENOMEM; }

    memset(stats, 0, sizeof(*stats));

    spin_lock_irqsave(&kb_lock, flags);

    if (unlikely(READ_ONCE(kb_shutdown)))
    {
        spin_unlock_irqrestore(&kb_lock, flags);
        kvfree(stats);
        return -ENODEV;
    }

    stats->uptime_ns = ktime_get_ns() - kb_init_ns;
    stats->last_vendor = kb_last_vendor;
    stats->last_product = kb_last_product;

    kb_window_from_ring(&stats->windows[0], kb_secs_ring, KB_SECS_RING_SIZE, kb_secs_idx, KB_SECS_RING_SIZE, &kb_live, kb_scratch_rd, !is_root);

    kb_window_from_ring(&stats->windows[1], kb_mins_ring, KB_MINS_RING_SIZE, kb_mins_idx, 5, &kb_live, kb_scratch_rd, !is_root);

    kb_window_from_ring(&stats->windows[2], kb_mins_ring, KB_MINS_RING_SIZE, kb_mins_idx, 30, &kb_live, kb_scratch_rd, !is_root);

    kb_window_from_ring(&stats->windows[3], kb_hours_ring, KB_HOURS_RING_SIZE, kb_hours_idx, 6, &kb_live, kb_scratch_rd, !is_root);

    kb_window_from_ring(&stats->windows[4], kb_hours_ring, KB_HOURS_RING_SIZE, kb_hours_idx, KB_HOURS_RING_SIZE, &kb_live, kb_scratch_rd, !is_root);

    kb_window_from_ring(&stats->windows[5], kb_days_ring, KB_DAYS_RING_SIZE, kb_days_idx, 7, &kb_live, kb_scratch_rd, !is_root);

    kb_window_from_ring(&stats->windows[6], kb_days_ring, KB_DAYS_RING_SIZE, kb_days_idx, 30, &kb_live, kb_scratch_rd, !is_root);

    kb_window_from_ring(&stats->windows[7], kb_days_ring, KB_DAYS_RING_SIZE, kb_days_idx, KB_DAYS_RING_SIZE, &kb_live, kb_scratch_rd, !is_root);

    spin_unlock_irqrestore(&kb_lock, flags);

    if (is_root)
    {
        if (unlikely(copy_to_user(buff, stats, sizeof(kb_stats_t))))
        {
            kvfree(stats);
            return -EFAULT;
        }

        *off += sizeof(kb_stats_t);
        kvfree(stats);
        return sizeof(kb_stats_t);
    }
    else
    {
        kb_stats_pub_t pub;
        size_t i = 0;

        memset(&pub, 0, sizeof(pub));
        pub.uptime_ns = stats->uptime_ns;
        pub.last_vendor = stats->last_vendor;
        pub.last_product = stats->last_product;

        for (i = 0; i < KB_WINDOW_CUNT; i++)
        {
            pub.windows[i].keystroke_cunt = stats->windows[i].keystroke_cunt;
            pub.windows[i].release_cunt = stats->windows[i].release_cunt;
            pub.windows[i].char_cunt = stats->windows[i].char_cunt;
            pub.windows[i].char_del_cunt = stats->windows[i].char_del_cunt;
            pub.windows[i].word_del_cunt = stats->windows[i].word_del_cunt;
            pub.windows[i].avg_kps = stats->windows[i].avg_kps;
            pub.windows[i].avg_cps = stats->windows[i].avg_cps;
            pub.windows[i].peak_kps = stats->windows[i].peak_kps;
            pub.windows[i].avg_hold_ns = stats->windows[i].avg_hold_ns;
            pub.windows[i].hold_var_ns = stats->windows[i].hold_var_ns;
            pub.windows[i].longest_hold_ns = stats->windows[i].longest_hold_ns;
            pub.windows[i].avg_gap_ns = stats->windows[i].avg_gap_ns;
            pub.windows[i].gap_var_ns = stats->windows[i].gap_var_ns;
            pub.windows[i].shortest_gap_ns = stats->windows[i].shortest_gap_ns;
            pub.windows[i].longest_gap_ns = stats->windows[i].longest_gap_ns;
        }

        kvfree(stats);

        if (unlikely(copy_to_user(buff, &pub, sizeof(kb_stats_pub_t)))) { return -EFAULT; }

        *off += sizeof(kb_stats_pub_t);
        return sizeof(kb_stats_pub_t);
    }
}

static int kb_dev_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations kb_fops =
{
    .owner = THIS_MODULE, .open = kb_dev_open, .release = kb_dev_release, .read = kb_dev_rd, .llseek = default_llseek, };

static struct miscdevice kb_misc_dev =
{
    .minor = MISC_DYNAMIC_MINOR, .name = "kaybeestat", .fops = &kb_fops, .mode = 0440, };

// input handler

static const struct input_device_id kb_ids[] =
{
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT, .evbit = { BIT_MASK(EV_KEY) }

    },
    {}
};
MODULE_DEVICE_TABLE(input, kb_ids);

static struct input_handler kb_handler =
{
    .event = kb_event, .connect = kb_connect, .disconnect = kb_disconnect, .name = "kaybeestat", .id_table = kb_ids
};

static void kb_event(struct input_handle *handle, unsigned int type, unsigned int code, int val)
{
    uint64_t now = 0;
    uint64_t hold_ns = 0;
    uint64_t gap_ns = 0;
    unsigned long flags = 0;

    if (unlikely(type != EV_KEY || val == 2)) { return; }

    if (unlikely(code >= KB_KEY_MAX)) { return; }

    now = ktime_get_ns();

    spin_lock_irqsave(&kb_lock, flags);

    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) { kb_ctrl_held = (val == 1); }

    if (code == KEY_LEFTALT || code == KEY_RIGHTALT) { kb_alt_held = (val == 1); }

    kb_last_vendor = handle->dev->id.vendor;
    kb_last_product = handle->dev->id.product;

    if (val == 1)
    {
        kb_live.press_cunt++;
        kb_live.per_key_cunt[code]++;
        if (kb_key_printable_is(code)) { kb_live.char_cunt++; }

        kb_key_press_ts[code] = now;

        if (code == KEY_BACKSPACE) { if (kb_alt_held) { kb_live.word_del_cunt++; }
            else { kb_live.char_del_cunt++; } }
        else if (code == KEY_W && kb_ctrl_held) { kb_live.word_del_cunt++; }

        if (kb_last_press_ns > 0 && now >= kb_last_press_ns)
        {
            uint64_t old_mean = 0;
            uint64_t new_mean = 0;
            int64_t delta = 0;
            int64_t delta2 = 0;

            gap_ns = now - kb_last_press_ns;

            old_mean = (kb_live.gap_cunt > 0) ? (kb_live.gap_sum_ns / kb_live.gap_cunt) : 0;
            kb_live.gap_sum_ns = KB_SAT_ADD64(kb_live.gap_sum_ns, gap_ns);
            kb_live.gap_cunt++;
            new_mean = kb_live.gap_sum_ns / kb_live.gap_cunt;
            delta = (int64_t)gap_ns - (int64_t)old_mean;
            delta2 = (int64_t)gap_ns - (int64_t)new_mean;
            kb_live.gap_m2 = KB_SAT_ADD64(kb_live.gap_m2, (uint64_t)(delta * delta2));

            if (gap_ns < kb_live.shortest_gap_ns) { kb_live.shortest_gap_ns = gap_ns; }

            if (gap_ns > kb_live.longest_gap_ns) { kb_live.longest_gap_ns = gap_ns; }
        }

        kb_last_press_ns = now;
    }
    else
    {
        kb_live.release_cunt++;

        if (kb_key_press_ts[code] > 0)
        {
            uint64_t old_mean = 0;
            uint64_t new_mean = 0;
            int64_t delta = 0;
            int64_t delta2 = 0;

            hold_ns = now - kb_key_press_ts[code];

            old_mean = (kb_live.hold_cunt > 0) ? (kb_live.hold_sum_ns / kb_live.hold_cunt) : 0;
            kb_live.hold_sum_ns = KB_SAT_ADD64(kb_live.hold_sum_ns, hold_ns);
            kb_live.hold_cunt++;
            new_mean = kb_live.hold_sum_ns / kb_live.hold_cunt;
            delta = (int64_t)hold_ns - (int64_t)old_mean;
            delta2 = (int64_t)hold_ns - (int64_t)new_mean;
            kb_live.hold_m2 = KB_SAT_ADD64(kb_live.hold_m2, (uint64_t)(delta * delta2));

            if (hold_ns > kb_live.longest_hold_ns) { kb_live.longest_hold_ns = hold_ns; }

            kb_key_press_ts[code] = 0;
        }
    }

    spin_unlock_irqrestore(&kb_lock, flags);
}

static int kb_connect(struct input_handler *handler, struct input_dev *dev, const struct input_device_id *id)
{
    struct input_handle *handle = NULL;
    int err = 0;

    if (!test_bit(EV_KEY, dev->evbit)) { return -ENODEV; }

    handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
    if (unlikely(!handle)) { return -ENOMEM; }

    handle->dev = dev;
    handle->handler = handler;
    handle->name = "kaybeestat";

    err = input_register_handle(handle);
    if (unlikely(err))
    {
        kfree(handle);
        return err;
    }

    err = input_open_device(handle);
    if (unlikely(err))
    {
        input_unregister_handle(handle);
        kfree(handle);
        return err;
    }

    printk(KERN_INFO "KayBeeStat: conn to dev: %s\n", dev->name);
    return 0;
}

static void kb_disconnect(struct input_handle *handle)
{
    printk(KERN_INFO "KayBeeStat: dc from dev: %s\n", handle->dev->name);

    input_close_device(handle);
    input_unregister_handle(handle);
    kfree(handle);
}

// module init

static int __init kb_init(void)
{
    int err = 0;
    size_t idx = 0;

    printk(KERN_INFO "KayBeeStat: loading...\n");

    kb_secs_ring = kvmalloc_array(KB_SECS_RING_SIZE, sizeof(kb_bucket_t), GFP_KERNEL | __GFP_ZERO);
    kb_mins_ring = kvmalloc_array(KB_MINS_RING_SIZE, sizeof(kb_bucket_t), GFP_KERNEL | __GFP_ZERO);
    kb_hours_ring = kvmalloc_array(KB_HOURS_RING_SIZE, sizeof(kb_bucket_t), GFP_KERNEL | __GFP_ZERO);
    kb_days_ring = kvmalloc_array(KB_DAYS_RING_SIZE, sizeof(kb_bucket_t), GFP_KERNEL | __GFP_ZERO);
    kb_scratch_timer = kvmalloc(sizeof(kb_bucket_t), GFP_KERNEL);
    kb_scratch_rd = kvmalloc(sizeof(kb_bucket_t), GFP_KERNEL);

    if (unlikely(!kb_secs_ring || !kb_mins_ring || !kb_hours_ring || !kb_days_ring || !kb_scratch_timer || !kb_scratch_rd))
    {
        printk(KERN_ERR "KayBeeStat: failed to alloc ring buffers\n");
        kvfree(kb_secs_ring);
        kvfree(kb_mins_ring);
        kvfree(kb_hours_ring);
        kvfree(kb_days_ring);
        kvfree(kb_scratch_timer);
        kvfree(kb_scratch_rd);
        return -ENOMEM;
    }

    for (idx = 0; idx < KB_SECS_RING_SIZE; idx++) { kb_secs_ring[idx].shortest_gap_ns = U64_MAX; }
    for (idx = 0; idx < KB_MINS_RING_SIZE; idx++) { kb_mins_ring[idx].shortest_gap_ns = U64_MAX; }
    for (idx = 0; idx < KB_HOURS_RING_SIZE; idx++) { kb_hours_ring[idx].shortest_gap_ns = U64_MAX; }
    for (idx = 0; idx < KB_DAYS_RING_SIZE; idx++) { kb_days_ring[idx].shortest_gap_ns = U64_MAX; }

    kb_bucket_zero(&kb_live);
    memset(kb_key_press_ts, 0, sizeof(kb_key_press_ts));

    kb_init_ns = ktime_get_ns();

    err = input_register_handler(&kb_handler);
    if (unlikely(err))
    {
        printk(KERN_ERR "KayBeeStat: failed to reg input handler\n");
        kvfree(kb_secs_ring);
        kvfree(kb_mins_ring);
        kvfree(kb_hours_ring);
        kvfree(kb_days_ring);
        kvfree(kb_scratch_timer);
        kvfree(kb_scratch_rd);
        return err;
    }

    err = misc_register(&kb_misc_dev);
    if (unlikely(err))
    {
        printk(KERN_ERR "KayBeeStat: failed to reg misc dev\n");
        input_unregister_handler(&kb_handler);
        kvfree(kb_secs_ring);
        kvfree(kb_mins_ring);
        kvfree(kb_hours_ring);
        kvfree(kb_days_ring);
        kvfree(kb_scratch_timer);
        kvfree(kb_scratch_rd);
        return err;
    }

    timer_setup(&kb_timer, kb_timer_cb, 0);
    mod_timer(&kb_timer, jiffies + HZ);

    printk(KERN_INFO "KayBeeStat: module init; /dev/kaybeestat ready\n");
    return 0;
}

static void __exit kb_exit(void)
{
    printk(KERN_INFO "KayBeeStat: unloading...\n");

    WRITE_ONCE(kb_shutdown, 1);
    smp_wmb();
    timer_delete_sync(&kb_timer);

    misc_deregister(&kb_misc_dev);
    input_unregister_handler(&kb_handler);

    spin_lock(&kb_lock);
    spin_unlock(&kb_lock);

    kvfree(kb_secs_ring);
    kvfree(kb_mins_ring);
    kvfree(kb_hours_ring);
    kvfree(kb_days_ring);
    kvfree(kb_scratch_timer);
    kvfree(kb_scratch_rd);

    printk(KERN_INFO "KayBeeStat: unloaded\n");
}

module_init(kb_init);
module_exit(kb_exit);
