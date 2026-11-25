#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kmath.h"
#include "quad/kernels.h"

/* native.rate (see quad/kernels.h, docs/adr/0020). Integer arithmetic only. */

typedef struct {
    QkKernel        k;          /* first, so that a QkKernel pointer is a Gen pointer */
    QkSlot         *slots;
    QkTransfer     *transfers;
    QkRateTransfer *uses;
    char           *names;
} Gen;

/* What the kernel keeps for one transfer. rate: the rate at the last time
 * that the kernel saw it change. since: that time. sign: the direction of
 * that change. turns: how many of the last changes turned the direction, one
 * after the other. cap: the longest step, or 0 for no limit. carry: the part
 * of the amount that is smaller than one quantum of the stock, in units of
 * one quantum of the rate for one tick. */
typedef struct {
    bool    have;
    int8_t  sign;
    uint8_t turns;
    int64_t rate, since, cap;
    int64_t carry_hi;
    uint64_t carry_lo;
} Flow;

/* A change of fewer quanta than this is the noise of rounding, and not a turn. */
enum { NOISE = 4 };

struct QkInstance {
    const QkHost *host;
    const Gen    *g;
    Flow          flow[];
};

static km_i128 carry_get(const Flow *f) { return (km_i128) (((km_u128) (uint64_t) f->carry_hi << 64) | f->carry_lo); }

static void carry_set(Flow *f, km_i128 v)
{
    f->carry_hi = (int64_t) (v >> 64);
    f->carry_lo = (uint64_t) v;
}

static QkResult create(const QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out)
{
    (void) call;
    const Gen *g = (const Gen *) self;
    QkInstance *inst = calloc(1, sizeof *inst + (g->k.ntransfers ? g->k.ntransfers : 1) * sizeof(Flow));
    if (!inst) return QK_ERROR;
    inst->host = host;
    inst->g = g;
    *out = inst;
    return QK_OK;
}

static void destroy(QkInstance *inst) { free(inst); }

/* The value of a slot of a transfer now: the middle of its range. False with
 * a message for the host if the range has no bound. what: "rate" or "ratio". */
static bool slot_now(QkInstance *inst, QkCall *call, uint32_t t, uint32_t slot, const char *what, int64_t *out)
{
    const Gen *g = inst->g;
    QInterval v = inst->host->read(call, slot);
    if (v.lo == Q_NINF || v.hi == Q_INF) {
        char msg[200];
        snprintf(msg, sizeof msg, "the %s of the interaction %s has no bound: a law or a value must give it", what,
                 g->transfers[t].name);
        inst->host->log(call, msg);
        return false;
    }
    *out = km_mid(v);
    return true;
}

static bool rate_now(QkInstance *inst, QkCall *call, uint32_t t, int64_t *out)
{
    return slot_now(inst, call, t, inst->g->uses[t].rate, "rate", out);
}

/* An amount that the kernel cannot compute or that the stock cannot take:
 * a message for the host, and the error. */
static QkResult too_large(QkInstance *inst, QkCall *call, uint32_t t)
{
    char msg[200];
    snprintf(msg, sizeof msg, "the amount of the interaction %s in one step is too large for the arithmetic of the kernel",
             inst->g->transfers[t].name);
    inst->host->log(call, msg);
    return QK_ERROR;
}

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

/* The step keeps the change of each rate at about `tolerance` of the rate.
 * The kernel knows how much the rate changed in the time since its last
 * change, and assumes the same slope. With no change in that time, the step
 * is as long as that time. A step is at most two times that time, because
 * the kernel does not repeat a step.
 *
 * That rule alone fails near a balance of two flows: each rate changes
 * little, the steps become longer than the time constant, and the stock goes
 * above and below the balance in turn. A rate that goes up and down in turn
 * shows this. The step then does not grow, and after two turns one after the
 * other the kernel sets a cap of half the last step. The cap grows slowly
 * while the rate goes in one direction. */
