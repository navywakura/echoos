/* hyperfine — command-line benchmarking for EchOS.
 * Runs a shell command N times via system(), timing each run with
 * clock_gettime(CLOCK_MONOTONIC), then reports min / mean / max /
 * stddev (own sqrt, no libm).
 * EchOS coretools — proprietary, from scratch. C99.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HF_MAX_RUNS 100000

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* own square root: Newton's method, no libm */
static double hf_sqrt(double x)
{
    double r = x > 1.0 ? x : 1.0;
    int i;
    if (x <= 0.0) return 0.0;
    for (i = 0; i < 64; i++) {
        double nr = 0.5 * (r + x / r);
        if (nr == r) break;
        r = nr;
    }
    return r;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: hyperfine [--runs N] 'COMMAND'\n"
        "  --runs N   number of runs (default 10, max %d)\n", HF_MAX_RUNS);
}

int main(int argc, char **argv)
{
    long runs = 10;
    const char *cmd = NULL;
    static double times[HF_MAX_RUNS];
    long i;
    double sum = 0, mean, variance = 0, stddev;
    double mn, mx;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
            runs = atol(argv[++i]);
            if (runs < 1) runs = 1;
            if (runs > HF_MAX_RUNS) runs = HF_MAX_RUNS;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (!cmd) {
            cmd = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (!cmd || !*cmd) { usage(); return 2; }

    printf("benchmarking: %s\n", cmd);
    printf("runs: %ld\n\n", runs);

    for (i = 0; i < runs; i++) {
        double t0 = now_sec();
        int rc = system(cmd);
        double t1 = now_sec();
        if (rc == -1 || !(rc == 0 || rc == (0 << 8))) {
            /* treat only hard spawn failure as fatal */
            if (rc < 0) {
                fprintf(stderr, "hyperfine: failed to run command\n");
                return 1;
            }
        }
        times[i] = t1 - t0;
        sum += times[i];
    }

    mean = sum / (double)runs;
    mn = mx = times[0];
    for (i = 0; i < runs; i++) {
        if (times[i] < mn) mn = times[i];
        if (times[i] > mx) mx = times[i];
    }
    for (i = 0; i < runs; i++) {
        double d = times[i] - mean;
        variance += d * d;
    }
    variance /= (double)(runs > 1 ? runs - 1 : 1); /* sample variance */
    stddev = hf_sqrt(variance);

    printf("  min:  %.4f ms\n", mn * 1e3);
    printf("  mean: %.4f ms\n", mean * 1e3);
    printf("  max:  %.4f ms\n", mx * 1e3);
    printf("  stddev: %.4f ms (%.2f%%)\n",
           stddev * 1e3, mean > 0 ? 100.0 * stddev / mean : 0.0);

    /* crude histogram of the distribution */
    printf("\n  summary:\n");
    printf("  fastest run:  %.4f ms\n", mn * 1e3);
    printf("  slowest run:  %.4f ms (%.2fx faster at best)\n",
           mx * 1e3, mn > 0 ? mx / mn : 0.0);
    return 0;
}
