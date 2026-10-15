
/*
  Three ways to make this work.
  
  1) Physically jumper 2 GPIO pins, and use one to read and other to write.
  2) use -m flag, this bypassed all checks and writes directly to register.  (only for Pi3 / 4)
  3) Use gpio-sim :-
       sudo modprobe gpio-sim
       cd /sys/kernel/config/gpio-sim
       sudo mkdir test test/bank0
       echo 8 | sudo tee test/bank0/num_lines
       echo 1 | sudo tee test/live
       gpiodetect                      # shows the new chip, e.g. gpiochip5
   
       MAke sure to use the gpiochip in both configs.


*/

/*
 * gpiotest.c - GPIO test tool for AquachemD: generate pulses, hold a level, or watch a pin.
 *
 * WHAT IT DOES (pick one)
 *
 *   pulse (default)  Toggle the pin at a frequency. One cycle = one rising edge, which is
 *                    what AquachemD counts. Tests gpio_pulse inputs (flow, RPM).
 *   -l 0|1           Hold the pin at a fixed level. Type 0, 1 or t (toggle) + Enter to
 *                    change it live. Tests gpio_input / gpio_condition inputs.
 *   -w               Watch the pin and print every change with a timestamp and how long
 *                    the previous state lasted. Tests AquachemD outputs (relays, pumps,
 *                    timers).
 *
 * HOW IT TOUCHES THE PIN (pick one)
 *
 *   (default)        Kernel GPIO driver. Drive: needs a jumper to AquachemD's input pin.
 *                    Watch: needs a jumper from AquachemD's output pin to a spare pin.
 *                    A line can only be owned by one process, so it cannot share a pin
 *                    with AquachemD.
 *   -m               Direct SoC registers via /dev/gpiomem. Works on the SAME pin AquachemD
 *                    is using, no jumper. Pi 4 / CM4 / Pi 3 and earlier only (not Pi 5).
 *                    Drive actions force the pin to an output and put its original function
 *                    back on exit. Watch only reads, it never changes the pin.
 *   -s dir           gpio-sim simulated chip (see below), no wiring at all.
 *
 *   Levels are PHYSICAL levels (0 = 0V, 1 = 3.3V). If AquachemD is configured active-low
 *   for that pin, 0 here is its "active" state. Pins are 3.3V only.
 *
 * NOTES
 * -----
 * Build (no libgpiod needed, uses the kernel GPIO uAPI / registers directly, Linux 5.10+):
 *
 *     gcc -Wall -Wextra gpiotest.c -o gpiotest
 *
 * Testing AquachemD INPUTS (same pin AquachemD monitors, register mode):
 *
 *     ./gpiotest 24 10 -m            pulses at 10 Hz       (gpio_pulse input)
 *     ./gpiotest 24 -l 1 -m          hold high, type 0/1/t (gpio_input / condition)
 *     ./gpiotest 24 -l 1 -m -t 5     hold high for 5 seconds then exit
 *
 * Testing AquachemD OUTPUTS (watch the pin AquachemD drives, register mode, read-only):
 *
 *     ./gpiotest 17 -w -m            print every change on GPIO 17
 *     ./gpiotest 17 -w -m -t 60      watch for 60 seconds
 *
 *   Without -m, jumper AquachemD's output pin to a spare pin and run ./gpiotest 23 -w
 *   (or ./gpiotest 23 -l 1 for inputs, with the jumper going the other way).
 *
 *   Needs access to /dev/gpiomem (root, or a member of the gpio group). If a drive-mode
 *   run is killed with SIGKILL the pin stays an output: restart AquachemD, or run gpiotest
 *   again and quit normally. Watch mode polls the pin every 250 us, so it can miss pulses
 *   shorter than that. Use it for relays and timers, not for counting fast pulses.
 *
 * gpio-sim setup (kernel 5.17+ with the gpio-sim module; check: modinfo gpio-sim).
 * Run as root. Creates a simulated chip with 8 lines:
 *
 *     sudo modprobe gpio-sim
 *     cd /sys/kernel/config/gpio-sim
 *     sudo mkdir -p test/bank0
 *     echo 8 | sudo tee test/bank0/num_lines
 *     echo 1 | sudo tee test/live
 *     cat test/dev_name test/bank0/chip_name     # e.g. gpio-sim.0 and gpiochip5
 *
 *   Point AquachemD at /dev/<chip_name> (the config option behind gpio_chip), then:
 *
 *     sudo ./gpiotest 3 10 -s auto           drive simulated line 3
 *     sudo ./gpiotest 3 -w -s auto           watch what AquachemD does to line 3
 *     sudo ./gpiotest 3 10 -s /sys/devices/platform/gpio-sim.0/gpiochip5
 *
 *   (gpio-sim paths and "pull"/"value" handling written from the kernel docs, so check
 *    Documentation/admin-guide/gpio/gpio-sim.rst if something doesn't match.)
 *   Remove the chip when finished:  echo 0 | sudo tee test/live; sudo rmdir test/bank0 test
 *
 * In sim mode each pulse costs two sysfs writes, so keep frequencies to a few kHz.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/gpio.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CHIP "/dev/gpiochip0"
#define DEFAULT_HZ   10.0
#define MAX_HZ       10000.0
#define SIM_GLOB     "/sys/devices/platform/gpio-sim.*/gpiochip*"

