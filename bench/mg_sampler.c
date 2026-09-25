/* mg_sampler.c — LD_PRELOAD SIGPROF sampler for ModernGekko profiling.
 *
 * Builds: cc -O2 -fPIC -shared -o mg_sampler.so mg_sampler.c
 *
 * Env:
 *   MG_SAMPLE_OUT  — output path (required; sampler is inert without it)
 *   MG_SAMPLE_HZ   — samples per second of process CPU time (default 499)
 *
 * Writes lines "<tid>:<rip_hex>" to the output file using only
 * async-signal-safe operations. Pair the samples file with a
 * /proc/<pid>/maps snapshot taken while the process is alive; symbolize
 * offline with mg_symbolize.py.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <fcntl.h>

/* sig_atomic_t so the SIGPROF handler sees the disarmed value promptly during
 * teardown; write() to a stale descriptor would be harmless (EBADF) but could
 * land in an unrelated file if the fd number got reused after close. */
static volatile sig_atomic_t s_fd = -1;

static unsigned long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);  /* vDSO: no syscall on x86-64 */
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
}

static void emit(unsigned long long ip, long tid)
{
    char tmp[64];
    int n = 0;
    unsigned long long ms = mono_ms();
    /* "ms@tid:ip\n" */
    char tdig[24];
    int tn = 0;
    unsigned long long t = ms;
    if (t == 0) tdig[tn++] = '0';
    while (t > 0) { tdig[tn++] = (char)('0' + (t % 10)); t /= 10; }
    while (tn > 0) tmp[n++] = tdig[--tn];
    tmp[n++] = '@';
    t = (unsigned long long)tid;
    tn = 0;
    if (t == 0) tdig[tn++] = '0';
    while (t > 0) { tdig[tn++] = (char)('0' + (t % 10)); t /= 10; }
    while (tn > 0) tmp[n++] = tdig[--tn];
    tmp[n++] = ':';
    for (int i = 60; i >= 0; i -= 4) {
        unsigned d = (unsigned)((ip >> i) & 0xF);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    tmp[n++] = '\n';
    /* One write() per sample: O_APPEND makes each call atomic, so concurrent
     * SIGPROF deliveries on different threads cannot interleave lines the way
     * a shared user-space buffer would (and a lock would be unsafe here). */
    if (s_fd >= 0) {
        ssize_t w = write(s_fd, tmp, (size_t)n);
        (void)w;
    }
}

static void on_prof(int sig, siginfo_t* info, void* ucv)
{
    (void)sig; (void)info;
    ucontext_t* uc = (ucontext_t*)ucv;
    unsigned long long ip = (unsigned long long)uc->uc_mcontext.gregs[REG_RIP];
    emit(ip, syscall(SYS_gettid));
}

__attribute__((constructor)) static void sampler_init(void)
{
    const char* out = getenv("MG_SAMPLE_OUT");
    if (!out || !*out)
        return;
    s_fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (s_fd < 0)
        return;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0)
        return;

    long hz = 499;
    const char* hz_env = getenv("MG_SAMPLE_HZ");
    if (hz_env && *hz_env) {
        /* Whole-token parse, matching the repo's env-knob convention:
         * "100junk" must not silently sample at 100 Hz. */
        char* end = NULL;
        const long v = strtol(hz_env, &end, 10);
        if (end != hz_env && *end == '\0' && v >= 10 && v <= 5000)
            hz = v;
    }
    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 1000000 / hz;
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, NULL);
}

__attribute__((destructor)) static void sampler_fini(void)
{
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_PROF, &it, NULL);
    if (s_fd >= 0) {
        /* Disarm the handler's write path before closing: a SIGPROF already
         * delivered between close() and s_fd = -1 could otherwise write into
         * whatever reused that descriptor next. */
        const int fd = (int)s_fd;
        s_fd = -1;
        close(fd);
    }
}
