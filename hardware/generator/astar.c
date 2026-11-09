/* Grid A* for the board router: 2 layers, 8 directions, turn and via costs. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float f; int32_t s; } Node;

static Node *heap;
static int heap_n, heap_cap;
static float *gscore;
static int32_t *parent;
static uint32_t *stamp;
static uint32_t cur_stamp;
static int alloc_states;

static void push(float f, int32_t s) {
    if (heap_n == heap_cap) {
        heap_cap = heap_cap ? heap_cap * 2 : 1 << 16;
        heap = realloc(heap, sizeof(Node) * heap_cap);
    }
    int i = heap_n++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (heap[p].f <= f) break;
        heap[i] = heap[p];
        i = p;
    }
    heap[i].f = f;
    heap[i].s = s;
}

static Node pop(void) {
    Node top = heap[0];
    Node last = heap[--heap_n];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        float mf = last.f;
        if (l < heap_n && heap[l].f < mf) { m = l; mf = heap[l].f; }
        if (r < heap_n && heap[r].f < mf) { m = r; }
        if (m == i) break;
        heap[i] = heap[m];
        i = m;
    }
    if (heap_n) heap[i] = last;
    return top;
}

static const int DI[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static const int DJ[8] = {0, 1, 1, 1, 0, -1, -1, -1};

/* state = ((layer * ny + j) * nx + i) * 9 + dir ; dir 8 = no direction yet */
int astar(int nx, int ny, const uint8_t *free_map, const uint8_t *via_ok, const uint8_t *dst_mask,
          int n_src, const int32_t *src, int wi0, int wj0, int wi1, int wj1,
          int bx0, int by0, int bx1, int by1, float via_cost, const float *layer_cost,
          const float *turn_cost, int max_exp, int32_t *out, int max_out) {
    int cells = nx * ny;
    int states = 2 * cells * 9;
    if (states > alloc_states) {
        free(gscore); free(parent); free(stamp);
        gscore = malloc(sizeof(float) * states);
        parent = malloc(sizeof(int32_t) * states);
        stamp = calloc(states, sizeof(uint32_t));
        alloc_states = states;
        cur_stamp = 0;
    }
    cur_stamp++;
    heap_n = 0;
    const float diag = 1.41421356f;
#define H(i, j) ({ int dx_ = (i) < bx0 ? bx0 - (i) : ((i) > bx1 ? (i) - bx1 : 0); \
                   int dy_ = (j) < by0 ? by0 - (j) : ((j) > by1 ? (j) - by1 : 0); \
                   (float)(dx_ > dy_ ? dx_ + 0.41421356f * dy_ : dy_ + 0.41421356f * dx_); })
    for (int k = 0; k < n_src; k++) {
        int L = src[2 * k], idx = src[2 * k + 1];
        int s = (L * cells + idx) * 9 + 8;
        stamp[s] = cur_stamp;
        gscore[s] = 0.f;
        parent[s] = -1;
        push(H(idx % nx, idx / nx), s);
    }
    int exp = 0;
    while (heap_n) {
        Node nd = pop();
        int s = nd.s;
        int d = s % 9;
        int li = s / 9;
        int L = li / cells, idx = li % cells;
        int i = idx % nx, j = idx / nx;
        float g = gscore[s];
        if (nd.f > g + H(i, j) + 1e-4f) continue;
        if (dst_mask[li]) {
            int n = 0;
            while (s >= 0 && n < max_out) {
                out[n++] = s / 9;
                s = parent[s];
            }
            return n;
        }
        if (++exp > max_exp) return -1;
        const uint8_t *fl = free_map + L * cells;
        for (int nd8 = 0; nd8 < 8; nd8++) {
            int ni = i + DI[nd8], nj = j + DJ[nd8];
            if (ni < wi0 || nj < wj0 || ni > wi1 || nj > wj1) continue;
            int nidx = nj * nx + ni;
            if (!fl[nidx]) continue;
            float tc = 0.f;
            if (d != 8) {
                int t = abs(nd8 - d);
                if (t > 4) t = 8 - t;
                tc = turn_cost[t];
            }
            float step = ((DI[nd8] && DJ[nd8]) ? diag : 1.f) * layer_cost[L];
            float ng = g + step + tc;
            int ns = ((L * cells) + nidx) * 9 + nd8;
            if (stamp[ns] != cur_stamp || ng < gscore[ns] - 1e-5f) {
                stamp[ns] = cur_stamp;
                gscore[ns] = ng;
                parent[ns] = s;
                push(ng + H(ni, nj), ns);
            }
        }
        if (via_ok[idx]) {
            int ol = 1 - L;
            if (free_map[ol * cells + idx]) {
                float ng = g + via_cost;
                int ns = ((ol * cells) + idx) * 9 + 8;
                if (stamp[ns] != cur_stamp || ng < gscore[ns] - 1e-5f) {
                    stamp[ns] = cur_stamp;
                    gscore[ns] = ng;
                    parent[ns] = s;
                    push(ng + H(i, j), ns);
                }
            }
        }
    }
    return -1;
}
