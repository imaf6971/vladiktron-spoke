// Headless real-time check of the main output, with the board on its own thread as in the app: a pattern plays
// while a reader takes mcb_audio every millisecond: with USB audio off, then with both USB streams on, then off
// again. (It also prints the process's CPU use per phase.)
// Each phase fails if the output runs below 48 kHz, if a block arrives late (more than LATE_MS behind the
// steadiest arrival line), or if a block comes out repeated: a sounding 32-frame block identical to the one
// before, or to the one two before (the same half of the OS's audio ring, played again because the OS had not
// rendered it yet).
//   mcrt_test <main_os.bin> <flash.bin> <emmc.img> [seconds per phase]
// Prints PASS/FAIL per check; exit status 0 only if all pass. Run it on an otherwise idle machine.
#include <math.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include "mcboard.h"

#define LATE_MS 10.0                                         // half the app's cushion (AudioOut, 1024 frames)
#define MIN_RATE 47990.0                                     // 48 kHz, less what 1 ms polling can mismeasure
static int failures;

static double cpu_s(void) { struct rusage u; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6; }
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static void *cpu(void *b) {                                  // the app's CPU thread runs at this QoS too, which
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // keeps it on the performance cores
    mcb_run(b, 0);
    return NULL;
}
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; }

// Reads the main output for `secs` like the app's audio callback would, every 1 ms (and feeds silence into
// USB playback when `usb` is on, as an idle input device would).
static void phase(mcboard *b, double secs, int usb, const char *name) {
    static float buf[2 * 4096], prev[2][64];
    size_t blocks = 0, repeats = 0, late = 0;
    double t0 = 0, worst = 0, first = 0, best = 1e30;
    double *arrive = malloc(sizeof(double) * (size_t)(secs * 1500 + 4096));
    float zero[2 * 48] = {0};
    int have_prev = 0;
    while (mcb_audio(b, buf, 4096)) {}                       // start from an empty FIFO
    double c0 = cpu_s(), w0 = now();
    for (double end = now() + secs; now() < end; ) {
        usleep(1000);
        double t = now();
        size_t k;
        while ((k = mcb_audio(b, buf, 32)) > 0) {
            if (k != 32) { fprintf(stderr, "partial block\n"); break; }
            if (!blocks) t0 = first = t;
            float pk = 0;
            for (int i = 0; i < 64; i++) if (fabsf(buf[i]) > pk) pk = fabsf(buf[i]);
            if (have_prev > 1 && pk > 1e-4f && (!memcmp(buf, prev[0], sizeof prev[0]) || !memcmp(buf, prev[1], sizeof prev[1])))
                repeats++;
            memcpy(prev[1], prev[0], sizeof prev[0]);
            memcpy(prev[0], buf, sizeof prev[0]);
            have_prev++;
            if (blocks < (size_t)(secs * 1500 + 4096)) arrive[blocks] = t;
            double ideal = t0 + blocks * (32 / 48000.0);     // steadiest line: the earliest offset seen
            if (t - ideal < best) best = t - ideal;
            blocks++;
        }
        while (usb && mcb_usb_audio_write_queued(b) < 960) mcb_usb_audio_write(b, zero, 48);
    }
    double cpu = (cpu_s() - c0) / (now() - w0) * 100;
    double span = arrive[blocks - 1] - first;
    double rate = blocks > 1 ? (blocks - 1) * 32 / span : 0;
    for (size_t i = 0; i < blocks && i < (size_t)(secs * 1500 + 4096); i++) {
        double l = (arrive[i] - (t0 + i * (32 / 48000.0)) - best) * 1000;
        if (l > worst) worst = l;
        late += l > LATE_MS;
    }
    char st[700], what[160];
    printf("      %s: %zu blocks, %.1f Hz, latest block %.2f ms behind, %zu late, %zu repeated; CPU %.0f %%\n", name,
           blocks, rate, worst, late, repeats, cpu);
    snprintf(what, sizeof what, "%s: main output at 48 kHz", name);
    check(rate >= MIN_RATE, what);
    snprintf(what, sizeof what, "%s: no block more than %.0f ms late", name, LATE_MS);
    check(late == 0, what);
    snprintf(what, sizeof what, "%s: no repeated blocks", name);
    check(repeats == 0, what);
    mcb_status(b, st, sizeof st);
    printf("      %s\n", st);
    free(arrive);
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    double secs = argc > 4 ? atof(argv[4]) : 20;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *os = malloc(n); if (fread(os, 1, n, f) != n) return 1; fclose(f);
    char err[256];
    mcboard *b = mcb_create(os, n, argv[2], argv[3], err, sizeof err);
    if (!b) { fprintf(stderr, "%s\n", err); return 1; }
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // the reader stands in for Core Audio's
    pthread_t t;                                                     // real-time thread: don't measure our own
    pthread_create(&t, NULL, cpu, b);                                // scheduling instead of the board's
    usleep(7000000);                                          // boot and enumerate
    check(mcb_usb_state(b) == 2, "USB: enumerated and configured");
    mcb_key(b, 7, 1); usleep(100000); mcb_key(b, 7, 0);       // PLAY
    usleep(1000000);
    phase(b, secs, 0, "USB audio off");
    mcb_usb_audio_enable(b, 1, 1);
    usleep(1000000);                                          // streams start
    phase(b, secs, 1, "USB audio on ");
    mcb_usb_audio_enable(b, 0, 0);
    usleep(1000000);
    phase(b, secs, 0, "USB audio off again");
    mcb_stop(b);
    pthread_join(t, NULL);
    mcb_destroy(b);
    return failures ? 1 : 0;
}
