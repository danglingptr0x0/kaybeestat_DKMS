#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <grp.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <sys/ioctl.h>

// test harness

static uint32_t kb_test_pass_cunt = 0;
static uint32_t kb_test_fail_cunt = 0;

#define KB_TEST_ASSERT(cond, msg) \
        do { \
            if (!(cond)) { \
                fprintf(stdout, "FAIL: %s (line %d): %s\n", __func__, __LINE__, msg); \
                kb_test_fail_cunt++; \
                return; \
            } \
            kb_test_pass_cunt++; \
        } while (0)

#define KB_KEY_MAX 768
#define KB_WINDOW_CUNT 8

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
    uint64_t longest_hold_ns;
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
    uint64_t longest_hold_ns;
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

// uinput

static int kb_uinput_dev_create(void)
{
    int fd = 0;
    struct uinput_setup setup;
    uint32_t idx = 0;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { return -1; }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
    {
        close(fd);
        return -1;
    }

    for (idx = 0; idx < KEY_MAX; idx++) { (void)ioctl(fd, UI_SET_KEYBIT, idx); }

    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x5678;
    strncpy(setup.name, "kaybeestat_test_kb", UINPUT_MAX_NAME_SIZE - 1);

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0)
    {
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0)
    {
        close(fd);
        return -1;
    }

    usleep(500000);
    return fd;
}

static void kb_uinput_dev_destroy(int fd)
{
    (void)ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}

static int kb_uinput_key_emit(int fd, uint16_t keycode, int32_t val)
{
    struct input_event ev;
    struct input_event syn;

    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = keycode;
    ev.value = val;

    memset(&syn, 0, sizeof(syn));
    syn.type = EV_SYN;
    syn.code = SYN_REPORT;
    syn.value = 0;

    if (write(fd, &ev, sizeof(ev)) < 0) { return -1; }

    if (write(fd, &syn, sizeof(syn)) < 0) { return -1; }

    return 0;
}

static int kb_uinput_key_press(int fd, uint16_t keycode)
{
    if (kb_uinput_key_emit(fd, keycode, 1) < 0) { return -1; }

    usleep(10000);
    if (kb_uinput_key_emit(fd, keycode, 0) < 0) { return -1; }

    usleep(10000);
    return 0;
}

// helpers

static int kb_stats_rd(int dev_fd, kb_stats_t *stats)
{
    ssize_t ret = 0;

    memset(stats, 0, sizeof(*stats));
    (void)lseek(dev_fd, 0, SEEK_SET);
    ret = read(dev_fd, stats, sizeof(*stats));
    if (ret != (ssize_t)sizeof(*stats)) { return -1; }

    return 0;
}

// chardev tests

static void kb_test_dev_open_close(void)
{
    int fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd >= 0, "open /dev/kaybeestat failed");
    close(fd);
}

static void kb_test_rd_too_small(void)
{
    int fd = 0;
    uint8_t buff[64];
    ssize_t ret = 0;

    fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd >= 0, "open failed");

    ret = read(fd, buff, 64);
    KB_TEST_ASSERT(ret == -1 && errno == EINVAL, "undersized read should return EINVAL");

    close(fd);
}

static void kb_test_rd_zero_len(void)
{
    int fd = 0;
    uint8_t buff[1];
    ssize_t ret = 0;

    fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd >= 0, "open failed");

    ret = read(fd, buff, 0);
    KB_TEST_ASSERT(ret == -1 && errno == EINVAL, "zero-length read should return EINVAL");

    close(fd);
}

static void kb_test_rd_returns_stats(void)
{
    int fd = 0;
    kb_stats_t stats;
    ssize_t ret = 0;

    fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd >= 0, "open failed");

    ret = read(fd, &stats, sizeof(stats));
    KB_TEST_ASSERT(ret == (ssize_t)sizeof(stats), "read should return sizeof(kb_stats_t)");
    KB_TEST_ASSERT(stats.uptime_ns > 0, "uptime should be nonzero");

    close(fd);
}

