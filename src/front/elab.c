#include <stdarg.h>
#include <stdio.h>

#include "front/front.h"
#include "quad/kernels.h"

/* The elaborator: stage 3 of the front end (ADR 0011 section 3, ADR 0016).
 *
 * It reads the result of a load and makes the IR. Each instance of a type
 * below #Phenomenon is a phenomenon. The work has three passes:
 *
 *   1. For each phenomenon: its connections, its cells, its aliases and its
 *      ports.
 *   2. A check of each connection against the port that it names.
 *   3. For each phenomenon: the propagators of its laws, and its kernel.
 *
 * Then the kinds, the enumerations and the dependency graph, in orders that
 * do not depend on the order of the source. */

/* One path to a value. owner: the type of the structure that holds the field. */
typedef struct { const QfNode *node; Sym text; uint32_t n; int port; bool set; uint32_t feat, owner; } Leaf;
typedef struct { U32Vec path; uint32_t target; U32Vec sub; uint32_t port; } Conn;

typedef struct {
    Front            *f;
    QfOp             *op;
    const QiOptions  *opt;
    QfArena          *mem;
    uint32_t          tphen, tport, fevaluator;
    U32Vec            insts;        /* for each phenomenon: its index in f->insts */
    U64Map            phenidx;      /* index in f->insts + 1 → phenomenon + 1 */
    VEC(QiPhen)       phens;
    VEC(QiCell)       cells;
    VEC(QType)        ctypes;       /* for each cell; no meaning for a set cell */
    VEC(QiAlias)      aliases;
    VEC(QiPort)       ports;
    VEC(QiProp)       props;
    VEC(QiKernelUse)  kernels;
    VEC(Conn)         conns;        /* every connection, with its phenomenon in ports[port] */
    U64Map            paths;        /* Sym of a path → cell + 1 */
    VEC(QiKind)       kinds;        /* the kinds that cells use, named and anonymous */
    U64Map            lawcount;     /* Sym of the name of a law → the derived cells that it has */
    U64Map            lawsdone;     /* Sym of the name and the text of a law → 1 */
    U32Vec            usedenums;    /* types */
    VEC(QiEnum)       enums;        /* parallel to usedenums */
} Elab;

static void err(Elab *e, Loc loc, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void err(Elab *e, Loc loc, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    qf_diag(e->f, true, loc, "%s", msg);
}

static const char *keep(Elab *e, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = qf_alloc_raw(e->mem, n);
    memcpy(p, s, n);
    return p;
}

/* The first declaration of an instance, by location: where its errors point. */
static Loc inst_loc(const InstInfo *in)
{
    Loc best = in->decls.v[0]->loc;
    for (size_t i = 1; i < in->decls.n; i++) {
        Loc l = in->decls.v[i]->loc;
        int c = strcmp(l.file ? l.file : "", best.file ? best.file : "");
        if (c < 0 || (c == 0 && (l.line < best.line || (l.line == best.line && l.col < best.col)))) best = l;
    }
    return best;
}

/* "name.a.b": the path of a place in a phenomenon, as the world writes it.
 * A path has the labels of the fields, without their packages. */
static Sym path_sym(Elab *e, const char *name, const uint32_t *path, size_t n)
{
    Buf b = { 0 };
    qf_buf_puts(&b, name);
    for (size_t i = 0; i < n; i++) {
        qf_buf_putc(&b, '.');
        qf_buf_puts(&b, e->f->feats.v[path[i]].label);
    }
    Sym s = qf_sym(e->f, b.s);
    qf_buf_free(&b);
    return s;
}

static void rel_text(Elab *e, Buf *b, const uint32_t *path, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (i) qf_buf_putc(b, '.');
        qf_buf_puts(b, e->f->feats.v[path[i]].label);
    }
}

/* A name that the world accepts for a phenomenon or an atom: letters, digits and '_'. */
static bool world_name(const char *s)
{
    if (!*s) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_')) return false;
    return true;
}

static bool starts_with(const uint32_t *path, size_t n, const U32Vec *prefix)
{
    return n >= prefix->n && (!prefix->n || !memcmp(path, prefix->v, prefix->n * sizeof(uint32_t)));
}

/* --- pass 1: connections, cells, aliases and ports ------------------------------------------- */

typedef struct {
    Elab           *e;
    uint32_t        phen;
    const InstInfo *in;
    Loc             loc;
    size_t          conn0;       /* the phenomenon's connections start here in e->conns */
    VEC(Leaf)       leaves;
    U32Vec          path;
    VEC(const QfNode *) stack;
    int             port;        /* the port that the walk is inside, or -1 */
    uint32_t        owner;       /* the type of the structure that the walk is in */
} Walk;

static uint32_t add_port(Elab *e, uint32_t phen, const uint32_t *path, size_t n, uint32_t type, QiPortState state)
{
    Buf b = { 0 };
    rel_text(e, &b, path, n);
    QiPort p = { phen, keep(e, b.s), keep(e, e->f->types.v[type].name), state, NULL, QI_NONE };
    qf_buf_free(&b);
    VEC_PUSH(e->ports, p);
    return (uint32_t) e->ports.n - 1;
}

/* Reads the reference records of a phenomenon. A reference to a port of
 * another phenomenon is a connection. */
