#include "quad/world.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- storage ------------------------------------------------------------------- */

typedef struct { char *name; QType type; bool conserved; } Kind;
typedef struct { char *path; QwKind kind; QwPhen owner; } CellInfo;
typedef struct { char *port, *target; } Port;
typedef struct { uint32_t slot; QnCell cell; QInterval range; bool explicit_; } Assume;

typedef struct {
    char           *name;
    Port           *ports;
    size_t          nports, capports;
    const QkKernel *k;
    QkInstance     *inst;
    QnCell         *bind;       /* for each slot */
    QConv          *to_slot;    /* cell scale to slot scale */
    QConv          *to_cell;    /* slot scale to cell scale */
    bool            predicted;
    QkPrediction    pred;
    Assume         *as;
    size_t          nas, capas;
    uint64_t        events, predicts;
} Phen;

typedef struct { QWorld *w; QwPhen id; } NarrowCtx;

enum { M_CREATE, M_PREDICT, M_ADVANCE, M_NARROW };

struct QkCall {
    QWorld *w;
    QwPhen  id;
    int     mode;
    int     err;
    char    msg[256];
};

struct QWorld {
    QNet     *net;
    uint64_t  seed;
    int8_t    tick;

    char    **bases;
    size_t    nbases, capbases;
    Kind     *kinds;
    size_t    nkinds, capkinds;
    CellInfo *cells;
    size_t    ncells, capcells;
    Phen     *phens;
    size_t    nphens, capphens;
    NarrowCtx **nctx;
    size_t    nnctx, capnctx;

    int64_t   now;
    uint64_t  boundaries;

    /* staging for one boundary, one entry for each cell */
    int64_t  *delta, *wval;
    bool     *written;
    size_t    capstage;

    bool      narrow_failed;
    char      narrow_msg[300];
    char      err[512];
    char      log[256];
    QkHost    host;
};

static bool grow(void **p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) return true;
    size_t n = *cap ? *cap * 2 : 8;
    while (n < need) n *= 2;
    void *q = realloc(*p, n * elem);
    if (!q) return false;
    *p = q;
    *cap = n;
    return true;
}

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static QwStatus fail(QWorld *w, QwStatus s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->err, sizeof w->err, fmt, ap);
    va_end(ap);
    return s;
}

const char *qw_status_str(QwStatus s)
{
    switch (s) {
    case QW_OK:             return "ok";
    case QW_ENAME:          return "unknown or malformed name";
    case QW_EDUP:           return "name already exists";
    case QW_ETYPE:          return "quantity types do not fit";
    case QW_EOWNER:         return "cell owned by another phenomenon";
    case QW_EKERNEL:        return "kernel error";
    case QW_EINAPPLICABLE:  return "input outside a kernel's valid range";
    case QW_ECONTRADICTION: return "contradiction";
    case QW_ESTOCK:         return "stock changed other than by a transfer";
    case QW_ETIME:          return "invalid time";
    case QW_ENOMEM:         return "out of memory";
    }
    return "unknown status";
}

const char *qw_error(const QWorld *w) { return w->err; }
QNet *qw_net(QWorld *w) { return w->net; }
int64_t qw_now(const QWorld *w) { return w->now; }
uint64_t qw_boundaries(const QWorld *w) { return w->boundaries; }

/* --- the host table ---------------------------------------------------------------- */

static Phen *call_phen(QkCall *c) { return &c->w->phens[c->id]; }

static int call_err(QkCall *c, int code, const char *fmt, ...)
{
    if (!c->err) {
        c->err = code;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->msg, sizeof c->msg, fmt, ap);
        va_end(ap);
    }
    return code;
}

static bool slot_ok(QkCall *c, uint32_t slot)
{
    Phen *p = call_phen(c);
    if (slot < p->k->nslots) return true;
    call_err(c, QK_HOST_EINDEX, "slot index %u out of range", slot);
    return false;
}

static void set_assume(Phen *p, uint32_t slot, QnCell cell, QInterval range, bool explicit_)
{
    for (size_t i = 0; i < p->nas; i++)
        if (p->as[i].slot == slot) {
            if (explicit_ || !p->as[i].explicit_) {
                p->as[i].range = range;
                p->as[i].explicit_ = explicit_;
            }
            return;
        }
    if (!grow((void **) &p->as, &p->capas, p->nas + 1, sizeof *p->as)) return;
    p->as[p->nas++] = (Assume) { slot, cell, range, explicit_ };
}