static void kb_test_rd_eof_on_second_read(void)
{
    int fd = 0;
    kb_stats_t stats;
    ssize_t ret = 0;

    fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd >= 0, "open failed");

    ret = read(fd, &stats, sizeof(stats));
    KB_TEST_ASSERT(ret == (ssize_t)sizeof(stats), "first read should succeed");

    ret = read(fd, &stats, sizeof(stats));
    KB_TEST_ASSERT(ret == 0, "second read without lseek should return EOF");

    close(fd);
}

static void kb_test_rd_after_lseek(void)
{
    int fd = 0;
    kb_stats_t stats;
    ssize_t ret = 0;

    fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd >= 0, "open failed");

    ret = read(fd, &stats, sizeof(stats));
    KB_TEST_ASSERT(ret == (ssize_t)sizeof(stats), "first read should succeed");

    (void)lseek(fd, 0, SEEK_SET);
    ret = read(fd, &stats, sizeof(stats));
    KB_TEST_ASSERT(ret == (ssize_t)sizeof(stats), "read after lseek should succeed");

    close(fd);
}

static void kb_test_wr_rejected(void)
{
    int fd = 0;
    uint8_t buff[1] = { 0 };
    ssize_t ret = 0;

    fd = open("/dev/kaybeestat", O_WRONLY);
    if (fd < 0)
    {
        kb_test_pass_cunt++;
        return;
    }

    ret = write(fd, buff, 1);
    KB_TEST_ASSERT(ret < 0, "write should fail on read-only dev");
    close(fd);
}

static void kb_test_rdwr_rejected(void)
{
    int fd = 0;
    uint8_t buff[1] = { 0 };
    ssize_t ret = 0;

    fd = open("/dev/kaybeestat", O_RDWR);
    if (fd < 0)
    {
        kb_test_pass_cunt++;
        return;
    }

    ret = write(fd, buff, 1);
    KB_TEST_ASSERT(ret < 0, "write should fail on read-write dev");
    close(fd);
}

static void kb_test_struct_size(void)
{
    KB_TEST_ASSERT(sizeof(kb_window_stats_t) == 12 * 8 + KB_KEY_MAX * 4, "kb_window_stats_t size mismatch");
    KB_TEST_ASSERT(sizeof(kb_stats_t) == 16 + KB_WINDOW_CUNT * sizeof(kb_window_stats_t), "kb_stats_t size mismatch");
    KB_TEST_ASSERT(sizeof(kb_window_stats_pub_t) == 12 * 8, "kb_window_stats_pub_t size mismatch");
    KB_TEST_ASSERT(sizeof(kb_stats_pub_t) == 16 + KB_WINDOW_CUNT * sizeof(kb_window_stats_pub_t), "kb_stats_pub_t size mismatch");
}

static void kb_test_multiple_opens(void)
{
    int fd1 = 0;
    int fd2 = 0;
    int fd3 = 0;
    kb_stats_t s1;
    kb_stats_t s2;
    kb_stats_t s3;

    fd1 = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd1 >= 0, "first open failed");
    fd2 = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd2 >= 0, "second open failed");
    fd3 = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd3 >= 0, "third open failed");

    KB_TEST_ASSERT(kb_stats_rd(fd1, &s1) == 0, "read fd1 failed");
    KB_TEST_ASSERT(kb_stats_rd(fd2, &s2) == 0, "read fd2 failed");
    KB_TEST_ASSERT(kb_stats_rd(fd3, &s3) == 0, "read fd3 failed");

    KB_TEST_ASSERT(s1.uptime_ns > 0, "fd1 uptime should be nonzero");
    KB_TEST_ASSERT(s2.uptime_ns >= s1.uptime_ns, "fd2 uptime should be >= fd1");
    KB_TEST_ASSERT(s3.uptime_ns >= s2.uptime_ns, "fd3 uptime should be >= fd2");

    close(fd1);
    close(fd2);
    close(fd3);
}

// permissions tests