/* BCM2835 / BCM2711 GPIO registers, as 32-bit word offsets from the GPIO block */
#define REG_GPFSEL0  0     /* function select, 10 pins per word, 3 bits each */
#define REG_GPSET0   7     /* write 1 bit to drive pin high (GPSET1 = +1 for pins 32-57) */
#define REG_GPCLR0   10    /* write 1 bit to drive pin low  (GPCLR1 = +1 for pins 32-57) */
#define REG_GPLEV0   13    /* read pin levels (GPLEV1 = +1 for pins 32-57) */
#define MAX_REG_PIN  57
#define GPIOMEM_LEN  4096

#define WATCH_POLL_MEM_NS  250000L     /* register mode sample period */
#define WATCH_POLL_SIM_NS  1000000L    /* sim mode sample period */

typedef enum { MODE_REAL, MODE_SIM, MODE_MEM } drive_mode_t;
typedef enum { ACT_PULSE, ACT_LEVEL, ACT_WATCH } action_t;

static volatile sig_atomic_t stop_flag = 0;
static drive_mode_t mode = MODE_REAL;
static int line_fd = -1;       /* real: GPIO line fd.  sim: sysfs "pull"/"value" file fd */
static uint64_t deadline_ns = 0;

/* register mode state */
static volatile uint32_t *gpio_reg = NULL;
static int mem_pin = -1;
static int mem_watch = 0;
static uint32_t mem_orig_fsel = 0;

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int time_up(void)
{
    return deadline_ns && now_ns() >= deadline_ns;
}