static QkResult predict(QkInstance *inst, QkCall *call, QkPrediction *out)
{
    const QkHost *h = inst->host;
    const Gen *g = inst->g;
    int64_t now = h->now(call);
    out->horizon = out->event_lo = out->event_hi = QK_NO_TIME;
    for (uint32_t t = 0; t < g->k.ntransfers; t++) {
        const Flow *f = &inst->flow[t];
        int64_t r, step = 1;
        if (!rate_now(inst, call, t, &r)) return QK_ERROR;
        int64_t tol = h->read(call, g->uses[t].tolerance).lo;
        if (f->have) {
            int64_t span = now > f->since ? now - f->since : 1;
            int64_t twice = span > INT64_MAX / 2 ? INT64_MAX : 2 * span;
            if (r == f->rate) {
                step = span;
            } else {
                /* span × tol × max(|r|, |old|) / |r − old|, with tol in units of 10^-9 */
                km_i128 big = abs64(r) > abs64(f->rate) ? abs64(r) : abs64(f->rate);
                km_i128 change = km_abs((km_i128) r - f->rate);
                km_i128 part = (km_i128) tol * big / change;                   /* the allowed part of the span, × 10^9 */
                step = part >= (km_i128) 2000000000 ? twice : (int64_t) ((km_i128) span * part / 1000000000);
            }
            if (step > twice) step = twice;
            km_i128 diff = (km_i128) r - f->rate;
            bool turn = f->sign && km_abs(diff) >= NOISE && (diff > 0 ? 1 : -1) != f->sign;
            if ((turn || f->turns) && step > span) step = span;
            if (f->cap && step > f->cap) step = f->cap;
        }
        int64_t until = km_horizon(now, step);
        if (until < out->horizon) out->horizon = until;
    }
    return QK_OK;
}

static QkResult advance(QkInstance *inst, QkCall *call, int64_t to)
{
    const QkHost *h = inst->host;
    const Gen *g = inst->g;
    int64_t now = h->now(call), dt = to - now;
    for (uint32_t t = 0; t < g->k.ntransfers; t++) {
        Flow *f = &inst->flow[t];
        int64_t r;
        if (!rate_now(inst, call, t, &r)) return QK_ERROR;
        if (!f->have) {
            f->have = true;
            f->rate = r;
            f->since = now;
        } else if (r != f->rate) {
            km_i128 diff = (km_i128) r - f->rate;
            if (km_abs(diff) >= NOISE) {
                int8_t sign = diff > 0 ? 1 : -1;
                f->turns = f->sign && sign != f->sign ? (uint8_t) (f->turns + 1) : 0;
                f->sign = sign;
                if (f->turns >= 2) {
                    f->cap = now - f->since > 1 ? (now - f->since) / 2 : 1;
                    f->turns = 0;
                } else if (f->cap && !f->turns && f->cap < INT64_MAX / 2) {
                    f->cap += f->cap / 16 + 1;
                }
            }
            f->rate = r;
            f->since = now;
        }
        /* amount in quanta of the stock = rate × dt × 10^e, and the rest stays for the next step */
        const QkTransfer *tr = &g->transfers[t];
        int stock = g->slots[tr->from != QK_NONE ? tr->from : tr->to].scale;
        int e = g->slots[g->uses[t].rate].scale + h->tick_scale(call) - stock;
        km_i128 n = (km_i128) r * dt, q;
        if (g->uses[t].ratio != QK_NONE) {
            /* a flow of a reaction: the amount of the reaction, r × dt, times the ratio of the flow */
            int64_t ratio;
            if (!slot_now(inst, call, t, g->uses[t].ratio, "ratio", &ratio)) return QK_ERROR;
            if (!km_mul3(r, ratio, dt, &n)) return too_large(inst, call, t);
            e += g->slots[g->uses[t].ratio].scale;
        }
        if (e >= 0) {
            if (e > 38 || (n != 0 && km_abs(n) > KM_I128_MAX / km_pow10(e))) return too_large(inst, call, t);
            q = n * km_pow10(e);
        } else {
            if (-e > 36) return too_large(inst, call, t);
            km_i128 d = km_pow10(-e), total = n + carry_get(f);
            q = total / d;
            carry_set(f, total - q * d);
        }
        if (q > Q_FMAX || q < Q_FMIN) return too_large(inst, call, t);
        if (q && h->transfer(call, t, (int64_t) q) != QK_HOST_OK) return QK_ERROR;
    }
    return QK_OK;
}