static void find_conns(Walk *w)
{
    Elab *e = w->e;
    Front *f = e->f;
    InstRefVec refs = { 0 };
    qf_refs(f, w->in->node, &w->in->refs, &refs);
    /* shorter paths first: a reference under a connection belongs to the copy */
    for (size_t len = 0, left = refs.n; left; len++)
        for (size_t i = 0; i < refs.n; i++) {
            const InstRef *r = &refs.v[i];
            if (r->path.n != len) continue;
            left--;
            uint64_t v, pv;
            if (!u64map_get(&f->instidx, (uint64_t) (uintptr_t) r->inst, &v) || !u64map_get(&e->phenidx, v, &pv))
                continue;                            /* a copy of data: its numbers are this phenomenon's cells */
            bool inside = false, twice = false;
            for (size_t c = w->conn0; c < e->conns.n; c++) {
                const Conn *o = &e->conns.v[c];
                if (o->path.n == len && starts_with(r->path.v, len, &o->path)) twice = true;
                else if (starts_with(r->path.v, len, &o->path)) inside = true;
            }
            if (inside) continue;
            Buf here = { 0 }, there = { 0 };
            rel_text(e, &here, r->path.v, r->path.n);
            qf_buf_puts(&there, r->inst);
            if (r->sub.n) qf_buf_putc(&there, '.');
            rel_text(e, &there, r->sub.v, r->sub.n);
            const QfNode *target = node_at(f->insts.v[v - 1].node, r->sub.v, r->sub.n);
            const QfNode *local = node_at(w->in->node, r->path.v, r->path.n);
            bool ok = false;
            if (twice)
                err(e, w->loc, "%s.%s has two connections: a port connects to one port", w->in->name, here.s);
            else if (!target || !r->sub.n || !is_subtype(f, target->type, e->tport))
                err(e, w->loc,
                    "%s.%s refers to %s, which is not a port: a phenomenon reaches another phenomenon only through a port",
                    w->in->name, here.s, there.s);
            else if (r->path.n != 1)
                err(e, w->loc, "%s.%s connects to %s: a connection must be a field of the phenomenon", w->in->name,
                    here.s, there.s);
            else if (local) {
                bool fwd, bwd;
                qf_subsumes(e->op, local, target, &fwd, &bwd);
                if (fwd && bwd) ok = true;
                else
                    err(e, w->loc,
                        "%s.%s connects to %s and adds a constraint to it: a connection cannot change a port in this stage",
                        w->in->name, here.s, there.s);
            }
            if (!twice) {
                Conn c = { { 0 }, (uint32_t) pv - 1, { 0 }, QI_NONE };
                for (size_t k = 0; k < r->path.n; k++) VEC_PUSH(c.path, r->path.v[k]);
                for (size_t k = 0; k < r->sub.n; k++) VEC_PUSH(c.sub, r->sub.v[k]);
                if (ok) {
                    c.port = add_port(e, w->phen, r->path.v, r->path.n, local->type, QI_PORT_CONNECTED);
                    e->ports.v[c.port].target = keep(e, there.s);
                    e->ports.v[c.port].target_phen = c.target;
                }
                VEC_PUSH(e->conns, c);
            }
            qf_buf_free(&here);
            qf_buf_free(&there);
        }
    qf_refs_free(&refs);
}

static void leaf(Walk *w, const QfNode *n, bool set)
{
    Elab *e = w->e;
    Leaf l = { n, path_sym(e, w->in->name, w->path.v, w->path.n), (uint32_t) w->path.n, w->port, set,
               w->path.v[w->path.n - 1], w->owner };
    VEC_PUSH(w->leaves, l);
}

static void walk(Walk *w, const QfNode *n)
{
    Elab *e = w->e;
    Front *f = e->f;
    size_t len = w->path.n;
    for (size_t c = w->conn0; c < e->conns.n; c++)
        if (e->conns.v[c].path.n == len && starts_with(w->path.v, len, &e->conns.v[c].path)) return;
    TypeKind tk = f->types.v[n->type].kind;
    if (n->flags & NF_VALUE) {
        if (tk == TK_KIND || tk == TK_DIMT) leaf(w, n, false);
        else if (tk == TK_NUMBER) {
            Buf b = { 0 };
            rel_text(e, &b, w->path.v, len);
            err(e, w->loc, "%s.%s is a number with no kind and no dimension: a cell needs one, as in `%s: %%energy`",
                w->in->name, b.s, f->feats.v[w->path.v[len - 1]].label);
            qf_buf_free(&b);
        }
        return;                                      /* a text or a constraint is not state */
    }
    if (len && is_subtype(f, n->type, e->tphen)) {
        Buf b = { 0 };
        rel_text(e, &b, w->path.v, len);
        err(e, w->loc, "%s.%s is a phenomenon inside a phenomenon, which is not in this stage", w->in->name, b.s);
        qf_buf_free(&b);
        return;
    }
    bool opened = false;
    if (len && w->port < 0 && is_subtype(f, n->type, e->tport)) {
        w->port = (int) add_port(e, w->phen, w->path.v, len, n->type, QI_PORT_VIEW);
        opened = true;
    }
    if (!n->narcs) {
        if (len && n->type != f->atomroot && is_subtype(f, n->type, f->atomroot)) leaf(w, n, true);
    } else {
        bool cyclic = false;
        for (size_t i = 0; i < w->stack.n; i++) cyclic |= w->stack.v[i] == n;
        if (!cyclic) {
            VEC_PUSH(w->stack, n);
            uint32_t outer = w->owner;
            w->owner = n->type;
            for (uint32_t i = 0; i < n->narcs; i++) {
                VEC_PUSH(w->path, n->arcs[i].feat);
                walk(w, n->arcs[i].val);
                w->path.n--;
            }
            w->owner = outer;
            w->stack.n--;
        }
    }
    if (opened) w->port = -1;
}

static int cmp_atom(const void *a, const void *b) { return strcmp(*(const char *const *) a, *(const char *const *) b); }

/* The name of an atom as the world writes it: the text of a string, or the
 * bare name of true and false. */
static const char *atom_name(Elab *e, Sym type_name)
{
    if (type_name[0] != '"') return keep(e, type_name);
    Buf b = { 0 };
    qf_string_decode(type_name, &b);
    const char *s = keep(e, b.s ? b.s : "");
    qf_buf_free(&b);
    return s;
}

/* The enumeration of a set cell: every atom that the field can have by its
 * declaration. Unrelated types can each have a field with one name (ADR 0017), so the
 * declaration is that of the most general supertype of the owner that gives
 * the field an enumeration. QI_NONE with a diagnostic. */
static uint32_t enum_of(Walk *w, const Leaf *l)
{
    Elab *e = w->e;
    Front *f = e->f;
    uint32_t u = QF_BOTTOM;
    for (uint32_t t = 0; t < f->types.n; t++) {
        if (!is_subtype(f, l->owner, t)) continue;
        const QfNode *x = f->types.v[t].expansion;
        const QfNode *decl = x && !(x->flags & NF_VALUE) ? node_arc(x, l->feat) : NULL;
        if (!decl || decl->type == f->atomroot || !is_subtype(f, decl->type, f->atomroot)) continue;
        if (!is_subtype(f, l->node->type, decl->type)) continue;
        if (u == QF_BOTTOM || is_subtype(f, u, decl->type)) u = decl->type;
    }
    if (u == QF_BOTTOM) u = l->node->type;
    for (size_t i = 0; i < e->usedenums.n; i++)
        if (e->usedenums.v[i] == u) return (uint32_t) i;
    VEC(const char *) atoms = { 0 };
    for (uint32_t t = 0; t < f->types.n; t++)
        if (f->types.v[t].kind == TK_ATOM && is_subtype(f, t, u)) VEC_PUSH(atoms, atom_name(e, f->types.v[t].name));
    if (!atoms.n || atoms.n > QN_MAXATOMS) {
        err(e, w->loc, "%s: the enumeration %s has %zu atoms, and a set cell has 1 to %d", l->text, f->types.v[u].name,
            atoms.n, QN_MAXATOMS);
        VEC_FREE(atoms);
        return QI_NONE;
    }
    for (size_t i = 0; i < atoms.n; i++)
        if (!world_name(atoms.v[i])) {
            err(e, w->loc, "%s: the atom \"%s\" needs a name of letters, digits and '_' to be in a set cell", l->text,
                atoms.v[i]);
            VEC_FREE(atoms);
            return QI_NONE;
        }
    qsort(atoms.v, atoms.n, sizeof *atoms.v, cmp_atom);
    QiEnum en = { keep(e, f->types.v[u].name), qf_alloc(e->mem, atoms.n * sizeof(char *)), (uint32_t) atoms.n };
    memcpy(en.atoms, atoms.v, atoms.n * sizeof(char *));
    VEC_FREE(atoms);
    VEC_PUSH(e->usedenums, u);
    VEC_PUSH(e->enums, en);
    return (uint32_t) e->usedenums.n - 1;
}