static void usage(const char *prog)
{
    printf(
"Usage: %s <gpio_pin> [freq_hz] [-l 0|1 | -w] [-c chip | -m | -s sim_dir] [-n count] [-t secs] [-h]\n"
"\n"
"  gpio_pin    GPIO line number (same numbering as AquachemD's pin settings)\n"
"  freq_hz     Pulse mode only: pulses per second (default %.0f, 0 = hold low, max %.0f)\n"
"\n"
"What to do (default is pulse):\n"
"  -l 0|1      Hold the pin at 0 or 1. Type 0, 1 or t (toggle) + Enter to change it\n"
"  -w          Watch the pin and print every change with timestamps (read only)\n"
"\n"
"How to touch the pin (default is the kernel driver, which needs a jumper):\n"
"  -c chip     Kernel GPIO chip device (default %s)\n"
"  -m          Direct register access via /dev/gpiomem. Works on the same pin AquachemD\n"
"              is using, no jumper. Pi 4 / CM4 / Pi 3 and earlier only, not Pi 5\n"
"  -s sim_dir  gpio-sim chip: its sysfs directory, e.g.\n"
"              /sys/devices/platform/gpio-sim.0/gpiochip5, or 'auto' (run as root)\n"
"\n"
"Limits:\n"
"  -n count    Pulse mode: stop after this many pulses. Watch mode: after this many changes\n"
"  -t secs     Stop after this many seconds (any mode)\n"
"  -h          Show this help\n"
"\n"
"Interactive keys: pulse mode takes a new frequency (0 pauses), level mode takes 0 / 1 / t,\n"
"and q quits in every mode. Levels are physical (0 = 0V, 1 = 3.3V).\n"
"\n"
"Expected AquachemD pulse readings:\n"
"  RPM : Hz * 60 / k_factor   (k_factor = pulses per revolution)\n"
"  LPM : Hz / k_factor        (k_factor = Hz per L/min)\n"
"\n"
"Examples:\n"
"  %s 24 7.5 -m           pulses on the pin AquachemD monitors (k_factor 7.5 -> 1 L/min)\n"
"  %s 24 -l 1 -m          hold a condition/input pin high\n"
"  %s 17 -w -m            print every change on an AquachemD output pin\n"
"  %s 23 20 -n 500        500 pulses at 20 Hz on a jumpered pin\n"
"  %s 3 -w -s auto        watch simulated line 3\n"
"\n"
"See the NOTES at the top of gpiotest.c for build and gpio-sim setup steps.\n",
        prog, DEFAULT_HZ, MAX_HZ, DEFAULT_CHIP, prog, prog, prog, prog, prog);
}

/* Drive the line high or low, in whichever mode we are in. Returns 0 on success. */
static int set_line(int value)
{
    if (mode == MODE_MEM) {
        uint32_t bank = (uint32_t)mem_pin >> 5;
        uint32_t bit  = 1u << (mem_pin & 31);
        gpio_reg[(value ? REG_GPSET0 : REG_GPCLR0) + bank] = bit;
        return 0;
    }

    if (mode == MODE_SIM) {
        const char *s = value ? "pull-up" : "pull-down";
        return pwrite(line_fd, s, strlen(s), 0) < 0 ? -1 : 0;
    }

    struct gpio_v2_line_values v;
    memset(&v, 0, sizeof(v));
    v.mask = 1;
    v.bits = value ? 1 : 0;
    return ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

/* Read the current level (watch mode). Returns 0/1, or -1 on error. */
static int read_level(void)
{
    if (mode == MODE_MEM)
        return (int)((gpio_reg[REG_GPLEV0 + ((uint32_t)mem_pin >> 5)] >> (mem_pin & 31)) & 1u);

    if (mode == MODE_SIM) {
        char c;
        if (pread(line_fd, &c, 1, 0) != 1) return -1;
        return c == '1';
    }

    struct gpio_v2_line_values v;
    memset(&v, 0, sizeof(v));
    v.mask = 1;
    if (ioctl(line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) return -1;
    return (int)(v.bits & 1);
}

static void add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec++;
    }
}

/* Non-blocking: returns 1 and fills buf with ONE line if a command is waiting on stdin.
 * Keeps leftover input so several lines arriving together (piped input) are each handled. */
static int read_cmd(char *buf, size_t n, int *stdin_ok)
{
    static char in[256];
    static size_t len = 0;

    char *nl = memchr(in, '\n', len);
    if (!nl && *stdin_ok) {
        struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
        if (poll(&p, 1, 0) > 0) {
            ssize_t r = read(STDIN_FILENO, in + len, sizeof(in) - 1 - len);
            if (r <= 0) *stdin_ok = 0;                 /* EOF: keep running as-is */
            else        len += (size_t)r;
        }
        nl = memchr(in, '\n', len);
    }
    if (!nl && !*stdin_ok && len > 0) nl = in + len;   /* last line had no newline */
    if (!nl) {
        if (len >= sizeof(in) - 1) len = 0;            /* overlong junk, drop it */
        return 0;
    }

    size_t linelen  = (size_t)(nl - in);
    size_t take     = linelen < n - 1 ? linelen : n - 1;
    size_t consumed = linelen + (nl < in + len ? 1 : 0);

    memcpy(buf, in, take);
    buf[take] = '\0';
    memmove(in, in + consumed, len - consumed);
    len -= consumed;
    return take > 0;                                   /* blank lines are ignored */
}

