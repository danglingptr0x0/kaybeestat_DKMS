#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <grp.h>
#include <time.h>

#define KB_KEY_MAX 768
#define KB_WINDOW_CUNT 8
#define KB_STATE_DIR "/var/lib/kaybeestat"
#define KB_STATE_FILE KB_STATE_DIR "/stats.bin"
#define KB_PUB_FILE KB_STATE_DIR "/stats.pub"
#define KB_DEV "/dev/kaybeestat"
#define KB_SAVE_INTERVAL_SECS 60

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

typedef struct
{
    uint64_t total_uptime_ns;
    uint64_t total_keystrokes;
    uint64_t total_releases;
    uint64_t total_char_dels;
    uint64_t total_word_dels;
} kb_persistent_t;

static volatile sig_atomic_t kb_running = 1;
static kb_persistent_t kb_baseline = { 0 };

static void kb_signal_handler(int sig)
{
    (void)sig;
    kb_running = 0;
}

static int kb_state_dir_ensure(void)
{
    struct group *grp = NULL;
    gid_t gid = 0;

    if (mkdir(KB_STATE_DIR, 0750) < 0 && errno != EEXIST) { return -1; }

    grp = getgrnam("kaybeestat");
    if (grp) { gid = grp->gr_gid; }

    (void)chown(KB_STATE_DIR, 0, gid);
    (void)chmod(KB_STATE_DIR, 0750);

    return 0;
}

static int kb_state_load(void)
{
    int fd = 0;
    ssize_t ret = 0;

    fd = open(KB_STATE_FILE, O_RDONLY);
    if (fd < 0) { return (errno == ENOENT) ? 0 : -1; }

    ret = read(fd, &kb_baseline, sizeof(kb_baseline));
    close(fd);

    if (ret != sizeof(kb_baseline))
    {
        memset(&kb_baseline, 0, sizeof(kb_baseline));
        return -1;
    }

    return 0;
}

static int kb_state_save(const kb_persistent_t *state)
{
    int fd = 0;
    ssize_t ret = 0;
    char tmp[256] = { 0 };

    snprintf(tmp, sizeof(tmp), "%s.tmp", KB_STATE_FILE);

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { return -1; }

    ret = write(fd, state, sizeof(*state));
    if (ret != sizeof(*state))
    {
        close(fd);
        unlink(tmp);
        return -1;
    }

    fsync(fd);
    close(fd);

    if (rename(tmp, KB_STATE_FILE) < 0)
    {
        unlink(tmp);
        return -1;
    }

    return 0;
}

static int kb_pub_file_write(const kb_stats_pub_t *pub)
{
    int fd = 0;
    ssize_t ret = 0;
    char tmp[256] = { 0 };
    struct group *grp = NULL;
    gid_t gid = 0;

    snprintf(tmp, sizeof(tmp), "%s.tmp", KB_PUB_FILE);

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd < 0) { return -1; }

    ret = write(fd, pub, sizeof(*pub));
    if (ret != sizeof(*pub))
    {
        close(fd);
        unlink(tmp);
        return -1;
    }

    fsync(fd);
    close(fd);

    grp = getgrnam("kaybeestat");
    if (grp) { gid = grp->gr_gid; }

    (void)chown(tmp, 0, gid);

    if (rename(tmp, KB_PUB_FILE) < 0)
    {
        unlink(tmp);
        return -1;
    }

    return 0;
}

static int kb_device_read(kb_stats_t *stats)
{
    int fd = 0;
    ssize_t ret = 0;

    fd = open(KB_DEV, O_RDONLY);
    if (fd < 0) { return -1; }

    ret = read(fd, stats, sizeof(*stats));
    close(fd);

    return (ret == sizeof(*stats)) ? 0 : -1;
}