static QInterval h_read(QkCall *c, uint32_t slot)
{
    if (!slot_ok(c, slot)) return qi_top();
    Phen *p = call_phen(c);
    const QkSlot *s = &p->k->slots[slot];
    if (s->role == QK_LINK) {
        call_err(c, QK_HOST_EROLE, "slot '%s' is a link, which cannot be read", s->name);
        return qi_top();
    }
    QInterval v = qn_get(c->w->net, p->bind[slot]);
    if (c->mode == M_PREDICT && s->role == QK_IN) set_assume(p, slot, p->bind[slot], v, false);
    return qi_conv(v, &p->to_slot[slot]);
}

static int h_write(QkCall *c, uint32_t slot, int64_t value)
{
    if (!slot_ok(c, slot)) return QK_HOST_EINDEX;
    Phen *p = call_phen(c);
    const QkSlot *s = &p->k->slots[slot];
    if (c->mode != M_ADVANCE) return call_err(c, QK_HOST_EMODE, "write to slot '%s' outside advance", s->name);
    if (s->role != QK_OUT && s->role != QK_STATE)
        return call_err(c, QK_HOST_EROLE, "write to slot '%s', which is not an output or a state", s->name);
    int64_t v;
    if (qp_conv(value, &p->to_cell[slot], &v) != Q_OK)
        return call_err(c, QK_HOST_EVALUE, "value for slot '%s' out of range", s->name);
    QnCell cell = p->bind[slot];
    c->w->wval[cell] = v;
    c->w->written[cell] = true;
    return QK_HOST_OK;
}

static int h_transfer(QkCall *c, uint32_t t, int64_t amount)
{
    Phen *p = call_phen(c);
    if (t >= p->k->ntransfers) return call_err(c, QK_HOST_EINDEX, "transfer index %u out of range", t);
    const QkTransfer *tr = &p->k->transfers[t];
    if (c->mode != M_ADVANCE) return call_err(c, QK_HOST_EMODE, "transfer '%s' outside advance", tr->name);
    QWorld *w = c->w;
    if (tr->from != QK_NONE) {
        QnCell f = p->bind[tr->from];
        if (qp_sub(w->delta[f], amount, &w->delta[f]) != Q_OK)
            return call_err(c, QK_HOST_EVALUE, "transfer '%s' out of range", tr->name);
    }
    if (tr->to != QK_NONE) {
        QnCell d = p->bind[tr->to];
        if (qp_add(w->delta[d], amount, &w->delta[d]) != Q_OK)
            return call_err(c, QK_HOST_EVALUE, "transfer '%s' out of range", tr->name);
    }
    return QK_HOST_OK;
}

static int h_tell(QkCall *c, uint32_t slot, QInterval range)
{
    if (!slot_ok(c, slot)) return QK_HOST_EINDEX;
    Phen *p = call_phen(c);
    const QkSlot *s = &p->k->slots[slot];
    if (c->mode != M_NARROW) return call_err(c, QK_HOST_EMODE, "tell on slot '%s' outside narrow", s->name);
    if (s->role == QK_LINK) return call_err(c, QK_HOST_EROLE, "tell on link slot '%s'", s->name);
    if (!qi_valid(range)) return call_err(c, QK_HOST_EVALUE, "invalid interval for slot '%s'", s->name);
    qn_tell(c->w->net, p->bind[slot], qi_conv(range, &p->to_cell[slot]));
    return QK_HOST_OK;
}

static int h_assume(QkCall *c, uint32_t slot, QInterval range)
{
    if (!slot_ok(c, slot)) return QK_HOST_EINDEX;
    Phen *p = call_phen(c);
    const QkSlot *s = &p->k->slots[slot];
    if (c->mode != M_PREDICT) return call_err(c, QK_HOST_EMODE, "assume on slot '%s' outside predict", s->name);
    if (s->role != QK_IN) return call_err(c, QK_HOST_EROLE, "assume on slot '%s', which is not an input", s->name);
    if (!qi_valid(range)) return call_err(c, QK_HOST_EVALUE, "invalid interval for slot '%s'", s->name);
    set_assume(p, slot, p->bind[slot], qi_conv(range, &p->to_cell[slot]), true);
    return QK_HOST_OK;
}