/* Resolve the gpio-sim chip directory ('auto' searches sysfs). */
static int sim_base(const char *dir, char *base, size_t len)
{
    glob_t g;

    if (strcmp(dir, "auto") != 0) {
        snprintf(base, len, "%s", dir);
        return 0;
    }
    if (glob(SIM_GLOB, 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        fprintf(stderr, "No gpio-sim chip found under %s\n"
                        "Is the module loaded and the chip live? (see NOTES in gpiotest.c)\n",
                        SIM_GLOB);
        globfree(&g);
        return -1;
    }
    snprintf(base, len, "%s", g.gl_pathv[0]);
    if (g.gl_pathc > 1)
        printf("Several gpio-sim chips found, using %s\n", base);
    globfree(&g);
    return 0;
}

/* Kernel driver: request the line. Drive = output at init_level. Watch = input with both edges. */
static int open_real(const char *chip, int pin, int watch, int init_level)
{
    int chip_fd = open(chip, O_RDWR | O_CLOEXEC);
    if (chip_fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", chip, strerror(errno));
        return -1;
    }

    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = (unsigned int)pin;
    req.num_lines = 1;
    strncpy(req.consumer, "gpiotest", sizeof(req.consumer) - 1);

    if (watch) {
        req.config.flags = GPIO_V2_LINE_FLAG_INPUT |
                           GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
    } else {
        req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
        req.config.num_attrs = 1;                       /* initial output value, no glitch */
        req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        req.config.attrs[0].attr.values = init_level ? 1 : 0;
        req.config.attrs[0].mask = 1;
    }

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        fprintf(stderr, "Cannot request GPIO %d on %s: %s\n", pin, chip, strerror(errno));
        close(chip_fd);
        return -1;
    }
    close(chip_fd);          /* the line fd stays valid after the chip fd is closed */
    line_fd = req.fd;
    return 0;
}