static void kb_stats_accumulate(kb_persistent_t *accum, const kb_stats_t *current)
{
    accum->total_uptime_ns = kb_baseline.total_uptime_ns + current->uptime_ns;
    accum->total_keystrokes = kb_baseline.total_keystrokes + current->windows[0].keystroke_cunt;
    accum->total_releases = kb_baseline.total_releases + current->windows[0].release_cunt;
    accum->total_char_dels = kb_baseline.total_char_dels + current->windows[0].char_del_cunt;
    accum->total_word_dels = kb_baseline.total_word_dels + current->windows[0].word_del_cunt;
}

static void kb_pub_build(kb_stats_pub_t *pub, const kb_stats_t *current, const kb_persistent_t *accum)
{
    size_t i = 0;

    memset(pub, 0, sizeof(*pub));
    pub->uptime_ns = accum->total_uptime_ns;

    for (i = 0; i < KB_WINDOW_CUNT; i++)
    {
        pub->windows[i].keystroke_cunt = current->windows[i].keystroke_cunt;
        pub->windows[i].release_cunt = current->windows[i].release_cunt;
        pub->windows[i].char_cunt = current->windows[i].char_cunt;
        pub->windows[i].char_del_cunt = current->windows[i].char_del_cunt;
        pub->windows[i].word_del_cunt = current->windows[i].word_del_cunt;
        pub->windows[i].avg_kps = current->windows[i].avg_kps;
        pub->windows[i].avg_cps = current->windows[i].avg_cps;
        pub->windows[i].peak_kps = current->windows[i].peak_kps;
        pub->windows[i].avg_hold_ns = current->windows[i].avg_hold_ns;
        pub->windows[i].hold_var_ns = current->windows[i].hold_var_ns;
        pub->windows[i].longest_hold_ns = current->windows[i].longest_hold_ns;
        pub->windows[i].avg_gap_ns = current->windows[i].avg_gap_ns;
        pub->windows[i].gap_var_ns = current->windows[i].gap_var_ns;
        pub->windows[i].shortest_gap_ns = current->windows[i].shortest_gap_ns;
        pub->windows[i].longest_gap_ns = current->windows[i].longest_gap_ns;
    }

    pub->windows[0].keystroke_cunt = accum->total_keystrokes;
    pub->windows[0].release_cunt = accum->total_releases;
    pub->windows[0].char_del_cunt = accum->total_char_dels;
    pub->windows[0].word_del_cunt = accum->total_word_dels;
}

int main(void)
{
    kb_stats_t current = { 0 };
    kb_stats_pub_t pub = { 0 };
    kb_persistent_t accum = { 0 };
    time_t last_save = 0;
    uint64_t last_module_uptime = 0;

    signal(SIGTERM, kb_signal_handler);
    signal(SIGINT, kb_signal_handler);

    if (kb_state_dir_ensure() < 0)
    {
        fprintf(stderr, "kaybeestatd: failed to create state dir\n");
        return 1;
    }

    (void)kb_state_load();

    fprintf(stdout, "kaybeestatd: started; baseline: %lu keystrokes\n", (unsigned long)kb_baseline.total_keystrokes);

    last_save = time(NULL);

    while (kb_running)
    {
        time_t now = time(NULL);

        if (kb_device_read(&current) == 0)
        {
            if ((current.uptime_ns < last_module_uptime || (current.uptime_ns < 1000000000ULL && last_module_uptime > 60000000000ULL)) && last_module_uptime > 0)
            {
                fprintf(stdout, "kaybeestatd: module reload detected; committing baseline\n");
                kb_baseline = accum;
                (void)kb_state_save(&kb_baseline);
            }

            last_module_uptime = current.uptime_ns;

            kb_stats_accumulate(&accum, &current);
            kb_pub_build(&pub, &current, &accum);
            (void)kb_pub_file_write(&pub);

            if (now - last_save >= KB_SAVE_INTERVAL_SECS) { if (kb_state_save(&accum) == 0) { last_save = now; } }
        }

        sleep(1);
    }

    if (kb_device_read(&current) == 0)
    {
        kb_stats_accumulate(&accum, &current);
        (void)kb_state_save(&accum);
    }

    fprintf(stdout, "kaybeestatd: shutdown; saved %lu keystrokes\n", (unsigned long)accum.total_keystrokes);

    return 0;
}