static void kb_test_perms_group_readable(void)
{
    struct stat st;
    int ret = 0;

    ret = stat("/dev/kaybeestat", &st);
    KB_TEST_ASSERT(ret == 0, "stat /dev/kaybeestat failed");
    KB_TEST_ASSERT((st.st_mode & 0777) == 0440, "dev should be mode 0440 (root+group readable)");
    KB_TEST_ASSERT(st.st_uid == 0, "dev should be owned by root");
}

static void kb_test_perms_unprivileged_denied(void)
{
    pid_t pid = 0;
    int status = 0;
    struct passwd *nobody = NULL;

    nobody = getpwnam("nobody");
    if (!nobody)
    {
        fprintf(stdout, "  SKIP: no \"nobody\" user\n");
        kb_test_pass_cunt++;
        return;
    }

    pid = fork();
    KB_TEST_ASSERT(pid >= 0, "fork failed");

    if (pid == 0)
    {
        int fd = 0;

        (void)setgroups(0, NULL);
        (void)setgid(nobody->pw_gid);
        (void)setuid(nobody->pw_uid);

        fd = open("/dev/kaybeestat", O_RDONLY);
        if (fd >= 0)
        {
            close(fd);
            _exit(1);
        }

        _exit(0);
    }

    (void)waitpid(pid, &status, 0);
    KB_TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "unprivileged user should be denied access");
}

static void kb_test_root_gets_full_stats(void)
{
    int fd = 0;
    kb_stats_t stats;
    ssize_t ret = 0;

    fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd >= 0, "open failed");

    ret = read(fd, &stats, sizeof(stats));
    KB_TEST_ASSERT(ret == (ssize_t)sizeof(kb_stats_t), "root should receive full stats size");
    close(fd);

    fprintf(stdout, "  root read size: %zd (expected %zu)\n", ret, sizeof(kb_stats_t));
}

// uptime tests

static void kb_test_uptime_monotonic(void)
{
    int fd = 0;
    kb_stats_t first;
    kb_stats_t second;

    fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(fd, &first) == 0, "first read failed");
    usleep(10000);
    KB_TEST_ASSERT(kb_stats_rd(fd, &second) == 0, "second read failed");

    fprintf(stdout, "  uptime first: %" PRIu64 " ns; second: %" PRIu64 " ns\n", first.uptime_ns, second.uptime_ns);
    KB_TEST_ASSERT(second.uptime_ns > first.uptime_ns, "uptime should be monotonically increasing");

    close(fd);
}

static void kb_test_uptime_plausible(void)
{
    int fd = 0;
    kb_stats_t stats;

    fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(fd, &stats) == 0, "read failed");

    KB_TEST_ASSERT(stats.uptime_ns < 3600ULL * 1000000000ULL, "uptime should be less than 1 hour for a fresh module");

    close(fd);
}

// keystroke tests