/* Sim mode: drive = write the "pull" file, watch = read the "value" file. */
static int open_sim(const char *dir, int pin, int watch)
{
    char base[512], path[600];

    if (sim_base(dir, base, sizeof(base)) != 0) return -1;

    snprintf(path, sizeof(path), "%s/sim_gpio%d/%s", base, pin, watch ? "value" : "pull");
    line_fd = open(path, (watch ? O_RDONLY : O_WRONLY) | O_CLOEXEC);
    if (line_fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    printf("Using simulated line %s\n", path);
    return 0;
}

/* Register mode. Watch: map read-only, never touch the pin. Drive: force the pin to output. */
static int open_mem(int pin, int watch, int init_level)
{
    if (pin > MAX_REG_PIN) {
        fprintf(stderr, "Register mode supports GPIO 0-%d\n", MAX_REG_PIN);
        return -1;
    }

    /* The Pi 5 moved GPIO behind the RP1 chip, so these registers don't apply there */
    char model[128] = "";
    FILE *fp = fopen("/proc/device-tree/model", "r");
    if (fp) {
        if (fgets(model, sizeof(model), fp) == NULL) model[0] = '\0';
        fclose(fp);
    }
    if (strstr(model, "Raspberry Pi 5") || strstr(model, "Compute Module 5")) {
        fprintf(stderr, "Register mode is not supported on this board (%s).\n"
                        "Use the default mode with a jumper, or -s with gpio-sim.\n", model);
        return -1;
    }

    int fd = open("/dev/gpiomem", watch ? (O_RDONLY | O_SYNC | O_CLOEXEC)
                                        : (O_RDWR | O_SYNC | O_CLOEXEC));
    if (fd < 0) {
        fprintf(stderr, "Cannot open /dev/gpiomem: %s\n"
                        "Run as root or add your user to the gpio group. Only on Raspberry Pi\n"
                        "boards before the Pi 5.\n", strerror(errno));
        return -1;
    }

    void *m = mmap(NULL, GPIOMEM_LEN, watch ? PROT_READ : (PROT_READ | PROT_WRITE),
                   MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        fprintf(stderr, "Cannot map GPIO registers: %s\n", strerror(errno));
        return -1;
    }
    gpio_reg  = (volatile uint32_t *)m;
    mem_pin   = pin;
    mem_watch = watch;
    if (watch) return 0;

    int reg   = REG_GPFSEL0 + pin / 10;
    int shift = (pin % 10) * 3;
    mem_orig_fsel = (gpio_reg[reg] >> shift) & 7u;

    if (mem_orig_fsel > 1)
        printf("Note: GPIO %d is in an alternate function (%u), overriding it until exit\n",
               pin, mem_orig_fsel);

    /* Latch the wanted level first so the pin doesn't glitch when it becomes an output */
    gpio_reg[(init_level ? REG_GPSET0 : REG_GPCLR0) + (pin >> 5)] = 1u << (pin & 31);
    gpio_reg[reg] = (gpio_reg[reg] & ~(7u << shift)) | (1u << shift);   /* 1 = output */
    return 0;
}

/* Undo whatever the open function did. */
static void close_line(void)
{
    if (mode == MODE_MEM && gpio_reg) {
        if (!mem_watch) {
            int reg   = REG_GPFSEL0 + mem_pin / 10;
            int shift = (mem_pin % 10) * 3;
            set_line(0);
            /* Put the pin back to exactly the function it had, so the kernel's view is right */
            gpio_reg[reg] = (gpio_reg[reg] & ~(7u << shift)) | (mem_orig_fsel << shift);
        }
        munmap((void *)gpio_reg, GPIOMEM_LEN);
        gpio_reg = NULL;
    } else if (line_fd >= 0) {
        close(line_fd);
        line_fd = -1;
    }
}

/* ------------------------------ actions ------------------------------ */

static int run_pulse(int pin, double freq, long max_pulses)
{
    struct timespec next;
    int level = 0, stdin_ok = 1, failed = 0;
    long pulses = 0;
    char buf[64];

    printf("Pulsing GPIO %d at %.3f Hz (type a new frequency, 0 to pause, q to quit)\n", pin, freq);
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!stop_flag && !time_up()) {
        if (read_cmd(buf, sizeof(buf), &stdin_ok)) {
            if (buf[0] == 'q' || buf[0] == 'Q') break;
            char *end;
            double f = strtod(buf, &end);
            if (end == buf || f < 0.0 || f > MAX_HZ) {
                printf("Ignored: enter a number from 0 to %.0f, or q to quit\n", MAX_HZ);
            } else {
                freq = f;
                if (f == 0.0) printf("Pulses stopped (line held low)\n");
                else          printf("Frequency now %.3f Hz\n", f);
            }
        }

        if (freq <= 0.0) {                       /* paused: hold low, poll stdin */
            if (level) { set_line(0); level = 0; }
            struct timespec nap = { 0, 100000000L };
            nanosleep(&nap, NULL);
            clock_gettime(CLOCK_MONOTONIC, &next);   /* resume cleanly later */
            continue;
        }

        level = !level;
        if (set_line(level) < 0) {
            fprintf(stderr, "Write to line failed: %s\n", strerror(errno));
            failed = 1;
            break;
        }
        if (level) pulses++;                     /* one rising edge = one pulse */

        add_ns(&next, (long)(500000000.0 / freq));
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        if (max_pulses > 0 && !level && pulses >= max_pulses) break;
    }

    set_line(0);
    printf("Done. %ld pulses sent.\n", pulses);
    return failed;
}

