#include "quad/world.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- storage ------------------------------------------------------------------- */

#define NO_MODEL (-1)

typedef struct { char *name; QType type; bool conserved; char **atoms; unsigned natoms; } Kind;   /* natoms 0: a quantity */
typedef struct {
    char     *path;
    uint64_t  hash;        /* of the path, for qw_hash */
    QwKind    kind;
    QwPhen    owner;
    int       model;
    QInterval init;        /* the declared base; an instance's cell returns to it at an end */
    bool      host_only;   /* "<key>.alive": only the host writes it */
    bool      derived;     /* a law gives its value: not state */
} CellInfo;
typedef struct { char *path; QnCell cell; } Alias;               /* qw_alias */
typedef struct { char *port, *target; } Port;                   /* qw_connect */
typedef struct { char *port; int model; char *target; } PortMap;   /* qw_port */
typedef struct { uint32_t slot; QnCell cell; QInterval range; bool explicit_; } Assume;

/* A kernel attached to a phenomenon, or to one model of it. */
typedef struct {
    const QkKernel *k;
    QkInstance     *inst;
    QnCell         *bind;       /* for each slot */
    QConv          *to_slot;    /* cell scale to slot scale */
    QConv          *to_cell;    /* slot scale to cell scale */
    bool            via_model;  /* a slot resolves through a port that depends on a model */
    bool            predicted;
    QkPrediction    pred;
    Assume         *as;
    size_t          nas, capas;
    uint64_t        events;
} Att;

typedef struct { char *name; QnGroup group; Att att; } Model;
typedef struct { char *name; int from, to; const QkKernel *map; } Transition;   /* qw_transition */

typedef struct {
    char    *name;
    Port    *ports;
    size_t   nports, capports;
    PortMap *pmaps;
    size_t   npmaps, cappmaps;
    Model   *models;
    size_t   nmodels, capmodels;
    int      active;            /* a model index, or NO_MODEL if the phenomenon has no models */
    Transition *trans;
    size_t   ntrans, captrans;
    Att      base;
    uint64_t predicts;
    uint64_t ident;             /* a hash of the name: the identity for random streams (ADR 0013) */
    QnCell  *cells;             /* the cells that the phenomenon owns */
    size_t   ncells, capcells;

    /* instances (ADR 0013) */
    bool            instance;
    bool            alive;      /* always true for a phenomenon that is not an instance */
    QnGroup         group;      /* the instance's propagators outside its models */
    QnCell          alive_cell;
    bool            has_cond;
    QnCell          cond;
    const QkKernel *start_map, *end_map;
    uint64_t        flipped;    /* the lifecycle step in which the instance last changed */
    uint64_t        mark;       /* scratch for one round of the lifecycle step */
} Phen;

typedef struct { QWorld *w; QwPhen id; int model; } NarrowCtx;

enum { M_CREATE, M_PREDICT, M_ADVANCE, M_NARROW, M_MAP };

struct QkCall {
    QWorld *w;
    QwPhen  id;
    Att    *att;
    int     mode;
    int     err;
    char    msg[256];
};

struct QWorld {
    QNet      *net;
    uint64_t   seed;
    int8_t     tick;

    char     **bases;
    size_t     nbases, capbases;
    Kind      *kinds;
    size_t     nkinds, capkinds;
    CellInfo  *cells;
    size_t     ncells, capcells;
    Alias     *aliases;
    size_t     naliases, capaliases;
    Phen      *phens;
    size_t     nphens, capphens;
    NarrowCtx **nctx;
    size_t     nnctx, capnctx;

    int64_t    now;
    uint64_t   boundaries, advances, switches, starts, ends;

    QwKind     boolean;     /* the built-in kind */
    QnGroup    build_group; /* the group of new propagators outside a model: 0, or an instance's */
    QwPhen    *conds;       /* the instances that have an existence condition */
    size_t     nconds, capconds;
    uint64_t   life_step, life_round;

    /* staging for one boundary or switch, one entry for each cell */
    int64_t   *delta, *wval;
    bool      *written;
    bool      *touched;     /* the cell is in the list `changed` */
    QnCell    *changed;     /* the cells that a write or a transfer touched */
    size_t     nchanged;
    size_t     capstage;

    /* generated kernels that the world frees */
    struct { QkKernel *k; void (*free_kernel)(QkKernel *); } *adopted;
    size_t     nadopted, capadopted;

    /* the change log */
    bool       logging;
    QwChange  *log_entries;
    size_t     nlog, caplog;

    bool       narrow_failed;
    char       narrow_msg[300];
    char       err[512];
    char       log[256];
    QkHost     host;
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
    case QW_ELIFE:          return "instance in the wrong state";
    case QW_ESNAPSHOT:      return "snapshot does not fit";
    case QW_ENOMEM:         return "out of memory";
    }
    return "unknown status";
}

const char *qw_error(const QWorld *w) { return w->err; }
QNet *qw_net(QWorld *w) { return w->net; }
int64_t qw_now(const QWorld *w) { return w->now; }
uint64_t qw_boundaries(const QWorld *w) { return w->boundaries; }
uint64_t qw_advances(const QWorld *w) { return w->advances; }
uint64_t qw_switches(const QWorld *w) { return w->switches; }
uint64_t qw_starts(const QWorld *w) { return w->starts; }
uint64_t qw_ends(const QWorld *w) { return w->ends; }
QwKind qw_boolean(const QWorld *w) { return w->boolean; }

/* Adds cell c to the list of touched cells, once. The list has room for
 * every cell, so this cannot fail. */
static void touch(QWorld *w, QnCell c)
{
    if (w->touched[c]) return;
    w->touched[c] = true;
    w->changed[w->nchanged++] = c;
}

/* Clears the staging of every touched cell. Untouched cells are already
 * clear, so the cost does not grow with the number of cells. */
static void clear_staging(QWorld *w)
{
    for (size_t i = 0; i < w->nchanged; i++) {
        QnCell c = w->changed[i];
        w->delta[c] = 0;
        w->written[c] = false;
        w->touched[c] = false;
    }
    w->nchanged = 0;
}

static void free_att(Att *a)
{
    if (a->k && a->k->destroy && a->inst) a->k->destroy(a->inst);
    free(a->bind);
    free(a->to_slot);
    free(a->to_cell);
    free(a->as);
    memset(a, 0, sizeof *a);
}

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
    if (slot < c->att->k->nslots) return true;
    call_err(c, QK_HOST_EINDEX, "slot index %u out of range", slot);
    return false;
}

static void set_assume(Att *a, uint32_t slot, QnCell cell, QInterval range, bool explicit_)
{
    for (size_t i = 0; i < a->nas; i++)
        if (a->as[i].slot == slot) {
            if (explicit_ || !a->as[i].explicit_) {
                a->as[i].range = range;
                a->as[i].explicit_ = explicit_;
            }
            return;
        }
    if (!grow((void **) &a->as, &a->capas, a->nas + 1, sizeof *a->as)) return;
    a->as[a->nas++] = (Assume) { slot, cell, range, explicit_ };
}

static uint64_t word(int64_t v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    return u;
}

static bool set_cell(const QWorld *w, QnCell c) { return qn_is_set(w->net, c); }

static QInterval h_read(QkCall *c, uint32_t slot)
{
    if (!slot_ok(c, slot)) return qi_top();
    Att *a = c->att;
    const QkSlot *s = &a->k->slots[slot];
    if (set_cell(c->w, a->bind[slot])) {
        call_err(c, QK_HOST_EROLE, "slot '%s' is a set; read it with read_set", s->name);
        return qi_top();
    }
    if (s->role == QK_LINK) {
        call_err(c, QK_HOST_EROLE, "slot '%s' is a link, which cannot be read", s->name);
        return qi_top();
    }
    QInterval v = qn_get(c->w->net, a->bind[slot]);
    if (c->mode == M_PREDICT && s->role == QK_IN) set_assume(a, slot, a->bind[slot], v, false);
    return qi_conv(v, &a->to_slot[slot]);
}

static bool changing(int mode) { return mode == M_ADVANCE || mode == M_MAP; }

static uint64_t h_read_set(QkCall *c, uint32_t slot)
{
    if (!slot_ok(c, slot)) return 0;
    Att *a = c->att;
    const QkSlot *s = &a->k->slots[slot];
    QnCell cell = a->bind[slot];
    if (!set_cell(c->w, cell)) {
        call_err(c, QK_HOST_EROLE, "slot '%s' is not a set", s->name);
        return 0;
    }
    if (c->mode == M_PREDICT && s->role == QK_IN) set_assume(a, slot, cell, qn_get(c->w->net, cell), false);
    return qn_get_set(c->w->net, cell);
}

static int h_atom(QkCall *c, uint32_t slot, const char *name)
{
    if (!slot_ok(c, slot)) return -1;
    QnCell cell = c->att->bind[slot];
    if (!set_cell(c->w, cell)) {
        call_err(c, QK_HOST_EROLE, "slot '%s' is not a set", c->att->k->slots[slot].name);
        return -1;
    }
    const Kind *k = &c->w->kinds[c->w->cells[cell].kind];
    for (unsigned i = 0; i < k->natoms; i++)
        if (!strcmp(k->atoms[i], name)) return (int) i;
    return -1;
}

static void touch(QWorld *w, QnCell c);

static int h_write_atom(QkCall *c, uint32_t slot, uint32_t atom)
{
    if (!slot_ok(c, slot)) return QK_HOST_EINDEX;
    Att *a = c->att;
    const QkSlot *s = &a->k->slots[slot];
    QnCell cell = a->bind[slot];
    if (!changing(c->mode)) return call_err(c, QK_HOST_EMODE, "write to slot '%s' outside advance", s->name);
    if (s->role != QK_OUT && s->role != QK_STATE)
        return call_err(c, QK_HOST_EROLE, "write to slot '%s', which is not an output or a state", s->name);
    if (!set_cell(c->w, cell)) return call_err(c, QK_HOST_EROLE, "slot '%s' is not a set", s->name);
    if (atom >= c->w->kinds[c->w->cells[cell].kind].natoms)
        return call_err(c, QK_HOST_EVALUE, "atom %u out of range for slot '%s'", atom, s->name);
    c->w->wval[cell] = atom;
    c->w->written[cell] = true;
    touch(c->w, cell);
    return QK_HOST_OK;
}

static int h_write(QkCall *c, uint32_t slot, int64_t value)
{
    if (!slot_ok(c, slot)) return QK_HOST_EINDEX;
    Att *a = c->att;
    const QkSlot *s = &a->k->slots[slot];
    if (set_cell(c->w, a->bind[slot]))
        return call_err(c, QK_HOST_EROLE, "slot '%s' is a set; write it with write_atom", s->name);
    if (!changing(c->mode)) return call_err(c, QK_HOST_EMODE, "write to slot '%s' outside advance", s->name);
    if (s->role != QK_OUT && s->role != QK_STATE)
        return call_err(c, QK_HOST_EROLE, "write to slot '%s', which is not an output or a state", s->name);
    int64_t v;
    if (qp_conv(value, &a->to_cell[slot], &v) != Q_OK)
        return call_err(c, QK_HOST_EVALUE, "value for slot '%s' out of range", s->name);
    QnCell cell = a->bind[slot];
    c->w->wval[cell] = v;
    c->w->written[cell] = true;
    touch(c->w, cell);
    return QK_HOST_OK;
}

static int h_transfer(QkCall *c, uint32_t t, int64_t amount)
{
    Att *a = c->att;
    if (t >= a->k->ntransfers) return call_err(c, QK_HOST_EINDEX, "transfer index %u out of range", t);
    const QkTransfer *tr = &a->k->transfers[t];
    if (!changing(c->mode)) return call_err(c, QK_HOST_EMODE, "transfer '%s' outside advance", tr->name);
    QWorld *w = c->w;
    if (tr->from != QK_NONE) {
        QnCell f = a->bind[tr->from];
        touch(w, f);
        if (qp_sub(w->delta[f], amount, &w->delta[f]) != Q_OK)
            return call_err(c, QK_HOST_EVALUE, "transfer '%s' out of range", tr->name);
    }
    if (tr->to != QK_NONE) {
        QnCell d = a->bind[tr->to];
        touch(w, d);
        if (qp_add(w->delta[d], amount, &w->delta[d]) != Q_OK)
            return call_err(c, QK_HOST_EVALUE, "transfer '%s' out of range", tr->name);
    }
    return QK_HOST_OK;
}