static uint32_t use_kind(Elab *e, uint32_t kindix)
{
    const KindInfo *k = &e->f->kinds.v[kindix];
    for (size_t i = 0; i < e->kinds.n; i++)
        if (!e->kinds.v[i].anonymous && !strcmp(e->kinds.v[i].name, k->name)) return (uint32_t) i;
    Buf dim = { 0 };
    qf_dim_format(e->f, &dim, &e->f->dims.v[k->dimix].dim);
    QiKind qk = { keep(e, k->name), keep(e, dim.s), k->scale, k->conserved, k->rounding, false };
    qf_buf_free(&dim);
    VEC_PUSH(e->kinds, qk);
    return (uint32_t) e->kinds.n - 1;
}

/* An anonymous kind: a dimension and a scale (ADR 0001, ADR 0018). */
static uint32_t use_anonymous(Elab *e, const QDim *dim, int8_t scale)
{
    Buf d = { 0 }, name = { 0 };
    qf_dim_format(e->f, &d, dim);
    qf_buf_printf(&name, "dim[%s]@%d", d.s, scale);
    uint32_t at = QI_NONE;
    for (size_t i = 0; i < e->kinds.n && at == QI_NONE; i++)
        if (e->kinds.v[i].anonymous && !strcmp(e->kinds.v[i].name, name.s)) at = (uint32_t) i;
    if (at == QI_NONE) {
        QiKind qk = { keep(e, name.s), keep(e, d.s), scale, false, Q_ROUND_HALF_EVEN, true };
        VEC_PUSH(e->kinds, qk);
        at = (uint32_t) e->kinds.n - 1;
    }
    qf_buf_free(&d);
    qf_buf_free(&name);
    return at;
}

/* The kind of the IR for a quantity type: its kind, if the type has the
 * scale of that kind, and else an anonymous kind. */
static uint32_t use_qtype(Elab *e, const QType *qt)
{
    if (qt->kind && e->f->kinds.v[qt->kind - 1].scale == qt->scale) return use_kind(e, qt->kind - 1);
    return use_anonymous(e, &qt->dim, qt->scale);
}

static void add_cell(Walk *w, const Leaf *l)
{
    Elab *e = w->e;
    Front *f = e->f;
    QiCell c = { keep(e, l->text), w->phen, QI_INTERVAL, 0, { 0, 0 }, 0, false, false };
    QType qt;
    memset(&qt, 0, sizeof qt);
    if (u64map_get(&e->paths, (uint64_t) (uintptr_t) l->text, NULL)) {
        /* two packages can each give a field with one label to one structure */
        err(e, w->loc, "%s is the path of two values: two fields of the phenomenon have the same label", l->text);
        return;
    }
    if (l->set) {
        uint32_t en = enum_of(w, l);
        if (en == QI_NONE) return;
        c.domain = QI_SET;
        c.kind = en;
        /* the atoms of the enumeration are in the order of their names */
        for (uint32_t t = 0; t < f->types.n; t++) {
            if (f->types.v[t].kind != TK_ATOM || !is_subtype(f, t, l->node->type)) continue;
            const char *name = atom_name(e, f->types.v[t].name);
            for (uint32_t a = 0; a < e->enums.v[en].natoms; a++)
                if (!strcmp(e->enums.v[en].atoms[a], name)) c.mask |= (uint64_t) 1 << a;
        }
    } else if (f->types.v[l->node->type].kind == TK_DIMT) {
        /* a dimension and no kind: an anonymous kind with the default scale of the dimension */
        QDim dim = f->dims.v[f->types.v[l->node->type].dimix].dim;
        int8_t scale = qdim_default_scale(&dim, f->tick_scale);
        QfDec d = { l->node->lo, l->node->hi, l->node->exp, (l->node->flags & NF_LO_OPEN) != 0,
                    (l->node->flags & NF_HI_OPEN) != 0 };
        const char *why = qf_dec_to_quanta(d, scale, &c.base.lo, &c.base.hi);
        if (why) {
            err(e, w->loc, "%s has no kind, so it has the scale %d of its dimension, and its value does not fit: %s. "
                "Give the field a kind with another scale", l->text, scale, why);
            return;
        }
        c.kind = use_anonymous(e, &dim, scale);
        qt = qtype_make(0, dim, scale);
    } else {
        const TypeInfo *ti = &f->types.v[l->node->type];
        const KindInfo *k = &f->kinds.v[ti->kindix];
        c.kind = use_kind(e, ti->kindix);
        c.base = (QInterval) { l->node->lo, l->node->hi };
        c.stock = k->conserved;
        qt = qtype_make(ti->kindix + 1, f->dims.v[k->dimix].dim, k->scale);
        qt.rounding = k->rounding;
        if (c.stock && (c.base.lo != c.base.hi || c.base.lo == Q_NINF || c.base.lo == Q_INF)) {
            err(e, w->loc, "the stock %s needs an exact value: only transfers change a stock, so its first value is a point",
                l->text);
            return;
        }
    }
    VEC_PUSH(e->cells, c);
    VEC_PUSH(e->ctypes, qt);
    u64map_put(&e->paths, (uint64_t) (uintptr_t) l->text, e->cells.n);
}

