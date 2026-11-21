#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kmath.h"
#include "quad/kernels.h"

/* The kernels of a room with N cells: native.diffusion, native.room.refine
 * and native.room.abstract (see quad/kernels.h). Their contracts depend on N,
 * so each one is generated. Integer arithmetic only. */

#define S_T   (-6)
#define S_C   (-3)
#define S_E   (-3)
#define S_G   (-3)
#define S_TOL (-9)
#define NAME  40

typedef struct {
    QkKernel    k;          /* first, so that a QkKernel pointer is a Gen pointer */
    unsigned    n;
    QkSlot     *slots;
    QkTransfer *transfers;
    char       *names;
} Gen;

struct QkInstance { const QkHost *host; unsigned n; };

static QkResult create(const QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out)
{
    (void) call;
    QkInstance *inst = malloc(sizeof *inst);
    if (!inst) return QK_ERROR;
    inst->host = host;
    inst->n = ((const Gen *) self)->n;
    *out = inst;
    return QK_OK;
}

static void destroy(QkInstance *inst) { free(inst); }

static Gen *gen_new(const char *kname, unsigned n, uint32_t nslots, uint32_t ntransfers, uint32_t flags)
{
    Gen *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->n = n;
    g->slots = calloc(nslots, sizeof *g->slots);
    g->transfers = calloc(ntransfers ? ntransfers : 1, sizeof *g->transfers);
    g->names = calloc((size_t) (nslots + ntransfers) * NAME, 1);
    if (!g->slots || !g->transfers || !g->names) {
        free(g->slots);
        free(g->transfers);
        free(g->names);
        free(g);
        return NULL;
    }
    g->k.abi_version = QK_ABI_VERSION;
    g->k.size = sizeof(QkKernel);
    g->k.qr_version = QR_VERSION;
    g->k.name = kname;
    g->k.flags = flags;
    g->k.slots = g->slots;
    g->k.nslots = nslots;
    g->k.transfers = g->transfers;
    g->k.ntransfers = ntransfers;
    g->k.create = create;
    g->k.destroy = destroy;
    return g;
}

static const char *name_at(Gen *g, uint32_t i, const char *fmt, unsigned v)
{
    char *s = g->names + (size_t) i * NAME;
    snprintf(s, NAME, fmt, v);
    return s;
}

static void set_slot(Gen *g, uint32_t i, const char *name, QkRole role, const char *dim, int8_t scale, QInterval valid)
{
    g->slots[i] = (QkSlot) { name, role, dim, scale, valid };
}

void qk_free(QkKernel *k)
{
    if (!k) return;
    Gen *g = (Gen *) k;
    free(g->slots);
    free(g->transfers);
    free(g->names);
    free(g);
}

static const QInterval ANY = { Q_NINF, Q_INF };

/* --- native.diffusion ------------------------------------------------------------- */

/* slots: c{i}.temperature, c{i}.capacity, c{i}.energy, then conductance and tolerance */
#define D_T(i)  (3 * (i))
#define D_C(i)  (3 * (i) + 1)
#define D_E(i)  (3 * (i) + 2)
#define D_G(n)  (3 * (n))
#define D_TOL(n) (3 * (n) + 1)

static QkResult d_predict(QkInstance *inst, QkCall *call, QkPrediction *out)
{
    const QkHost *h = inst->host;
    unsigned n = inst->n;
    out->event_lo = out->event_hi = QK_NO_TIME;
    int64_t g = h->read(call, D_G(n)).hi, tol = h->read(call, D_TOL(n)).lo;
    int64_t cmin = INT64_MAX;
    for (unsigned i = 0; i < n; i++) {
        int64_t c = h->read(call, D_C(i)).lo;
        if (c < cmin) cmin = c;
    }
    if (n < 2 || g == 0) {
        out->horizon = QK_NO_TIME;
        return QK_OK;
    }
    /* An interior cell exchanges with two neighbours, so explicit steps are
     * stable below C / (2G). The step is tol × C_min / (2G). */
    int e = S_TOL + S_C - S_G - h->tick_scale(call);
    out->horizon = km_horizon(h->now(call), km_scaled_floor((km_i128) tol * cmin, e, (km_i128) 2 * g));
    return QK_OK;
}

static QkResult d_advance(QkInstance *inst, QkCall *call, int64_t to)
{
    const QkHost *h = inst->host;
    unsigned n = inst->n;
    int64_t dt = to - h->now(call);
    int64_t g = km_mid(h->read(call, D_G(n)));
    int e = S_G + S_T + h->tick_scale(call) - S_E;
    int64_t t_prev = km_mid(h->read(call, D_T(0)));
    for (unsigned i = 0; i + 1 < n; i++) {
        int64_t t_next = km_mid(h->read(call, D_T(i + 1)));
        km_i128 x;
        int64_t q;
        if (!km_mul3(g, t_prev - t_next, dt, &x) || !km_scale_half_even(x, e, &q)) return QK_ERROR;
        h->transfer(call, i, q);
        t_prev = t_next;
    }
    return QK_OK;
}