static QRCursor h_random(QkCall *c, const char *stream)
{
    Phen *p = call_phen(c);
    QRStream s = qr_stream(c->w->seed, c->id, stream);
    return qr_cursor(&s, p->events);
}

static int64_t h_now(QkCall *c) { return c->w->now; }
static int8_t h_tick_scale(QkCall *c) { return c->w->tick; }

static void h_log(QkCall *c, const char *message)
{
    snprintf(c->w->log, sizeof c->w->log, "%s: %s", call_phen(c)->name, message);
}

/* --- building ---------------------------------------------------------------------- */

QWorld *qw_new(uint64_t seed, int8_t tick_scale)
{
    QWorld *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->net = qn_new();
    if (!w->net) {
        free(w);
        return NULL;
    }
    w->seed = seed;
    w->tick = tick_scale;
    w->host = (QkHost) { QK_ABI_VERSION, sizeof(QkHost), h_read,   h_write,      h_transfer, h_tell,
                         h_assume,       h_random,        h_now,  h_tick_scale, h_log };
    return w;
}

void qw_free(QWorld *w)
{
    if (!w) return;
    for (size_t i = 0; i < w->nphens; i++) {
        Phen *p = &w->phens[i];
        if (p->k && p->k->destroy && p->inst) p->k->destroy(p->inst);
        for (size_t j = 0; j < p->nports; j++) {
            free(p->ports[j].port);
            free(p->ports[j].target);
        }
        free(p->ports);
        free(p->name);
        free(p->bind);
        free(p->to_slot);
        free(p->to_cell);
        free(p->as);
    }
    for (size_t i = 0; i < w->nbases; i++) free(w->bases[i]);
    for (size_t i = 0; i < w->nkinds; i++) free(w->kinds[i].name);
    for (size_t i = 0; i < w->ncells; i++) free(w->cells[i].path);
    for (size_t i = 0; i < w->nnctx; i++) free(w->nctx[i]);
    free(w->bases);
    free(w->kinds);
    free(w->cells);
    free(w->phens);
    free(w->nctx);
    free(w->delta);
    free(w->wval);
    free(w->written);
    qn_free(w->net);
    free(w);
}

static bool valid_name(const char *s)
{
    if (!*s) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_'))
            return false;
    return true;
}

QwStatus qw_base_dim(QWorld *w, const char *name)
{
    if (!valid_name(name)) return fail(w, QW_ENAME, "invalid base dimension name '%s'", name);
    if (Q_NSI + w->nbases >= Q_MAXBASE) return fail(w, QW_ETYPE, "too many base dimensions");
    for (size_t i = 0; i < w->nbases; i++)
        if (!strcmp(w->bases[i], name)) return fail(w, QW_EDUP, "base dimension '%s' already exists", name);
    if (!grow((void **) &w->bases, &w->capbases, w->nbases + 1, sizeof *w->bases)) return QW_ENOMEM;
    if (!(w->bases[w->nbases] = dupstr(name))) return QW_ENOMEM;
    w->nbases++;
    return QW_OK;
}

static QStatus parse_dim(const QWorld *w, const char *text, QDim *out)
{
    return q_parse_dim(text, strlen(text), (const char *const *) w->bases, w->nbases, out);
}

QwStatus qw_kind(QWorld *w, const char *name, const char *dim, int scale, bool conserved, QwKind *out)
{
    if (!valid_name(name)) return fail(w, QW_ENAME, "invalid kind name '%s'", name);
    for (size_t i = 0; i < w->nkinds; i++)
        if (!strcmp(w->kinds[i].name, name)) return fail(w, QW_EDUP, "kind '%s' already exists", name);
    QDim d;
    if (parse_dim(w, dim, &d) != Q_OK) return fail(w, QW_ETYPE, "kind '%s': cannot read dimension '%s'", name, dim);
    if (scale < INT8_MIN || scale > INT8_MAX) return fail(w, QW_ETYPE, "kind '%s': scale out of range", name);
    if (!grow((void **) &w->kinds, &w->capkinds, w->nkinds + 1, sizeof *w->kinds)) return QW_ENOMEM;
    Kind *k = &w->kinds[w->nkinds];
    if (!(k->name = dupstr(name))) return QW_ENOMEM;
    k->type = qtype_make((uint32_t) w->nkinds + 1, d, (int8_t) scale);
    k->conserved = conserved;
    *out = (QwKind) w->nkinds++;
    return QW_OK;
}