static void phen_cells(Elab *e, uint32_t phen)
{
    Front *f = e->f;
    Walk w = { 0 };
    w.e = e;
    w.phen = phen;
    w.in = &f->insts.v[e->insts.v[phen]];
    w.loc = inst_loc(w.in);
    w.conn0 = e->conns.n;
    w.port = -1;
    find_conns(&w);
    walk(&w, w.in->node);

    /* A value is one node, and it can stand at several paths. Its cell has
     * the shortest path outside a port, and the other paths are aliases. A
     * value that stands only in a port has no cell: the port is open. */
    bool *done = qf_xcalloc(w.leaves.n ? w.leaves.n : 1, sizeof *done);
    for (size_t i = 0; i < w.leaves.n; i++) {
        if (done[i]) continue;
        const Leaf *best = NULL;
        for (size_t j = i; j < w.leaves.n; j++) {
            const Leaf *l = &w.leaves.v[j];
            if (l->node != w.leaves.v[i].node) continue;
            done[j] = true;
            if (l->port >= 0) continue;
            if (!best || l->n < best->n || (l->n == best->n && strcmp(l->text, best->text) < 0)) best = l;
        }
        size_t ncells = e->cells.n;
        if (best) add_cell(&w, best);
        bool made = e->cells.n > ncells;
        for (size_t j = i; j < w.leaves.n; j++) {
            const Leaf *l = &w.leaves.v[j];
            if (l->node != w.leaves.v[i].node || l == best) continue;
            if (!best) e->ports.v[l->port].state = QI_PORT_OPEN;
            else if (made && u64map_get(&e->paths, (uint64_t) (uintptr_t) l->text, NULL))
                err(e, w.loc, "%s is the path of two values: two fields of the phenomenon have the same label", l->text);
            else if (made) {
                QiAlias a = { keep(e, l->text), (uint32_t) e->cells.n - 1 };
                VEC_PUSH(e->aliases, a);
                u64map_put(&e->paths, (uint64_t) (uintptr_t) l->text, e->cells.n);
            }
        }
    }
    free(done);
    VEC_FREE(w.leaves);
    VEC_FREE(w.path);
    VEC_FREE(w.stack);
}

/* --- pass 2: each connection names a port that is a view ------------------------------------ */

static void check_conns(Elab *e)
{
    Front *f = e->f;
    for (size_t c = 0; c < e->conns.n; c++) {
        const Conn *cn = &e->conns.v[c];
        if (cn->port == QI_NONE) continue;
        const QiPort *port = &e->ports.v[cn->port];
        Buf sub = { 0 };
        rel_text(e, &sub, cn->sub.v, cn->sub.n);
        const QiPort *target = NULL;
        for (size_t p = 0; p < e->ports.n; p++)
            if (e->ports.v[p].phen == cn->target && !strcmp(e->ports.v[p].name, sub.s)) target = &e->ports.v[p];
        qf_buf_free(&sub);
        Loc loc = inst_loc(&f->insts.v[e->insts.v[port->phen]]);
        const char *name = e->phens.v[port->phen].name;
        if (!target)
            err(e, loc, "%s.%s connects to %s, which is a part of a port: connect to the whole port", name, port->name,
                port->target);
        else if (target->state == QI_PORT_CONNECTED)
            err(e, loc, "%s.%s connects to %s, which is itself a connection to %s: connect to that port", name, port->name,
                port->target, target->target);
        else if (target->state == QI_PORT_OPEN)
            err(e, loc,
                "%s.%s connects to %s, which has a field with no cell: each field of that port must be a field of its phenomenon",
                name, port->name, port->target);
    }
}

/* --- pass 3: laws and kernels ---------------------------------------------------------------- */

/* The cell at a path of a phenomenon, through a connection if the path
 * starts with a connected port. QI_NONE if there is none; then *open is the
 * open port that the path starts with, if it does. */
static uint32_t find_cell(Elab *e, uint32_t phen, const char *rel, const char **open)
{
    Buf b = { 0 };
    qf_buf_printf(&b, "%s.%s", e->phens.v[phen].name, rel);
    uint64_t v = 0;
    bool found = u64map_get(&e->paths, (uint64_t) (uintptr_t) qf_sym(e->f, b.s), &v);
    if (open) *open = NULL;
    if (!found) {
        size_t first = strcspn(rel, ".");
        for (size_t p = 0; p < e->ports.n && !found; p++) {
            const QiPort *port = &e->ports.v[p];
            if (port->phen != phen || strlen(port->name) != first || memcmp(port->name, rel, first)) continue;
            if (port->state == QI_PORT_CONNECTED && rel[first]) {
                b.len = 0;
                qf_buf_printf(&b, "%s%s", port->target, rel + first);
                found = u64map_get(&e->paths, (uint64_t) (uintptr_t) qf_sym(e->f, b.s), &v);
            } else if (port->state == QI_PORT_OPEN && open)
                *open = port->name;
        }
    }
    qf_buf_free(&b);
    return found ? (uint32_t) v - 1 : QI_NONE;
}

typedef struct {
    Elab           *e;
    uint32_t        phen;
    const QfRecUse *use;
    const char     *law;         /* the text of the law */
    const char     *name;        /* its path: roomA.storage */
    Loc             loc;
} Law;

/* The cell of a reference in a law. QI_NONE with a diagnostic. */
static uint32_t operand(Law *l, const X *x)
{
    Elab *e = l->e;
    const ArithRec *rec = l->use->rec;
    for (size_t t = 0; t < rec->tags.n; t++) {
        if (rec->tags.v[t].name != x->u.name) continue;
        Buf b = { 0 };
        rel_text(e, &b, l->use->prefix.v, l->use->prefix.n);
        if (b.len && rec->tags.v[t].path.n) qf_buf_putc(&b, '.');
        rel_text(e, &b, rec->tags.v[t].path.v, rec->tags.v[t].path.n);
        const char *open;
        uint32_t c = find_cell(e, l->phen, b.s ? b.s : "", &open);
        if (c == QI_NONE) {
            if (open)
                err(e, l->loc, "the law %s reads %s.%s, and the port %s.%s is not connected", l->law,
                    e->phens.v[l->phen].name, b.s, e->phens.v[l->phen].name, open);
            else
                err(e, l->loc, "the law %s reads %s.%s, which is not a cell", l->law, e->phens.v[l->phen].name,
                    b.s ? b.s : "");
        } else if (e->cells.v[c].domain == QI_SET) {
            err(e, l->loc, "the law %s uses %s, which is a set of atoms: arithmetic needs quantities", l->law,
                e->cells.v[c].path);
            c = QI_NONE;
        }
        qf_buf_free(&b);
        return c;
    }
    err(e, l->loc, "the law %s: %s is not bound to a field", l->law, x->u.name);
    return QI_NONE;
}