static int run_level(int pin, int level)
{
    int stdin_ok = 1;
    char buf[32];

    if (set_line(level) < 0) {
        fprintf(stderr, "Write to line failed: %s\n", strerror(errno));
        return 1;
    }
    printf("GPIO %d held at %d (type 0, 1 or t to change, q to quit)\n", pin, level);

    while (!stop_flag && !time_up()) {
        if (read_cmd(buf, sizeof(buf), &stdin_ok)) {
            char c = buf[0];
            int nl = level;

            if (c == 'q' || c == 'Q') break;
            else if (c == '0') nl = 0;
            else if (c == '1') nl = 1;
            else if (c == 't' || c == 'T') nl = !level;
            else { printf("Ignored: type 0, 1, t or q\n"); continue; }

            if (nl != level) {
                level = nl;
                if (set_line(level) < 0) {
                    fprintf(stderr, "Write to line failed: %s\n", strerror(errno));
                    return 1;
                }
                printf("GPIO %d now %d\n", pin, level);
            }
        }
        struct timespec nap = { 0, 50000000L };
        nanosleep(&nap, NULL);
    }

    set_line(0);
    printf("Done. Pin released.\n");
    return 0;
}

static void report(int pin, int64_t t_ns, int64_t start_ns, int level,
                   int64_t *last_ns, long *changes)
{
    double t    = (double)(t_ns - start_ns) / 1e9;
    double held = (double)(t_ns - *last_ns) / 1e9;

    if (t < 0.0) t = 0.0;
    printf("[%9.3f] GPIO %d -> %d   (was %d for %s%.3f s)\n",
           t, pin, level, !level, *changes == 0 ? "at least " : "", held);
    fflush(stdout);
    *last_ns = t_ns;
    (*changes)++;
}

static int run_watch(int pin, long max_changes)
{
    int stdin_ok = 1, bad_reads = 0;
    char buf[32];
    long changes = 0;
    int64_t start = (int64_t)now_ns(), last = start;

    int level = read_level();
    if (level < 0) {
        fprintf(stderr, "Cannot read GPIO %d: %s\n", pin, strerror(errno));
        return 1;
    }
    printf("Watching GPIO %d, currently %d (q to quit)\n", pin, level);
    fflush(stdout);

    while (!stop_flag && !time_up()) {
        if (read_cmd(buf, sizeof(buf), &stdin_ok) && (buf[0] == 'q' || buf[0] == 'Q')) break;

        if (mode == MODE_REAL) {
            /* kernel edge events carry accurate timestamps */
            struct pollfd p = { line_fd, POLLIN, 0 };
            int r = poll(&p, 1, 50);
            if (r < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "poll failed: %s\n", strerror(errno));
                return 1;
            }
            if (r > 0) {
                struct gpio_v2_line_event ev[16];
                ssize_t n = read(line_fd, ev, sizeof(ev));
                if (n < 0) {
                    fprintf(stderr, "Reading events failed: %s\n", strerror(errno));
                    return 1;
                }
                for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
                    int lv = (ev[i].id == GPIO_V2_LINE_EVENT_RISING_EDGE);
                    report(pin, (int64_t)ev[i].timestamp_ns, start, lv, &last, &changes);
                }
            }
        } else {
            /* register and sim modes: sample the level (ignore a few bad samples in a row) */
            int lv = read_level();
            if (lv < 0) {
                if (++bad_reads > 100) {
                    fprintf(stderr, "Cannot read GPIO %d: %s\n", pin, strerror(errno));
                    return 1;
                }
            } else {
                bad_reads = 0;
                if (lv != level) {
                    report(pin, (int64_t)now_ns(), start, lv, &last, &changes);
                    level = lv;
                }
            }
            struct timespec nap = { 0, mode == MODE_MEM ? WATCH_POLL_MEM_NS : WATCH_POLL_SIM_NS };
            nanosleep(&nap, NULL);
        }

        if (max_changes > 0 && changes >= max_changes) break;
    }

    printf("Done. %ld changes seen.\n", changes);
    return 0;
}