QwStatus qw_kind_type(const QWorld *w, QwKind kind, QType *out)
{
    if (kind >= w->nkinds) return QW_ENAME;
    *out = w->kinds[kind].type;
    return QW_OK;
}

static bool find_phen(const QWorld *w, const char *name, size_t len, QwPhen *out)
{
    for (size_t i = 0; i < w->nphens; i++)
        if (strlen(w->phens[i].name) == len && !memcmp(w->phens[i].name, name, len)) {
            *out = (QwPhen) i;
            return true;
        }
    return false;
}

QwStatus qw_phenomenon(QWorld *w, const char *name, QwPhen *out)
{
    if (!valid_name(name)) return fail(w, QW_ENAME, "invalid phenomenon name '%s'", name);
    QwPhen dup;
    if (find_phen(w, name, strlen(name), &dup)) return fail(w, QW_EDUP, "phenomenon '%s' already exists", name);
    if (!grow((void **) &w->phens, &w->capphens, w->nphens + 1, sizeof *w->phens)) return QW_ENOMEM;
    Phen *p = &w->phens[w->nphens];
    memset(p, 0, sizeof *p);
    if (!(p->name = dupstr(name))) return QW_ENOMEM;
    *out = (QwPhen) w->nphens++;
    return QW_OK;
}

QwStatus qw_find(const QWorld *w, const char *path, QnCell *out)
{
    for (size_t i = 0; i < w->ncells; i++)
        if (!strcmp(w->cells[i].path, path)) {
            *out = (QnCell) i;
            return QW_OK;
        }
    return QW_ENAME;
}

const char *qw_cell_path(const QWorld *w, QnCell c) { return c < w->ncells ? w->cells[c].path : "?"; }

QwStatus qw_cell(QWorld *w, const char *path, QwKind kind, QInterval base, QnCell *out)
{
    const char *dot = strchr(path, '.');
    QwPhen owner;
    if (!dot || !dot[1]) return fail(w, QW_ENAME, "cell path '%s' has no phenomenon prefix", path);
    if (!find_phen(w, path, (size_t) (dot - path), &owner))
        return fail(w, QW_ENAME, "cell path '%s' names no phenomenon", path);
    QnCell dup;
    if (qw_find(w, path, &dup) == QW_OK) return fail(w, QW_EDUP, "cell '%s' already exists", path);
    if (kind >= w->nkinds) return fail(w, QW_ENAME, "cell '%s': no such kind", path);
    const Kind *k = &w->kinds[kind];
    if (k->conserved && (!qi_is_point(base) || base.lo == Q_NINF || base.hi == Q_INF))
        return fail(w, QW_ESTOCK, "stock '%s' needs an exact value", path);
    if (!grow((void **) &w->cells, &w->capcells, w->ncells + 1, sizeof *w->cells)) return QW_ENOMEM;
    size_t need = w->ncells + 1;
    size_t cap = w->capstage;
    if (need > cap) {
        if (!grow((void **) &w->delta, &cap, need, sizeof *w->delta)) return QW_ENOMEM;
        cap = w->capstage;
        if (!grow((void **) &w->wval, &cap, need, sizeof *w->wval)) return QW_ENOMEM;
        cap = w->capstage;
        if (!grow((void **) &w->written, &cap, need, sizeof *w->written)) return QW_ENOMEM;
        w->capstage = cap;
    }
    QnCell c;
    QnStatus s = qn_cell(w->net, &k->type, base, &c);
    if (s == QN_EVALUE) return fail(w, QW_ETYPE, "cell '%s': invalid interval", path);
    if (s) return fail(w, QW_ENOMEM, "cell '%s': %s", path, qn_status_str(s));
    CellInfo *ci = &w->cells[w->ncells];
    if (!(ci->path = dupstr(path))) return QW_ENOMEM;
    ci->kind = kind;
    ci->owner = owner;
    w->delta[c] = w->wval[c] = 0;
    w->written[c] = false;
    w->ncells++;
    if (out) *out = c;
    return QW_OK;
}