static void unsupported(Law *l)
{
    err(l->e, l->loc,
        "the law %s has a form that the elaborator cannot lower. A law is an equation or an inequality "
        "(< <= > >=) of references, numbers, and + - * /",
        l->law);
}

static bool fits(const QType *z, const QType *x, const QType *y, QiOp op, QStatus *why)
{
    QType t1, t2;
    QConv k;
    switch (op) {
    case QI_EQUAL: case QI_LESS_EQUAL: case QI_LESS: return !(*why = qtype_sum(z, x, &t1));
    case QI_SUM: return !(*why = qtype_sum(x, y, &t1)) && !(*why = qtype_sum(&t1, z, &t2));
    case QI_DIFFERENCE: return fits(x, z, y, QI_SUM, why);                 /* x = z + y */
    case QI_PRODUCT:
        return !(*why = qtype_mul_plan(x, y, z, &k)) && !(*why = qtype_div_plan(z, y, x, &k)) &&
               !(*why = qtype_div_plan(z, x, y, &k));
    case QI_QUOTIENT: return fits(x, z, y, QI_PRODUCT, why);               /* x = z * y */
    }
    return false;
}

static void misfit(Law *l, QStatus why)
{
    err(l->e, l->loc, "the law %s does not fit the kinds of its quantities: %s", l->law, q_status_str(why));
}

/* A derived cell of a law (ADR 0018): the value of an inner expression, or
 * a constant. Its path is the name of the law and a number, in the order in
 * which the expression makes the cells. */
static uint32_t derived(Law *l, const QType *qt, QInterval base)
{
    Elab *e = l->e;
    uint64_t key = (uint64_t) (uintptr_t) qf_sym(e->f, l->name), n = 0;
    u64map_get(&e->lawcount, key, &n);
    u64map_put(&e->lawcount, key, ++n);
    Buf b = { 0 };
    qf_buf_printf(&b, "%s.%llu", l->name, (unsigned long long) n);
    Sym path = qf_sym(e->f, b.s);
    qf_buf_free(&b);
    if (u64map_get(&e->paths, (uint64_t) (uintptr_t) path, NULL)) {
        err(e, l->loc, "the law %s needs the path %s for an inner value, and a field has that path", l->law, path);
        return QI_NONE;
    }
    QiCell c = { keep(e, path), l->phen, QI_INTERVAL, use_qtype(e, qt), base, 0, false, true };
    VEC_PUSH(e->cells, c);
    VEC_PUSH(e->ctypes, *qt);
    u64map_put(&e->paths, (uint64_t) (uintptr_t) path, e->cells.n);
    return (uint32_t) e->cells.n - 1;
}

static void emit(Law *l, QiOp op, uint32_t z, uint32_t a, uint32_t b)
{
    Elab *e = l->e;
    QiProp p = { l->phen, op, { z, a, b }, keep(e, l->name), keep(e, l->law), l->loc.file ? keep(e, l->loc.file) : NULL,
                 l->loc.line, l->loc.col };
    VEC_PUSH(e->props, p);
}

static uint32_t cell_of(Law *l, const X *x);

/* z = a op b for an expression. z is the cell of the result, or QI_NONE:
 * then a new derived cell is the result. The result, or QI_NONE with a diagnostic. */
static uint32_t lower_arith(Law *l, const X *x, uint32_t z)
{
    Elab *e = l->e;
    uint32_t a = cell_of(l, x->u.ar.a);
    if (a == QI_NONE) return QI_NONE;
    uint32_t b = cell_of(l, x->u.ar.b);
    if (b == QI_NONE) return QI_NONE;
    QiOp op = x->u.ar.op == '+' ? QI_SUM : x->u.ar.op == '-' ? QI_DIFFERENCE : x->u.ar.op == '*' ? QI_PRODUCT : QI_QUOTIENT;
    QType ta = e->ctypes.v[a], tb = e->ctypes.v[b];
    QStatus why = Q_OK;
    if (z == QI_NONE) {
        QType t;
        why = op == QI_PRODUCT ? qtype_mul(&ta, &tb, &t) : op == QI_QUOTIENT ? qtype_div(&ta, &tb, &t) : qtype_sum(&ta, &tb, &t);
        if (why) { misfit(l, why); return QI_NONE; }
        z = derived(l, &t, qi_top());
        if (z == QI_NONE) return QI_NONE;
    } else {
        QType tz = e->ctypes.v[z];
        if (!fits(&tz, &ta, &tb, op, &why)) { misfit(l, why); return QI_NONE; }
    }
    emit(l, op, z, a, b);
    return z;
}

/* The cell that has the value of an expression: the cell of a reference, a
 * constant for a number, or a derived cell for an operation. */
static uint32_t cell_of(Law *l, const X *x)
{
    Elab *e = l->e;
    if (x->k == X_TAG) return operand(l, x);
    if (x->k == X_ARITH) return lower_arith(l, x, QI_NONE);
    if (x->k == X_NUM) {
        /* a number is exact with its own scale: 273.15K is 27315 with the scale -2 */
        QDim dim;
        int64_t value;
        int exp;
        char why[200];
        if (!qf_literal(e->f, &x->u.num, &dim, &value, &exp, why, sizeof why)) {
            err(e, x->loc, "the law %s: %s", l->law, why);
            return QI_NONE;
        }
        while (value && value % 10 == 0 && exp < INT8_MAX) { value /= 10; exp++; }
        if (!value) exp = 0;
        if (exp < INT8_MIN || exp > INT8_MAX) {
            err(e, x->loc, "the law %s has a number with a scale out of range: give the value a field with a kind", l->law);
            return QI_NONE;
        }
        QType qt = qtype_make(0, dim, (int8_t) exp);
        return derived(l, &qt, qi_point(value));
    }
    unsupported(l);
    return QI_NONE;
}