int main(int argc, char **argv)
{
    const char *chip = DEFAULT_CHIP;
    const char *sim_dir = NULL;
    action_t action = ACT_PULSE;
    int use_mem = 0, hold_level = 0;
    double freq = DEFAULT_HZ, run_secs = 0.0;
    long count = 0;
    int pin = -1, npos = 0;

    if (argc < 2) { usage(argv[0]); return 0; }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            chip = argv[++i];
        } else if (!strcmp(argv[i], "-m")) {
            use_mem = 1;
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            sim_dir = argv[++i];
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            count = atol(argv[++i]);
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            run_secs = strtod(argv[++i], NULL);
        } else if (!strcmp(argv[i], "-w")) {
            if (action == ACT_LEVEL) { fprintf(stderr, "Use only one of -l and -w\n"); return 1; }
            action = ACT_WATCH;
        } else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
            const char *v = argv[++i];
            if (action == ACT_WATCH) { fprintf(stderr, "Use only one of -l and -w\n"); return 1; }
            if (strcmp(v, "0") && strcmp(v, "1")) {
                fprintf(stderr, "-l needs 0 or 1\n");
                return 1;
            }
            action = ACT_LEVEL;
            hold_level = (v[0] == '1');
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option or missing value '%s' (use -h for help)\n", argv[i]);
            return 1;
        } else if (npos == 0) {
            pin = atoi(argv[i]);
            npos++;
        } else if (npos == 1) {
            freq = strtod(argv[i], NULL);
            npos++;
        } else {
            fprintf(stderr, "Too many arguments (use -h for help)\n");
            return 1;
        }
    }

    if (pin < 0) { fprintf(stderr, "A valid GPIO pin is required\n"); return 1; }
    if (freq < 0.0 || freq > MAX_HZ) {
        fprintf(stderr, "Frequency must be 0 to %.0f Hz\n", MAX_HZ);
        return 1;
    }
    if (npos > 1 && action != ACT_PULSE) {
        fprintf(stderr, "A frequency only applies to pulse mode (without -l or -w)\n");
        return 1;
    }
    if (use_mem && sim_dir) {
        fprintf(stderr, "Use only one of -m and -s\n");
        return 1;
    }

    mode = use_mem ? MODE_MEM : (sim_dir ? MODE_SIM : MODE_REAL);
    int watch = (action == ACT_WATCH);
    int init_level = (action == ACT_LEVEL) ? hold_level : 0;

    int rc = 0;
    switch (mode) {
        case MODE_MEM:  rc = open_mem(pin, watch, init_level);          break;
        case MODE_SIM:  rc = open_sim(sim_dir, pin, watch);             break;
        default:        rc = open_real(chip, pin, watch, init_level);   break;
    }
    if (rc) return 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (run_secs > 0.0) deadline_ns = now_ns() + (uint64_t)(run_secs * 1e9);

    if (mode == MODE_SIM)      printf("Method: gpio-sim chip\n");
    else if (mode == MODE_MEM) printf("Method: direct register access (/dev/gpiomem)\n");
    else                       printf("Method: kernel GPIO driver on %s\n", chip);

    int failed;
    if (action == ACT_WATCH) {
        failed = run_watch(pin, count);
    } else {
        /* drive modes start from a known state (sim/register modes set it here) */
        if (mode == MODE_SIM && set_line(init_level) < 0) {
            fprintf(stderr, "Cannot set initial line state: %s\n", strerror(errno));
            close_line();
            return 1;
        }
        failed = (action == ACT_LEVEL) ? run_level(pin, hold_level)
                                       : run_pulse(pin, freq, count);
    }

    close_line();
    return failed ? 1 : 0;
}