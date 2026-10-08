#include "snn_q6.h"

unsigned snn_popcount64(uint64_t x) {
    unsigned c = 0;
    while (x) {
        x &= x - 1u;
        c++;
    }
    return c;
}

unsigned snn_hamming(unsigned u, unsigned v) {
    return snn_popcount64((uint64_t)(u ^ v));
}

void snn_q6_reset(snn_q6_t *n) {
    int i;
    n->spikes = 0;
    for (i = 0; i < SNN_Q6_N; i++) n->v[i] = 0;
}

static uint64_t codebook_mask(void) {
    static const unsigned cw[SNN_Q6_NCODE] = { SNN_Q6_CODEWORDS };
    uint64_t m = 0;
    int i;
    for (i = 0; i < SNN_Q6_NCODE; i++) m |= 1ull << cw[i];
    return m;
}

static void init_common(snn_q6_t *n) {
    int i, b;
    n->codebook = codebook_mask();
    n->dense = 0;
    for (i = 0; i < SNN_Q6_N; i++)
        for (b = 0; b < SNN_Q6_DEGREE; b++)
            n->nbr[i][b] = (uint8_t)(i ^ (1u << b));
    snn_q6_reset(n);
}

void snn_q6_init_cube(snn_q6_t *n)  { init_common(n); }
void snn_q6_init_dense(snn_q6_t *n) { init_common(n); n->dense = 1; }

uint64_t snn_q6_step(snn_q6_t *n, unsigned stim, int stim_on, int hop_on) {
    uint64_t prev = n->spikes;
    uint64_t nxt = 0;
    int i, k;

    stim &= (SNN_Q6_N - 1);
    for (i = 0; i < SNN_Q6_N; i++) {
        unsigned cur = 0;
        unsigned pot;
        unsigned thresh;
        if (stim_on && (unsigned)i == stim) cur += SNN_Q6_I_STIM;
        /* Hop only when the caller says so (gated in snn_q6_run). */
        if (hop_on && !n->dense) {
            for (k = 0; k < SNN_Q6_DEGREE; k++) {
                if (n->nbr[stim][k] == (uint8_t)i) {
                    cur += SNN_Q6_I_HOP;
                    break;
                }
            }
        }
        if (n->dense) {
            if (prev & ~(1ull << i)) cur += SNN_Q6_W_EDGE;
        } else {
            for (k = 0; k < SNN_Q6_DEGREE; k++) {
                unsigned j = n->nbr[i][k];
                if ((prev >> j) & 1ull) cur += SNN_Q6_W_EDGE;
            }
        }
        pot = ((unsigned)n->v[i] * SNN_Q6_LEAK_NUM) / SNN_Q6_LEAK_DEN + cur;
        if (pot > 255u) pot = 255u;
        thresh = ((n->codebook >> i) & 1ull)
                 ? (unsigned)(SNN_Q6_VTH - SNN_Q6_I_CODE)
                 : (unsigned)SNN_Q6_VTH;
        if (pot >= thresh) {
            nxt |= (1ull << i);
            pot = 0;
        }
        n->v[i] = (uint8_t)pot;
    }
    n->spikes = nxt;
    return nxt;
}

void snn_q6_run(snn_q6_t *n, unsigned stim, unsigned target,
                 snn_q6_run_t *out) {
    int t, i, saw_code = 0, hop;
    int best = -1, winner = -1;
    uint64_t mask;

    snn_q6_reset(n);
    for (i = 0; i < SNN_Q6_N; i++) out->counts[i] = 0;
    out->first_target_spike = SNN_Q6_T_MAX;
    out->spikes_total = 0;
    stim &= (SNN_Q6_N - 1);
    target &= (SNN_Q6_N - 1);

    for (t = 0; t < SNN_Q6_T_MAX; t++) {
        uint64_t bits, x;
        hop = (t >= SNN_Q6_STIM_WINDOW + 1) && !saw_code && !n->dense;
        bits = snn_q6_step(n, stim, t < SNN_Q6_STIM_WINDOW, hop);
        if (bits & n->codebook) saw_code = 1;
        out->spikes_total += (int)snn_popcount64(bits);
        if (((bits >> target) & 1ull) && out->first_target_spike == SNN_Q6_T_MAX)
            out->first_target_spike = t;
        x = bits;
        while (x) {
            uint64_t lsb = x & -x;
            unsigned idx = snn_popcount64(lsb - 1u);
            if (out->counts[idx] < 255) out->counts[idx]++;
            x ^= lsb;
        }
    }

    mask = n->codebook;
    i = 0;
    while (mask) {
        if (mask & 1ull) {
            int c = out->counts[i];
            if (c > 0 && (c > best || (c == best && (winner < 0 || i < winner)))) {
                best = c;
                winner = i;
            }
        }
        mask >>= 1;
        i++;
    }
    out->winner = winner;
}

unsigned snn_nearest_codeword(unsigned x, uint64_t codebook) {
    unsigned best = 0, best_d = SNN_Q6_DIM + 1;
    int seen = 0;
    unsigned w;
    for (w = 0; w < SNN_Q6_N; w++) {
        unsigned d;
        if (((codebook >> w) & 1ull) == 0) continue;
        d = snn_hamming(x, w);
        if (!seen || d < best_d || (d == best_d && w < best)) {
            best = w;
            best_d = d;
            seen = 1;
        }
    }
    return best;
}

int snn_graph_ok(const snn_q6_t *n) {
    int i, b, edges = 0;
    if (!n) return 0;
    if (n->dense) return 1;
    for (i = 0; i < SNN_Q6_N; i++) {
        for (b = 0; b < SNN_Q6_DEGREE; b++) {
            unsigned j = n->nbr[i][b];
            int k, back = 0;
            if (j != ((unsigned)i ^ (1u << b))) return 0;
            if (snn_hamming((unsigned)i, j) != 1) return 0;
            for (k = 0; k < SNN_Q6_DEGREE; k++)
                if (n->nbr[j][k] == (uint8_t)i) back = 1;
            if (!back) return 0;
            if (i < (int)j) edges++;
        }
    }
    if (snn_popcount64(n->codebook) != (unsigned)SNN_Q6_NCODE) return 0;
    return edges == SNN_Q6_EDGES;
}