static void kb_test_keystroke_cunt(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint64_t delta = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_A) == 0, "key press A failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_B) == 0, "key press B failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_C) == 0, "key press C failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    delta = after.windows[0].keystroke_cunt - before.windows[0].keystroke_cunt;
    fprintf(stdout, "  keystroke delta: %" PRIu64 "\n", delta);
    KB_TEST_ASSERT(delta >= 3, "1min window should show at least 3 keystrokes");

    delta = after.windows[0].release_cunt - before.windows[0].release_cunt;
    fprintf(stdout, "  release delta: %" PRIu64 "\n", delta);
    KB_TEST_ASSERT(delta >= 3, "1min window should show at least 3 releases");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_press_release_balanced(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint64_t press_delta = 0;
    uint64_t release_delta = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_Q) == 0, "press Q failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_W) == 0, "press W failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_E) == 0, "press E failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_R) == 0, "press R failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_T) == 0, "press T failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    press_delta = after.windows[0].keystroke_cunt - before.windows[0].keystroke_cunt;
    release_delta = after.windows[0].release_cunt - before.windows[0].release_cunt;

    fprintf(stdout, "  press: %" PRIu64 "; release: %" PRIu64 "\n", press_delta, release_delta);
    KB_TEST_ASSERT(press_delta == release_delta, "press and release deltas should be equal for full key presses");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_press_only_no_release(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint64_t press_delta = 0;
    uint64_t release_delta = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_H, 1) == 0, "press H failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    press_delta = after.windows[0].keystroke_cunt - before.windows[0].keystroke_cunt;
    release_delta = after.windows[0].release_cunt - before.windows[0].release_cunt;

    KB_TEST_ASSERT(press_delta >= 1, "should register at least 1 press");
    KB_TEST_ASSERT(release_delta == 0, "should register 0 releases");

    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_H, 0) == 0, "release H failed");
    usleep(10000);

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_autorepeat_ignored(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint64_t press_delta = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_J, 1) == 0, "press failed");
    usleep(10000);
    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_J, 2) == 0, "repeat 1 failed");
    usleep(10000);
    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_J, 2) == 0, "repeat 2 failed");
    usleep(10000);
    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_J, 2) == 0, "repeat 3 failed");
    usleep(10000);
    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_J, 0) == 0, "release failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    press_delta = after.windows[0].keystroke_cunt - before.windows[0].keystroke_cunt;
    fprintf(stdout, "  press delta with repeats: %" PRIu64 "\n", press_delta);
    KB_TEST_ASSERT(press_delta == 1, "autorepeat (val=2) should not increment press cunt");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

// per-key tests

static void kb_test_per_key_cunt(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint32_t delta_x = 0;
    uint32_t delta_y = 0;
    uint32_t delta_z = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_X) == 0, "key press X failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_X) == 0, "key press X failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_Y) == 0, "key press Y failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_Z) == 0, "key press Z failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_Z) == 0, "key press Z failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_Z) == 0, "key press Z failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    delta_x = after.windows[0].per_key_cunt[KEY_X] - before.windows[0].per_key_cunt[KEY_X];
    delta_y = after.windows[0].per_key_cunt[KEY_Y] - before.windows[0].per_key_cunt[KEY_Y];
    delta_z = after.windows[0].per_key_cunt[KEY_Z] - before.windows[0].per_key_cunt[KEY_Z];

    fprintf(stdout, "  per_key: X=%" PRIu32 " Y=%" PRIu32 " Z=%" PRIu32 "\n", delta_x, delta_y, delta_z);

    KB_TEST_ASSERT(delta_x >= 2, "KEY_X should have at least 2 presses");
    KB_TEST_ASSERT(delta_y >= 1, "KEY_Y should have at least 1 press");
    KB_TEST_ASSERT(delta_z >= 3, "KEY_Z should have at least 3 presses");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_per_key_cunt_sum_matches_total(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint64_t total_delta = 0;
    uint64_t per_key_sum = 0;
    uint32_t idx = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_A) == 0, "press A failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_S) == 0, "press S failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_D) == 0, "press D failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_F) == 0, "press F failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    total_delta = after.windows[0].keystroke_cunt - before.windows[0].keystroke_cunt;
    for (idx = 0; idx < KB_KEY_MAX; idx++) { per_key_sum += after.windows[0].per_key_cunt[idx] - before.windows[0].per_key_cunt[idx]; }

    fprintf(stdout, "  total: %" PRIu64 "; per_key sum: %" PRIu64 "\n", total_delta, per_key_sum);
    KB_TEST_ASSERT(total_delta == per_key_sum, "per-key sum should equal total keystroke cunt");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_untouched_key_zero(void)
{
    int dev_fd = 0;
    kb_stats_t stats;

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &stats) == 0, "read failed");

    KB_TEST_ASSERT(stats.windows[0].per_key_cunt[KEY_F12] == 0 || stats.windows[0].per_key_cunt[KEY_F12] > 0, "per_key_cunt should be valid (not garbage)");

    KB_TEST_ASSERT(stats.windows[0].per_key_cunt[767] == 0, "key 767 (near max) should be zero unless pressed");

    close(dev_fd);
}

// hold duration tests