static void lower(Elab *e, uint32_t phen, const QfRecUse *use)
{
    const X *x = use->rec->expr;
    char *text = x_text(x);
    Buf nb = { 0 }, rel = { 0 };
    rel_text(e, &rel, use->prefix.v, use->prefix.n);
    if (rel.len && use->rec->path.n) qf_buf_putc(&rel, '.');
    rel_text(e, &rel, use->rec->path.v, use->rec->path.n);
    qf_buf_printf(&nb, "%s%s%s", e->phens.v[phen].name, rel.len ? "." : "", rel.s ? rel.s : "");
    Law l = { e, phen, use, text, nb.s, x->loc };
    /* one law that two declarations state is one law */
    Buf key = { 0 };
    qf_buf_printf(&key, "%s\n%s", nb.s, text);
    uint64_t done = (uint64_t) (uintptr_t) qf_sym(e->f, key.s);
    qf_buf_free(&key);
    if (!u64map_get(&e->lawsdone, done, NULL)) {
        u64map_put(&e->lawsdone, done, 1);
        if (x->k == X_CMP && x->u.ar.op == 'e') {
            const X *lhs = x->u.ar.a, *rhs = x->u.ar.b;
            if (lhs->k != X_TAG && rhs->k == X_TAG) { const X *t = lhs; lhs = rhs; rhs = t; }
            if (lhs->k == X_TAG && rhs->k == X_ARITH) {
                /* a reference and one operation: the operation writes the cell of the reference */
                uint32_t z = operand(&l, lhs);
                if (z != QI_NONE) lower_arith(&l, rhs, z);
            } else {
                uint32_t a = cell_of(&l, lhs), b = a == QI_NONE ? QI_NONE : cell_of(&l, rhs);
                if (b != QI_NONE) {
                    QType ta = e->ctypes.v[a], tb = e->ctypes.v[b];
                    QStatus why = Q_OK;
                    if (!fits(&ta, &tb, NULL, QI_EQUAL, &why)) misfit(&l, why);
                    else emit(&l, QI_EQUAL, a, b, QI_NONE);
                }
            }
        } else if (x->k == X_CMP && (x->u.ar.op == 'l' || x->u.ar.op == '<' || x->u.ar.op == 'g' || x->u.ar.op == '>')) {
            bool greater = x->u.ar.op == 'g' || x->u.ar.op == '>', strict = x->u.ar.op == '<' || x->u.ar.op == '>';
            uint32_t a = cell_of(&l, x->u.ar.a), b = a == QI_NONE ? QI_NONE : cell_of(&l, x->u.ar.b);
            if (b != QI_NONE) {
                QType ta = e->ctypes.v[a], tb = e->ctypes.v[b];
                QStatus why = Q_OK;
                if (!fits(&ta, &tb, NULL, QI_LESS_EQUAL, &why)) misfit(&l, why);
                else emit(&l, strict ? QI_LESS : QI_LESS_EQUAL, greater ? b : a, greater ? a : b, QI_NONE);
            }
        } else if (x->k == X_ARITH) {
            /* a value that an expression gives: the field where it stands is the result */
            uint32_t z = find_cell(e, phen, rel.s ? rel.s : "", NULL);
            if (z == QI_NONE)
                err(e, l.loc, "%s has the value %s, and it is not a cell: give the field a kind", nb.s, text);
            else lower_arith(&l, x, z);
        } else {
            unsupported(&l);
        }
    }
    qf_buf_free(&nb);
    qf_buf_free(&rel);
    free(text);
}

static void phen_laws(Elab *e, uint32_t phen, size_t conn0, size_t conn1)
{
    const InstInfo *in = &e->f->insts.v[e->insts.v[phen]];
    QfRecUses uses = { 0 };
    qf_records(e->f, in->node, &in->ariths, &uses);
    for (size_t i = 0; i < uses.n; i++) {
        const QfRecUse *u = &uses.v[i];
        /* a record under a connection is a law of the other phenomenon */
        U32Vec at = { 0 };
        for (size_t k = 0; k < u->prefix.n; k++) VEC_PUSH(at, u->prefix.v[k]);
        for (size_t k = 0; k < u->rec->path.n; k++) VEC_PUSH(at, u->rec->path.v[k]);
        bool copied = false;
        for (size_t c = conn0; c < conn1; c++) copied |= starts_with(at.v, at.n, &e->conns.v[c].path);
        VEC_FREE(at);
        if (!copied) lower(e, phen, u);
    }
    qf_records_free(&uses);
}

const QkKernel *qi_find_kernel(const QiOptions *opt, const char *name)
{
    for (size_t i = 0; opt && i < opt->nkernels; i++)
        if (opt->kernels[i] && !strcmp(opt->kernels[i]->name, name)) return opt->kernels[i];
    const QkKernel *native[] = { qk_conduction() };
    for (size_t i = 0; i < sizeof native / sizeof native[0]; i++)
        if (!strcmp(native[i]->name, name)) return native[i];
    return NULL;
}

static void phen_kernel(Elab *e, uint32_t phen)
{
    Front *f = e->f;
    const InstInfo *in = &f->insts.v[e->insts.v[phen]];
    const QfNode *ev = e->fevaluator == QF_BOTTOM ? NULL : node_arc(in->node, e->fevaluator);
    if (!ev || !(ev->flags & NF_VALUE) || f->types.v[ev->type].kind != TK_TEXT || ev->lo != ev->hi) return;
    Loc loc = inst_loc(in);
    Buf name = { 0 };
    qf_string_decode((Sym) (intptr_t) ev->lo, &name);
    const QkKernel *k = qi_find_kernel(e->opt, name.s ? name.s : "");
    if (!k) {
        err(e, loc, "%s: unknown evaluator \"%s\"", in->name, name.s ? name.s : "");
        qf_buf_free(&name);
        return;
    }
    qf_buf_free(&name);
    QiKernelUse ku = { phen, keep(e, k->name), qf_alloc(e->mem, (k->nslots ? k->nslots : 1) * sizeof(QiBind)), k->nslots,
                       qf_alloc(e->mem, (k->ntransfers ? k->ntransfers : 1) * sizeof(QiTransfer)), k->ntransfers };
    bool ok = true;
    for (uint32_t s = 0; s < k->nslots; s++) {
        const char *open;
        uint32_t c = find_cell(e, phen, k->slots[s].name, &open);
        if (c == QI_NONE) {
            if (open)
                err(e, loc, "%s: the kernel %s needs %s.%s, and the port %s.%s is not connected", in->name, k->name,
                    in->name, k->slots[s].name, in->name, open);
            else
                err(e, loc, "%s: the kernel %s needs %s.%s, which is not a cell", in->name, k->name, in->name,
                    k->slots[s].name);
            ok = false;
        }
        ku.binds[s] = (QiBind) { keep(e, k->slots[s].name), k->slots[s].role, c };
    }
    for (uint32_t t = 0; ok && t < k->ntransfers; t++) {
        const QkTransfer *kt = &k->transfers[t];
        ku.transfers[t] = (QiTransfer) { keep(e, kt->name), kt->from == QK_NONE ? QI_NONE : ku.binds[kt->from].cell,
                                         kt->to == QK_NONE ? QI_NONE : ku.binds[kt->to].cell, kt->conserved };
    }
    if (ok) VEC_PUSH(e->kernels, ku);
}