QwStatus qw_connect(QWorld *w, QwPhen pid, const char *port, const char *target)
{
    if (pid >= w->nphens) return fail(w, QW_ENAME, "no such phenomenon");
    Phen *p = &w->phens[pid];
    if (!valid_name(port)) return fail(w, QW_ENAME, "%s: invalid port name '%s'", p->name, port);
    if (p->k) return fail(w, QW_EKERNEL, "%s: connect ports before the kernel is attached", p->name);
    for (size_t i = 0; i < p->nports; i++)
        if (!strcmp(p->ports[i].port, port)) return fail(w, QW_EDUP, "%s: port '%s' already connected", p->name, port);
    if (!grow((void **) &p->ports, &p->capports, p->nports + 1, sizeof *p->ports)) return QW_ENOMEM;
    Port *pt = &p->ports[p->nports];
    if (!(pt->port = dupstr(port)) || !(pt->target = dupstr(target))) return QW_ENOMEM;
    p->nports++;
    return QW_OK;
}

/* The cell path for a slot: through a connected port, or under the phenomenon. */
static void slot_path(const Phen *p, const char *slot, char *buf, size_t len)
{
    for (size_t i = 0; i < p->nports; i++) {
        size_t n = strlen(p->ports[i].port);
        if (!strncmp(slot, p->ports[i].port, n) && slot[n] == '.') {
            snprintf(buf, len, "%s.%s", p->ports[i].target, slot + n + 1);
            return;
        }
    }
    snprintf(buf, len, "%s.%s", p->name, slot);
}

static void narrow_trampoline(QNet *net, void *ctx, const QnCell *cells, size_t n)
{
    (void) net;
    (void) cells;
    (void) n;
    NarrowCtx *nc = ctx;
    QWorld *w = nc->w;
    Phen *p = &w->phens[nc->id];
    QkCall call = { w, nc->id, M_NARROW, 0, { 0 } };
    p->k->narrow(p->inst, &call);
    if (call.err && !w->narrow_failed) {
        w->narrow_failed = true;
        snprintf(w->narrow_msg, sizeof w->narrow_msg, "%s (%s): %s", p->name, p->k->name, call.msg);
    }
}

static bool is_stock_role(QkRole r) { return r == QK_STOCK || r == QK_LINK; }