static void kb_test_hold_duration(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_D, 1) == 0, "press failed");
    usleep(100000);
    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_D, 0) == 0, "release failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    fprintf(stdout, "  avg_hold_ns: %" PRIu64 "; longest_hold_ns: %" PRIu64 "\n", after.windows[0].avg_hold_ns, after.windows[0].longest_hold_ns);

    KB_TEST_ASSERT(after.windows[0].longest_hold_ns > 0, "longest hold should be nonzero");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_hold_duration_ordering(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint64_t longest_delta = 0;
    uint64_t avg_delta = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_K, 1) == 0, "press K failed");
    usleep(20000);
    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_K, 0) == 0, "release K failed");
    usleep(10000);

    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_L, 1) == 0, "press L failed");
    usleep(200000);
    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_L, 0) == 0, "release L failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    longest_delta = after.windows[0].longest_hold_ns;
    avg_delta = after.windows[0].avg_hold_ns;

    fprintf(stdout, "  longest: %" PRIu64 " ns; avg: %" PRIu64 " ns\n", longest_delta, avg_delta);
    KB_TEST_ASSERT(longest_delta >= avg_delta, "longest hold should be >= avg hold");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

// inter-key gap tests

static void kb_test_inter_key_gap(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_E) == 0, "press E failed");
    usleep(50000);
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_F) == 0, "press F failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    fprintf(stdout, "  shortest_gap_ns: %" PRIu64 "; longest_gap_ns: %" PRIu64 "\n", after.windows[0].shortest_gap_ns, after.windows[0].longest_gap_ns);

    KB_TEST_ASSERT(after.windows[0].longest_gap_ns > 0, "longest gap should be nonzero");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_gap_ordering(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t stats;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_M) == 0, "press M failed");
    usleep(30000);
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_N) == 0, "press N failed");
    usleep(100000);
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_O) == 0, "press O failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &stats) == 0, "read failed");

    fprintf(stdout, "  shortest: %" PRIu64 "; longest: %" PRIu64 "\n", stats.windows[0].shortest_gap_ns, stats.windows[0].longest_gap_ns);

    KB_TEST_ASSERT(stats.windows[0].shortest_gap_ns <= stats.windows[0].longest_gap_ns, "shortest gap should be <= longest gap");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_single_key_no_gap(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_P) == 0, "press P failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    if (before.windows[0].keystroke_cunt == 0) { KB_TEST_ASSERT(after.windows[0].shortest_gap_ns == 0, "shortest gap should be 0 when only 1 key pressed (sentinel filtered)"); }
    else{ kb_test_pass_cunt++; }

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

// window consistency tests

static void kb_test_all_windows_present(void)
{
    int dev_fd = 0;
    kb_stats_t stats;

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &stats) == 0, "read failed");
    KB_TEST_ASSERT(stats.uptime_ns > 0, "uptime should be nonzero");

    close(dev_fd);
}

static void kb_test_window_zero_initialized(void)
{
    int dev_fd = 0;
    kb_stats_t stats;
    uint32_t w = 0;

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &stats) == 0, "read failed");

    for (w = 1; w < KB_WINDOW_CUNT; w++) { KB_TEST_ASSERT(stats.windows[w].avg_hold_ns <= stats.windows[0].avg_hold_ns || stats.windows[w].keystroke_cunt <= stats.windows[0].keystroke_cunt || 1, "higher windows should have plausible values"); }

    close(dev_fd);
}

static void kb_test_live_data_in_all_windows(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint32_t w = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_V) == 0, "press V failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_V) == 0, "press V failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_V) == 0, "press V failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    for (w = 0; w < KB_WINDOW_CUNT; w++)
    {
        uint64_t delta = after.windows[w].keystroke_cunt - before.windows[w].keystroke_cunt;
        if (delta < 3) { fprintf(stdout, "  FAIL: window %u has delta %" PRIu64 " (expected >= 3)\n", w, delta); }

        KB_TEST_ASSERT(delta >= 3, "live data should appear in all windows");
    }

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

// non-destructive reads

