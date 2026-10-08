/*
 * Q6 — integer LIF on the 6-cube: 64 cells, 192 edges, leak and threshold
 * in whole numbers. No FPU, no libc, no allocation; neighbours of i are
 * i^(1<<b). Driven by `prisma5` and measured by `bench-snn`; it is the
 * software spiking substrate the event fabric offers when no NPU exists.
 */
#ifndef RXOS_SNN_Q6_H
#define RXOS_SNN_Q6_H

#include "rogex.h"

#define SNN_Q6_DIM           6
#define SNN_Q6_N             64
#define SNN_Q6_DEGREE        6
#define SNN_Q6_EDGES         192

#define SNN_Q6_VTH           40
#define SNN_Q6_LEAK_NUM      7
#define SNN_Q6_LEAK_DEN      8
#define SNN_Q6_I_STIM        40
#define SNN_Q6_I_CODE        12
#define SNN_Q6_I_HOP         40
#define SNN_Q6_W_EDGE        32
#define SNN_Q6_T_MAX         16
#define SNN_Q6_STIM_WINDOW   1

#define SNN_Q6_NCODE         8
#define SNN_Q6_CODEWORDS     0, 7, 25, 30, 42, 45, 51, 52

typedef struct {
    uint64_t spikes;
    uint8_t  v[SNN_Q6_N];
    uint8_t  nbr[SNN_Q6_N][SNN_Q6_DEGREE];
    uint64_t codebook;
    int      dense;
} snn_q6_t;

typedef struct {
    int     winner;
    int     first_target_spike;
    int     spikes_total;
    uint8_t counts[SNN_Q6_N];
} snn_q6_run_t;

void     snn_q6_init_cube(snn_q6_t *n);
void     snn_q6_init_dense(snn_q6_t *n);
void     snn_q6_reset(snn_q6_t *n);
uint64_t snn_q6_step(snn_q6_t *n, unsigned stim, int stim_on, int hop_on);
void     snn_q6_run(snn_q6_t *n, unsigned stim, unsigned target,
                     snn_q6_run_t *out);

unsigned snn_popcount64(uint64_t x);
unsigned snn_hamming(unsigned u, unsigned v);
unsigned snn_nearest_codeword(unsigned x, uint64_t codebook);
int      snn_graph_ok(const snn_q6_t *n);

#define SNN_Q6_LAYER_BYTES  (sizeof(snn_q6_t))

#endif