/* --- orders and the dependency graph --------------------------------------------------------- */

/* An index with the name that gives its place in an order. */
typedef struct { const char *name; uint32_t ix; } Named;

static int cmp_named(const void *a, const void *b)
{
    return strcmp(((const Named *) a)->name, ((const Named *) b)->name);
}

static int cmp_cell(const void *a, const void *b)
{
    const QiCell *x = a, *y = b;
    return x->phen != y->phen ? (x->phen < y->phen ? -1 : 1) : strcmp(x->path, y->path);
}

static int cmp_alias(const void *a, const void *b)
{
    return strcmp(((const QiAlias *) a)->path, ((const QiAlias *) b)->path);
}

static int cmp_port(const void *a, const void *b)
{
    const QiPort *x = a, *y = b;
    return x->phen != y->phen ? (x->phen < y->phen ? -1 : 1) : strcmp(x->name, y->name);
}

static int cmp_prop(const void *a, const void *b)
{
    const QiProp *x = a, *y = b;
    if (x->phen != y->phen) return x->phen < y->phen ? -1 : 1;
    int c = strcmp(x->name, y->name);
    if (!c) c = strcmp(x->law, y->law);
    if (c) return c;
    if (x->op != y->op) return x->op < y->op ? -1 : 1;
    for (int i = 0; i < 3; i++)
        if (x->cell[i] != y->cell[i]) return x->cell[i] < y->cell[i] ? -1 : 1;
    return 0;
}

static int cmp_dep(const void *a, const void *b)
{
    const QiDep *x = a, *y = b;
    if (x->cell != y->cell) return x->cell < y->cell ? -1 : 1;
    if (x->kernel != y->kernel) return x->kernel ? 1 : -1;
    if (x->node != y->node) return x->node < y->node ? -1 : 1;
    return x->use != y->use ? (x->use < y->use ? -1 : 1) : 0;
}

static void *take(Elab *e, const void *v, size_t n, size_t size)
{
    void *p = qf_alloc(e->mem, (n ? n : 1) * size);
    if (n) memcpy(p, v, n * size);
    return p;
}

typedef VEC(QiDep) DepVec;

static void add_dep(DepVec *deps, uint32_t cell, bool kernel, uint32_t node, QiUse use)
{
    if (cell == QI_NONE) return;
    for (size_t i = 0; i < deps->n; i++) {
        const QiDep *d = &deps->v[i];
        if (d->cell == cell && d->kernel == kernel && d->node == node && d->use == use) return;
    }
    QiDep d = { cell, kernel, node, use };
    VEC_PUSH(*deps, d);
}

static QIr *finish(Elab *e)
{
    Front *f = e->f;
    QIr *ir = qf_alloc(e->mem, sizeof *ir);
    ir->mem = e->mem;
    ir->tick_scale = f->tick_scale;
    ir->nbases = (uint32_t) f->bases.n;
    ir->bases = qf_alloc(e->mem, (f->bases.n ? f->bases.n : 1) * sizeof(char *));
    for (size_t i = 0; i < f->bases.n; i++) ir->bases[i] = keep(e, f->bases.v[i]);

    /* cells in the order of their paths, inside each phenomenon */
    uint32_t ncells = (uint32_t) e->cells.n;
    uint32_t *newix = qf_xcalloc(ncells ? ncells : 1, sizeof *newix);
    QiCell *sorted = take(e, e->cells.v, ncells, sizeof(QiCell));
    qsort(sorted, ncells, sizeof(QiCell), cmp_cell);
    for (uint32_t i = 0; i < ncells; i++) {
        uint64_t v = 0;
        u64map_get(&e->paths, (uint64_t) (uintptr_t) qf_sym(f, sorted[i].path), &v);
        newix[v - 1] = i;
    }
    ir->cells = sorted;
    ir->ncells = ncells;
#define REMAP(c) do { if ((c) != QI_NONE) (c) = newix[c]; } while (0)
    for (size_t i = 0; i < e->aliases.n; i++) REMAP(e->aliases.v[i].cell);
    for (size_t i = 0; i < e->props.n; i++)
        for (int k = 0; k < 3; k++) REMAP(e->props.v[i].cell[k]);
    for (size_t i = 0; i < e->kernels.n; i++) {
        for (uint32_t s = 0; s < e->kernels.v[i].nbinds; s++) REMAP(e->kernels.v[i].binds[s].cell);
        for (uint32_t t = 0; t < e->kernels.v[i].ntransfers; t++) {
            REMAP(e->kernels.v[i].transfers[t].from);
            REMAP(e->kernels.v[i].transfers[t].to);
        }
    }
#undef REMAP
    free(newix);

    /* kinds and enumerations in the order of their names */
    uint32_t nk = (uint32_t) e->kinds.n, ne = (uint32_t) e->usedenums.n;
    Named *kord = qf_xcalloc(nk ? nk : 1, sizeof *kord), *eord = qf_xcalloc(ne ? ne : 1, sizeof *eord);
    uint32_t *kmap = qf_xcalloc(nk ? nk : 1, sizeof *kmap), *emap = qf_xcalloc(ne ? ne : 1, sizeof *emap);
    for (uint32_t i = 0; i < nk; i++) kord[i] = (Named) { e->kinds.v[i].name, i };
    if (nk) qsort(kord, nk, sizeof *kord, cmp_named);
    ir->kinds = qf_alloc(e->mem, (nk ? nk : 1) * sizeof(QiKind));
    ir->nkinds = nk;
    for (uint32_t i = 0; i < nk; i++) {
        ir->kinds[i] = e->kinds.v[kord[i].ix];
        kmap[kord[i].ix] = i;
    }
    for (uint32_t i = 0; i < ne; i++) eord[i] = (Named) { e->enums.v[i].name, i };
    if (ne) qsort(eord, ne, sizeof *eord, cmp_named);
    ir->enums = qf_alloc(e->mem, (ne ? ne : 1) * sizeof(QiEnum));
    ir->nenums = ne;
    for (uint32_t i = 0; i < ne; i++) {
        ir->enums[i] = e->enums.v[eord[i].ix];
        emap[eord[i].ix] = i;
    }
    for (uint32_t i = 0; i < ncells; i++)
        ir->cells[i].kind = ir->cells[i].domain == QI_SET ? emap[ir->cells[i].kind] : kmap[ir->cells[i].kind];
    free(kord);
    free(kmap);
    free(eord);
    free(emap);

    ir->phens = take(e, e->phens.v, e->phens.n, sizeof(QiPhen));
    ir->nphens = (uint32_t) e->phens.n;
    if (e->aliases.n) qsort(e->aliases.v, e->aliases.n, sizeof(QiAlias), cmp_alias);
    ir->aliases = take(e, e->aliases.v, e->aliases.n, sizeof(QiAlias));
    ir->naliases = (uint32_t) e->aliases.n;
    if (e->ports.n) qsort(e->ports.v, e->ports.n, sizeof(QiPort), cmp_port);
    ir->ports = take(e, e->ports.v, e->ports.n, sizeof(QiPort));
    ir->nports = (uint32_t) e->ports.n;

    /* propagators in the order of their phenomenon and the name of their
     * law; one law that two declarations state gives one propagator */
    if (e->props.n) qsort(e->props.v, e->props.n, sizeof(QiProp), cmp_prop);
    size_t np = 0;
    for (size_t i = 0; i < e->props.n; i++)
        if (!np || cmp_prop(&e->props.v[np - 1], &e->props.v[i])) e->props.v[np++] = e->props.v[i];
    ir->props = take(e, e->props.v, np, sizeof(QiProp));
    ir->nprops = (uint32_t) np;
    ir->kernels = take(e, e->kernels.v, e->kernels.n, sizeof(QiKernelUse));   /* in the order of the phenomena */
    ir->nkernels = (uint32_t) e->kernels.n;

    /* the dependency graph */
    DepVec deps = { 0 };
    for (uint32_t i = 0; i < ir->nprops; i++)
        for (int k = 0; k < 3; k++) add_dep(&deps, ir->props[i].cell[k], false, i, QI_NARROWS);
    for (uint32_t i = 0; i < ir->nkernels; i++) {
        const QiKernelUse *ku = &ir->kernels[i];
        for (uint32_t s = 0; s < ku->nbinds; s++) {
            QkRole r = ku->binds[s].role;
            if (r != QK_LINK) add_dep(&deps, ku->binds[s].cell, true, i, QI_READS);
            if (r == QK_OUT || r == QK_STATE) add_dep(&deps, ku->binds[s].cell, true, i, QI_WRITES);
        }
        for (uint32_t t = 0; t < ku->ntransfers; t++) {
            add_dep(&deps, ku->transfers[t].from, true, i, QI_TRANSFERS);
            add_dep(&deps, ku->transfers[t].to, true, i, QI_TRANSFERS);
        }
    }
    if (deps.n) qsort(deps.v, deps.n, sizeof(QiDep), cmp_dep);
    ir->deps = take(e, deps.v, deps.n, sizeof(QiDep));
    ir->ndeps = (uint32_t) deps.n;
    VEC_FREE(deps);
    return ir;
}