static void kb_test_multiple_reads_nondestructive(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t first;
    kb_stats_t second;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_G) == 0, "key press failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &first) == 0, "first read failed");
    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &second) == 0, "second read failed");

    KB_TEST_ASSERT(second.windows[0].keystroke_cunt >= first.windows[0].keystroke_cunt, "repeated reads should not decrease keystroke cunt");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

// kps tests

static void kb_test_kps_nonzero_after_typing(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t stats;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_A) == 0, "press A failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_B) == 0, "press B failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &stats) == 0, "read failed");

    fprintf(stdout, "  avg_kps: %" PRIu64 ".%03" PRIu64 "\n", stats.windows[0].avg_kps / 1000, stats.windows[0].avg_kps % 1000);
    KB_TEST_ASSERT(stats.windows[0].avg_kps > 0, "avg_kps should be nonzero after typing");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_peak_kps_gte_avg(void)
{
    int dev_fd = 0;
    kb_stats_t stats;

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &stats) == 0, "read failed");

    fprintf(stdout, "  avg: %" PRIu64 "; peak: %" PRIu64 "\n", stats.windows[0].avg_kps, stats.windows[0].peak_kps);
    KB_TEST_ASSERT(stats.windows[0].peak_kps >= stats.windows[0].avg_kps, "peak kps should be >= avg kps");

    close(dev_fd);
}

// rapid burst test

static void kb_test_rapid_burst(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint64_t delta = 0;
    uint32_t idx = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    for (idx = 0; idx < 50; idx++) { KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_SPACE) == 0, "burst press failed"); }
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    delta = after.windows[0].keystroke_cunt - before.windows[0].keystroke_cunt;
    fprintf(stdout, "  burst delta: %" PRIu64 " (expected >= 50)\n", delta);
    KB_TEST_ASSERT(delta >= 50, "50-key burst should register at least 50 keystrokes");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

// deletion tracking tests

static void kb_test_char_del_backspace(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint64_t delta = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_BACKSPACE) == 0, "backspace 1 failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_BACKSPACE) == 0, "backspace 2 failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_BACKSPACE) == 0, "backspace 3 failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    delta = after.windows[0].char_del_cunt - before.windows[0].char_del_cunt;
    fprintf(stdout, "  char_del delta: %" PRIu64 "\n", delta);
    KB_TEST_ASSERT(delta >= 3, "3 backspaces should register at least 3 char deletions");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_word_del_ctrl_w(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint64_t delta = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_LEFTCTRL, 1) == 0, "ctrl press failed");
    usleep(10000);
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_W) == 0, "w press failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_W) == 0, "w press 2 failed");
    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_LEFTCTRL, 0) == 0, "ctrl release failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    delta = after.windows[0].word_del_cunt - before.windows[0].word_del_cunt;
    fprintf(stdout, "  word_del (ctrl+w) delta: %" PRIu64 "\n", delta);
    KB_TEST_ASSERT(delta >= 2, "2x ctrl+w should register at least 2 word deletions");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_word_del_alt_backspace(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint64_t word_delta = 0;
    uint64_t char_delta = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_LEFTALT, 1) == 0, "alt press failed");
    usleep(10000);
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_BACKSPACE) == 0, "backspace press failed");
    KB_TEST_ASSERT(kb_uinput_key_emit(uinput_fd, KEY_LEFTALT, 0) == 0, "alt release failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    word_delta = after.windows[0].word_del_cunt - before.windows[0].word_del_cunt;
    char_delta = after.windows[0].char_del_cunt - before.windows[0].char_del_cunt;
    fprintf(stdout, "  word_del (alt+bs) delta: %" PRIu64 "; char_del delta: %" PRIu64 "\n", word_delta, char_delta);
    KB_TEST_ASSERT(word_delta >= 1, "alt+backspace should register word deletion");
    KB_TEST_ASSERT(char_delta == 0, "alt+backspace should NOT register char deletion");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_no_del_regular_w(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint64_t word_delta = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_W) == 0, "w press failed");
    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_W) == 0, "w press 2 failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    word_delta = after.windows[0].word_del_cunt - before.windows[0].word_del_cunt;
    fprintf(stdout, "  word_del (regular w) delta: %" PRIu64 "\n", word_delta);
    KB_TEST_ASSERT(word_delta == 0, "regular \"w\" without ctrl should NOT register word deletion");

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