static int h_tell(QkCall *c, uint32_t slot, QInterval range)
{
    if (!slot_ok(c, slot)) return QK_HOST_EINDEX;
    Att *a = c->att;
    const QkSlot *s = &a->k->slots[slot];
    if (c->mode != M_NARROW) return call_err(c, QK_HOST_EMODE, "tell on slot '%s' outside narrow", s->name);
    if (s->role == QK_LINK) return call_err(c, QK_HOST_EROLE, "tell on link slot '%s'", s->name);
    if (!qi_valid(range)) return call_err(c, QK_HOST_EVALUE, "invalid interval for slot '%s'", s->name);
    qn_tell(c->w->net, a->bind[slot], qi_conv(range, &a->to_cell[slot]));
    return QK_HOST_OK;
}

static int h_assume(QkCall *c, uint32_t slot, QInterval range)
{
    if (!slot_ok(c, slot)) return QK_HOST_EINDEX;
    Att *a = c->att;
    const QkSlot *s = &a->k->slots[slot];
    if (c->mode != M_PREDICT) return call_err(c, QK_HOST_EMODE, "assume on slot '%s' outside predict", s->name);
    if (s->role != QK_IN) return call_err(c, QK_HOST_EROLE, "assume on slot '%s', which is not an input", s->name);
    if (!qi_valid(range)) return call_err(c, QK_HOST_EVALUE, "invalid interval for slot '%s'", s->name);
    set_assume(a, slot, a->bind[slot], qi_conv(range, &a->to_cell[slot]), true);
    return QK_HOST_OK;
}

/* An advance draws with its event count. A map draws with the epoch (ADR 0010).
 * The identity is a hash of the phenomenon's name, never its index, so the
 * draws do not depend on the order of creation (ADR 0013). */
static QRCursor h_random(QkCall *c, const char *stream)
{
    QRStream s = qr_stream(c->w->seed, call_phen(c)->ident, stream);
    return qr_cursor(&s, c->mode == M_MAP ? qn_epoch(c->w->net) : c->att->events);
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
                         h_assume,       h_random,        h_now,  h_tick_scale, h_log,
                         h_atom,         h_read_set,      h_write_atom };
    static const char *const atoms[2] = { "false", "true" };
    if (qw_enum_kind(w, "boolean", atoms, 2, &w->boolean) != QW_OK) {
        qw_free(w);
        return NULL;
    }
    return w;
}

void qw_free(QWorld *w)
{
    if (!w) return;
    for (size_t i = 0; i < w->nphens; i++) {
        Phen *p = &w->phens[i];
        free_att(&p->base);
        for (size_t j = 0; j < p->nmodels; j++) {
            free_att(&p->models[j].att);
            free(p->models[j].name);
        }
        for (size_t j = 0; j < p->nports; j++) {
            free(p->ports[j].port);
            free(p->ports[j].target);
        }
        for (size_t j = 0; j < p->npmaps; j++) {
            free(p->pmaps[j].port);
            free(p->pmaps[j].target);
        }
        for (size_t j = 0; j < p->ntrans; j++) free(p->trans[j].name);
        free(p->trans);
        free(p->models);
        free(p->ports);
        free(p->pmaps);
        free(p->cells);
        free(p->name);
    }
    for (size_t i = 0; i < w->nbases; i++) free(w->bases[i]);
    for (size_t i = 0; i < w->nkinds; i++) {
        for (unsigned j = 0; j < w->kinds[i].natoms; j++) free(w->kinds[i].atoms[j]);
        free(w->kinds[i].atoms);
        free(w->kinds[i].name);
    }
    for (size_t i = 0; i < w->ncells; i++) free(w->cells[i].path);
    for (size_t i = 0; i < w->naliases; i++) free(w->aliases[i].path);
    for (size_t i = 0; i < w->nnctx; i++) free(w->nctx[i]);
    free(w->aliases);
    free(w->bases);
    free(w->kinds);
    free(w->cells);
    free(w->phens);
    free(w->conds);
    free(w->nctx);
    free(w->delta);
    free(w->wval);
    free(w->written);
    free(w->touched);
    free(w->changed);
    free(w->log_entries);
    qn_free(w->net);
    for (size_t i = 0; i < w->nadopted; i++) w->adopted[i].free_kernel(w->adopted[i].k);
    free(w->adopted);
    free(w);
}

QwStatus qw_adopt_kernel(QWorld *w, QkKernel *k, void (*free_kernel)(QkKernel *))
{
    if (!k || !free_kernel) return fail(w, QW_EKERNEL, "qw_adopt_kernel needs a kernel and its free function");
    if (!grow((void **) &w->adopted, &w->capadopted, w->nadopted + 1, sizeof *w->adopted)) return fail(w, QW_ENOMEM, "out of memory");
    w->adopted[w->nadopted].k = k;
    w->adopted[w->nadopted++].free_kernel = free_kernel;
    return QW_OK;
}

static bool valid_name(const char *s)
{
    if (!*s) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_'))
            return false;
    return true;
}

/* The name of a phenomenon. An instance's key has the form
 * "rule(participant,participant)", so a name can also contain '(', ')' and
 * ','. The world does not interpret the key. */
static bool valid_key(const char *s)
{
    if (!*s) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' ||
              *s == '(' || *s == ')' || *s == ','))
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
    k->atoms = NULL;
    k->natoms = 0;
    *out = (QwKind) w->nkinds++;
    return QW_OK;
}

QwStatus qw_anonymous_kind(QWorld *w, const char *dim, int scale, QwKind *out)
{
    QDim d;
    if (parse_dim(w, dim, &d) != Q_OK) return fail(w, QW_ETYPE, "anonymous kind: cannot read dimension '%s'", dim);
    if (scale < INT8_MIN || scale > INT8_MAX) return fail(w, QW_ETYPE, "anonymous kind of '%s': scale out of range", dim);
    for (size_t i = 0; i < w->nkinds; i++) {
        const Kind *k = &w->kinds[i];
        if (!k->natoms && k->type.kind == 0 && k->type.scale == scale && qdim_eq(&k->type.dim, &d)) {
            *out = (QwKind) i;
            return QW_OK;
        }
    }
    if (!grow((void **) &w->kinds, &w->capkinds, w->nkinds + 1, sizeof *w->kinds)) return QW_ENOMEM;
    Kind *k = &w->kinds[w->nkinds];
    memset(k, 0, sizeof *k);
    char name[160];
    snprintf(name, sizeof name, "anonymous %s, scale %d", dim[0] ? dim : "1", scale);
    if (!(k->name = dupstr(name))) return QW_ENOMEM;
    k->type = qtype_make(0, d, (int8_t) scale);
    *out = (QwKind) w->nkinds++;
    return QW_OK;
}

QwStatus qw_enum_kind(QWorld *w, const char *name, const char *const *atoms, unsigned n, QwKind *out)
{
    if (!valid_name(name)) return fail(w, QW_ENAME, "invalid kind name '%s'", name);
    for (size_t i = 0; i < w->nkinds; i++)
        if (!strcmp(w->kinds[i].name, name)) return fail(w, QW_EDUP, "kind '%s' already exists", name);
    if (n < 1 || n > QN_MAXATOMS) return fail(w, QW_ETYPE, "kind '%s' needs 1 to %d atoms", name, QN_MAXATOMS);
    for (unsigned i = 0; i < n; i++) {
        if (!valid_name(atoms[i])) return fail(w, QW_ENAME, "kind '%s': invalid atom name '%s'", name, atoms[i]);
        for (unsigned j = 0; j < i; j++)
            if (!strcmp(atoms[i], atoms[j])) return fail(w, QW_EDUP, "kind '%s': atom '%s' twice", name, atoms[i]);
    }
    if (!grow((void **) &w->kinds, &w->capkinds, w->nkinds + 1, sizeof *w->kinds)) return QW_ENOMEM;
    Kind *k = &w->kinds[w->nkinds];
    memset(k, 0, sizeof *k);
    if (!(k->name = dupstr(name)) || !(k->atoms = calloc(n, sizeof *k->atoms))) {
        free(k->name);
        return QW_ENOMEM;
    }
    for (unsigned i = 0; i < n; i++)
        if (!(k->atoms[i] = dupstr(atoms[i]))) {
            for (unsigned j = 0; j < i; j++) free(k->atoms[j]);
            free(k->atoms);
            free(k->name);
            return QW_ENOMEM;
        }
    k->natoms = n;
    k->type = qtype_make((uint32_t) w->nkinds + 1, qdim_none(), 0);
    *out = (QwKind) w->nkinds++;
    return QW_OK;
}