QwStatus qw_attach(QWorld *w, QwPhen pid, const QkKernel *k)
{
    if (pid >= w->nphens) return fail(w, QW_ENAME, "no such phenomenon");
    Phen *p = &w->phens[pid];
    const char *pn = p->name;
    if (p->k) return fail(w, QW_EKERNEL, "%s: already has a kernel", pn);
    if (k->abi_version != QK_ABI_VERSION || k->size < sizeof(QkKernel))
        return fail(w, QW_EKERNEL, "%s: kernel '%s' has interface version %u, expected %u", pn, k->name,
                    k->abi_version, QK_ABI_VERSION);
    if (k->qr_version != QR_VERSION)
        return fail(w, QW_EKERNEL, "%s: kernel '%s' expects random version %u", pn, k->name, k->qr_version);
    if (k->flags & (QK_MAP | QK_RATES))
        return fail(w, QW_EKERNEL, "%s: kernel '%s': map and rate kernels are not supported yet", pn, k->name);
    if ((k->flags & QK_EVOLVE) && (!k->predict || !k->advance))
        return fail(w, QW_EKERNEL, "%s: kernel '%s' is an evolution kernel without predict or advance", pn, k->name);
    if ((k->flags & QK_NARROW) && !k->narrow)
        return fail(w, QW_EKERNEL, "%s: kernel '%s' is a narrowing kernel without narrow", pn, k->name);

    QnCell *bind = calloc(k->nslots ? k->nslots : 1, sizeof *bind);
    QConv *ts = calloc(k->nslots ? k->nslots : 1, sizeof *ts);
    QConv *tc = calloc(k->nslots ? k->nslots : 1, sizeof *tc);
    QwStatus st = QW_OK;
    if (!bind || !ts || !tc) {
        st = QW_ENOMEM;
        goto out;
    }

    for (uint32_t i = 0; i < k->nslots; i++) {
        const QkSlot *s = &k->slots[i];
        char path[512];
        slot_path(p, s->name, path, sizeof path);
        QnCell c;
        if (qw_find(w, path, &c) != QW_OK) {
            st = fail(w, QW_ENAME, "%s: slot '%s' of kernel '%s' needs the cell '%s', which does not exist", pn,
                      s->name, k->name, path);
            goto out;
        }
        const CellInfo *ci = &w->cells[c];
        const Kind *kind = &w->kinds[ci->kind];
        QDim d;
        if (parse_dim(w, s->dimension, &d) != Q_OK) {
            st = fail(w, QW_ETYPE, "%s: slot '%s' of kernel '%s': cannot read dimension '%s'", pn, s->name, k->name,
                      s->dimension);
            goto out;
        }
        if (!qdim_eq(&d, &kind->type.dim)) {
            st = fail(w, QW_ETYPE, "%s: slot '%s' has dimension '%s', but '%s' is a %s", pn, s->name, s->dimension,
                      path, kind->name);
            goto out;
        }
        if (!qi_valid(s->valid)) {
            st = fail(w, QW_EKERNEL, "%s: slot '%s' of kernel '%s' has an invalid valid range", pn, s->name, k->name);
            goto out;
        }
        if (is_stock_role(s->role)) {
            if (!kind->conserved) {
                st = fail(w, QW_ETYPE, "%s: slot '%s' is a stock or link, but '%s' is not conserved", pn, s->name, path);
                goto out;
            }
            if (s->scale != kind->type.scale) {
                st = fail(w, QW_ETYPE, "%s: slot '%s' has scale %d, but the stock '%s' has scale %d", pn, s->name,
                          s->scale, path, kind->type.scale);
                goto out;
            }
        }
        if ((s->role == QK_STOCK || s->role == QK_STATE || s->role == QK_OUT) && ci->owner != pid) {
            st = fail(w, QW_EOWNER, "%s: slot '%s' would write '%s', which belongs to %s", pn, s->name, path,
                      w->phens[ci->owner].name);
            goto out;
        }
        if ((s->role == QK_STATE || s->role == QK_OUT) && kind->conserved) {
            st = fail(w, QW_ESTOCK, "%s: slot '%s' would write the stock '%s'; a stock changes only by transfers", pn,
                      s->name, path);
            goto out;
        }
        QType slot_type = kind->type;
        slot_type.scale = s->scale;
        qtype_conv_plan(&kind->type, &slot_type, &ts[i]);
        qtype_conv_plan(&slot_type, &kind->type, &tc[i]);
        bind[i] = c;
    }

    for (uint32_t i = 0; i < k->ntransfers; i++) {
        const QkTransfer *t = &k->transfers[i];
        uint32_t ends[2] = { t->from, t->to };
        for (int e = 0; e < 2; e++)
            if (ends[e] != QK_NONE && (ends[e] >= k->nslots || !is_stock_role(k->slots[ends[e]].role))) {
                st = fail(w, QW_EKERNEL, "%s: transfer '%s' of kernel '%s' has an endpoint that is not a stock or link",
                          pn, t->name, k->name);
                goto out;
            }
        if (t->from == QK_NONE && t->to == QK_NONE) {
            st = fail(w, QW_EKERNEL, "%s: transfer '%s' has no endpoint", pn, t->name);
            goto out;
        }
        if (t->conserved && (t->from == QK_NONE || t->to == QK_NONE)) {
            st = fail(w, QW_EKERNEL, "%s: transfer '%s' is declared conserved but has an open endpoint", pn, t->name);
            goto out;
        }
        if (t->from != QK_NONE && t->to != QK_NONE && w->cells[bind[t->from]].kind != w->cells[bind[t->to]].kind) {
            st = fail(w, QW_ETYPE, "%s: transfer '%s' joins '%s' and '%s', which are different kinds", pn, t->name,
                      w->cells[bind[t->from]].path, w->cells[bind[t->to]].path);
            goto out;
        }
    }

    p->k = k;
    p->bind = bind;
    p->to_slot = ts;
    p->to_cell = tc;
    bind = NULL;
    ts = tc = NULL;
    p->predicted = false;

    if (k->create) {
        QkCall call = { w, pid, M_CREATE, 0, { 0 } };
        if (k->create(&w->host, &call, &p->inst) != QK_OK || call.err) {
            st = fail(w, QW_EKERNEL, "%s: kernel '%s' failed to start: %s", pn, k->name, call.msg);
            /* the kernel did not start, so it is not attached */
            free(p->bind);
            free(p->to_slot);
            free(p->to_cell);
            p->bind = NULL;
            p->to_slot = p->to_cell = NULL;
            p->inst = NULL;
            p->k = NULL;
            goto out;
        }
    }
    if (k->flags & QK_NARROW) {
        if (!grow((void **) &w->nctx, &w->capnctx, w->nnctx + 1, sizeof *w->nctx)) return QW_ENOMEM;
        NarrowCtx *nc = malloc(sizeof *nc);
        if (!nc) return QW_ENOMEM;
        *nc = (NarrowCtx) { w, pid };
        w->nctx[w->nnctx++] = nc;
        if (qn_custom(w->net, p->bind, k->nslots, narrow_trampoline, nc) != QN_OK) return QW_ENOMEM;
    }
out:
    free(bind);
    free(ts);
    free(tc);
    return st;
}