QkKernel *qk_diffusion_new(unsigned n)
{
    if (n < 1 || n > 100000) return NULL;
    Gen *g = gen_new("native.diffusion", n, 3 * n + 2, n - 1, QK_EVOLVE | QK_STATELESS | QK_PORTABLE);
    if (!g) return NULL;
    uint32_t nm = 0;
    for (unsigned i = 0; i < n; i++) {
        set_slot(g, D_T(i), name_at(g, nm++, "c%u.temperature", i), QK_IN, "K", S_T, (QInterval) { 0, Q_INF });
        set_slot(g, D_C(i), name_at(g, nm++, "c%u.capacity", i), QK_IN, "J/K", S_C, (QInterval) { 1, Q_INF });
        set_slot(g, D_E(i), name_at(g, nm++, "c%u.energy", i), QK_STOCK, "J", S_E, ANY);
    }
    set_slot(g, D_G(n), "conductance", QK_IN, "W/K", S_G, (QInterval) { 0, Q_INF });
    set_slot(g, D_TOL(n), "tolerance", QK_IN, "", S_TOL, (QInterval) { 1, 1000000000 });
    for (unsigned i = 0; i + 1 < n; i++)
        g->transfers[i] = (QkTransfer) { name_at(g, nm++, "flow%u", i), D_E(i), D_E(i + 1), true };
    g->k.predict = d_predict;
    g->k.advance = d_advance;
    return &g->k;
}

/* --- native.room.refine --------------------------------------------------------- */

/* slots: from.energy, to.c{i}.energy, to.c{i}.capacity, spread */
#define R_FROM       0
#define R_E(i)       (1 + 2 * (i))
#define R_C(i)       (2 + 2 * (i))
#define R_SPREAD(n)  (1 + 2 * (n))

static QkResult r_map(QkInstance *inst, QkCall *call)
{
    const QkHost *h = inst->host;
    unsigned n = inst->n;
    int64_t e = h->read(call, R_FROM).lo, spread = h->read(call, R_SPREAD(n)).lo;
    km_i128 ctot = 0;
    for (unsigned i = 0; i < n; i++) ctot += h->read(call, R_C(i)).lo;
    /* the mean temperature in µK: (E in mJ) / (C in mJ/K) × 10^6 */
    km_i128 t = (km_i128) e * 1000000 / ctot;
    QRCursor cur = h->random(call, "refine");
    km_i128 given = 0;
    for (unsigned i = 0; i < n; i++) {
        int64_t amount;
        if (i + 1 < n) {
            /* each cell gets the mean temperature, moved by a seeded amount
             * in [-spread, spread]; the last cell takes the remainder, so the
             * source ends at exactly zero */
            int64_t delta = spread ? qr_range(&cur, -spread, spread) : 0;
            km_i128 a = (km_i128) h->read(call, R_C(i)).lo * (t + delta) / 1000000;
            if (a > Q_FMAX || a < Q_FMIN) return QK_ERROR;
            amount = (int64_t) a;
        } else {
            km_i128 rest = (km_i128) e - given;
            if (rest > Q_FMAX || rest < Q_FMIN) return QK_ERROR;
            amount = (int64_t) rest;
        }
        given += amount;
        h->transfer(call, i, amount);
    }
    return QK_OK;
}

QkKernel *qk_refine_new(unsigned n)
{
    if (n < 1 || n > 100000) return NULL;
    Gen *g = gen_new("native.room.refine", n, 2 * n + 2, n, QK_MAP | QK_STATELESS | QK_PORTABLE);
    if (!g) return NULL;
    uint32_t nm = 0;
    set_slot(g, R_FROM, "from.energy", QK_STOCK, "J", S_E, ANY);
    for (unsigned i = 0; i < n; i++) {
        set_slot(g, R_E(i), name_at(g, nm++, "to.c%u.energy", i), QK_STOCK, "J", S_E, ANY);
        set_slot(g, R_C(i), name_at(g, nm++, "to.c%u.capacity", i), QK_IN, "J/K", S_C, (QInterval) { 1, Q_INF });
        g->transfers[i] = (QkTransfer) { name_at(g, nm++, "fill%u", i), R_FROM, R_E(i), true };
    }
    set_slot(g, R_SPREAD(n), "spread", QK_IN, "K", S_T, (QInterval) { 0, Q_INF });
    g->k.map = r_map;
    return &g->k;
}

/* --- native.room.abstract ------------------------------------------------------- */

/* slots: from.c{i}.energy, to.energy */
#define A_E(i)   (i)
#define A_TO(n)  (n)

static QkResult a_map(QkInstance *inst, QkCall *call)
{
    const QkHost *h = inst->host;
    for (unsigned i = 0; i < inst->n; i++) h->transfer(call, i, h->read(call, A_E(i)).lo);
    return QK_OK;
}

QkKernel *qk_abstract_new(unsigned n)
{
    if (n < 1 || n > 100000) return NULL;
    Gen *g = gen_new("native.room.abstract", n, n + 1, n, QK_MAP | QK_STATELESS | QK_PORTABLE);
    if (!g) return NULL;
    uint32_t nm = 0;
    for (unsigned i = 0; i < n; i++) {
        set_slot(g, A_E(i), name_at(g, nm++, "from.c%u.energy", i), QK_STOCK, "J", S_E, ANY);
        g->transfers[i] = (QkTransfer) { name_at(g, nm++, "gather%u", i), A_E(i), A_TO(n), true };
    }
    set_slot(g, A_TO(n), "to.energy", QK_STOCK, "J", S_E, ANY);
    g->k.map = a_map;
    return &g->k;
}