static size_t save(QkInstance *inst, void *buf, size_t len)
{
    size_t need = inst->g->k.ntransfers * sizeof(Flow);
    if (buf && len >= need) memcpy(buf, inst->flow, need);
    return need;
}

static QkResult restore(QkInstance *inst, const void *buf, size_t len)
{
    size_t need = inst->g->k.ntransfers * sizeof(Flow);
    if (len != need) return QK_ERROR;
    memcpy(inst->flow, buf, need);
    return QK_OK;
}

void qk_rate_free(QkKernel *k)
{
    if (!k) return;
    Gen *g = (Gen *) k;
    free(g->slots);
    free(g->transfers);
    free(g->uses);
    free(g->names);
    free(g);
}

QkKernel *qk_rate_new(const QkSlot *slots, uint32_t nslots, const QkRateTransfer *transfers, uint32_t ntransfers)
{
    if (!nslots || !ntransfers) return NULL;
    size_t text = 0;
    for (uint32_t i = 0; i < nslots; i++) text += strlen(slots[i].name) + strlen(slots[i].dimension) + 2;
    for (uint32_t i = 0; i < ntransfers; i++) text += strlen(transfers[i].name) + 1;
    Gen *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->slots = calloc(nslots, sizeof *g->slots);
    g->transfers = calloc(ntransfers, sizeof *g->transfers);
    g->uses = calloc(ntransfers, sizeof *g->uses);
    g->names = malloc(text);
    if (!g->slots || !g->transfers || !g->uses || !g->names) {
        qk_rate_free(&g->k);
        return NULL;
    }
    char *p = g->names;
    for (uint32_t i = 0; i < nslots; i++) {
        g->slots[i] = slots[i];
        g->slots[i].name = strcpy(p, slots[i].name);
        p += strlen(p) + 1;
        g->slots[i].dimension = strcpy(p, slots[i].dimension);
        p += strlen(p) + 1;
    }
    for (uint32_t i = 0; i < ntransfers; i++) {
        const QkRateTransfer *u = &transfers[i];
        bool from = u->from != QK_NONE, to = u->to != QK_NONE;
        /* a rate and a tolerance that the kernel reads, and one stock at least.
         * The rate can be a stock of the phenomenon: a speed is the rate of a position */
        bool ok = u->rate < nslots && u->tolerance < nslots && (slots[u->rate].role == QK_IN || slots[u->rate].role == QK_STOCK) &&
                  slots[u->tolerance].role == QK_IN && (from || to) && (!from || u->from < nslots) && (!to || u->to < nslots) &&
                  (u->ratio == QK_NONE || (u->ratio < nslots && slots[u->ratio].role == QK_IN));
        if (!ok) {
            qk_rate_free(&g->k);
            return NULL;
        }
        g->uses[i] = *u;
        g->uses[i].name = strcpy(p, u->name);
        p += strlen(p) + 1;
        g->transfers[i] = (QkTransfer) { g->uses[i].name, u->from, u->to, from && to };
    }
    g->k = (QkKernel) {
        .abi_version = QK_ABI_VERSION,
        .size = sizeof(QkKernel),
        .qr_version = QR_VERSION,
        .name = "native.rate",
        .flags = QK_EVOLVE | QK_PORTABLE,
        .slots = g->slots,
        .nslots = nslots,
        .transfers = g->transfers,
        .ntransfers = ntransfers,
        .create = create,
        .destroy = destroy,
        .predict = predict,
        .advance = advance,
        .save = save,
        .restore = restore,
    };
    return &g->k;
}
