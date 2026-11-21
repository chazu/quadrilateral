#include <stdlib.h>

#include "kmath.h"
#include "quad/kernels.h"

/* native.conduction (see quad/kernels.h). Integer arithmetic only. */

enum { HOT_T, COLD_T, HOT_C, COLD_C, HOT_E, COLD_E, COND, TOL, NSLOTS };
enum { HEAT };

/* Slot scales: temperature in µK, capacity in mJ/K, energy in mJ, conductance
 * in mW/K, tolerance in units of 10^-9. */
#define S_T   (-6)
#define S_C   (-3)
#define S_E   (-3)
#define S_G   (-3)
#define S_TOL (-9)

static const QkSlot slots[NSLOTS] = {
    [HOT_T]  = { "hot.temperature",  QK_IN,   "K",   S_T,   { 0, Q_INF } },
    [COLD_T] = { "cold.temperature", QK_IN,   "K",   S_T,   { 0, Q_INF } },
    [HOT_C]  = { "hot.capacity",     QK_IN,   "J/K", S_C,   { 1, Q_INF } },
    [COLD_C] = { "cold.capacity",    QK_IN,   "J/K", S_C,   { 1, Q_INF } },
    [HOT_E]  = { "hot.energy",       QK_LINK, "J",   S_E,   { Q_NINF, Q_INF } },
    [COLD_E] = { "cold.energy",      QK_LINK, "J",   S_E,   { Q_NINF, Q_INF } },
    [COND]   = { "conductance",      QK_IN,   "W/K", S_G,   { 0, Q_INF } },
    [TOL]    = { "tolerance",        QK_IN,   "",    S_TOL, { 1, 1000000000 } },
};

static const QkTransfer transfers[] = {
    [HEAT] = { "heat", HOT_E, COLD_E, true },
};

/* The kernel keeps no state of its own. Its instance holds only the host
 * table, which the host gives once, at create. */
struct QkInstance { const QkHost *host; };

static QkResult create(const QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out)
{
    (void) self;
    (void) call;
    QkInstance *inst = malloc(sizeof *inst);
    if (!inst) return QK_ERROR;
    inst->host = host;
    *out = inst;
    return QK_OK;
}

static void destroy(QkInstance *inst) { free(inst); }

static QkResult predict(QkInstance *inst, QkCall *call, QkPrediction *out)
{
    const QkHost *h = inst->host;
    int64_t ch = h->read(call, HOT_C).lo, cc = h->read(call, COLD_C).lo;
    int64_t g = h->read(call, COND).hi, tol = h->read(call, TOL).lo;
    out->event_lo = out->event_hi = QK_NO_TIME;
    if (g == 0) {
        out->horizon = QK_NO_TIME;
        return QK_OK;
    }
    /* In seconds, the step is tol × C_h C_c / (G (C_h + C_c)). With every
     * value in its quanta, that is
     *   tol_q C_h,q C_c,q / (G_q (C_h,q + C_c,q)) × 10^(S_TOL + S_C − S_G)
     * seconds, and the clock counts ticks of 10^ts s. */
    km_i128 n;
    if (!km_mul3(tol, ch, cc, &n)) return QK_ERROR;
    int e = S_TOL + S_C - S_G - h->tick_scale(call);
    out->horizon = km_horizon(h->now(call), km_scaled_floor(n, e, (km_i128) g * ((km_i128) ch + cc)));
    return QK_OK;
}

static QkResult advance(QkInstance *inst, QkCall *call, int64_t to)
{
    const QkHost *h = inst->host;
    int64_t dt = to - h->now(call);
    int64_t th = km_mid(h->read(call, HOT_T)), tc = km_mid(h->read(call, COLD_T));
    int64_t g = km_mid(h->read(call, COND));
    /* energy in quanta of 10^S_E J = G × ΔT × dt × 10^(S_G + S_T + ts − S_E) */
    km_i128 n;
    int64_t q;
    if (!km_mul3(g, th - tc, dt, &n)) return QK_ERROR;
    if (!km_scale_half_even(n, S_G + S_T + h->tick_scale(call) - S_E, &q)) return QK_ERROR;
    h->transfer(call, HEAT, q);
    return QK_OK;
}

static const QkKernel kernel = {
    .abi_version = QK_ABI_VERSION,
    .size = sizeof(QkKernel),
    .qr_version = QR_VERSION,
    .name = "native.conduction",
    .flags = QK_EVOLVE | QK_STATELESS | QK_PORTABLE,
    .slots = slots,
    .nslots = NSLOTS,
    .transfers = transfers,
    .ntransfers = sizeof transfers / sizeof transfers[0],
    .create = create,
    .destroy = destroy,
    .predict = predict,
    .advance = advance,
};

const QkKernel *qk_conduction(void) { return &kernel; }