/* --- running ------------------------------------------------------------------------- */

static QwStatus settle(QWorld *w)
{
    QnStatus s = qn_propagate(w->net);
    if (w->narrow_failed) {
        w->narrow_failed = false;
        return fail(w, QW_EKERNEL, "%s", w->narrow_msg);
    }
    if (s == QN_CONTRADICTION) {
        QnCell c = 0;
        qn_contradicted(w->net, &c);
        return fail(w, QW_ECONTRADICTION, "contradiction at '%s'", qw_cell_path(w, c));
    }
    return QW_OK;
}

static bool evolving(const Phen *p) { return p->k && (p->k->flags & QK_EVOLVE); }

/* Every input inside its valid range, in the slot's scale */
static QwStatus check_valid(QWorld *w, Phen *p)
{
    for (uint32_t i = 0; i < p->k->nslots; i++) {
        const QkSlot *s = &p->k->slots[i];
        if (s->role == QK_LINK) continue;
        QInterval v = qi_conv(qn_get(w->net, p->bind[i]), &p->to_slot[i]);
        if (!qi_subset(v, s->valid)) {
            char a[64], b[64];
            q_format_decimal(v.lo, s->scale, a, sizeof a);
            q_format_decimal(v.hi, s->scale, b, sizeof b);
            return fail(w, QW_EINAPPLICABLE, "%s: kernel '%s' is not valid for '%s' in [%s, %s]", p->name, p->k->name,
                        w->cells[p->bind[i]].path, a, b);
        }
    }
    return QW_OK;
}

static QwStatus kernel_failed(QWorld *w, Phen *p, const char *what, const QkCall *call, QkResult r)
{
    if (r == QK_INAPPLICABLE)
        return fail(w, QW_EINAPPLICABLE, "%s: kernel '%s' is not applicable (%s)", p->name, p->k->name, what);
    return fail(w, QW_EKERNEL, "%s: kernel '%s' failed in %s: %s", p->name, p->k->name, what,
                call->err ? call->msg : "kernel error");
}

static QwStatus predict(QWorld *w, QwPhen id)
{
    Phen *p = &w->phens[id];
    QwStatus st = check_valid(w, p);
    if (st) return st;
    p->nas = 0;
    QkCall call = { w, id, M_PREDICT, 0, { 0 } };
    QkPrediction pr = { QK_NO_TIME, QK_NO_TIME, QK_NO_TIME };
    QkResult r = p->k->predict(p->inst, &call, &pr);
    p->predicts++;
    if (r != QK_OK || call.err) return kernel_failed(w, p, "predict", &call, r);
    if (pr.horizon <= w->now)
        return fail(w, QW_EKERNEL, "%s: kernel '%s' predicted a horizon that is not after now", p->name, p->k->name);
    if (pr.event_lo != QK_NO_TIME && (pr.event_lo <= w->now || pr.event_hi < pr.event_lo))
        return fail(w, QW_EKERNEL, "%s: kernel '%s' predicted an invalid event time", p->name, p->k->name);
    p->pred = pr;
    p->predicted = true;
    return QW_OK;
}