/* --- the elaborator --------------------------------------------------------------------------- */

/* A type of the prelude. It is in the package `quad`. If the root declares a
 * type with its name, the prelude adds to that type, and the type is the
 * root's. */
static uint32_t prelude_type(Front *f, const char *name)
{
    char full[64];
    snprintf(full, sizeof full, "quad.%s", name);
    uint32_t t = type_lookup(f, qf_sym(f, full));
    return t != QF_BOTTOM ? t : type_lookup(f, qf_sym(f, name));
}

QIr *qf_elaborate(Front *f, const QiOptions *opt)
{
    size_t errors0 = f->nerrors;
    Elab e;
    memset(&e, 0, sizeof e);
    e.f = f;
    e.opt = opt;
    e.op = qf_op_new(f);
    e.mem = qf_arena_new();
    e.tphen = prelude_type(f, "#Phenomenon");
    e.tport = prelude_type(f, "#Port");
    e.fevaluator = feat_lookup(f, qf_sym(f, "quad.evaluator"));

    /* the phenomena, in the order of their names */
    VEC(Named) named = { 0 };
    for (uint32_t i = 0; i < f->insts.n; i++)
        if (f->insts.v[i].node && is_subtype(f, f->insts.v[i].node->type, e.tphen)) {
            Named nm = { f->insts.v[i].name, i };
            VEC_PUSH(named, nm);
        }
    if (named.n) qsort(named.v, named.n, sizeof(Named), cmp_named);
    for (size_t i = 0; i < named.n; i++) VEC_PUSH(e.insts, named.v[i].ix);
    VEC_FREE(named);
    for (uint32_t p = 0; p < e.insts.n; p++) {
        const InstInfo *in = &f->insts.v[e.insts.v[p]];
        if (!world_name(in->name))
            err(&e, inst_loc(in), "the phenomenon %s needs a name of letters, digits and '_', with no package", in->name);
        QiPhen ph = { keep(&e, in->name), keep(&e, f->types.v[in->node->type].name) };
        VEC_PUSH(e.phens, ph);
        u64map_put(&e.phenidx, (uint64_t) e.insts.v[p] + 1, (uint64_t) p + 1);
    }

    U32Vec conn0 = { 0 };
    for (uint32_t p = 0; p < e.phens.n; p++) {
        VEC_PUSH(conn0, (uint32_t) e.conns.n);
        phen_cells(&e, p);
    }
    VEC_PUSH(conn0, (uint32_t) e.conns.n);
    check_conns(&e);
    if (f->nerrors == errors0)
        for (uint32_t p = 0; p < e.phens.n; p++) {
            phen_laws(&e, p, conn0.v[p], conn0.v[p + 1]);
            phen_kernel(&e, p);
        }
    QIr *ir = f->nerrors == errors0 ? finish(&e) : NULL;

    VEC_FREE(conn0);
    for (size_t c = 0; c < e.conns.n; c++) {
        VEC_FREE(e.conns.v[c].path);
        VEC_FREE(e.conns.v[c].sub);
    }
    VEC_FREE(e.conns);
    VEC_FREE(e.insts);
    VEC_FREE(e.phens);
    VEC_FREE(e.cells);
    VEC_FREE(e.ctypes);
    VEC_FREE(e.aliases);
    VEC_FREE(e.ports);
    VEC_FREE(e.props);
    VEC_FREE(e.kernels);
    VEC_FREE(e.kinds);
    u64map_free(&e.lawcount);
    u64map_free(&e.lawsdone);
    VEC_FREE(e.usedenums);
    VEC_FREE(e.enums);
    u64map_free(&e.phenidx);
    u64map_free(&e.paths);
    qf_op_free(e.op);
    if (!ir) qf_arena_free(e.mem);
    qf_sort_diags(f);
    return ir;
}