int qw_atom(const QWorld *w, QwKind kind, const char *name)
{
    if (kind >= w->nkinds) return -1;
    const Kind *k = &w->kinds[kind];
    for (unsigned i = 0; i < k->natoms; i++)
        if (!strcmp(k->atoms[i], name)) return (int) i;
    return -1;
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

static int find_model(const Phen *p, const char *name, size_t len)
{
    for (size_t i = 0; i < p->nmodels; i++)
        if (strlen(p->models[i].name) == len && !memcmp(p->models[i].name, name, len)) return (int) i;
    return NO_MODEL;
}

static QwStatus new_phen(QWorld *w, const char *name, QwPhen *out)
{
    if (!valid_key(name)) return fail(w, QW_ENAME, "invalid phenomenon name '%s'", name);
    QwPhen dup;
    if (find_phen(w, name, strlen(name), &dup)) return fail(w, QW_EDUP, "phenomenon '%s' already exists", name);
    uint64_t ident = qr_name_hash(name, strlen(name));
    for (size_t i = 0; i < w->nphens; i++)
        if (w->phens[i].ident == ident)
            return fail(w, QW_EDUP, "the names '%s' and '%s' have the same hash, so their random streams would be equal",
                        name, w->phens[i].name);
    if (!grow((void **) &w->phens, &w->capphens, w->nphens + 1, sizeof *w->phens)) return QW_ENOMEM;
    Phen *p = &w->phens[w->nphens];
    memset(p, 0, sizeof *p);
    p->active = NO_MODEL;
    p->alive = true;
    p->ident = ident;
    if (!(p->name = dupstr(name))) return QW_ENOMEM;
    *out = (QwPhen) w->nphens++;
    return QW_OK;
}

QwStatus qw_phenomenon(QWorld *w, const char *name, QwPhen *out) { return new_phen(w, name, out); }

QwStatus qw_find_phenomenon(const QWorld *w, const char *name, QwPhen *out)
{
    return find_phen(w, name, strlen(name), out) ? QW_OK : QW_ENAME;
}

QwStatus qw_model(QWorld *w, QwPhen pid, const char *name)
{
    if (pid >= w->nphens) return fail(w, QW_ENAME, "no such phenomenon");
    Phen *p = &w->phens[pid];
    if (!valid_name(name)) return fail(w, QW_ENAME, "%s: invalid model name '%s'", p->name, name);
    if (find_model(p, name, strlen(name)) != NO_MODEL)
        return fail(w, QW_EDUP, "%s: model '%s' already exists", p->name, name);
    char prefix[512];
    snprintf(prefix, sizeof prefix, "%s.%s", p->name, name);
    for (size_t i = 0; i < w->ncells; i++) {
        size_t n = strlen(prefix);
        if (!strncmp(w->cells[i].path, prefix, n) && (w->cells[i].path[n] == '.' || !w->cells[i].path[n]))
            return fail(w, QW_EDUP, "%s: model '%s' would contain the existing cell '%s'", p->name, name,
                        w->cells[i].path);
    }
    if (!grow((void **) &p->models, &p->capmodels, p->nmodels + 1, sizeof *p->models)) return QW_ENOMEM;
    Model *m = &p->models[p->nmodels];
    memset(m, 0, sizeof *m);
    if (!(m->name = dupstr(name))) return QW_ENOMEM;
    bool first = p->nmodels == 0;
    if (qn_new_group(w->net, first && p->alive, &m->group) != QN_OK) return QW_ENOMEM;
    if (first) p->active = 0;
    p->nmodels++;
    return QW_OK;
}

QwStatus qw_model_begin(QWorld *w, QwPhen pid, const char *model)
{
    if (pid >= w->nphens) return fail(w, QW_ENAME, "no such phenomenon");
    Phen *p = &w->phens[pid];
    int m = find_model(p, model, strlen(model));
    if (m == NO_MODEL) return fail(w, QW_ENAME, "%s: no model '%s'", p->name, model);
    qn_set_group(w->net, p->models[m].group);
    return QW_OK;
}

void qw_model_end(QWorld *w) { qn_set_group(w->net, w->build_group); }

const char *qw_active_model(const QWorld *w, QwPhen pid)
{
    if (pid >= w->nphens || w->phens[pid].active == NO_MODEL) return NULL;
    return w->phens[pid].models[w->phens[pid].active].name;
}

QwStatus qw_find(const QWorld *w, const char *path, QnCell *out)
{
    for (size_t i = 0; i < w->ncells; i++)
        if (!strcmp(w->cells[i].path, path)) {
            *out = (QnCell) i;
            return QW_OK;
        }
    for (size_t i = 0; i < w->naliases; i++)
        if (!strcmp(w->aliases[i].path, path)) {
            *out = w->aliases[i].cell;
            return QW_OK;
        }
    return QW_ENAME;
}

const char *qw_cell_path(const QWorld *w, QnCell c) { return c < w->ncells ? w->cells[c].path : "?"; }

static QwStatus add_cell(QWorld *w, const char *path, QwKind kind, bool is_set, QInterval base, uint64_t mask,
                         bool host_only, QnCell *out)
{
    const char *dot = strchr(path, '.');
    QwPhen owner;
    if (!dot || !dot[1]) return fail(w, QW_ENAME, "cell path '%s' has no phenomenon prefix", path);
    if (!find_phen(w, path, (size_t) (dot - path), &owner))
        return fail(w, QW_ENAME, "cell path '%s' names no phenomenon", path);
    if (!host_only && !strcmp(dot + 1, "alive"))
        return fail(w, QW_ENAME, "cell path '%s': the feature name 'alive' is reserved", path);
    QnCell dup;
    if (qw_find(w, path, &dup) == QW_OK) return fail(w, QW_EDUP, "cell '%s' already exists", path);
    if (kind >= w->nkinds) return fail(w, QW_ENAME, "cell '%s': no such kind", path);
    Phen *p = &w->phens[owner];
    const char *second = dot + 1, *dot2 = strchr(second, '.');
    int model = find_model(p, second, dot2 ? (size_t) (dot2 - second) : strlen(second));
    if (model != NO_MODEL && !dot2) return fail(w, QW_ENAME, "cell path '%s' is the name of a model", path);
    const Kind *k = &w->kinds[kind];
    if (is_set != (k->natoms > 0))
        return fail(w, QW_ETYPE, "cell '%s': kind '%s' %s", path, k->name,
                    is_set ? "is not an enumeration" : "is an enumeration; use qw_set_cell");
    if (k->conserved && (!qi_is_point(base) || base.lo == Q_NINF || base.hi == Q_INF))
        return fail(w, QW_ESTOCK, "stock '%s' needs an exact value", path);
    if (k->conserved && model != NO_MODEL && model != p->active && base.lo != 0)
        return fail(w, QW_ESTOCK, "stock '%s' belongs to an inactive model, so it must be zero", path);
    if (k->conserved && p->instance && base.lo != 0)
        return fail(w, QW_ESTOCK, "stock '%s' belongs to an instance, so it must be declared as zero", path);
    if (!grow((void **) &w->cells, &w->capcells, w->ncells + 1, sizeof *w->cells)) return QW_ENOMEM;
    if (!grow((void **) &p->cells, &p->capcells, p->ncells + 1, sizeof *p->cells)) return QW_ENOMEM;
    size_t need = w->ncells + 1;
    if (need > w->capstage) {
        size_t c1 = w->capstage, c2 = w->capstage, c3 = w->capstage, c4 = w->capstage, c5 = w->capstage;
        if (!grow((void **) &w->delta, &c1, need, sizeof *w->delta)) return QW_ENOMEM;
        if (!grow((void **) &w->wval, &c2, need, sizeof *w->wval)) return QW_ENOMEM;
        if (!grow((void **) &w->written, &c3, need, sizeof *w->written)) return QW_ENOMEM;
        if (!grow((void **) &w->touched, &c4, need, sizeof *w->touched)) return QW_ENOMEM;
        if (!grow((void **) &w->changed, &c5, need, sizeof *w->changed)) return QW_ENOMEM;
        w->capstage = c1;
    }
    QnCell c;
    QnStatus s = is_set ? qn_set_cell(w->net, k->type.kind, k->natoms, mask, &c) : qn_cell(w->net, &k->type, base, &c);
    if (s == QN_EVALUE) return fail(w, QW_ETYPE, "cell '%s': invalid value", path);
    if (s) return fail(w, QW_ENOMEM, "cell '%s': %s", path, qn_status_str(s));
    CellInfo *ci = &w->cells[w->ncells];
    if (!(ci->path = dupstr(path))) return QW_ENOMEM;
    ci->hash = qr_name_hash(path, strlen(path));
    ci->kind = kind;
    ci->owner = owner;
    ci->model = model;
    ci->init = qn_base(w->net, c);
    ci->host_only = host_only;
    ci->derived = false;
    p->cells[p->ncells++] = c;
    w->delta[c] = w->wval[c] = 0;
    w->written[c] = false;
    w->touched[c] = false;
    w->ncells++;
    if (out) *out = c;
    return QW_OK;
}

QwStatus qw_cell(QWorld *w, const char *path, QwKind kind, QInterval base, QnCell *out)
{
    return add_cell(w, path, kind, false, base, 0, false, out);
}

QwStatus qw_derived_cell(QWorld *w, const char *path, QwKind kind, QInterval base, QnCell *out)
{
    if (kind < w->nkinds && w->kinds[kind].conserved)
        return fail(w, QW_ESTOCK, "derived cell '%s': a stock is state, so it cannot be derived", path);
    QnCell c;
    QwStatus s = add_cell(w, path, kind, false, base, 0, false, &c);
    if (s) return s;
    w->cells[c].derived = true;
    if (out) *out = c;
    return QW_OK;
}

QwStatus qw_set_cell(QWorld *w, const char *path, QwKind kind, uint64_t base, QnCell *out)
{
    return add_cell(w, path, kind, true, qi_top(), base, false, out);
}

QwStatus qw_alias(QWorld *w, const char *path, const char *cell)
{
    QnCell c, dup;
    if (qw_find(w, cell, &c) != QW_OK) return fail(w, QW_ENAME, "alias '%s': no cell '%s'", path, cell);
    const char *dot = strchr(path, '.');
    QwPhen owner;
    if (!dot || !dot[1] || !find_phen(w, path, (size_t) (dot - path), &owner))
        return fail(w, QW_ENAME, "alias '%s' names no phenomenon", path);
    if (owner != w->cells[c].owner)
        return fail(w, QW_EOWNER, "alias '%s': the cell '%s' belongs to another phenomenon", path, w->cells[c].path);
    if (qw_find(w, path, &dup) == QW_OK) return fail(w, QW_EDUP, "alias '%s': the path already exists", path);
    if (!grow((void **) &w->aliases, &w->capaliases, w->naliases + 1, sizeof *w->aliases)) return QW_ENOMEM;
    if (!(w->aliases[w->naliases].path = dupstr(path))) return QW_ENOMEM;
    w->aliases[w->naliases++].cell = c;
    return QW_OK;
}

QwStatus qw_connect(QWorld *w, QwPhen pid, const char *port, const char *target)
{
    if (pid >= w->nphens) return fail(w, QW_ENAME, "no such phenomenon");
    Phen *p = &w->phens[pid];
    if (!valid_name(port)) return fail(w, QW_ENAME, "%s: invalid port name '%s'", p->name, port);
    if (p->base.k) return fail(w, QW_EKERNEL, "%s: connect ports before a kernel is attached", p->name);
    for (size_t i = 0; i < p->nports; i++)
        if (!strcmp(p->ports[i].port, port)) return fail(w, QW_EDUP, "%s: port '%s' already connected", p->name, port);
    if (!grow((void **) &p->ports, &p->capports, p->nports + 1, sizeof *p->ports)) return QW_ENOMEM;
    Port *pt = &p->ports[p->nports];
    if (!(pt->port = dupstr(port)) || !(pt->target = dupstr(target))) return QW_ENOMEM;
    p->nports++;
    return QW_OK;
}

QwStatus qw_port(QWorld *w, QwPhen pid, const char *port, const char *model, const char *target)
{
    if (pid >= w->nphens) return fail(w, QW_ENAME, "no such phenomenon");
    Phen *p = &w->phens[pid];
    if (!valid_name(port)) return fail(w, QW_ENAME, "%s: invalid port name '%s'", p->name, port);
    int m = find_model(p, model, strlen(model));
    if (m == NO_MODEL) return fail(w, QW_ENAME, "%s: no model '%s'", p->name, model);
    for (size_t i = 0; i < p->npmaps; i++)
        if (!strcmp(p->pmaps[i].port, port) && p->pmaps[i].model == m)
            return fail(w, QW_EDUP, "%s: port '%s' already mapped for model '%s'", p->name, port, model);
    if (!grow((void **) &p->pmaps, &p->cappmaps, p->npmaps + 1, sizeof *p->pmaps)) return QW_ENOMEM;
    PortMap *pm = &p->pmaps[p->npmaps];
    if (!(pm->port = dupstr(port)) || !(pm->target = dupstr(target))) return QW_ENOMEM;
    pm->model = m;
    p->npmaps++;
    return QW_OK;
}

/* The phenomenon that the target of a port names: the first component of
 * the target path. */
static bool port_phen(const QWorld *w, const char *target, QwPhen *out)
{
    const char *dot = strchr(target, '.');
    return find_phen(w, target, dot ? (size_t) (dot - target) : strlen(target), out);
}

/* "P.port.rest" becomes "<target>.rest" when P maps `port` for its active
 * model. Sets *via when a mapping applied. */
static void resolve_ports(const QWorld *w, const char *path, char *out, size_t len, bool *via)
{
    const char *dot = strchr(path, '.');
    QwPhen pid;
    if (dot && find_phen(w, path, (size_t) (dot - path), &pid)) {
        const Phen *p = &w->phens[pid];
        const char *port = dot + 1, *rest = strchr(port, '.');
        size_t plen = rest ? (size_t) (rest - port) : strlen(port);
        for (size_t i = 0; i < p->npmaps; i++) {
            const PortMap *pm = &p->pmaps[i];
            if (pm->model == p->active && strlen(pm->port) == plen && !memcmp(pm->port, port, plen)) {
                snprintf(out, len, "%s%s", pm->target, rest ? rest : "");
                *via = true;
                return;
            }
        }
    }
    snprintf(out, len, "%s", path);
}

/* The cell path for a slot of a kernel attached to phenomenon p, in model m
 * (or NO_MODEL), with extra connections first (used by map kernels). */
static void slot_path(const QWorld *w, const Phen *p, int m, const Port *extra, size_t nextra, const char *slot,
                      char *out, size_t len, bool *via)
{
    char raw[512];
    for (size_t pass = 0; pass < 2; pass++) {
        const Port *ports = pass == 0 ? extra : p->ports;
        size_t n = pass == 0 ? nextra : p->nports;
        for (size_t i = 0; i < n; i++) {
            size_t pl = strlen(ports[i].port);
            if (!strncmp(slot, ports[i].port, pl) && slot[pl] == '.') {
                snprintf(raw, sizeof raw, "%s.%s", ports[i].target, slot + pl + 1);
                resolve_ports(w, raw, out, len, via);
                return;
            }
        }
    }
    QnCell c;
    if (m != NO_MODEL) {
        snprintf(raw, sizeof raw, "%s.%s.%s", p->name, p->models[m].name, slot);
        if (qw_find(w, raw, &c) == QW_OK) {
            snprintf(out, len, "%s", raw);
            return;
        }
    }
    snprintf(raw, sizeof raw, "%s.%s", p->name, slot);
    resolve_ports(w, raw, out, len, via);
}

static bool is_stock_role(QkRole r) { return r == QK_STOCK || r == QK_LINK; }

static QwStatus check_contract(QWorld *w, const char *pn, const QkKernel *k)
{
    if (k->abi_version != QK_ABI_VERSION || k->size < sizeof(QkKernel))
        return fail(w, QW_EKERNEL, "%s: kernel '%s' has interface version %u, expected %u", pn, k->name,
                    k->abi_version, QK_ABI_VERSION);
    if (k->qr_version != QR_VERSION)
        return fail(w, QW_EKERNEL, "%s: kernel '%s' expects random version %u", pn, k->name, k->qr_version);
    if (k->flags & QK_RATES) return fail(w, QW_EKERNEL, "%s: kernel '%s': rate kernels are not supported yet", pn, k->name);
    if ((k->flags & QK_EVOLVE) && (!k->predict || !k->advance))
        return fail(w, QW_EKERNEL, "%s: kernel '%s' is an evolution kernel without predict or advance", pn, k->name);
    if ((k->flags & QK_NARROW) && !k->narrow)
        return fail(w, QW_EKERNEL, "%s: kernel '%s' is a narrowing kernel without narrow", pn, k->name);
    if ((k->flags & QK_MAP) && !k->map)
        return fail(w, QW_EKERNEL, "%s: kernel '%s' is a map kernel without map", pn, k->name);
    return QW_OK;
}

/* Resolves and checks every slot and transfer of kernel k for phenomenon pid
 * in model m. On success, the arrays are new and belong to the caller. */
static QwStatus bind_kernel(QWorld *w, QwPhen pid, int m, const QkKernel *k, const Port *extra, size_t nextra,
                            QnCell **bind_out, QConv **ts_out, QConv **tc_out, bool *via_out)
{
    const Phen *p = &w->phens[pid];
    const char *pn = p->name;
    size_t n = k->nslots ? k->nslots : 1;
    QnCell *bind = calloc(n, sizeof *bind);
    QConv *ts = calloc(n, sizeof *ts), *tc = calloc(n, sizeof *tc);
    QwStatus st = QW_OK;
    bool via_any = false;
    if (!bind || !ts || !tc) {
        st = QW_ENOMEM;
        goto out;
    }
    for (uint32_t i = 0; i < k->nslots; i++) {
        const QkSlot *s = &k->slots[i];
        char path[512];
        bool via = false;
        slot_path(w, p, m, extra, nextra, s->name, path, sizeof path, &via);
        via_any |= via;
        QnCell c;
        if (qw_find(w, path, &c) != QW_OK) {
            st = fail(w, QW_ENAME, "%s: slot '%s' of kernel '%s' needs the cell '%s', which does not exist", pn,
                      s->name, k->name, path);
            goto out;
        }
        const CellInfo *ci = &w->cells[c];
        const Kind *kind = &w->kinds[ci->kind];
        bool atoms_slot = !strcmp(s->dimension, "atoms");
        if (atoms_slot != (kind->natoms > 0)) {
            st = fail(w, QW_ETYPE, "%s: slot '%s' %s, but '%s' is %s", pn, s->name,
                      atoms_slot ? "holds atoms" : "holds a quantity", path,
                      kind->natoms ? "a set cell" : "a quantity cell");
            goto out;
        }
        if (atoms_slot) {
            if (s->role == QK_STOCK || s->role == QK_LINK) {
                st = fail(w, QW_ETYPE, "%s: slot '%s' holds atoms, which cannot be a stock", pn, s->name);
                goto out;
            }
            if ((s->role == QK_STATE || s->role == QK_OUT) && ci->owner != pid) {
                st = fail(w, QW_EOWNER, "%s: slot '%s' would write '%s', which belongs to %s", pn, s->name, path,
                          w->phens[ci->owner].name);
                goto out;
            }
            if ((s->role == QK_STATE || s->role == QK_OUT) && ci->host_only) {
                st = fail(w, QW_EOWNER, "%s: slot '%s' would write '%s', which only the host writes", pn, s->name, path);
                goto out;
            }
            bind[i] = c;
            continue;
        }
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
        if ((s->role == QK_STATE || s->role == QK_OUT) && ci->derived) {
            st = fail(w, QW_EOWNER, "%s: slot '%s' would write '%s', which is derived: a law gives its value", pn, s->name,
                      path);
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
    if ((k->flags & QK_NARROW) && via_any) {
        st = fail(w, QW_EKERNEL, "%s: narrowing kernel '%s' reads through a port that depends on a model; not supported yet",
                  pn, k->name);
        goto out;
    }
    *bind_out = bind;
    *ts_out = ts;
    *tc_out = tc;
    *via_out = via_any;
    return QW_OK;
out:
    free(bind);
    free(ts);
    free(tc);
    return st;
}

static Att *att_of(QWorld *w, QwPhen pid, int m) { return m == NO_MODEL ? &w->phens[pid].base : &w->phens[pid].models[m].att; }

static void narrow_trampoline(QNet *net, void *ctx, const QnCell *cells, size_t n)
{
    (void) net;
    (void) cells;
    (void) n;
    NarrowCtx *nc = ctx;
    QWorld *w = nc->w;
    Phen *p = &w->phens[nc->id];
    if (nc->model != NO_MODEL && nc->model != p->active) return;
    Att *a = att_of(w, nc->id, nc->model);
    QkCall call = { w, nc->id, a, M_NARROW, 0, { 0 } };
    a->k->narrow(a->inst, &call);
    if (call.err && !w->narrow_failed) {
        w->narrow_failed = true;
        snprintf(w->narrow_msg, sizeof w->narrow_msg, "%s (%s): %s", p->name, a->k->name, call.msg);
    }
}

static QwStatus attach(QWorld *w, QwPhen pid, int m, const QkKernel *k)
{
    Phen *p = &w->phens[pid];
    const char *pn = p->name;
    Att *a = att_of(w, pid, m);
    if (a->k) return fail(w, QW_EKERNEL, "%s: already has a kernel%s%s", pn, m == NO_MODEL ? "" : " in model ",
                          m == NO_MODEL ? "" : p->models[m].name);
    QwStatus st = check_contract(w, pn, k);
    if (st) return st;
    if (k->flags & QK_MAP) return fail(w, QW_EKERNEL, "%s: kernel '%s' is a map kernel; use qw_switch", pn, k->name);
    if (!p->instance)
        for (size_t i = 0; i < p->nports; i++) {
            QwPhen q;
            if (port_phen(w, p->ports[i].target, &q) && w->phens[q].instance)
                return fail(w, QW_ELIFE, "%s: port '%s' connects to the instance '%s', but %s is not an instance", pn,
                            p->ports[i].port, w->phens[q].name, pn);
        }
    QnCell *bind;
    QConv *ts, *tc;
    bool via;
    if ((st = bind_kernel(w, pid, m, k, NULL, 0, &bind, &ts, &tc, &via))) return st;
    a->k = k;
    a->bind = bind;
    a->to_slot = ts;
    a->to_cell = tc;
    a->via_model = via;
    a->predicted = false;
    if (k->create && p->alive) {   /* a dormant instance creates its kernels at its start */
        QkCall call = { w, pid, a, M_CREATE, 0, { 0 } };
        if (k->create(k, &w->host, &call, &a->inst) != QK_OK || call.err) {
            st = fail(w, QW_EKERNEL, "%s: kernel '%s' failed to start: %s", pn, k->name, call.msg);
            a->inst = NULL;
            a->k = NULL;   /* free_att must not call destroy */
            free_att(a);
            return st;
        }
    }
    if (k->flags & QK_NARROW) {
        if (!grow((void **) &w->nctx, &w->capnctx, w->nnctx + 1, sizeof *w->nctx)) return QW_ENOMEM;
        NarrowCtx *nc = malloc(sizeof *nc);
        if (!nc) return QW_ENOMEM;
        *nc = (NarrowCtx) { w, pid, m };
        w->nctx[w->nnctx++] = nc;
        qn_set_group(w->net, m != NO_MODEL ? p->models[m].group : p->group);
        QnStatus s = qn_custom(w->net, a->bind, k->nslots, narrow_trampoline, nc);
        qn_set_group(w->net, w->build_group);
        if (s != QN_OK) return QW_ENOMEM;
    }
    return QW_OK;
}

QwStatus qw_attach(QWorld *w, QwPhen pid, const QkKernel *k)
{
    if (pid >= w->nphens) return fail(w, QW_ENAME, "no such phenomenon");
    return attach(w, pid, NO_MODEL, k);
}

QwStatus qw_model_attach(QWorld *w, QwPhen pid, const char *model, const QkKernel *k)
{
    if (pid >= w->nphens) return fail(w, QW_ENAME, "no such phenomenon");
    int m = find_model(&w->phens[pid], model, strlen(model));
    if (m == NO_MODEL) return fail(w, QW_ENAME, "%s: no model '%s'", w->phens[pid].name, model);
    return attach(w, pid, m, k);
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

/* The running kernels of a phenomenon: its base kernel, then its active
 * model's kernel. */
static Att *running(QWorld *w, size_t pid, int which)
{
    Phen *p = &w->phens[pid];
    if (!p->alive) return NULL;
    Att *a = which == 0 ? &p->base : (p->active == NO_MODEL ? NULL : &p->models[p->active].att);
    return a && a->k && (a->k->flags & QK_EVOLVE) ? a : NULL;
}

static QwStatus check_valid(QWorld *w, const Phen *p, const Att *a)
{
    for (uint32_t i = 0; i < a->k->nslots; i++) {
        const QkSlot *s = &a->k->slots[i];
        if (s->role == QK_LINK || set_cell(w, a->bind[i])) continue;
        QInterval v = qi_conv(qn_get(w->net, a->bind[i]), &a->to_slot[i]);
        if (!qi_subset(v, s->valid)) {
            char lo[64], hi[64];
            q_format_decimal(v.lo, s->scale, lo, sizeof lo);
            q_format_decimal(v.hi, s->scale, hi, sizeof hi);
            return fail(w, QW_EINAPPLICABLE, "%s: kernel '%s' is not valid for '%s' in [%s, %s]", p->name, a->k->name,
                        w->cells[a->bind[i]].path, lo, hi);
        }
    }
    return QW_OK;
}

static QwStatus kernel_failed(QWorld *w, const Phen *p, const Att *a, const char *what, const QkCall *call, QkResult r)
{
    if (r == QK_INAPPLICABLE)
        return fail(w, QW_EINAPPLICABLE, "%s: kernel '%s' is not applicable (%s)", p->name, a->k->name, what);
    /* the reason: an error of a host function, or else the last message that the kernel logged */
    const char *said = strstr(w->log, ": ");
    return fail(w, QW_EKERNEL, "%s: kernel '%s' failed in %s: %s", p->name, a->k->name, what,
                call->err ? call->msg : said ? said + 2 : "kernel error");
}

static QwStatus predict(QWorld *w, QwPhen id, Att *a)
{
    Phen *p = &w->phens[id];
    QwStatus st = check_valid(w, p, a);
    if (st) return st;
    a->nas = 0;
    QkCall call = { w, id, a, M_PREDICT, 0, { 0 } };
    QkPrediction pr = { QK_NO_TIME, QK_NO_TIME, QK_NO_TIME };
    w->log[0] = 0;
    QkResult r = a->k->predict(a->inst, &call, &pr);
    p->predicts++;
    if (r != QK_OK || call.err) return kernel_failed(w, p, a, "predict", &call, r);
    if (pr.horizon <= w->now)
        return fail(w, QW_EKERNEL, "%s: kernel '%s' predicted a horizon that is not after now", p->name, a->k->name);
    if (pr.event_lo != QK_NO_TIME && (pr.event_lo <= w->now || pr.event_hi < pr.event_lo))
        return fail(w, QW_EKERNEL, "%s: kernel '%s' predicted an invalid event time", p->name, a->k->name);
    a->pred = pr;
    a->predicted = true;
    return QW_OK;
}

/* After a boundary, an input or a switch: drop each prediction that has
 * expired or whose assumptions no longer hold. */
static void invalidate(QWorld *w)
{
    for (size_t i = 0; i < w->nphens; i++)
        for (int which = 0; which < 2; which++) {
            Att *a = running(w, i, which);
            if (!a || !a->predicted) continue;
            if (w->now >= a->pred.horizon || w->now >= a->pred.event_lo) {
                a->predicted = false;
                continue;
            }
            for (size_t j = 0; j < a->nas; j++)
                if (set_cell(w, a->as[j].cell) ? (qn_get_set(w->net, a->as[j].cell) & ~word(a->as[j].range.lo)) != 0
                                               : !qi_subset(qn_get(w->net, a->as[j].cell), a->as[j].range)) {
                    a->predicted = false;
                    break;
                }
        }
}

/* --- the change log ----------------------------------------------------------------- */

void qw_log_changes(QWorld *w, bool on) { w->logging = on; }

const QwChange *qw_changes(const QWorld *w, size_t *n)
{
    *n = w->nlog;
    return w->log_entries;
}

void qw_changes_discard(QWorld *w, uint64_t before_epoch)
{
    size_t k = 0;   /* entries are in epoch order, so the old ones come first */
    while (k < w->nlog && w->log_entries[k].epoch < before_epoch) k++;
    if (!k) return;
    memmove(w->log_entries, w->log_entries + k, (w->nlog - k) * sizeof *w->log_entries);
    w->nlog -= k;
}

/* Makes room for n more entries, if logging is on. */
static bool log_reserve(QWorld *w, size_t n)
{
    return !w->logging || grow((void **) &w->log_entries, &w->caplog, w->nlog + n, sizeof *w->log_entries);
}

/* Records a change. The caller reserved the room. The epoch is not known
 * yet, so log_stamp sets it after qn_begin_epoch. */
static void log_change(QWorld *w, QnCell c, QwChangeKind kind, QInterval before, QInterval after, int64_t amount)
{
    if (!w->logging) return;
    if (kind == QW_CHANGE_VALUE && before.lo == after.lo && before.hi == after.hi) return;
    w->log_entries[w->nlog++] = (QwChange) { 0, c, kind, before, after, amount };
}

/* Sets the epoch of the entries from index `from` to the current epoch. */
static void log_stamp(QWorld *w, size_t from)
{
    uint64_t e = qn_epoch(w->net);
    for (size_t i = from; i < w->nlog; i++) w->log_entries[i].epoch = e;
}

static int cmp_cell(const void *a, const void *b)
{
    QnCell x = *(const QnCell *) a, y = *(const QnCell *) b;
    return (x > y) - (x < y);
}

/* Stages the touched cells in order of cell index, and logs each change.
 * The cost grows with the number of touched cells only. If a stock goes out
 * of range, nothing is staged or logged. On return, the staging is clear. */
/* Checks that the staged changes can commit: no stock out of range, and room
 * in the change log. On failure, the staging is clear. After a success,
 * commit cannot fail. */
static QwStatus commit_check(QWorld *w)
{
    for (size_t i = 0; i < w->nchanged; i++) {
        QnCell c = w->changed[i];
        int64_t v;
        if (w->delta[c] && qp_add(qn_base(w->net, c).lo, w->delta[c], &v) != Q_OK) {
            QwStatus st = fail(w, QW_ESTOCK, "stock '%s' out of range", w->cells[c].path);
            clear_staging(w);
            return st;
        }
        if (w->delta[c] && !w->phens[w->cells[c].owner].alive) {
            QwStatus st = fail(w, QW_ESTOCK, "transfer to the stock '%s' of a dormant instance", w->cells[c].path);
            clear_staging(w);
            return st;
        }
    }
    if (!log_reserve(w, w->nchanged)) {
        clear_staging(w);
        return fail(w, QW_ENOMEM, "no memory for the change log");
    }
    return QW_OK;
}

static QwStatus commit(QWorld *w)
{
    QwStatus st = commit_check(w);
    if (st) return st;
    qsort(w->changed, w->nchanged, sizeof *w->changed, cmp_cell);
    for (size_t i = 0; i < w->nchanged; i++) {
        QnCell c = w->changed[i];
        QInterval base = qn_base(w->net, c);
        if (w->written[c] && set_cell(w, c)) {
            uint64_t mask = (uint64_t) 1 << w->wval[c];
            qn_stage_set(w->net, c, mask);
            log_change(w, c, QW_CHANGE_VALUE, base, qn_set_value(mask, word(base.hi)), 0);
        } else if (w->written[c]) {
            QInterval v = qi_point(w->wval[c]);
            qn_stage(w->net, c, v);
            log_change(w, c, QW_CHANGE_VALUE, base, v, 0);
        }
        if (w->delta[c]) {
            int64_t v;
            (void) qp_add(base.lo, w->delta[c], &v);   /* checked above */
            qn_stage(w->net, c, qi_point(v));
            log_change(w, c, QW_CHANGE_STOCK, base, qi_point(v), w->delta[c]);
        }
    }
    clear_staging(w);
    return QW_OK;
}

static QwStatus lifecycle(QWorld *w);

QwStatus qw_run(QWorld *w, int64_t until)
{
    if (until < w->now) return fail(w, QW_ETIME, "cannot run back to %lld from %lld", (long long) until, (long long) w->now);
    QwStatus st = settle(w);
    if (st) return st;
    if ((st = lifecycle(w))) return st;
    while (w->now < until) {
        int64_t b = until;
        for (size_t i = 0; i < w->nphens; i++)
            for (int which = 0; which < 2; which++) {
                Att *a = running(w, i, which);
                if (!a) continue;
                if (!a->predicted && (st = predict(w, (QwPhen) i, a))) return st;
                if (a->pred.horizon < b) b = a->pred.horizon;
                if (a->pred.event_lo < b) b = a->pred.event_lo;
            }

        for (size_t i = 0; i < w->nphens; i++)
            for (int which = 0; which < 2; which++) {
                Att *a = running(w, i, which);
                if (!a) continue;
                Phen *p = &w->phens[i];
                if ((st = check_valid(w, p, a))) {
                    clear_staging(w);
                    return st;
                }
                QkCall call = { w, (QwPhen) i, a, M_ADVANCE, 0, { 0 } };
                w->log[0] = 0;
                QkResult r = a->k->advance(a->inst, &call, b);
                a->events++;
                w->advances++;
                if (r != QK_OK || call.err) {
                    clear_staging(w);
                    return kernel_failed(w, p, a, "advance", &call, r);
                }
            }
        size_t logged = w->nlog;
        if ((st = commit(w))) return st;
        w->now = b;
        w->boundaries++;
        qn_begin_epoch(w->net);
        log_stamp(w, logged);
        if ((st = settle(w))) return st;
        invalidate(w);
        if ((st = lifecycle(w))) return st;
    }
    return QW_OK;
}

/* --- switching models ---------------------------------------------------------------- */

typedef struct { QnCell *bind; QConv *ts, *tc; bool via; } Binding;

QwStatus qw_switch(QWorld *w, QwPhen pid, const char *model, const QkKernel *map)
{
    if (pid >= w->nphens) return fail(w, QW_ENAME, "no such phenomenon");
    Phen *p = &w->phens[pid];
    const char *pn = p->name;
    int to = find_model(p, model, strlen(model)), from = p->active;
    if (to == NO_MODEL) return fail(w, QW_ENAME, "%s: no model '%s'", pn, model);
    if (to == from) return fail(w, QW_EDUP, "%s: model '%s' is already active", pn, model);
    if (!p->alive) return fail(w, QW_ELIFE, "%s: a dormant instance cannot switch its model", pn);
    QwStatus st = check_contract(w, pn, map);
    if (st) return st;
    if (!(map->flags & QK_MAP)) return fail(w, QW_EKERNEL, "%s: kernel '%s' is not a map kernel", pn, map->name);
    if ((st = settle(w))) return st;

    /* 1. bind the map: from.* is the active model, to.* the new one */
    char fromt[512], tot[512];
    snprintf(fromt, sizeof fromt, "%s.%s", pn, p->models[from].name);
    snprintf(tot, sizeof tot, "%s.%s", pn, p->models[to].name);
    Port extra[2] = { { (char *) "from", fromt }, { (char *) "to", tot } };
    Att ma;
    memset(&ma, 0, sizeof ma);
    ma.k = map;
    if ((st = bind_kernel(w, pid, NO_MODEL, map, extra, 2, &ma.bind, &ma.to_slot, &ma.to_cell, &ma.via_model)))
        return st;

    /* 2. run it */
    QkCall call = { w, pid, &ma, M_CREATE, 0, { 0 } };
    if (map->create && map->create(map, &w->host, &call, &ma.inst) != QK_OK) {
        st = fail(w, QW_EKERNEL, "%s: map kernel '%s' failed to start: %s", pn, map->name, call.msg);
        ma.inst = NULL;
        goto fail_map;
    }
    call.mode = M_MAP;
    QkResult r = map->map(ma.inst, &call);
    if (r != QK_OK || call.err) {
        st = kernel_failed(w, p, &ma, "map", &call, r);
        goto fail_map;
    }

    /* 3. every stock of the old model must now be exactly zero */
    for (size_t c = 0; c < w->ncells; c++) {
        if (w->cells[c].owner != pid || w->cells[c].model != from || !w->kinds[w->cells[c].kind].conserved) continue;
        int64_t v;
        if (qp_add(qn_base(w->net, (QnCell) c).lo, w->delta[c], &v) != Q_OK || v != 0) {
            st = fail(w, QW_ESTOCK, "%s: map kernel '%s' left %lld quanta in the stock '%s'", pn, map->name,
                      (long long) v, w->cells[c].path);
            goto fail_map;
        }
    }

    /* the commit in step 5 must not fail after the switch takes effect */
    if ((st = commit_check(w))) goto fail_map;

    /* 4. make the new model active and bind every kernel again */
    p->active = to;
    size_t natt = 0;
    for (size_t i = 0; i < w->nphens; i++) natt += 1 + w->phens[i].nmodels;
    Binding *nb = calloc(natt, sizeof *nb);
    if (!nb) {
        st = QW_ENOMEM;
        p->active = from;
        goto fail_map;
    }
    size_t at = 0;
    for (size_t i = 0; i < w->nphens && !st; i++)
        for (int m = NO_MODEL; m < (int) w->phens[i].nmodels && !st; m++, at++) {
            Att *a = att_of(w, (QwPhen) i, m);
            if (a->k) st = bind_kernel(w, (QwPhen) i, m, a->k, NULL, 0, &nb[at].bind, &nb[at].ts, &nb[at].tc, &nb[at].via);
        }
    if (st) {
        p->active = from;
        for (size_t i = 0; i < natt; i++) {
            free(nb[i].bind);
            free(nb[i].ts);
            free(nb[i].tc);
        }
        free(nb);
        goto fail_map;
    }
    at = 0;
    for (size_t i = 0; i < w->nphens; i++)
        for (int m = NO_MODEL; m < (int) w->phens[i].nmodels; m++, at++) {
            Att *a = att_of(w, (QwPhen) i, m);
            if (!a->k) continue;
            if (memcmp(a->bind, nb[at].bind, a->k->nslots * sizeof *a->bind)) a->predicted = false;
            free(a->bind);
            free(a->to_slot);
            free(a->to_cell);
            a->bind = nb[at].bind;
            a->to_slot = nb[at].ts;
            a->to_cell = nb[at].tc;
            a->via_model = nb[at].via;
        }
    free(nb);
    p->models[from].att.predicted = false;
    p->models[to].att.predicted = false;

    /* 5. commit, swap the propagator groups, and open a new epoch */
    size_t logged = w->nlog;
    if ((st = commit(w))) goto done;
    qn_group_enable(w->net, p->models[from].group, false);
    qn_group_enable(w->net, p->models[to].group, true);
    qn_begin_epoch(w->net);
    log_stamp(w, logged);
    w->switches++;
    st = settle(w);
    invalidate(w);
done:
    if (map->destroy && ma.inst) map->destroy(ma.inst);
    ma.inst = NULL;
    ma.k = NULL;
    free_att(&ma);
    return st;

fail_map:
    clear_staging(w);
    if (map->destroy && ma.inst) map->destroy(ma.inst);
    ma.inst = NULL;
    ma.k = NULL;
    free_att(&ma);
    return st;
}

/* --- transitions (ADR 0021 section 5) --------------------------------------------------- */

QwStatus qw_transition(QWorld *w, QwPhen pid, const char *name, const char *from, const char *to, const QkKernel *map)
{
    if (pid >= w->nphens) return fail(w, QW_ENAME, "no such phenomenon");
    Phen *p = &w->phens[pid];
    if (!valid_name(name)) return fail(w, QW_ENAME, "%s: invalid transition name '%s'", p->name, name);
    int f = find_model(p, from, strlen(from)), t = find_model(p, to, strlen(to));
    if (f == NO_MODEL || t == NO_MODEL) return fail(w, QW_ENAME, "%s: no model '%s'", p->name, f == NO_MODEL ? from : to);
    if (f == t) return fail(w, QW_EDUP, "%s: the transition '%s' goes from the model '%s' to itself", p->name, name, from);
    for (size_t i = 0; i < p->ntrans; i++) {
        if (!strcmp(p->trans[i].name, name)) return fail(w, QW_EDUP, "%s: the transition '%s' already exists", p->name, name);
        if (p->trans[i].from == f && p->trans[i].to == t)
            return fail(w, QW_EDUP, "%s: the transition '%s' already goes from '%s' to '%s'", p->name, p->trans[i].name, from, to);
    }
    QwStatus st = check_contract(w, p->name, map);
    if (st) return st;
    if (!(map->flags & QK_MAP)) return fail(w, QW_EKERNEL, "%s: kernel '%s' is not a map kernel", p->name, map->name);
    if (!grow((void **) &p->trans, &p->captrans, p->ntrans + 1, sizeof *p->trans)) return QW_ENOMEM;
    Transition *tr = &p->trans[p->ntrans];
    memset(tr, 0, sizeof *tr);
    if (!(tr->name = dupstr(name))) return QW_ENOMEM;
    tr->from = f;
    tr->to = t;
    tr->map = map;
    p->ntrans++;
    return QW_OK;
}

QwStatus qw_switch_to(QWorld *w, QwPhen pid, const char *model)
{
    if (pid >= w->nphens) return fail(w, QW_ENAME, "no such phenomenon");
    Phen *p = &w->phens[pid];
    int to = find_model(p, model, strlen(model));
    if (to == NO_MODEL) return fail(w, QW_ENAME, "%s: no model '%s'", p->name, model);
    if (to == p->active) return fail(w, QW_EDUP, "%s: model '%s' is already active", p->name, model);
    for (size_t i = 0; i < p->ntrans; i++)
        if (p->trans[i].from == p->active && p->trans[i].to == to) return qw_switch(w, pid, model, p->trans[i].map);
    return fail(w, QW_ENAME, "%s: no transition from the model '%s' to the model '%s'", p->name, p->models[p->active].name,
                model);
}

/* --- instances (ADR 0013) -------------------------------------------------------------- */

QwStatus qw_instance(QWorld *w, const char *key, QwPhen *out)
{
    if (strlen(key) > 255) return fail(w, QW_ENAME, "the key of an instance can have 255 characters at most");
    QwPhen pid;
    QwStatus st = new_phen(w, key, &pid);
    if (st) return st;
    Phen *p = &w->phens[pid];
    p->instance = true;
    p->alive = false;
    if (qn_new_group(w->net, false, &p->group) != QN_OK) return QW_ENOMEM;
    char path[300];
    snprintf(path, sizeof path, "%s.alive", key);
    if ((st = add_cell(w, path, w->boolean, true, qi_top(), 1, true, &p->alive_cell))) return st;
    *out = pid;
    return QW_OK;
}

static Phen *instance_of(QWorld *w, QwPhen pid)
{
    if (pid >= w->nphens) {
        fail(w, QW_ENAME, "no such phenomenon");
        return NULL;
    }
    if (!w->phens[pid].instance) {
        fail(w, QW_ELIFE, "%s is not an instance", w->phens[pid].name);
        return NULL;
    }
    return &w->phens[pid];
}

QwStatus qw_build_begin(QWorld *w, QwPhen pid)
{
    Phen *p = instance_of(w, pid);
    if (!p) return pid >= w->nphens ? QW_ENAME : QW_ELIFE;
    w->build_group = p->group;
    qn_set_group(w->net, p->group);
    return QW_OK;
}

void qw_build_end(QWorld *w)
{
    w->build_group = 0;
    qn_set_group(w->net, 0);
}

QwStatus qw_instance_maps(QWorld *w, QwPhen pid, const QkKernel *start, const QkKernel *end)
{
    Phen *p = instance_of(w, pid);
    if (!p) return pid >= w->nphens ? QW_ENAME : QW_ELIFE;
    const QkKernel *maps[2] = { start, end };
    for (int i = 0; i < 2; i++) {
        if (!maps[i]) continue;
        QwStatus st = check_contract(w, p->name, maps[i]);
        if (st) return st;
        if (!(maps[i]->flags & QK_MAP))
            return fail(w, QW_EKERNEL, "%s: kernel '%s' is not a map kernel", p->name, maps[i]->name);
    }
    p->start_map = start;
    p->end_map = end;
    return QW_OK;
}

QwStatus qw_exists_when(QWorld *w, QwPhen pid, const char *condition)
{
    Phen *p = instance_of(w, pid);
    if (!p) return pid >= w->nphens ? QW_ENAME : QW_ELIFE;
    if (p->has_cond) return fail(w, QW_EDUP, "%s: already has an existence condition", p->name);
    QnCell c;
    if (qw_find(w, condition, &c) != QW_OK) return fail(w, QW_ENAME, "%s: no cell '%s'", p->name, condition);
    if (w->cells[c].kind != w->boolean)
        return fail(w, QW_ETYPE, "%s: the condition '%s' is not a boolean cell", p->name, condition);
    if (!grow((void **) &w->conds, &w->capconds, w->nconds + 1, sizeof *w->conds)) return QW_ENOMEM;
    w->conds[w->nconds++] = pid;
    p->cond = c;
    p->has_cond = true;
    return QW_OK;
}

bool qw_alive(const QWorld *w, QwPhen pid) { return pid < w->nphens && w->phens[pid].alive; }

/* Phenomenon `from` has a port that connects to phenomenon `to`. */
static bool connects_to(const QWorld *w, QwPhen from, QwPhen to)
{
    const Phen *p = &w->phens[from];
    QwPhen q;
    for (size_t i = 0; i < p->nports; i++)
        if (port_phen(w, p->ports[i].target, &q) && q == to && q != from) return true;
    return false;
}

/* A dormant instance has no kernel instances, so no private kernel state
 * survives an end. */
static void destroy_kernels(QWorld *w, QwPhen pid)
{
    for (int m = NO_MODEL; m < (int) w->phens[pid].nmodels; m++) {
        Att *a = att_of(w, pid, m);
        if (a->k && a->k->destroy && a->inst) a->k->destroy(a->inst);
        a->inst = NULL;
        a->predicted = false;
        a->nas = 0;
    }
}

static QwStatus create_kernels(QWorld *w, QwPhen pid)
{
    for (int m = NO_MODEL; m < (int) w->phens[pid].nmodels; m++) {
        Att *a = att_of(w, pid, m);
        if (!a->k || !a->k->create) continue;
        QkCall call = { w, pid, a, M_CREATE, 0, { 0 } };
        if (a->k->create(a->k, &w->host, &call, &a->inst) != QK_OK || call.err) {
            QwStatus st = fail(w, QW_EKERNEL, "%s: kernel '%s' failed to start: %s", w->phens[pid].name, a->k->name,
                               call.msg);
            a->inst = NULL;
            destroy_kernels(w, pid);
            return st;
        }
    }
    return QW_OK;
}

/* Binds and runs the start map or the end map of an instance. Its writes
 * and transfers are staged. The caller commits them or clears the staging. */
static QwStatus run_map(QWorld *w, QwPhen pid, const QkKernel *map)
{
    Phen *p = &w->phens[pid];
    Att ma;
    memset(&ma, 0, sizeof ma);
    ma.k = map;
    QwStatus st = bind_kernel(w, pid, NO_MODEL, map, NULL, 0, &ma.bind, &ma.to_slot, &ma.to_cell, &ma.via_model);
    if (st) return st;
    QkCall call = { w, pid, &ma, M_CREATE, 0, { 0 } };
    if (map->create && (map->create(map, &w->host, &call, &ma.inst) != QK_OK || call.err)) {
        st = fail(w, QW_EKERNEL, "%s: map kernel '%s' failed to start: %s", p->name, map->name, call.msg);
        ma.inst = NULL;
    } else {
        call.mode = M_MAP;
        QkResult r = map->map(ma.inst, &call);
        if (r != QK_OK || call.err) st = kernel_failed(w, p, &ma, "map", &call, r);
    }
    if (map->destroy && ma.inst) map->destroy(ma.inst);
    ma.inst = NULL;
    ma.k = NULL;
    free_att(&ma);
    return st;
}

static void set_groups(QWorld *w, const Phen *p, bool on)
{
    qn_group_enable(w->net, p->group, on);
    if (p->active != NO_MODEL) qn_group_enable(w->net, p->models[p->active].group, on);
}

/* Stages a write of the cell "<key>.alive". */
static void stage_alive(QWorld *w, const Phen *p, bool alive)
{
    w->wval[p->alive_cell] = alive;
    w->written[p->alive_cell] = true;
    touch(w, p->alive_cell);
}

/* The start of a dormant instance (ADR 0013 section 3). If it fails, nothing
 * changes. */
static QwStatus do_start(QWorld *w, QwPhen pid)
{
    Phen *p = &w->phens[pid];
    for (size_t i = 0; i < p->nports; i++) {
        QwPhen q;
        if (port_phen(w, p->ports[i].target, &q) && q != pid && !w->phens[q].alive)
            return fail(w, QW_ELIFE, "%s: cannot start while its participant '%s' is dormant", p->name,
                        w->phens[q].name);
    }
    QwStatus st = create_kernels(w, pid);
    if (st) return st;
    if (p->start_map && (st = run_map(w, pid, p->start_map))) {
        clear_staging(w);
        destroy_kernels(w, pid);
        return st;
    }
    stage_alive(w, p, true);
    p->alive = true;   /* the commit refuses a transfer to a dormant instance */
    size_t logged = w->nlog;
    if ((st = commit(w))) {
        p->alive = false;
        destroy_kernels(w, pid);
        return st;
    }
    set_groups(w, p, true);
    qn_begin_epoch(w->net);
    log_stamp(w, logged);
    w->starts++;
    st = settle(w);
    invalidate(w);
    return st;
}

static int cmp_change(const void *a, const void *b)
{
    QnCell x = ((const QwChange *) a)->cell, y = ((const QwChange *) b)->cell;
    return (x > y) - (x < y);
}

/* The end of an alive instance (ADR 0013 section 4). If it fails, nothing
 * changes. In the lifecycle step, `cascade` is true: the instances that
 * connect to this one are already ending. */
static QwStatus do_end(QWorld *w, QwPhen pid, bool cascade)
{
    Phen *p = &w->phens[pid];
    const char *pn = p->name;
    if (!cascade)
        for (size_t q = 0; q < w->nphens; q++)
            if (w->phens[q].instance && w->phens[q].alive && connects_to(w, (QwPhen) q, pid))
                return fail(w, QW_ELIFE, "%s: cannot end while '%s', which connects to it, is alive", pn,
                            w->phens[q].name);
    QwStatus st;
    if (p->end_map && (st = run_map(w, pid, p->end_map))) {
        clear_staging(w);
        return st;
    }
    for (size_t i = 0; i < p->ncells; i++) {
        QnCell c = p->cells[i];
        if (!w->kinds[w->cells[c].kind].conserved) {
            w->written[c] = false;   /* the host sets the cell to its declared value, below */
            continue;
        }
        int64_t v;
        if (qp_add(qn_base(w->net, c).lo, w->delta[c], &v) != Q_OK || v != 0) {
            st = fail(w, QW_ESTOCK, "%s: %lld quanta remain in the stock '%s' at the end", pn, (long long) v,
                      w->cells[c].path);
            clear_staging(w);
            return st;
        }
    }
    stage_alive(w, p, false);
    if (!log_reserve(w, w->nchanged + p->ncells)) {
        clear_staging(w);
        return fail(w, QW_ENOMEM, "no memory for the change log");
    }
    size_t logged = w->nlog;
    if ((st = commit(w))) return st;

    /* A dormant instance holds no state: every cell that is not a stock
     * returns to its declared value. */
    for (size_t i = 0; i < p->ncells; i++) {
        QnCell c = p->cells[i];
        const CellInfo *ci = &w->cells[c];
        if (w->kinds[ci->kind].conserved || c == p->alive_cell) continue;
        QInterval base = qn_base(w->net, c);
        if (base.lo == ci->init.lo && base.hi == ci->init.hi) continue;
        if (set_cell(w, c)) qn_stage_set(w->net, c, word(ci->init.lo));
        else qn_stage(w->net, c, ci->init);
        log_change(w, c, QW_CHANGE_VALUE, base, ci->init, 0);
    }
    set_groups(w, p, false);
    p->alive = false;
    destroy_kernels(w, pid);
    qn_begin_epoch(w->net);
    log_stamp(w, logged);
    if (w->nlog > logged) qsort(w->log_entries + logged, w->nlog - logged, sizeof *w->log_entries, cmp_change);
    w->ends++;
    st = settle(w);
    invalidate(w);
    return st;
}

QwStatus qw_start(QWorld *w, QwPhen pid)
{
    Phen *p = instance_of(w, pid);
    if (!p) return pid >= w->nphens ? QW_ENAME : QW_ELIFE;
    if (p->has_cond) return fail(w, QW_ELIFE, "%s: has an existence condition, so the world starts it", p->name);
    if (p->alive) return fail(w, QW_ELIFE, "%s: is already alive", p->name);
    QwStatus st = settle(w);
    return st ? st : do_start(w, pid);
}

QwStatus qw_end(QWorld *w, QwPhen pid)
{
    Phen *p = instance_of(w, pid);
    if (!p) return pid >= w->nphens ? QW_ENAME : QW_ELIFE;
    if (p->has_cond) return fail(w, QW_ELIFE, "%s: has an existence condition, so the world ends it", p->name);
    if (!p->alive) return fail(w, QW_ELIFE, "%s: is dormant", p->name);
    QwStatus st = settle(w);
    return st ? st : do_end(w, pid, false);
}

/* --- the lifecycle step (ADR 0013 section 5) --------------------------------------------- */

typedef struct { QwPhen id; int depth; const char *name; } LifeItem;
typedef struct { LifeItem *items; size_t n, cap; } LifeList;

static bool life_push(LifeList *l, QwPhen id, const char *name)
{
    if (!grow((void **) &l->items, &l->cap, l->n + 1, sizeof *l->items)) return false;
    l->items[l->n++] = (LifeItem) { id, 0, name };
    return true;
}

/* The length of the longest chain of instances from `id` through ports to
 * participants. An instance has a larger depth than each of its
 * participants. In a cycle of ports, the key order decides. */
static int life_depth(const QWorld *w, QwPhen id, int *memo)
{
    if (memo[id] >= 0) return memo[id];
    if (memo[id] == -2) return 0;
    memo[id] = -2;
    const Phen *p = &w->phens[id];
    int d = 0;
    for (size_t i = 0; i < p->nports; i++) {
        QwPhen q;
        if (!port_phen(w, p->ports[i].target, &q) || q == id || !w->phens[q].instance) continue;
        int dq = 1 + life_depth(w, q, memo);
        if (dq > d) d = dq;
    }
    return memo[id] = d;
}

/* Starts: in key order. An instance starts only when its participants are
 * alive, so it is never in one round with a participant that starts. */
static int cmp_start(const void *a, const void *b)
{
    return strcmp(((const LifeItem *) a)->name, ((const LifeItem *) b)->name);
}

/* Ends: an instance before its participants, then in key order. */
static int cmp_end(const void *a, const void *b)
{
    const LifeItem *x = a, *y = b;
    return x->depth != y->depth ? (x->depth < y->depth) - (x->depth > y->depth) : strcmp(x->name, y->name);
}

/* The mask of a boolean cell: bit 0 is "false" and bit 1 is "true". */
enum { COND_FALSE = 1, COND_TRUE = 2 };

/* One round: decides every start and end from the current fixed point, then
 * applies them. Sets *changed if an instance changed. */
static QwStatus life_round(QWorld *w, bool *changed)
{
    LifeList ends = { 0 }, starts = { 0 };
    int *memo = NULL;
    QwStatus st = QW_ENOMEM;
    uint64_t round = ++w->life_round;

    /* the ends: each alive instance whose condition is false ... */
    for (size_t i = 0; i < w->nconds; i++) {
        Phen *p = &w->phens[w->conds[i]];
        if (!p->alive || qn_get_set(w->net, p->cond) != COND_FALSE) continue;
        if (!life_push(&ends, w->conds[i], p->name)) goto out;
        p->mark = round;
    }
    /* ... and each alive instance that connects to an instance that ends */
    for (size_t k = 0; k < ends.n; k++)
        for (size_t q = 0; q < w->nphens; q++) {
            Phen *d = &w->phens[q];
            if (!d->instance || !d->alive || d->mark == round || !connects_to(w, (QwPhen) q, ends.items[k].id)) continue;
            if (!life_push(&ends, (QwPhen) q, d->name)) goto out;
            d->mark = round;
        }
    /* the starts: each dormant instance whose condition is true and whose
     * participants are alive and stay alive. An unknown condition changes
     * nothing: a decision from the absence of information (ADR 0008). */
    for (size_t i = 0; i < w->nconds; i++) {
        QwPhen id = w->conds[i];
        Phen *p = &w->phens[id];
        if (p->alive || qn_get_set(w->net, p->cond) != COND_TRUE) continue;
        bool ready = true;
        for (size_t j = 0; j < p->nports && ready; j++) {
            QwPhen q;
            if (port_phen(w, p->ports[j].target, &q) && q != id && (!w->phens[q].alive || w->phens[q].mark == round))
                ready = false;
        }
        if (ready && !life_push(&starts, id, p->name)) goto out;
    }
    st = QW_OK;
    if (!ends.n && !starts.n) goto out;
    *changed = true;

    for (int pass = 0; pass < 2; pass++) {
        const LifeList *l = pass == 0 ? &ends : &starts;
        for (size_t i = 0; i < l->n; i++)
            if (w->phens[l->items[i].id].flipped == w->life_step) {
                st = fail(w, QW_ELIFE, "%s: the existence conditions do not settle; the instance would %s again at one boundary",
                          l->items[i].name, pass == 0 ? "end" : "start");
                goto out;
            }
    }
    if (!(memo = malloc(w->nphens * sizeof *memo))) {
        st = QW_ENOMEM;
        goto out;
    }
    for (size_t i = 0; i < w->nphens; i++) memo[i] = -1;
    for (size_t i = 0; i < ends.n; i++) ends.items[i].depth = life_depth(w, ends.items[i].id, memo);
    if (ends.n) qsort(ends.items, ends.n, sizeof *ends.items, cmp_end);
    if (starts.n) qsort(starts.items, starts.n, sizeof *starts.items, cmp_start);

    for (size_t i = 0; i < ends.n && !st; i++) {
        w->phens[ends.items[i].id].flipped = w->life_step;
        st = do_end(w, ends.items[i].id, true);
    }
    for (size_t i = 0; i < starts.n && !st; i++) {
        w->phens[starts.items[i].id].flipped = w->life_step;
        st = do_start(w, starts.items[i].id);
    }
out:
    free(memo);
    free(ends.items);
    free(starts.items);
    return st;
}

/* Step 6 of a boundary: starts and ends the instances that have an
 * existence condition. A change can make another condition true or false,
 * so the rounds repeat until nothing changes. An instance changes at most
 * once in one step, so the step ends.
 *
 * Reserved: the relation engine will declare instances for new candidates
 * here, before the first round (ADR 0013 section 5). */
static QwStatus lifecycle(QWorld *w)
{
    if (!w->nconds) return QW_OK;
    w->life_step++;
    for (;;) {
        bool changed = false;
        QwStatus st = life_round(w, &changed);
        if (st || !changed) return st;
    }
}

/* --- inputs and inspection ------------------------------------------------------------ */

QwStatus qw_input(QWorld *w, const char *path, QInterval v)
{
    QnCell c;
    if (qw_find(w, path, &c) != QW_OK) return fail(w, QW_ENAME, "no cell '%s'", path);
    if (w->kinds[w->cells[c].kind].conserved)
        return fail(w, QW_ESTOCK, "'%s' is a stock; a stock changes only by transfers", path);
    if (set_cell(w, c)) return fail(w, QW_ETYPE, "'%s' is a set cell; use qw_input_set", path);
    if (w->cells[c].derived) return fail(w, QW_EOWNER, "'%s' is derived: a law gives its value", path);
    if (!qi_valid(v)) return fail(w, QW_ETYPE, "invalid interval for '%s'", path);
    if (!log_reserve(w, 1)) return fail(w, QW_ENOMEM, "no memory for the change log");
    size_t logged = w->nlog;
    log_change(w, c, QW_CHANGE_VALUE, qn_base(w->net, c), v, 0);
    qn_stage(w->net, c, v);
    qn_begin_epoch(w->net);
    log_stamp(w, logged);
    QwStatus st = settle(w);
    invalidate(w);
    return st;
}

QwStatus qw_input_set(QWorld *w, const char *path, uint64_t mask)
{
    QnCell c;
    if (qw_find(w, path, &c) != QW_OK) return fail(w, QW_ENAME, "no cell '%s'", path);
    if (!set_cell(w, c)) return fail(w, QW_ETYPE, "'%s' is not a set cell", path);
    if (w->cells[c].host_only) return fail(w, QW_EOWNER, "only the host writes '%s'", path);
    QInterval base = qn_base(w->net, c);
    uint64_t universe = word(base.hi);
    if (!mask || (mask & ~universe)) return fail(w, QW_ETYPE, "invalid atoms for '%s'", path);
    if (!log_reserve(w, 1)) return fail(w, QW_ENOMEM, "no memory for the change log");
    size_t logged = w->nlog;
    log_change(w, c, QW_CHANGE_VALUE, base, qn_set_value(mask, universe), 0);
    qn_stage_set(w->net, c, mask);
    qn_begin_epoch(w->net);
    log_stamp(w, logged);
    QwStatus st = settle(w);
    invalidate(w);
    return st;
}

uint64_t qw_get_set(const QWorld *w, const char *path)
{
    char resolved[512];
    bool via = false;
    resolve_ports(w, path, resolved, sizeof resolved, &via);
    QnCell c;
    if (qw_find(w, resolved, &c) != QW_OK) return 0;
    return qn_get_set(w->net, c);
}

QInterval qw_get(const QWorld *w, const char *path)
{
    char resolved[512];
    bool via = false;
    resolve_ports(w, path, resolved, sizeof resolved, &via);
    QnCell c;
    if (qw_find(w, resolved, &c) != QW_OK) return qi_top();
    return qn_get(w->net, c);
}

uint64_t qw_predicts(const QWorld *w, QwPhen p) { return p < w->nphens ? w->phens[p].predicts : 0; }

int64_t qw_total(const QWorld *w, QwKind kind)
{
    int64_t t = 0;
    for (size_t c = 0; c < w->ncells; c++)
        if (w->cells[c].kind == kind && !set_cell(w, (QnCell) c)) {
            int64_t v;
            if (qp_add(t, qn_base(w->net, (QnCell) c).lo, &v) == Q_OK) t = v;
        }
    return t;
}

static uint64_t fnv(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* --- snapshot and restore (ADR 0004 rule 8) -------------------------------------------- */

typedef struct { uint8_t *p; size_t n, cap; bool oom; } Out;
typedef struct { const uint8_t *p; size_t n, at; bool bad; } In;

static void put_bytes(Out *o, const void *v, size_t n)
{
    if (o->oom || !grow((void **) &o->p, &o->cap, o->n + n, 1)) { o->oom = true; return; }
    if (n) memcpy(o->p + o->n, v, n);
    o->n += n;
}

static void put(Out *o, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t) (v >> (8 * i));       /* the same bytes on each platform */
    put_bytes(o, b, 8);
}

static uint64_t get(In *in)
{
    if (in->bad || in->n - in->at < 8) { in->bad = true; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t) in->p[in->at + (size_t) i] << (8 * i);
    in->at += 8;
    return v;
}

#define SNAP_MAGIC 0x3130504e53575151ULL       /* "QQWSNP01" */

/* A hash of the structure of the world: a snapshot fits only a world with the same one. */
static uint64_t structure_hash(const QWorld *w)
{
    uint64_t h = fnv(0xcbf29ce484222325ULL, (uint64_t) w->nphens);
    h = fnv(h, (uint64_t) w->ncells);
    h = fnv(h, (uint64_t) qn_group_count(w->net));
    for (size_t i = 0; i < w->nphens; i++) {
        const Phen *p = &w->phens[i];
        h = fnv(fnv(h, p->ident), (uint64_t) p->nmodels);
        for (int m = NO_MODEL; m < (int) p->nmodels; m++) {
            const Att *a = m == NO_MODEL ? &p->base : &p->models[m].att;
            h = fnv(h, a->k ? qr_name_hash(a->k->name, strlen(a->k->name)) : 0);
        }
    }
    for (size_t c = 0; c < w->ncells; c++) h = fnv(fnv(h, w->cells[c].hash), (uint64_t) w->cells[c].derived);
    return h;
}

QwStatus qw_snapshot(QWorld *w, void **bytes, size_t *len)
{
    Out o = { 0 };
    put(&o, SNAP_MAGIC);
    put(&o, structure_hash(w));
    const uint64_t head[] = { (uint64_t) w->now, qn_epoch(w->net), w->boundaries, w->advances, w->switches, w->starts,
                              w->ends, w->life_step, w->life_round };
    for (size_t i = 0; i < sizeof head / sizeof head[0]; i++) put(&o, head[i]);
    for (size_t c = 0; c < w->ncells; c++) {
        QInterval b = qn_base(w->net, (QnCell) c);
        put(&o, (uint64_t) b.lo);
        put(&o, (uint64_t) b.hi);
    }
    for (size_t g = 0; g < qn_group_count(w->net); g++) put(&o, qn_group_enabled(w->net, (QnGroup) g));
    for (size_t i = 0; i < w->nphens; i++) {
        Phen *p = &w->phens[i];
        put(&o, (uint64_t) (int64_t) p->active);
        put(&o, p->alive);
        put(&o, p->flipped);
        put(&o, p->predicts);
        for (int m = NO_MODEL; m < (int) p->nmodels; m++) {
            Att *a = att_of(w, (QwPhen) i, m);
            if (!a->k) continue;
            put(&o, a->predicted);
            put(&o, (uint64_t) a->pred.horizon);
            put(&o, (uint64_t) a->pred.event_lo);
            put(&o, (uint64_t) a->pred.event_hi);
            put(&o, a->events);
            put(&o, a->nas);
            for (size_t j = 0; j < a->nas; j++) {
                put(&o, a->as[j].slot);
                put(&o, a->as[j].cell);
                put(&o, (uint64_t) a->as[j].range.lo);
                put(&o, (uint64_t) a->as[j].range.hi);
                put(&o, a->as[j].explicit_);
            }
            /* the private state of the kernel */
            size_t need = 0;
            if (a->inst && !(a->k->flags & QK_STATELESS)) {
                if (!a->k->save) {
                    free(o.p);
                    return fail(w, QW_EKERNEL, "%s: kernel '%s' has private state and no save function: no snapshot",
                                p->name, a->k->name);
                }
                need = a->k->save(a->inst, NULL, 0);
            }
            put(&o, need);
            if (need) {
                void *buf = malloc(need);
                if (!buf) o.oom = true;
                else {
                    a->k->save(a->inst, buf, need);
                    put_bytes(&o, buf, need);
                    free(buf);
                }
            }
        }
    }
    if (o.oom) {
        free(o.p);
        return fail(w, QW_ENOMEM, "out of memory");
    }
    *bytes = o.p;
    *len = o.n;
    return QW_OK;
}

/* Binds each kernel again, after the active models changed. */
static QwStatus rebind_all(QWorld *w)
{
    for (size_t i = 0; i < w->nphens; i++)
        for (int m = NO_MODEL; m < (int) w->phens[i].nmodels; m++) {
            Att *a = att_of(w, (QwPhen) i, m);
            if (!a->k) continue;
            QnCell *bind = NULL;
            QConv *ts = NULL, *tc = NULL;
            bool via = false;
            QwStatus st = bind_kernel(w, (QwPhen) i, m, a->k, NULL, 0, &bind, &ts, &tc, &via);
            if (st) return st;
            free(a->bind);
            free(a->to_slot);
            free(a->to_cell);
            a->bind = bind;
            a->to_slot = ts;
            a->to_cell = tc;
            a->via_model = via;
        }
    return QW_OK;
}

/* Reads a whole snapshot and changes nothing. False if the bytes end too soon, have bytes after the end, or
 * have a value that the world cannot take. The caller checked the magic number and the structure. */
static bool snapshot_whole(const QWorld *w, const void *bytes, size_t len)
{
    In in = { bytes, len, 16, false };
    for (int i = 0; i < 9; i++) get(&in);
    for (size_t c = 0; c < w->ncells; c++) {
        QInterval b = { (int64_t) get(&in), (int64_t) get(&in) };
        if (!in.bad && !w->cells[c].derived && !set_cell(w, (QnCell) c) && !qi_valid(b)) return false;
    }
    for (size_t g = 0; g < qn_group_count(w->net); g++) get(&in);
    for (size_t i = 0; i < w->nphens && !in.bad; i++) {
        const Phen *p = &w->phens[i];
        int64_t active = (int64_t) get(&in);
        for (int k = 0; k < 3; k++) get(&in);
        if (active < NO_MODEL || active >= (int64_t) p->nmodels || (p->nmodels && active == NO_MODEL)) return false;
        for (int m = NO_MODEL; m < (int) p->nmodels && !in.bad; m++) {
            const Att *a = m == NO_MODEL ? &p->base : &p->models[m].att;
            if (!a->k) continue;
            for (int k = 0; k < 5; k++) get(&in);
            uint64_t nas = get(&in);
            if (in.bad || nas > (in.n - in.at) / 40) return false;
            for (uint64_t j = 0; j < nas; j++) {
                get(&in);
                if (get(&in) >= w->ncells) return false;
                for (int k = 0; k < 3; k++) get(&in);
            }
            uint64_t need = get(&in);
            if (in.bad || need > in.n - in.at) return false;
            in.at += (size_t) need;
        }
    }
    return !in.bad && in.at == in.n;
}

QwStatus qw_restore(QWorld *w, const void *bytes, size_t len)
{
    In in = { bytes, len, 0, false };
    if (get(&in) != SNAP_MAGIC || in.bad) return fail(w, QW_ESNAPSHOT, "the bytes are not a snapshot of a world");
    if (get(&in) != structure_hash(w))
        return fail(w, QW_ESNAPSHOT, "the snapshot is of a world with another structure: the phenomena, the cells, the models "
                                  "and the kernels must be the same");
    if (!snapshot_whole(w, bytes, len)) return fail(w, QW_ESNAPSHOT, "the snapshot is cut or damaged");
    uint64_t head[9];
    for (size_t i = 0; i < 9; i++) head[i] = get(&in);
    clear_staging(w);
    /* the base of each cell that is state */
    for (size_t c = 0; c < w->ncells; c++) {
        QInterval b = { (int64_t) get(&in), (int64_t) get(&in) };
        if (in.bad) break;
        if (w->cells[c].derived) continue;
        QnStatus s = set_cell(w, (QnCell) c) ? qn_stage_set(w->net, (QnCell) c, (uint64_t) b.lo) : qn_stage(w->net, (QnCell) c, b);
        if (s != QN_OK) return fail(w, QW_ESNAPSHOT, "the snapshot has a value that the cell '%s' cannot take", w->cells[c].path);
    }
    for (size_t g = 0; g < qn_group_count(w->net); g++) {
        bool on = get(&in) != 0;
        if (!in.bad) qn_group_enable(w->net, (QnGroup) g, on);
    }
    /* the active model and the life of each phenomenon, before the kernels bind again */
    size_t phens_at = in.at;
    for (size_t i = 0; i < w->nphens && !in.bad; i++) {
        Phen *p = &w->phens[i];
        int active = (int) (int64_t) get(&in);
        bool alive = get(&in) != 0;
        uint64_t flipped = get(&in), predicts = get(&in);
        p->active = active;
        if (alive != p->alive) {
            p->alive = alive;
            if (alive) {
                QwStatus st = create_kernels(w, (QwPhen) i);
                if (st) return st;
            } else {
                destroy_kernels(w, (QwPhen) i);
            }
        }
        p->flipped = flipped;
        p->predicts = predicts;
        /* skip the kernels of this phenomenon: the second pass reads them */
        for (int m = NO_MODEL; m < (int) p->nmodels && !in.bad; m++) {
            if (!att_of(w, (QwPhen) i, m)->k) continue;
            for (int k = 0; k < 5; k++) get(&in);
            in.at += (size_t) get(&in) * 40;
            in.at += (size_t) get(&in);
        }
    }
    QwStatus st = rebind_all(w);
    if (st) return st;
    if (qn_restore_epoch(w->net, head[1]) != QN_OK) return fail(w, QW_ESNAPSHOT, "the network cannot open the epoch of the snapshot");
    w->now = (int64_t) head[0];
    w->boundaries = head[2];
    w->advances = head[3];
    w->switches = head[4];
    w->starts = head[5];
    w->ends = head[6];
    w->life_step = head[7];
    w->life_round = head[8];
    w->nlog = 0;
    if ((st = settle(w))) return st;
    /* the prediction, the assumptions, the event count and the private state of each kernel */
    in.at = phens_at;
    for (size_t i = 0; i < w->nphens; i++) {
        Phen *p = &w->phens[i];
        for (int k = 0; k < 4; k++) get(&in);
        for (int m = NO_MODEL; m < (int) p->nmodels; m++) {
            Att *a = att_of(w, (QwPhen) i, m);
            if (!a->k) continue;
            a->predicted = get(&in) != 0;
            a->pred.horizon = (int64_t) get(&in);
            a->pred.event_lo = (int64_t) get(&in);
            a->pred.event_hi = (int64_t) get(&in);
            a->events = get(&in);
            size_t nas = (size_t) get(&in);
            if (!grow((void **) &a->as, &a->capas, nas, sizeof *a->as)) return fail(w, QW_ENOMEM, "out of memory");
            a->nas = nas;
            for (size_t j = 0; j < nas; j++) {
                a->as[j].slot = (uint32_t) get(&in);
                a->as[j].cell = (QnCell) get(&in);
                a->as[j].range.lo = (int64_t) get(&in);
                a->as[j].range.hi = (int64_t) get(&in);
                a->as[j].explicit_ = get(&in) != 0;
            }
            size_t need = (size_t) get(&in);
            if (need) {
                if (!a->inst || !a->k->restore || a->k->restore(a->inst, in.p + in.at, need) != QK_OK)
                    return fail(w, QW_EKERNEL, "%s: kernel '%s' cannot take its state from the snapshot", p->name, a->k->name);
                in.at += need;
            }
        }
    }
    return QW_OK;
}

uint64_t qw_hash(const QWorld *w)
{
    /* Each phenomenon and each cell gives one hash, from its name or path
     * and its state. The sum of those hashes does not depend on the order of
     * declaration (ADR 0013). */
    const uint64_t seed = 0xcbf29ce484222325ULL;
    uint64_t sum = 0;
    for (size_t i = 0; i < w->nphens; i++) {
        /* the active model by its name: the order of the models is not state */
        const Phen *p = &w->phens[i];
        const char *model = p->active == NO_MODEL ? "" : p->models[p->active].name;
        sum += fnv(fnv(seed, p->ident), qr_name_hash(model, strlen(model)));
    }
    for (size_t c = 0; c < w->ncells; c++) {
        if (w->cells[c].derived) continue;       /* not state */
        QInterval b = qn_base(w->net, (QnCell) c);
        sum += fnv(fnv(fnv(seed, w->cells[c].hash), (uint64_t) b.lo), (uint64_t) b.hi);
    }
    uint64_t h = fnv(seed, (uint64_t) w->now);
    h = fnv(h, qn_epoch(w->net));
    return fnv(h, sum);
}