/* After a boundary or an input: drop each prediction that has expired or
 * whose assumptions no longer hold. */
static void invalidate(QWorld *w)
{
    for (size_t i = 0; i < w->nphens; i++) {
        Phen *p = &w->phens[i];
        if (!evolving(p) || !p->predicted) continue;
        if (w->now >= p->pred.horizon || w->now >= p->pred.event_lo) {
            p->predicted = false;
            continue;
        }
        for (size_t j = 0; j < p->nas; j++)
            if (!qi_subset(qn_get(w->net, p->as[j].cell), p->as[j].range)) {
                p->predicted = false;
                break;
            }
    }
}

static QwStatus commit(QWorld *w)
{
    for (size_t c = 0; c < w->ncells; c++) {
        if (w->written[c]) {
            qn_stage(w->net, (QnCell) c, qi_point(w->wval[c]));
            w->written[c] = false;
        }
        if (w->delta[c]) {
            int64_t v;
            QInterval base = qn_base(w->net, (QnCell) c);
            if (qp_add(base.lo, w->delta[c], &v) != Q_OK)
                return fail(w, QW_ESTOCK, "stock '%s' out of range", w->cells[c].path);
            qn_stage(w->net, (QnCell) c, qi_point(v));
            w->delta[c] = 0;
        }
    }
    return QW_OK;
}

QwStatus qw_run(QWorld *w, int64_t until)
{
    if (until < w->now) return fail(w, QW_ETIME, "cannot run back to %lld from %lld", (long long) until, (long long) w->now);
    QwStatus st = settle(w);
    if (st) return st;
    while (w->now < until) {
        int64_t b = until;
        for (size_t i = 0; i < w->nphens; i++) {
            Phen *p = &w->phens[i];
            if (!evolving(p)) continue;
            if (!p->predicted && (st = predict(w, (QwPhen) i))) return st;
            if (p->pred.horizon < b) b = p->pred.horizon;
            if (p->pred.event_lo < b) b = p->pred.event_lo;
        }

        for (size_t i = 0; i < w->nphens; i++) {
            Phen *p = &w->phens[i];
            if (!evolving(p)) continue;
            if ((st = check_valid(w, p))) return st;
            QkCall call = { w, (QwPhen) i, M_ADVANCE, 0, { 0 } };
            QkResult r = p->k->advance(p->inst, &call, b);
            p->events++;
            if (r != QK_OK || call.err) {
                for (size_t c = 0; c < w->ncells; c++) {
                    w->delta[c] = 0;
                    w->written[c] = false;
                }
                return kernel_failed(w, p, "advance", &call, r);
            }
        }
        if ((st = commit(w))) return st;
        w->now = b;
        w->boundaries++;
        qn_begin_epoch(w->net);
        if ((st = settle(w))) return st;
        invalidate(w);
    }
    return QW_OK;
}

QwStatus qw_input(QWorld *w, const char *path, QInterval v)
{
    QnCell c;
    if (qw_find(w, path, &c) != QW_OK) return fail(w, QW_ENAME, "no cell '%s'", path);
    if (w->kinds[w->cells[c].kind].conserved)
        return fail(w, QW_ESTOCK, "'%s' is a stock; a stock changes only by transfers", path);
    if (!qi_valid(v)) return fail(w, QW_ETYPE, "invalid interval for '%s'", path);
    qn_stage(w->net, c, v);
    qn_begin_epoch(w->net);
    QwStatus st = settle(w);
    invalidate(w);
    return st;
}

QInterval qw_get(const QWorld *w, const char *path)
{
    QnCell c;
    if (qw_find(w, path, &c) != QW_OK) return qi_top();
    return qn_get(w->net, c);
}

uint64_t qw_predicts(const QWorld *w, QwPhen p) { return p < w->nphens ? w->phens[p].predicts : 0; }

static uint64_t fnv(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint64_t qw_hash(const QWorld *w)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    h = fnv(h, (uint64_t) w->now);
    h = fnv(h, qn_epoch(w->net));
    for (size_t c = 0; c < w->ncells; c++) {
        QInterval b = qn_base(w->net, (QnCell) c);
        h = fnv(h, (uint64_t) b.lo);
        h = fnv(h, (uint64_t) b.hi);
    }
    return h;
}
