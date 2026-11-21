#include <stdlib.h>

#include "quad/kernels.h"

/* native.conduction (see quad/kernels.h). Integer arithmetic only. */

__extension__ typedef __int128 i128;
__extension__ typedef unsigned __int128 u128;

#define I128_MAX ((i128) (((u128) 1 << 127) - 1))

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

static i128 pow10_(int k)
{
    i128 p = 1;
    while (k-- > 0) p *= 10;
    return p;
}

static i128 abs128(i128 v) { return v < 0 ? -v : v; }

/* The midpoint of a finite interval, rounded toward negative infinity */
static int64_t mid(QInterval v)
{
    i128 s = (i128) v.lo + v.hi;
    i128 m = s / 2;
    if (s < 0 && s % 2) m -= 1;
    return (int64_t) m;
}

static QkResult create(const QkHost *host, QkCall *call, QkInstance **out)
{
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
    int64_t now = h->now(call);
    out->event_lo = out->event_hi = QK_NO_TIME;
    if (g == 0) {
        out->horizon = QK_NO_TIME;
        return QK_OK;
    }
    /* In seconds, the step is tol × C_h C_c / (G (C_h + C_c)). With every
     * value in its quanta, that is
     *   tol_q C_h,q C_c,q / (G_q (C_h,q + C_c,q)) × 10^(S_TOL + S_C − S_G)
     * seconds, and the clock counts ticks of 10^ts s. */
    int e = S_TOL + S_C - S_G - h->tick_scale(call);
    i128 n = (i128) tol * ch * cc, d = (i128) g * ((i128) ch + cc);
    int64_t step;
    if (e >= 0) {
        if (e > 38 || n > I128_MAX / pow10_(e)) step = INT64_MAX;
        else {
            i128 q = n * pow10_(e) / d;
            step = q > INT64_MAX ? INT64_MAX : (int64_t) q;
        }
    } else {
        if (-e > 38 || d > I128_MAX / pow10_(-e)) step = 0;
        else {
            i128 q = n / (d * pow10_(-e));
            step = q > INT64_MAX ? INT64_MAX : (int64_t) q;
        }
    }
    if (step < 1) step = 1;
    out->horizon = step >= QK_NO_TIME - now ? QK_NO_TIME - 1 : now + step;
    return QK_OK;
}

static QkResult advance(QkInstance *inst, QkCall *call, int64_t to)
{
    const QkHost *h = inst->host;
    int64_t dt = to - h->now(call);
    int64_t th = mid(h->read(call, HOT_T)), tc = mid(h->read(call, COLD_T));
    int64_t g = mid(h->read(call, COND));

    /* energy in quanta of 10^S_E J = G × ΔT × dt × 10^(S_G + S_T + ts − S_E) */
    i128 n = (i128) g * ((i128) th - tc);
    if (n != 0 && dt > I128_MAX / abs128(n)) return QK_ERROR;
    n *= dt;
    int e = S_G + S_T + h->tick_scale(call) - S_E;
    i128 q;
    if (e >= 0) {
        if (e > 38 || abs128(n) > I128_MAX / pow10_(e)) return QK_ERROR;
        q = n * pow10_(e);
    } else if (-e > 38) {
        q = 0;
    } else {
        /* round half to even */
        i128 d = pow10_(-e);
        q = n / d;
        i128 r = abs128(n % d);
        if (r > d - r || (r != 0 && r == d - r && (q & 1))) q += n < 0 ? -1 : 1;
    }
    if (q > Q_FMAX || q < Q_FMIN) return QK_ERROR;
    h->transfer(call, HEAT, (int64_t) q);
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