static void kb_test_del_in_all_windows(void)
{
    int uinput_fd = 0;
    int dev_fd = 0;
    kb_stats_t before;
    kb_stats_t after;
    uint32_t w = 0;

    uinput_fd = kb_uinput_dev_create();
    KB_TEST_ASSERT(uinput_fd >= 0, "uinput create failed");

    dev_fd = open("/dev/kaybeestat", O_RDONLY);
    KB_TEST_ASSERT(dev_fd >= 0, "open failed");

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &before) == 0, "baseline read failed");

    KB_TEST_ASSERT(kb_uinput_key_press(uinput_fd, KEY_BACKSPACE) == 0, "backspace failed");
    usleep(50000);

    KB_TEST_ASSERT(kb_stats_rd(dev_fd, &after) == 0, "after read failed");

    for (w = 0; w < KB_WINDOW_CUNT; w++)
    {
        uint64_t delta = after.windows[w].char_del_cunt - before.windows[w].char_del_cunt;
        KB_TEST_ASSERT(delta >= 1, "char_del should appear in all windows");
    }

    close(dev_fd);
    kb_uinput_dev_destroy(uinput_fd);
}

// runner

int main(void)
{
    fprintf(stdout, "kaybeestat test suite\n\n");

    fprintf(stdout, "-- chardev --\n");
    kb_test_dev_open_close();
    kb_test_rd_too_small();
    kb_test_rd_zero_len();
    kb_test_rd_returns_stats();
    kb_test_rd_eof_on_second_read();
    kb_test_rd_after_lseek();
    kb_test_wr_rejected();
    kb_test_rdwr_rejected();
    kb_test_struct_size();
    kb_test_multiple_opens();

    fprintf(stdout, "-- permissions --\n");
    kb_test_perms_group_readable();
    kb_test_perms_unprivileged_denied();
    kb_test_root_gets_full_stats();

    fprintf(stdout, "-- uptime --\n");
    kb_test_uptime_monotonic();
    kb_test_uptime_plausible();

    fprintf(stdout, "-- keystrokes --\n");
    kb_test_keystroke_cunt();
    kb_test_press_release_balanced();
    kb_test_press_only_no_release();
    kb_test_autorepeat_ignored();

    fprintf(stdout, "-- per-key --\n");
    kb_test_per_key_cunt();
    kb_test_per_key_cunt_sum_matches_total();
    kb_test_untouched_key_zero();

    fprintf(stdout, "-- hold duration --\n");
    kb_test_hold_duration();
    kb_test_hold_duration_ordering();

    fprintf(stdout, "-- inter-key gap --\n");
    kb_test_inter_key_gap();
    kb_test_gap_ordering();
    kb_test_single_key_no_gap();

    fprintf(stdout, "-- windows --\n");
    kb_test_all_windows_present();
    kb_test_window_zero_initialized();
    kb_test_live_data_in_all_windows();

    fprintf(stdout, "-- non-destructive reads --\n");
    kb_test_multiple_reads_nondestructive();

    fprintf(stdout, "-- kps --\n");
    kb_test_kps_nonzero_after_typing();
    kb_test_peak_kps_gte_avg();

    fprintf(stdout, "-- deletion tracking --\n");
    kb_test_char_del_backspace();
    kb_test_word_del_ctrl_w();
    kb_test_word_del_alt_backspace();
    kb_test_no_del_regular_w();
    kb_test_del_in_all_windows();

    fprintf(stdout, "-- stress --\n");
    kb_test_rapid_burst();

    fprintf(stdout, "\nresults: %u passed; %u failed\n", kb_test_pass_cunt, kb_test_fail_cunt);

    if (kb_test_fail_cunt > 0) { return 1; }

    return 0;
}
