#include <stdarg.h>
#include <stdio.h>

#include "front/front.h"
#include "quad/kernels.h"

/* The elaborator: stage 3 of the front end (ADR 0011 section 3, ADR 0016).
 *
 * It reads the result of a load and makes the IR. Each instance of a type
 * below #Phenomenon is a phenomenon. The work has four passes:
 *
 *   1. For each phenomenon: its connections, its cells, its aliases and its
 *      ports.
 *   2. A check of each connection against the port that it names.
 *   3. For each phenomenon: the propagators of its laws, and its kernel: the
 *      one that it names, or native.rate for its interactions with a rate
 *      (ADR 0020). Then its maps.
 *   4. The start: the first value of each stock, from the laws (ADR 0019).
 *
 * A phenomenon can have models (ADR 0021). A model is a field with a type
 * below #Model. Its cells have paths below it, and its laws and its kernel
 * work only while it is active. A port of the phenomenon with no view is the
 * port of the active model.
 *
 * Then the kinds, the enumerations and the dependency graph, in orders that
 * do not depend on the order of the source. */

/* One path to a value. owner: the type of the structure that holds the field. */
typedef struct {
    const QfNode *node;
    Sym text;
    uint32_t n;
    int port;
    bool set;
    uint32_t feat, owner;
    size_t p0;
    uint32_t model;              /* the model that the path is in, or QI_NONE */
} Leaf;
typedef struct { U32Vec path; uint32_t target; U32Vec sub; uint32_t port; } Conn;
/* A model of a phenomenon: the field, its structure, and its name. */
typedef struct { uint32_t feat; const QfNode *node; const char *name; } ModelRef;

typedef struct {
    Front            *f;
    QfOp             *op;
    const QiOptions  *opt;
    QfArena          *mem;
    uint32_t          tphen, tport, fevaluator;
    uint32_t          tinter, fsource, fsink, frate, ftol, fcons;    /* #Interaction and its fields (ADR 0020) */
    uint32_t          tmodel, tmap, fmodel, ffrom, fto;              /* #Model, #Map and their fields (ADR 0021) */
    uint32_t          treact, fratio;                                /* #Reaction, and the ratio of a flow (ADR 0021) */
    uint32_t          fwhen;                                         /* the condition of a map (ADR 0022) */
    VEC(ModelRef)     models;       /* the models of each phenomenon, in the order of their names */
    U32Vec            model0;       /* for each phenomenon: its first model in `models`, and one entry more */
    U32Vec            mstart;       /* for each phenomenon: its model at the start, or QI_NONE */
    VEC(QiMap)        maps;
    /* How find_cell goes through a port of the active model (ADR 0021 section 4) */
    uint32_t          assume;       /* in: the model to take, or QI_NONE for the model at the start */
    uint32_t          via;          /* out: the phenomenon whose model decided a cell, or QI_NONE */
    bool              via_two;      /* out: the models of two phenomena decided */
    U32Vec            insts;        /* for each phenomenon: its index in f->insts */
    U64Map            phenidx;      /* index in f->insts + 1 → phenomenon + 1 */
    VEC(QiPhen)       phens;
    VEC(QiCell)       cells;
    VEC(QType)        ctypes;       /* for each cell; no meaning for a set cell */
    VEC(QInterval)    start;        /* for each cell: its value at the start, which a derived field does not keep */
    VEC(QiAlias)      aliases;
    VEC(QiPort)       ports;
    VEC(QiProp)       props;
    VEC(QiKernelUse)  kernels;
    VEC(Conn)         conns;        /* every connection, with its phenomenon in ports[port] */
    U64Map            paths;        /* Sym of a path → cell + 1 */
    VEC(QiKind)       kinds;        /* the kinds that cells use, named and anonymous */
    U64Map            lawcount;     /* Sym of the name of a law → the inner values that it has */
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

/* The text of a field that has one string as its value. False if the field
 * has no value, or a value that is not one string. */
static bool text_value(Elab *e, const QfNode *n, Buf *out)
{
    if (!n || !(n->flags & NF_VALUE) || e->f->types.v[n->type].kind != TK_TEXT || n->lo != n->hi) return false;
    qf_string_decode((Sym) (intptr_t) n->lo, out);
    if (!out->s) qf_buf_puts(out, "");
    return true;
}

/* --- models (ADR 0021) ----------------------------------------------------------------------- */

static uint32_t nmodels(const Elab *e, uint32_t phen) { return e->model0.v[phen + 1] - e->model0.v[phen]; }
static const ModelRef *model_at(const Elab *e, uint32_t phen, uint32_t m) { return &e->models.v[e->model0.v[phen] + m]; }

/* The model that a path of a phenomenon is in: the model whose field is the
 * first component of the path. QI_NONE for a path outside the models. */
static uint32_t path_model(const Elab *e, uint32_t phen, const uint32_t *path, size_t n)
{
    for (uint32_t m = 0; n && m < nmodels(e, phen); m++)
        if (model_at(e, phen, m)->feat == path[0]) return m;
    return QI_NONE;
}

static uint32_t model_named(const Elab *e, uint32_t phen, const char *name)
{
    for (uint32_t m = 0; m < nmodels(e, phen); m++)
        if (!strcmp(model_at(e, phen, m)->name, name)) return m;
    return QI_NONE;
}

static int cmp_model(const void *a, const void *b) { return strcmp(((const ModelRef *) a)->name, ((const ModelRef *) b)->name); }

/* "coarse, fine": the models of a phenomenon, for a message. */
static void model_names(const Elab *e, uint32_t phen, Buf *b)
{
    for (uint32_t m = 0; m < nmodels(e, phen); m++) qf_buf_printf(b, "%s%s", m ? ", " : "", model_at(e, phen, m)->name);
}

/* Finds the models of a phenomenon, and the model that is active at the
 * start. Source has no order of declarations, so the phenomenon names that
 * model (ADR 0021 section 2). */
static void find_models(Elab *e, uint32_t phen)
{
    Front *f = e->f;
    const InstInfo *in = &f->insts.v[e->insts.v[phen]];
    Loc loc = inst_loc(in);
    size_t first = e->models.n;
    for (uint32_t i = 0; e->tmodel != QF_BOTTOM && i < in->node->narcs; i++) {
        const QfNode *v = in->node->arcs[i].val;
        if ((v->flags & NF_VALUE) || !is_subtype(f, v->type, e->tmodel)) continue;
        ModelRef m = { in->node->arcs[i].feat, v, f->feats.v[in->node->arcs[i].feat].label };
        if (!world_name(m.name))
            err(e, loc, "the model %s.%s needs a name of letters, digits and '_'", in->name, m.name);
        VEC_PUSH(e->models, m);
    }
    size_t n = e->models.n - first;
    if (n) qsort(e->models.v + first, n, sizeof(ModelRef), cmp_model);
    for (size_t i = 1; i < n; i++)
        if (!strcmp(e->models.v[first + i].name, e->models.v[first + i - 1].name))
            err(e, loc, "%s has two models with the name %s", in->name, e->models.v[first + i].name);
    uint32_t start = QI_NONE;
    Buf name = { 0 }, all = { 0 };
    bool named = text_value(e, e->fmodel == QF_BOTTOM ? NULL : node_arc(in->node, e->fmodel), &name);
    for (size_t i = 0; i < n; i++) qf_buf_printf(&all, "%s%s", i ? ", " : "", e->models.v[first + i].name);
    for (size_t i = 0; named && i < n; i++)
        if (!strcmp(e->models.v[first + i].name, name.s)) start = (uint32_t) i;
    if (n && !named)
        err(e, loc, "%s has models and does not name the model that is active at the start. Write `model: \"%s\"`. "
            "Its models: %s", in->name, e->models.v[first].name, all.s);
    else if (n && start == QI_NONE)
        err(e, loc, "%s names the model \"%s\" for the start, and it has no model with that name. Its models: %s",
            in->name, name.s, all.s);
    else if (!n && named)
        err(e, loc, "%s names the model \"%s\", and it has no models: a model is a field with a type below #Model",
            in->name, name.s);
    qf_buf_free(&name);
    qf_buf_free(&all);
    VEC_PUSH(e->mstart, start);
}

/* --- pass 1: connections, cells, aliases and ports ------------------------------------------- */

typedef struct {
    Elab           *e;
    uint32_t        phen;
    const InstInfo *in;
    Loc             loc;
    size_t          conn0;       /* the phenomenon's connections start here in e->conns */
    VEC(Leaf)       leaves;
    U32Vec          lpaths;      /* the paths of the leaves: leaf l has l.n features from l.p0 */
    U32Vec          path;
    VEC(const QfNode *) stack;
    int             port;        /* the port that the walk is inside, or -1 */
    uint32_t        owner;       /* the type of the structure that the walk is in */
    U64Map          rates;       /* a rate with a dimension and no kind → its scale + 1000 (ADR 0020) */
} Walk;

static uint32_t add_port(Elab *e, uint32_t phen, const uint32_t *path, size_t n, uint32_t type, QiPortState state)
{
    Buf b = { 0 };
    rel_text(e, &b, path, n);
    QiPort p = { phen, keep(e, b.s), keep(e, e->f->types.v[type].name), state, NULL, QI_NONE, NULL };
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
               w->path.v[w->path.n - 1], w->owner, w->lpaths.n, path_model(e, w->phen, w->path.v, w->path.n) };
    for (size_t i = 0; i < w->path.n; i++) VEC_PUSH(w->lpaths, w->path.v[i]);
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
        else if (tk == TK_NUMBER && u64map_get(&w->rates, (uint64_t) (uintptr_t) n, NULL)) leaf(w, n, false);   /* a ratio */
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
    /* a model and a map are fields of the phenomenon itself (ADR 0021 sections 1 and 5) */
    bool model = e->tmodel != QF_BOTTOM && is_subtype(f, n->type, e->tmodel);
    bool map = e->tmap != QF_BOTTOM && is_subtype(f, n->type, e->tmap);
    if (len > 1 && (model || map)) {
        Buf b = { 0 };
        rel_text(e, &b, w->path.v, len);
        err(e, w->loc, "%s.%s is a %s inside a structure of %s: a %s is a field of the phenomenon itself", w->in->name, b.s,
            model ? "model" : "map", w->in->name, model ? "model" : "map");
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
            /* The source and the sink of an interaction are paths to stocks,
             * and its flag is a statement: none of them is a cell (ADR 0020). */
            bool inter = e->tinter != QF_BOTTOM && is_subtype(f, n->type, e->tinter);
            /* An interaction with no rate states a transfer of a kernel in C
             * (ADR 0021 section 6): its tolerance has no use, and it is not a cell. */
            const QfNode *rate = inter ? node_arc(n, e->frate) : NULL;
            bool stated = inter && (!rate || (!(rate->flags & NF_VALUE) && rate->type == f->top && !rate->narcs));
            for (uint32_t i = 0; i < n->narcs; i++) {
                uint32_t ft = n->arcs[i].feat;
                if (inter && (ft == e->fsource || ft == e->fsink || ft == e->fcons)) continue;
                if (stated && ft == e->ftol) continue;
                VEC_PUSH(w->path, ft);
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

/* A type marks the field of a leaf with @derived (ADR 0019): a structure on
 * the path has a type below the type of a mark, and the rest of the path is
 * the path of that mark. */
static bool leaf_derived(const Walk *w, const Leaf *l)
{
    const Front *f = w->e->f;
    const uint32_t *path = w->lpaths.v + l->p0;
    for (uint32_t k = 0; k < l->n; k++) {
        const QfNode *at = node_at(w->in->node, path, k);
        for (size_t m = 0; at && m < f->derived.n; m++) {
            const DerivedMark *dm = &f->derived.v[m];
            if (dm->path.n == l->n - k && !memcmp(dm->path.v, path + k, dm->path.n * sizeof(uint32_t)) &&
                is_subtype(f, at->type, dm->type))
                return true;
        }
    }
    return false;
}

/* The range of a value node in quanta of `scale`. False if the node is not a
 * quantity with that scale: it then says nothing about the range. */
static bool node_range(const Front *f, const QfNode *n, int8_t scale, QInterval *out)
{
    const TypeInfo *tn = &f->types.v[n->type];
    if (!(n->flags & NF_VALUE)) return false;
    if (tn->kind == TK_KIND) {
        if (f->kinds.v[tn->kindix].scale != scale) return false;
        *out = (QInterval) { n->lo, n->hi };
        return true;
    }
    if (tn->kind != TK_DIMT) return false;
    QfDec d = { n->lo, n->hi, n->exp, (n->flags & NF_LO_OPEN) != 0, (n->flags & NF_HI_OPEN) != 0 };
    return !qf_dec_to_quanta(d, scale, &out->lo, &out->hi);
}

/* The range that the types give the field of a leaf, without the value of
 * the instance: the meet of what the type of each structure on the path
 * declares for the rest of the path. */
static void type_range(const Walk *w, const Leaf *l, int8_t scale, QInterval *range)
{
    const Front *f = w->e->f;
    const uint32_t *path = w->lpaths.v + l->p0;
    for (uint32_t k = 0; k < l->n; k++) {
        const QfNode *at = node_at(w->in->node, path, k);
        QfNode *x = at ? f->types.v[at->type].expansion : NULL;
        const QfNode *decl = x && !(x->flags & NF_VALUE) ? node_at(x, path + k, l->n - k) : NULL;
        QInterval r;
        if (decl && node_range(f, decl, scale, &r)) qi_meet(*range, r, range);
    }
}

/* --- interactions: the structures (ADR 0020) --- */

typedef VEC(const QfNode *) NodeStack;
typedef struct { U32Vec path; const QfNode *node; Sym text; } Inter;
typedef VEC(Inter) InterVec;

/* The structures below the type `type` in a phenomenon, outside its
 * connections: its interactions, or its reactions. */
static void find_typed(Elab *e, uint32_t type, const char *phen, const QfNode *n, U32Vec *path, NodeStack *stack, size_t conn0,
                       size_t conn1, InterVec *out)
{
    Front *f = e->f;
    for (size_t c = conn0; c < conn1; c++)
        if (e->conns.v[c].path.n == path->n && starts_with(path->v, path->n, &e->conns.v[c].path)) return;
    if (n->flags & NF_VALUE) return;
    if (path->n && is_subtype(f, n->type, type)) {
        Inter in = { { 0 }, n, path_sym(e, phen, path->v, path->n) };
        for (size_t i = 0; i < path->n; i++) VEC_PUSH(in.path, path->v[i]);
        VEC_PUSH(*out, in);
        return;
    }
    for (size_t i = 0; i < stack->n; i++)
        if (stack->v[i] == n) return;
    VEC_PUSH(*stack, n);
    for (uint32_t i = 0; i < n->narcs; i++) {
        VEC_PUSH(*path, n->arcs[i].feat);
        find_typed(e, type, phen, n->arcs[i].val, path, stack, conn0, conn1, out);
        path->n--;
    }
    stack->n--;
}

static void find_inters(Elab *e, const char *phen, const QfNode *n, U32Vec *path, NodeStack *stack, size_t conn0, size_t conn1,
                        InterVec *out)
{
    find_typed(e, e->tinter, phen, n, path, stack, conn0, conn1, out);
}

/* A field with no value and no structure: the author did not state it. */
static bool unstated(const Front *f, const QfNode *n) { return !n || (!(n->flags & NF_VALUE) && n->type == f->top && !n->narcs); }

/* The reaction that an interaction is a flow of (ADR 0021 section 7): the
 * structure below #Reaction on its path. *len gets the length of the path of
 * the reaction. NULL if the interaction is not in a reaction. */
static const QfNode *reaction_of(Elab *e, uint32_t phen, const Inter *in, size_t *len)
{
    Front *f = e->f;
    *len = 0;
    if (e->treact == QF_BOTTOM) return NULL;
    for (size_t k = 1; k < in->path.n; k++) {
        const QfNode *at = node_at(f->insts.v[e->insts.v[phen]].node, in->path.v, k);
        if (at && !(at->flags & NF_VALUE) && is_subtype(f, at->type, e->treact)) {
            *len = k;
            return at;
        }
    }
    return NULL;
}

/* Makes the cell of a value. first: the first leaf of the value in
 * w->leaves; l: the leaf that gives the cell its path. */
static void add_cell(Walk *w, size_t first, const Leaf *l)
{
    Elab *e = w->e;
    Front *f = e->f;
    QiCell c = { keep(e, l->text), w->phen, l->model, QI_INTERVAL, 0, { 0, 0 }, 0, false, false, false };
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
    } else if (f->types.v[l->node->type].kind == TK_DIMT || f->types.v[l->node->type].kind == TK_NUMBER) {
        /* a dimension and no kind: an anonymous kind with the default scale of the dimension. A number that
         * is the ratio of a flow has no dimension */
        QDim dim = f->types.v[l->node->type].kind == TK_NUMBER ? qdim_none() : f->dims.v[f->types.v[l->node->type].dimix].dim;
        int8_t scale = qdim_default_scale(&dim, f->tick_scale);
        uint64_t rs;
        if (u64map_get(&w->rates, (uint64_t) (uintptr_t) l->node, &rs)) scale = (int8_t) ((int) rs - 1000);
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
    }
    /* The value of the instance is the value at the start. A stock gets its
     * exact value in pass 4. A derived field keeps only the range of its
     * types: the laws give its value in each epoch (ADR 0019). */
    QInterval start = c.base;
    if (c.domain == QI_INTERVAL && !c.stock) {
        QInterval typed = qi_top();
        for (size_t j = first; j < w->leaves.n; j++) {
            const Leaf *o = &w->leaves.v[j];
            if (o->node != l->node) continue;
            c.derived |= leaf_derived(w, o);
            type_range(w, o, qt.scale, &typed);
        }
        if (c.derived) c.base = typed;
    }
    /* A model that is not active at the start has no state: each of its
     * stocks is zero, and a derived field has no value (ADR 0021 section 3).
     * The map gives the model its state at a switch. */
    uint32_t active = e->mstart.v[w->phen];
    if (l->model != QI_NONE && active != QI_NONE && l->model != active && c.domain == QI_INTERVAL && (c.stock || c.derived)) {
        bool given = c.stock ? !(start.lo == Q_NINF && start.hi == Q_INF) && !(start.lo == 0 && start.hi == 0)
                             : start.lo != c.base.lo || start.hi != c.base.hi;
        if (given)
            err(e, w->loc, "%s has a value, and its model %s is not active at the start: a model that is not active has "
                "no state. The map gives the model its state at a switch. Remove the value, or start with `model: \"%s\"`",
                l->text, model_at(e, w->phen, l->model)->name, model_at(e, w->phen, l->model)->name);
        if (c.stock) c.base = qi_point(0);
        start = c.base;
    }
    VEC_PUSH(e->cells, c);
    VEC_PUSH(e->ctypes, qt);
    VEC_PUSH(e->start, start);
    u64map_put(&e->paths, (uint64_t) (uintptr_t) l->text, e->cells.n);
}

/* The rate of an interaction that has a dimension and no kind gets the scale
 * of its stock with 6 more decimals: one quantum of the stock in 10^6
 * seconds. The default scale of the dimension is too coarse for a slow flow:
 * the rate then reads as zero and the flow stops (ADR 0020). An inner
 * quotient of a law has 6 more decimals for the same reason (ADR 0018). */
static void rate_scales(Walk *w)
{
    Elab *e = w->e;
    Front *f = e->f;
    if (e->tinter == QF_BOTTOM) return;
    InterVec inters = { 0 };
    U32Vec path = { 0 };
    NodeStack stack = { 0 };
    find_inters(e, w->in->name, w->in->node, &path, &stack, w->conn0, e->conns.n, &inters);
    for (size_t i = 0; i < inters.n; i++) {
        size_t rlen;
        const QfNode *rx = reaction_of(e, w->phen, &inters.v[i], &rlen);
        const QfNode *rn = node_arc(rx ? rx : inters.v[i].node, e->frate);
        const QfNode *qn = rx && e->fratio != QF_BOTTOM ? node_arc(inters.v[i].node, e->fratio) : NULL;
        TypeKind qk = qn && (qn->flags & NF_VALUE) ? f->types.v[qn->type].kind : TK_TOP;
        /* The ratio of a flow of a reaction (ADR 0021 section 7). A number
         * has 9 decimals. A quantity with no kind has 6 decimals more than
         * the default of its dimension, as an inner quotient has. */
        bool plain = qk == TK_NUMBER || (qk == TK_DIMT && qdim_is_none(&f->dims.v[f->types.v[qn->type].dimix].dim));
        if (plain) u64map_put(&w->rates, (uint64_t) (uintptr_t) qn, (uint64_t) (-9 + 1000));
        else if (qk == TK_DIMT) {
            int scale = qdim_default_scale(&f->dims.v[f->types.v[qn->type].dimix].dim, f->tick_scale) - 6;
            u64map_put(&w->rates, (uint64_t) (uintptr_t) qn, (uint64_t) ((scale < -18 ? -18 : scale) + 1000));
        }
        /* The rate of a reaction with no kind: 6 decimals more than the
         * default of its dimension. If a flow has a number as its ratio, the
         * rate has the dimension of that stock for each second, and it gets
         * the scale of that stock with 6 more decimals, as for an interaction. */
        bool free_rate = rn && (rn->flags & NF_VALUE) && f->types.v[rn->type].kind == TK_DIMT;
        uint64_t have;
        if (rx && free_rate && !u64map_get(&w->rates, (uint64_t) (uintptr_t) rn, &have)) {
            int scale = qdim_default_scale(&f->dims.v[f->types.v[rn->type].dimix].dim, f->tick_scale) - 6;
            u64map_put(&w->rates, (uint64_t) (uintptr_t) rn, (uint64_t) ((scale < -18 ? -18 : scale) + 1000));
        }
        const uint32_t ends[2] = { e->fsource, e->fsink };
        for (int k = 0; k < 2 && free_rate && (!rx || plain); k++) {
            const QfNode *sn = node_arc(inters.v[i].node, ends[k]);
            if (!sn || !(sn->flags & NF_VALUE) || f->types.v[sn->type].kind != TK_KIND) continue;
            int scale = f->kinds.v[f->types.v[sn->type].kindix].scale - 6;
            if (scale < -18) scale = -18;
            if (rx && u64map_get(&w->rates, (uint64_t) (uintptr_t) rn, &have) && (int) have - 1000 < scale)
                scale = (int) have - 1000;                    /* the finest scale that a flow asks for */
            u64map_put(&w->rates, (uint64_t) (uintptr_t) rn, (uint64_t) (scale + 1000));
            break;
        }
        VEC_FREE(inters.v[i].path);
    }
    VEC_FREE(inters);
    VEC_FREE(path);
    VEC_FREE(stack);
}

static uint32_t port_named(const Elab *e, uint32_t phen, const char *name, size_t len)
{
    for (size_t p = 0; p < e->ports.n; p++)
        if (e->ports.v[p].phen == phen && strlen(e->ports.v[p].name) == len && !memcmp(e->ports.v[p].name, name, len))
            return (uint32_t) p;
    return QI_NONE;
}

/* The field of a structure with a label, or NULL. */
static const QfNode *arc_labeled(const Front *f, const QfNode *n, const char *label)
{
    for (uint32_t i = 0; n && !(n->flags & NF_VALUE) && i < n->narcs; i++)
        if (!strcmp(f->feats.v[n->arcs[i].feat].label, label)) return n->arcs[i].val;
    return NULL;
}

/* A port of a phenomenon with models that has no view is the port of the
 * active model: each model gives a port with that name, as a view of its own
 * state (ADR 0021 section 4). If no model has the port, it stays open. */
static void model_ports(Walk *w)
{
    Elab *e = w->e;
    Front *f = e->f;
    uint32_t n = nmodels(e, w->phen);
    for (size_t p = 0; n && p < e->ports.n; p++) {
        if (e->ports.v[p].phen != w->phen || e->ports.v[p].state != QI_PORT_OPEN || strchr(e->ports.v[p].name, '.')) continue;
        const char *name = e->ports.v[p].name;
        const QfNode *own = arc_labeled(f, w->in->node, name);
        const char **targets = qf_alloc(e->mem, n * sizeof *targets);
        uint32_t have = 0;
        bool ok = true;
        Buf lacks = { 0 };
        for (uint32_t m = 0; m < n; m++) {
            const ModelRef *mr = model_at(e, w->phen, m);
            Buf rel = { 0 }, full = { 0 };
            qf_buf_printf(&rel, "%s.%s", mr->name, name);
            qf_buf_printf(&full, "%s.%s", w->in->name, rel.s);
            uint32_t mp = port_named(e, w->phen, rel.s, rel.len);
            const QfNode *theirs = arc_labeled(f, mr->node, name);
            targets[m] = keep(e, full.s);
            if (mp == QI_NONE) {
                qf_buf_printf(&lacks, "%s%s", lacks.len ? ", " : "", mr->name);
            } else {
                have++;
                if (e->ports.v[mp].state != QI_PORT_VIEW) {
                    err(e, w->loc, "%s is the port %s of the model %s, and it has a field with no cell: a model gives the "
                        "port as a view of its own state, as in `temperature: .temperature`", full.s, name, mr->name);
                    ok = false;
                } else if (own && theirs && !is_subtype(f, theirs->type, own->type)) {
                    err(e, w->loc, "%s.%s is a %s, and the model %s gives a %s: the port of a model has the type of the "
                        "port of the phenomenon, or a type below it", w->in->name, name, f->types.v[own->type].name, mr->name,
                        f->types.v[theirs->type].name);
                    ok = false;
                }
            }
            qf_buf_free(&rel);
            qf_buf_free(&full);
        }
        if (have && lacks.len)
            err(e, w->loc, "%s.%s is a port with no view, so each model must give it. These models have no port %s: %s",
                w->in->name, name, name, lacks.s);
        else if (have && ok) {
            e->ports.v[p].state = QI_PORT_MODEL;
            e->ports.v[p].targets = targets;
        }
        qf_buf_free(&lacks);
    }
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
    rate_scales(&w);
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
        if (best) add_cell(&w, i, best);
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
    model_ports(&w);
    u64map_free(&w.rates);
    VEC_FREE(w.leaves);
    VEC_FREE(w.lpaths);
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
static bool path_cell(Elab *e, const char *path, uint64_t *v)
{
    return u64map_get(&e->paths, (uint64_t) (uintptr_t) qf_sym(e->f, path), v);
}

/* The cell at `rest` below a port of the active model. The model is
 * e->assume, or the model at the start. Sets e->via. */
static bool through_model(Elab *e, const QiPort *port, const char *rest, uint64_t *v)
{
    uint32_t m = e->assume != QI_NONE ? e->assume : e->mstart.v[port->phen];
    if (m == QI_NONE || m >= nmodels(e, port->phen)) return false;
    Buf b = { 0 };
    qf_buf_printf(&b, "%s%s", port->targets[m], rest);
    bool found = path_cell(e, b.s, v);
    qf_buf_free(&b);
    if (found) e->via = port->phen;
    return found;
}

/* The cell at a path of a phenomenon, through a connection if the path
 * starts with a connected port. QI_NONE if there is none; then *open is the
 * open port that the path starts with, if it does.
 *
 * A path can go through a port of the active model of a phenomenon (ADR 0021
 * section 4). The cell then depends on that model: e->via gets the
 * phenomenon, and the cell is that of the model e->assume, or of the model at
 * the start. If not, e->via gets QI_NONE. */
static uint32_t find_cell(Elab *e, uint32_t phen, const char *rel, const char **open)
{
    Buf b = { 0 };
    qf_buf_printf(&b, "%s.%s", e->phens.v[phen].name, rel);
    uint64_t v = 0;
    bool found = path_cell(e, b.s, &v);
    if (open) *open = NULL;
    e->via = QI_NONE;
    if (!found) {
        size_t first = strcspn(rel, ".");
        uint32_t p = port_named(e, phen, rel, first);
        const QiPort *port = p == QI_NONE ? NULL : &e->ports.v[p];
        if (port && port->state == QI_PORT_CONNECTED && rel[first]) {
            b.len = 0;
            qf_buf_printf(&b, "%s%s", port->target, rel + first);
            found = path_cell(e, b.s, &v);
            /* the other port can be the port of the active model of its phenomenon */
            const char *sub = strchr(port->target, '.');
            uint32_t tp = found || !sub ? QI_NONE : port_named(e, port->target_phen, sub + 1, strlen(sub + 1));
            if (tp != QI_NONE && e->ports.v[tp].state == QI_PORT_MODEL) found = through_model(e, &e->ports.v[tp], rel + first, &v);
        } else if (port && port->state == QI_PORT_MODEL && rel[first]) {
            found = through_model(e, port, rel + first, &v);
        } else if (port && port->state == QI_PORT_OPEN && open) {
            *open = port->name;
        }
    }
    qf_buf_free(&b);
    return found ? (uint32_t) v - 1 : QI_NONE;
}

/* After a find_cell that went through a port of the active model: the cell
 * of the same path for each model of that phenomenon. NULL with no such port.
 * A model can lack the cell: QI_NONE. */
static uint32_t *model_cells(Elab *e, uint32_t phen, const char *rel, uint32_t *mphen)
{
    *mphen = e->via;
    if (e->via == QI_NONE) return NULL;
    uint32_t n = nmodels(e, *mphen);
    uint32_t *cells = qf_alloc(e->mem, (n ? n : 1) * sizeof *cells);
    for (uint32_t m = 0; m < n; m++) {
        e->assume = m;
        cells[m] = find_cell(e, phen, rel, NULL);
    }
    e->assume = QI_NONE;
    e->via = *mphen;
    return cells;
}

/* A reference in source makes one node stand at several paths of a
 * phenomenon. This looks at each path of the node `want`, outside the
 * structure at `skip`, and takes the best one that is a cell: the shortest,
 * and then the first by its text. *open: an open port that a path goes
 * through, if no path is a cell. */
static void node_cell_walk(Elab *e, uint32_t phen, const QfNode *n, const QfNode *want, U32Vec *path, NodeStack *stack,
                           const U32Vec *skip, uint32_t *cell, size_t *len, Buf *rel, const char **open, uint32_t *via)
{
    if (skip && path->n == skip->n && starts_with(path->v, path->n, skip)) return;
    if (n == want && path->n) {
        Buf text = { 0 };
        rel_text(e, &text, path->v, path->n);
        const char *op = NULL;
        uint32_t c = find_cell(e, phen, text.s, &op);
        if (c == QI_NONE && op && !*open) *open = op;
        if (c != QI_NONE && (*cell == QI_NONE || path->n < *len || (path->n == *len && strcmp(text.s, rel->s) < 0))) {
            *cell = c;
            *len = path->n;
            *via = e->via;
            rel->len = 0;
            qf_buf_puts(rel, text.s);
        }
        qf_buf_free(&text);
        return;
    }
    if (n->flags & NF_VALUE) return;
    for (size_t i = 0; i < stack->n; i++)
        if (stack->v[i] == n) return;
    VEC_PUSH(*stack, n);
    for (uint32_t i = 0; i < n->narcs; i++) {
        VEC_PUSH(*path, n->arcs[i].feat);
        node_cell_walk(e, phen, n->arcs[i].val, want, path, stack, skip, cell, len, rel, open, via);
        path->n--;
    }
    stack->n--;
}

/* The cell of a node of a phenomenon, or QI_NONE. rel: gets the path that
 * gave the cell. e->via is as after find_cell for that path. */
static uint32_t node_cell(Elab *e, uint32_t phen, const QfNode *want, const U32Vec *skip, Buf *rel, const char **open)
{
    U32Vec path = { 0 };
    NodeStack stack = { 0 };
    uint32_t cell = QI_NONE, via = QI_NONE;
    size_t len = 0;
    *open = NULL;
    node_cell_walk(e, phen, e->f->insts.v[e->insts.v[phen]].node, want, &path, &stack, skip, &cell, &len, rel, open, &via);
    VEC_FREE(path);
    VEC_FREE(stack);
    e->via = via;
    return cell;
}

typedef struct {
    Elab           *e;
    uint32_t        phen;
    const QfRecUse *use;
    const char     *law;         /* the text of the law */
    const char     *name;        /* its path: roomA.storage */
    Loc             loc;
    uint32_t        own;         /* the model that states the law, or QI_NONE */
} Law;

static uint32_t mirror(Law *l, uint32_t cell, const uint32_t *mcells, uint32_t mphen);

/* The cell of a reference in a law. sets: the cell can be a set cell, for a
 * truth value (ADR 0022). QI_NONE with a diagnostic. */
static uint32_t ref_cell(Law *l, const X *x, bool sets)
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
        Buf rel = { 0 };
        qf_buf_puts(&rel, b.s ? b.s : "");
        uint32_t c = find_cell(e, l->phen, rel.s, &open);
        if (c == QI_NONE && !open) {
            /* the path of the reference is not the path of the cell: the same value stands at another path */
            U32Vec full = { 0 };
            for (size_t k = 0; k < l->use->prefix.n; k++) VEC_PUSH(full, l->use->prefix.v[k]);
            for (size_t k = 0; k < rec->tags.v[t].path.n; k++) VEC_PUSH(full, rec->tags.v[t].path.v[k]);
            const QfNode *n = node_at(e->f->insts.v[e->insts.v[l->phen]].node, full.v, full.n);
            rel.len = 0;
            if (n) c = node_cell(e, l->phen, n, NULL, &rel, &open);
            VEC_FREE(full);
        }
        size_t errors = e->f->nerrors;
        if (c != QI_NONE && e->via != QI_NONE && (sets || e->cells.v[c].domain != QI_SET)) {
            /* the cell depends on the active model of a phenomenon: the law reads a copy */
            uint32_t mphen, *mcells = model_cells(e, l->phen, rel.s, &mphen);
            c = mirror(l, c, mcells, mphen);
        }
        qf_buf_free(&rel);
        if (c == QI_NONE && e->f->nerrors == errors) {
            if (open)
                err(e, l->loc, "the law %s reads %s.%s, and the port %s.%s is not connected", l->law,
                    e->phens.v[l->phen].name, b.s, e->phens.v[l->phen].name, open);
            else
                err(e, l->loc, "the law %s reads %s.%s, which is not a cell", l->law, e->phens.v[l->phen].name,
                    b.s ? b.s : "");
        } else if (c != QI_NONE && !sets && e->cells.v[c].domain == QI_SET) {
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

static uint32_t operand(Law *l, const X *x) { return ref_cell(l, x, false); }

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
    case QI_IN_RANGE: case QI_IN_SET: case QI_AND: case QI_OR: case QI_NOT: break;   /* truth values have no kind */
    }
    return false;
}

/* A law with quantities that do not fit. The message names each quantity
 * with its kind, so the author sees which one is wrong. */
static void misfit(Law *l, QStatus why, uint32_t z, uint32_t a, uint32_t b)
{
    Elab *e = l->e;
    Buf who = { 0 };
    uint32_t cs[3] = { z, a, b };
    for (int i = 0; i < 3; i++) {
        if (cs[i] == QI_NONE) continue;
        const QiCell *c = &e->cells.v[cs[i]];
        const QiKind *k = &e->kinds.v[c->kind];
        qf_buf_printf(&who, "%s%s is ", who.len ? ", " : "", c->inner ? "an inner value" : c->path);
        if (k->anonymous) qf_buf_printf(&who, "in %s", k->dimension);
        else qf_buf_printf(&who, "a %s", k->name);
    }
    err(e, l->loc, "the law %s does not fit the kinds of its quantities: %s (%s)", l->law, q_status_str(why), who.s);
    qf_buf_free(&who);
}

/* An inner value of a law (ADR 0018): the value of an inner expression, or
 * a constant. It is a derived cell with no field in source. Its path is the
 * name of the law and a number, in the order in which the expression makes
 * the cells. */
static Sym inner_path(Law *l)
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
        return NULL;
    }
    return path;
}

static uint32_t inner(Law *l, const QType *qt, QInterval base)
{
    Elab *e = l->e;
    Sym path = inner_path(l);
    if (!path) return QI_NONE;
    QiCell c = { keep(e, path), l->phen, l->own, QI_INTERVAL, use_qtype(e, qt), base, 0, false, true, true };
    VEC_PUSH(e->cells, c);
    VEC_PUSH(e->ctypes, *qt);
    VEC_PUSH(e->start, base);
    u64map_put(&e->paths, (uint64_t) (uintptr_t) path, e->cells.n);
    return (uint32_t) e->cells.n - 1;
}

/* A set cell with the path `path`: the cell of a condition, or an inner
 * value with atoms (ADR 0022 section 3). The laws give its value in each
 * epoch. */
static uint32_t derived_set(Elab *e, Sym path, uint32_t phen, uint32_t model, uint32_t en, uint64_t mask, bool is_inner)
{
    QiCell c = { keep(e, path), phen, model, QI_SET, en, { 0, 0 }, mask, false, true, is_inner };
    QType none;
    memset(&none, 0, sizeof none);
    VEC_PUSH(e->cells, c);
    VEC_PUSH(e->ctypes, none);
    VEC_PUSH(e->start, c.base);
    u64map_put(&e->paths, (uint64_t) (uintptr_t) path, e->cells.n);
    return (uint32_t) e->cells.n - 1;
}

static uint32_t inner_set(Law *l, uint32_t en, uint64_t mask)
{
    Sym path = inner_path(l);
    return path ? derived_set(l->e, path, l->phen, l->own, en, mask, true) : QI_NONE;
}

static uint64_t all_atoms(const QiEnum *en) { return en->natoms >= 64 ? ~(uint64_t) 0 : ((uint64_t) 1 << en->natoms) - 1; }

/* A law of a model is a propagator of that model: it works only while the
 * model is active (ADR 0021 section 1). */
static void emit(Law *l, QiOp op, uint32_t z, uint32_t a, uint32_t b)
{
    Elab *e = l->e;
    QiProp p = { l->phen, l->own == QI_NONE ? QI_NONE : l->phen, l->own, op, { z, a, b }, keep(e, l->name), keep(e, l->law),
                 l->loc.file ? keep(e, l->loc.file) : NULL, l->loc.line, l->loc.col, { 0, 0 }, 0 };
    VEC_PUSH(e->props, p);
}

/* A copy of a cell that depends on the active model of the phenomenon
 * `mphen` (ADR 0021 section 4). The copy is an inner value of the law. For
 * each model, one propagator of that model makes the copy equal to the cell
 * of that model. So the law has one form, and the copy follows a switch.
 * QI_NONE with a diagnostic. */
static uint32_t mirror(Law *l, uint32_t cell, const uint32_t *mcells, uint32_t mphen)
{
    Elab *e = l->e;
    QType qt = e->ctypes.v[cell];
    qt.kind = 0;                                     /* the copy is not state, so it has no conserved kind */
    uint32_t en = e->cells.v[cell].kind;
    uint32_t z = e->cells.v[cell].domain == QI_SET ? inner_set(l, en, all_atoms(&e->enums.v[en])) : inner(l, &qt, qi_top());
    for (uint32_t m = 0; z != QI_NONE && m < nmodels(e, mphen); m++) {
        if (mcells[m] == QI_NONE) {
            err(e, l->loc, "the law %s reads %s through a port of %s, and the model %s gives no cell for it", l->law,
                e->cells.v[cell].path, e->phens.v[mphen].name, model_at(e, mphen, m)->name);
            return QI_NONE;
        }
        QiProp p = { l->phen, mphen, m, QI_EQUAL, { z, mcells[m], QI_NONE }, keep(e, l->name), keep(e, l->law),
                     l->loc.file ? keep(e, l->loc.file) : NULL, l->loc.line, l->loc.col, { 0, 0 }, 0 };
        VEC_PUSH(e->props, p);
    }
    return z;
}

static uint32_t cell_of(Law *l, const X *x);

/* The limits of the scale of an inner value: at most 12 decimals, and for a
 * quotient at least 6 decimals more than the default scale of its dimension. */
#define DERIVED_FINEST (-12)
#define DERIVED_DIGITS 6

/* z = a op b for an expression. z is the cell of the result, or QI_NONE:
 * then a new inner value is the result. The result, or QI_NONE with a diagnostic. */
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
        if (why) { misfit(l, why, QI_NONE, a, b); return QI_NONE; }
        /* The scale of an inner product or quotient (ADR 0018 section 3). The
         * exact scale of a product is the sum of the scales, and a long
         * product gets a scale so fine that no value fits. The exact scale of
         * a quotient is the difference, which can be coarse. So the scale has
         * limits, and the propagator rounds outward. */
        if (op == QI_PRODUCT && t.scale < DERIVED_FINEST) t.scale = DERIVED_FINEST;
        if (op == QI_QUOTIENT) {
            int fine = qdim_default_scale(&t.dim, e->f->tick_scale) - DERIVED_DIGITS;
            if (t.scale > fine) t.scale = (int8_t) fine;
            if (t.scale < DERIVED_FINEST) t.scale = DERIVED_FINEST;
        }
        z = inner(l, &t, qi_top());
        if (z == QI_NONE) return QI_NONE;
    } else {
        QType tz = e->ctypes.v[z];
        if (!fits(&tz, &ta, &tb, op, &why)) { misfit(l, why, z, a, b); return QI_NONE; }
    }
    emit(l, op, z, a, b);
    return z;
}

/* The cell that has the value of an expression: the cell of a reference, a
 * constant for a number, or an inner value for an operation. */
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
        return inner(l, &qt, qi_point(value));
    }
    unsupported(l);
    return QI_NONE;
}

static bool is_truth(const Elab *e, uint32_t c);
static bool truth_expr(const X *x) { return x->k == X_LOGIC || x->k == X_NOT || x->k == X_CMP; }
static uint32_t truth(Law *l, const X *x, uint32_t z);
static void truth_equal(Law *l, uint32_t z, uint32_t c);

static void lower(Elab *e, uint32_t phen, const QfRecUse *use, uint32_t own)
{
    const X *x = use->rec->expr;
    char *text = x_text(x);
    Buf nb = { 0 }, rel = { 0 };
    rel_text(e, &rel, use->prefix.v, use->prefix.n);
    if (rel.len && use->rec->path.n) qf_buf_putc(&rel, '.');
    rel_text(e, &rel, use->rec->path.v, use->rec->path.n);
    qf_buf_printf(&nb, "%s%s%s", e->phens.v[phen].name, rel.len ? "." : "", rel.s ? rel.s : "");
    Law l = { e, phen, use, text, nb.s, x->loc, own };
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
            if (truth_expr(lhs) || truth_expr(rhs)) {
                /* a field with the type bool gets the value of a condition (ADR 0022 section 4) */
                uint32_t z = lhs->k == X_TAG ? ref_cell(&l, lhs, true) : truth(&l, lhs, QI_NONE);
                if (z != QI_NONE && !is_truth(e, z))
                    err(e, l.loc, "the law %s gives the value of a condition to %s, which does not have the type bool", text,
                        e->cells.v[z].path);
                else if (z != QI_NONE) truth(&l, rhs, z);
            } else if (lhs->k == X_TAG && rhs->k == X_TAG) {
                uint32_t a = ref_cell(&l, lhs, true), b = a == QI_NONE ? QI_NONE : ref_cell(&l, rhs, true);
                bool sa = b != QI_NONE && e->cells.v[a].domain == QI_SET, sb = b != QI_NONE && e->cells.v[b].domain == QI_SET;
                if (b == QI_NONE) {
                } else if (sa || sb) {
                    /* two truth values are equal */
                    uint32_t bad = !is_truth(e, a) ? a : !is_truth(e, b) ? b : QI_NONE;
                    if (sa != sb)
                        err(e, l.loc, "the law %s has %s, which is a quantity, and %s, which is a set of atoms. An equation "
                            "has two quantities, or two fields with the type bool", text, e->cells.v[sa ? b : a].path,
                            e->cells.v[sa ? a : b].path);
                    else if (bad != QI_NONE)
                        err(e, l.loc, "the law %s has %s, which does not have the type bool. A law with atoms makes two "
                            "fields with the type bool equal, or gives a field with the type bool the value of a condition",
                            text, e->cells.v[bad].path);
                    else truth_equal(&l, a, b);
                } else {
                    QType ta = e->ctypes.v[a], tb = e->ctypes.v[b];
                    QStatus why = Q_OK;
                    if (!fits(&ta, &tb, NULL, QI_EQUAL, &why)) misfit(&l, why, QI_NONE, a, b);
                    else emit(&l, QI_EQUAL, a, b, QI_NONE);
                }
            } else if (lhs->k == X_TAG && rhs->k == X_ARITH) {
                /* a reference and one operation: the operation writes the cell of the reference */
                uint32_t z = operand(&l, lhs);
                if (z != QI_NONE) lower_arith(&l, rhs, z);
            } else {
                uint32_t a = cell_of(&l, lhs), b = a == QI_NONE ? QI_NONE : cell_of(&l, rhs);
                if (b != QI_NONE) {
                    QType ta = e->ctypes.v[a], tb = e->ctypes.v[b];
                    QStatus why = Q_OK;
                    if (!fits(&ta, &tb, NULL, QI_EQUAL, &why)) misfit(&l, why, QI_NONE, a, b);
                    else emit(&l, QI_EQUAL, a, b, QI_NONE);
                }
            }
        } else if (x->k == X_CMP && (x->u.ar.op == 'l' || x->u.ar.op == '<' || x->u.ar.op == 'g' || x->u.ar.op == '>')) {
            bool greater = x->u.ar.op == 'g' || x->u.ar.op == '>', strict = x->u.ar.op == '<' || x->u.ar.op == '>';
            uint32_t a = cell_of(&l, x->u.ar.a), b = a == QI_NONE ? QI_NONE : cell_of(&l, x->u.ar.b);
            if (b != QI_NONE) {
                QType ta = e->ctypes.v[a], tb = e->ctypes.v[b];
                QStatus why = Q_OK;
                if (!fits(&ta, &tb, NULL, QI_LESS_EQUAL, &why)) misfit(&l, why, QI_NONE, a, b);
                else emit(&l, strict ? QI_LESS : QI_LESS_EQUAL, greater ? b : a, greater ? a : b, QI_NONE);
            }
        } else if (x->k == X_ARITH) {
            /* a value that an expression gives: the field where it stands is the result */
            uint32_t z = find_cell(e, phen, rel.s ? rel.s : "", NULL);
            if (z == QI_NONE)
                err(e, l.loc, "%s has the value %s, and it is not a cell: give the field a kind", nb.s, text);
            else lower_arith(&l, x, z);
        } else if (x->k == X_LOGIC || x->k == X_NOT) {
            err(e, l.loc, "the law %s is a condition with a logic operator. A law can give a condition to a field with the "
                "type bool, as in `.busy == (%s)`", text, text);
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
        uint32_t own = path_model(e, phen, at.v, at.n);
        /* the condition of a map is not a law: phen_maps reads it (ADR 0022) */
        const QfNode *holder = at.n == 2 && at.v[1] == e->fwhen && e->tmap != QF_BOTTOM ? node_at(in->node, at.v, 1) : NULL;
        if (holder && !(holder->flags & NF_VALUE) && is_subtype(e->f, holder->type, e->tmap)) copied = true;
        VEC_FREE(at);
        if (!copied) lower(e, phen, u, own);
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

/* The kernel that a phenomenon names, or that its model `model` names. A
 * slot of the kernel of a model is first a path in the model, and then a
 * path in the phenomenon, as in the world (ADR 0010). */
static bool room_kernel_name(const char *name)
{
    return !strcmp(name, "native.diffusion") || !strcmp(name, "native.room.refine") || !strcmp(name, "native.room.abstract");
}

QkKernel *qi_room_kernel(const char *name, unsigned n)
{
    return !n ? NULL
           : !strcmp(name, "native.diffusion")     ? qk_diffusion_new(n)
           : !strcmp(name, "native.room.refine")   ? qk_refine_new(n)
           : !strcmp(name, "native.room.abstract") ? qk_abstract_new(n)
                                                   : NULL;
}

/* The number of the cells c0, c1, ... of a room: the fields with an energy
 * at `prefix` in a phenomenon. prefix: "" or the name of a model and '.'. */
static unsigned room_cells(Elab *e, uint32_t phen, const char *prefix)
{
    unsigned n = 0;
    for (;; n++) {
        char rel[160];
        snprintf(rel, sizeof rel, "%sc%u.energy", prefix, n);
        if (find_cell(e, phen, rel, NULL) == QI_NONE) return n;
    }
}

static void kernel_use(Elab *e, uint32_t phen, uint32_t model)
{
    Front *f = e->f;
    const InstInfo *in = &f->insts.v[e->insts.v[phen]];
    const ModelRef *mr = model == QI_NONE ? NULL : model_at(e, phen, model);
    Buf name = { 0 }, who = { 0 };
    if (e->fevaluator == QF_BOTTOM || !text_value(e, node_arc(mr ? mr->node : in->node, e->fevaluator), &name)) {
        qf_buf_free(&name);
        return;
    }
    Loc loc = inst_loc(in);
    qf_buf_printf(&who, "%s%s%s", in->name, mr ? "." : "", mr ? mr->name : "");
    const QkKernel *k = qi_find_kernel(e->opt, name.s);
    QkKernel *made = NULL;
    unsigned size = 0;
    if (!k) {
        /* a kernel of a room has a contract for each number of cells */
        char prefix[160];
        snprintf(prefix, sizeof prefix, "%s%s", mr ? mr->name : "", mr ? "." : "");
        size = room_cells(e, phen, prefix);
        k = made = qi_room_kernel(name.s, size);
    }
    if (!mr && nmodels(e, phen)) {
        err(e, loc, "%s has models and names the evaluator \"%s\": a phenomenon with models has no kernel of its own. "
            "Give the evaluator to a model", in->name, name.s);
        k = NULL;
    } else if (!k && room_kernel_name(name.s)) {
        err(e, loc, "%s: the kernel %s is for a room with the cells c0, c1, and so on, and %s has no field c0 with an "
            "energy", who.s, name.s, who.s);
    } else if (!k) {
        err(e, loc, "%s: unknown evaluator \"%s\"", who.s, name.s);
    } else if (k->flags & QK_MAP) {
        err(e, loc, "%s: the evaluator \"%s\" is a map kernel, which moves the state at a switch. Name it in a map, as in "
            "`refine: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"%s\" }`", who.s, name.s, name.s);
        k = NULL;
    }
    qf_buf_free(&name);
    if (!k) {
        qf_buf_free(&who);
        qk_free(made);
        return;
    }
    QiKernelUse ku = { phen, model, keep(e, k->name), made ? size : 0,
                       qf_alloc(e->mem, (k->nslots ? k->nslots : 1) * sizeof(QiBind)), k->nslots,
                       qf_alloc(e->mem, (k->ntransfers ? k->ntransfers : 1) * sizeof(QiTransfer)), k->ntransfers };
    bool ok = true;
    for (uint32_t s = 0; s < k->nslots; s++) {
        const char *slot = k->slots[s].name, *open = NULL;
        uint32_t c = QI_NONE, cp = port_named(e, phen, slot, strcspn(slot, "."));
        Buf rel = { 0 };
        if (mr && !(cp != QI_NONE && e->ports.v[cp].state == QI_PORT_CONNECTED)) {
            qf_buf_printf(&rel, "%s.%s", mr->name, slot);
            c = find_cell(e, phen, rel.s, NULL);
        }
        if (c == QI_NONE) {
            rel.len = 0;
            qf_buf_puts(&rel, slot);
            c = find_cell(e, phen, rel.s, &open);
        }
        uint32_t mphen = QI_NONE, *mcells = c == QI_NONE ? NULL : model_cells(e, phen, rel.s, &mphen);
        qf_buf_free(&rel);
        if (c == QI_NONE) {
            if (open)
                err(e, loc, "%s: the kernel %s needs %s.%s, and the port %s.%s is not connected", who.s, k->name, who.s,
                    slot, in->name, open);
            else
                err(e, loc, "%s: the kernel %s needs %s.%s, which is not a cell", who.s, k->name, who.s, slot);
            ok = false;
        } else if (e->cells.v[c].derived && (k->slots[s].role == QK_OUT || k->slots[s].role == QK_STATE)) {
            err(e, loc, "%s: the kernel %s writes its slot %s, and %s is a derived field: the laws compute it, so a "
                "kernel cannot write it", who.s, k->name, slot, e->cells.v[c].path);
            ok = false;
        }
        for (uint32_t m = 0; mcells && m < nmodels(e, mphen); m++)
            if (mcells[m] == QI_NONE) {
                err(e, loc, "%s: the kernel %s needs %s.%s through a port of %s, and the model %s gives no cell for it",
                    who.s, k->name, who.s, slot, e->phens.v[mphen].name, model_at(e, mphen, m)->name);
                ok = false;
            }
        ku.binds[s] = (QiBind) { keep(e, slot), k->slots[s].role, c, mphen, mcells };
    }
    for (uint32_t t = 0; ok && t < k->ntransfers; t++) {
        const QkTransfer *kt = &k->transfers[t];
        ku.transfers[t] = (QiTransfer) { keep(e, kt->name), kt->from == QK_NONE ? QI_NONE : ku.binds[kt->from].cell,
                                         kt->to == QK_NONE ? QI_NONE : ku.binds[kt->to].cell, kt->conserved, QI_NONE,
                                         QI_NONE, NULL, QI_NONE };
    }
    if (ok) VEC_PUSH(e->kernels, ku);
    qf_buf_free(&who);
    qk_free(made);
}

/* --- interactions with a rate (ADR 0020) ------------------------------------------------------------ */

static int cmp_inter(const void *a, const void *b) { return strcmp(((const Inter *) a)->text, ((const Inter *) b)->text); }

/* The unit of a dimension, for a message: m^3/s is "m^3 s^-1". */
static void dim_unit(Elab *e, Buf *b, const QDim *d)
{
    Buf t = { 0 };
    qf_dim_format(e->f, &t, d);
    qf_buf_puts(b, qi_unit(t.s));
    qf_buf_free(&t);
}

/* One end of an interaction: the stock, the name of its slot, and for a
 * stock behind a port of the active model, the stock of each model. */
typedef struct { uint32_t cell; const char *slot; uint32_t mphen; uint32_t *mcells; } End;

/* One end of an interaction: the stock that the field `feat` names. The cell
 * of *end is QI_NONE if the interaction does not state the end. False with a
 * diagnostic. */
static bool inter_end(Elab *e, uint32_t phen, const Inter *in, uint32_t feat, const char *what, Loc loc, End *end)
{
    Front *f = e->f;
    const InstInfo *ii = &f->insts.v[e->insts.v[phen]];
    const QfNode *n = node_arc(in->node, feat);
    *end = (End) { QI_NONE, NULL, QI_NONE, NULL };
    if (!n) return true;
    Buf rel = { 0 };
    const char *open;
    uint32_t c = node_cell(e, phen, n, &in->path, &rel, &open);
    uint32_t mphen = QI_NONE, *mcells = c == QI_NONE ? NULL : model_cells(e, phen, rel.s, &mphen);
    bool ok = true;
    if (c == QI_NONE) {
        /* no cell: the end is not stated, or it is not a path to a cell */
        if (open) {
            err(e, loc, "the %s of %s goes through the port %s.%s, which is not connected", what, in->text, ii->name, open);
            ok = false;
        } else if (n->type != f->top || (n->flags & NF_VALUE) || n->narcs) {
            err(e, loc, "the %s of %s must be a path to a stock, as in `%s: .energy`", what, in->text, what);
            ok = false;
        }
    } else {
        if (!e->cells.v[c].stock) {
            err(e, loc, "the %s of %s is %s, which is not a stock: an interaction moves a quantity of a kind with "
                "`conserved: true`", what, in->text, e->cells.v[c].path);
            ok = false;
        } else {
            /* the name of the slot: the path in its own phenomenon, or the path through the port */
            const char *slot = e->cells.v[c].phen == phen && mphen == QI_NONE ? keep(e, e->cells.v[c].path + strlen(ii->name) + 1)
                                                                              : keep(e, rel.s);
            *end = (End) { c, slot, mphen, mcells };
        }
        for (uint32_t m = 0; ok && mcells && m < nmodels(e, mphen); m++)
            if (mcells[m] == QI_NONE || !e->cells.v[mcells[m]].stock) {
                err(e, loc, "the %s of %s goes through a port of %s, and the model %s gives no stock for it", what,
                    in->text, e->phens.v[mphen].name, model_at(e, mphen, m)->name);
                ok = false;
            }
    }
    qf_buf_free(&rel);
    return ok;
}

/* The bind of a cell in a kernel use: one slot for one cell. A stock of the
 * phenomenon can also be the rate of another interaction, as a speed is for
 * a position: the kernel reads a stock slot. */
static void inter_bind(QiKernelUse *ku, size_t cap, QkRole role, End end)
{
    for (uint32_t i = 0; i < ku->nbinds; i++) {
        if (ku->binds[i].cell != end.cell) continue;
        if (role == QK_STOCK) ku->binds[i].role = QK_STOCK;
        return;
    }
    if (ku->nbinds < cap) ku->binds[ku->nbinds++] = (QiBind) { end.slot, role, end.cell, end.mphen, end.mcells };
}

/* "roomA.energy -> roomB.energy, conserved": a transfer, for a message. */
static void put_transfer(Elab *e, Buf *b, uint32_t from, uint32_t to, bool conserved)
{
    qf_buf_printf(b, "%s -> %s%s", from == QI_NONE ? "nothing" : e->cells.v[from].path,
                  to == QI_NONE ? "nothing" : e->cells.v[to].path, conserved ? ", conserved" : ", not conserved");
}

/* An interaction with no rate states one transfer of the kernel that its
 * phenomenon or its model names (ADR 0021 section 6). The kernel must make a
 * transfer from the stock of `source` to the stock of `sink`, with the same
 * flag `conserved`. The statement is optional: a kernel can make a transfer
 * that no interaction states. */
static void stated_transfer(Elab *e, uint32_t phen, uint32_t owner, const Inter *in, Loc loc)
{
    Front *f = e->f;
    QiKernelUse *ku = NULL;
    for (size_t k = 0; k < e->kernels.n; k++)
        if (e->kernels.v[k].phen == phen && e->kernels.v[k].model == owner && strcmp(e->kernels.v[k].kernel, "native.rate"))
            ku = &e->kernels.v[k];
    if (!ku) return;                                 /* the kernel has an error of its own */
    End from, to;
    bool ends = inter_end(e, phen, in, e->fsource, "source", loc, &from);
    if (!inter_end(e, phen, in, e->fsink, "sink", loc, &to) || !ends) return;
    const QfNode *cn = node_arc(in->node, e->fcons);
    bool conserved = !cn || strcmp(f->types.v[cn->type].name, "false") != 0;
    if (from.cell == QI_NONE && to.cell == QI_NONE) {
        err(e, loc, "%s has no rate, no source and no sink: an interaction with no rate states a transfer of the kernel %s, "
            "as in `source: .hot.energy, sink: .cold.energy`", in->text, ku->kernel);
        return;
    }
    QiTransfer *match = NULL;
    for (uint32_t t = 0; t < ku->ntransfers && !match; t++)
        if (ku->transfers[t].from == from.cell && ku->transfers[t].to == to.cell) match = &ku->transfers[t];
    Buf says = { 0 }, has = { 0 };
    put_transfer(e, &says, from.cell, to.cell, conserved);
    for (uint32_t t = 0; t < ku->ntransfers; t++) {
        qf_buf_printf(&has, "%s%s (", t ? ", " : "", ku->transfers[t].name);
        put_transfer(e, &has, ku->transfers[t].from, ku->transfers[t].to, ku->transfers[t].conserved);
        qf_buf_putc(&has, ')');
    }
    if (!match)
        err(e, loc, "%s states the transfer %s, and the kernel %s makes no transfer between these stocks. Its transfers: %s",
            in->text, says.s, ku->kernel, has.len ? has.s : "none");
    else if (match->conserved != conserved)
        err(e, loc, "%s states the transfer %s, and the transfer %s of the kernel %s is %s: write `conserved: %s`", in->text,
            says.s, match->name, ku->kernel, match->conserved ? "conserved" : "not conserved",
            match->conserved ? "true" : "false");
    else if (match->stated)
        err(e, loc, "%s and %s state the same transfer, %s of the kernel %s: one interaction states one transfer", match->stated,
            in->text, match->name, ku->kernel);
    else match->stated = keep(e, in->text);
    qf_buf_free(&says);
    qf_buf_free(&has);
}

/* Makes the kernel native.rate of a phenomenon, or of its model `owner`,
 * from the interactions with a rate that stand there: each is one transfer
 * (ADR 0020 section 6). */
static void owner_interactions(Elab *e, uint32_t phen, uint32_t owner, const InterVec *inters)
{
    Front *f = e->f;
    const InstInfo *ii = &f->insts.v[e->insts.v[phen]];
    const ModelRef *mr = owner == QI_NONE ? NULL : model_at(e, phen, owner);
    Loc loc = inst_loc(ii);
    Buf who = { 0 };
    qf_buf_printf(&who, "%s%s%s", ii->name, mr ? "." : "", mr ? mr->name : "");
    /* the evaluator that the phenomenon or the model names, if it names one */
    Buf evname = { 0 };
    if (e->fevaluator != QF_BOTTOM) text_value(e, node_arc(mr ? mr->node : ii->node, e->fevaluator), &evname);
    const char *evaluator = evname.s;

    QiKernelUse ku = { phen, owner, "native.rate", 0, qf_alloc(e->mem, (inters->n ? inters->n : 1) * 5 * sizeof(QiBind)), 0,
                       qf_alloc(e->mem, (inters->n ? inters->n : 1) * sizeof(QiTransfer)), 0 };
    bool ok = true;
    for (size_t i = 0; i < inters->n; i++) {
        const Inter *in = &inters->v[i];
        if (path_model(e, phen, in->path.v, in->path.n) != owner) continue;
        const char *name = in->text + strlen(ii->name) + 1;
        size_t errors = f->nerrors;                  /* each interaction reports its own errors */
        const QfNode *rn = node_arc(in->node, e->frate), *cn = node_arc(in->node, e->fcons);
        const QfNode *qn = e->fratio == QF_BOTTOM ? NULL : node_arc(in->node, e->fratio);
        /* A flow of a reaction has a ratio and no rate: the rate and the
         * tolerance are those of the reaction (ADR 0021 section 7). */
        size_t rlen;
        const QfNode *rx = reaction_of(e, phen, in, &rlen);
        U32Vec holder = in->path;                    /* the structure that has the rate: the interaction, or its reaction */
        if (rx) holder.n = rlen;
        Sym rtext = rx ? path_sym(e, ii->name, holder.v, holder.n) : in->text;
        const char *rname = rtext + strlen(ii->name) + 1;
        if (rx && !unstated(f, rn)) {
            err(e, loc, "%s is a flow of the reaction %s, and it has a rate: the reaction gives the rate. Give the flow a "
                "ratio, as in `ratio: 1`", in->text, rtext);
            ok = false;
            continue;
        }
        if (rx && unstated(f, qn)) {
            err(e, loc, "%s is a flow of the reaction %s, and it has no ratio: give the amount of its stock for each unit of "
                "the reaction, as in `ratio: 1`", in->text, rtext);
            ok = false;
            continue;
        }
        if (!rx && !unstated(f, qn)) {
            err(e, loc, "%s has a ratio, and it is not in a reaction: a ratio is for a flow of a #Reaction. An interaction "
                "alone has a rate, as in `rate: .power`", in->text);
            ok = false;
            continue;
        }
        if (rx) rn = node_arc(rx, e->frate);
        TypeKind rk = rn ? f->types.v[rn->type].kind : TK_TOP;
        if (unstated(f, rn)) {
            /* no rate: the interaction states a transfer of the kernel in C */
            if (rx) {
                err(e, loc, "the reaction %s has no rate: give the amount of the reaction for each second, as in `rate: "
                    ".burning`", rtext);
                ok = false;
            } else if (!evaluator) {
                err(e, loc, "%s has no rate, and %s names no evaluator: nothing moves the quantity. Give the interaction "
                    "a rate, as in `rate: .power`", in->text, who.s);
                ok = false;
            } else stated_transfer(e, phen, owner, in, loc);
            continue;
        }
        if (evaluator) {
            err(e, loc, "%s has a rate, and %s names the evaluator \"%s\": a phenomenon moves its quantities with rates or "
                "with a kernel, and not with both", rtext, who.s, evaluator);
            ok = false;
            continue;
        }
        if (!mr && nmodels(e, phen)) {
            err(e, loc, "%s has a rate, and %s has models: a phenomenon with models has no kernel of its own. Put the "
                "%s in a model", rtext, ii->name, rx ? "reaction" : "interaction");
            ok = false;
            continue;
        }
        if (!(rn->flags & NF_VALUE) || (rk != TK_KIND && rk != TK_DIMT)) {
            err(e, loc, "the rate of %s must be a quantity: the amount for each second", rtext);
            ok = false;
            continue;
        }
        Buf rate_path = { 0 }, tol_path = { 0 }, ratio_path = { 0 };
        qf_buf_printf(&rate_path, "%s.rate", rname);
        qf_buf_printf(&tol_path, "%s.tolerance", rname);
        qf_buf_printf(&ratio_path, "%s.ratio", name);
        uint32_t rate = find_cell(e, phen, rate_path.s, NULL), tol = find_cell(e, phen, tol_path.s, NULL);
        uint32_t ratio = rx ? find_cell(e, phen, ratio_path.s, NULL) : QI_NONE;
        qf_buf_free(&rate_path);
        qf_buf_free(&tol_path);
        qf_buf_free(&ratio_path);
        End from, to;
        bool ends = inter_end(e, phen, in, e->fsource, "source", loc, &from);
        if (!inter_end(e, phen, in, e->fsink, "sink", loc, &to)) ends = false;
        /* a rate that is a field of another phenomenon stands also behind a port: the walk gave it no cell there */
        Buf other = { 0 };
        const char *open;
        uint32_t behind = node_cell(e, phen, rn, &holder, &other, &open);
        qf_buf_free(&other);
        if (rate == QI_NONE || tol == QI_NONE || (behind != QI_NONE && e->cells.v[behind].phen != phen)) {
            err(e, loc, "the rate and the tolerance of %s must be fields of %s: give %s a field with a law for the rate",
                rtext, ii->name, ii->name);
            ok = false;
            continue;
        }
        /* a ratio that is a field of another phenomenon stands behind a port, as a rate can */
        Buf rother = { 0 };
        uint32_t rbehind = rx ? node_cell(e, phen, qn, &in->path, &rother, &open) : QI_NONE;
        qf_buf_free(&rother);
        if (rbehind != QI_NONE && e->cells.v[rbehind].phen != phen) ratio = QI_NONE;
        if (rx && (ratio == QI_NONE || e->cells.v[ratio].domain == QI_SET || e->cells.v[ratio].phen != phen)) {
            err(e, loc, "the ratio of %s must be a number or a quantity that is a field of %s, as in `ratio: 1` or `ratio: "
                ".heating`", in->text, ii->name);
            ok = false;
            continue;
        }
        if (!ends) { ok = false; continue; }
        bool conserved = !cn || strcmp(f->types.v[cn->type].name, "false") != 0;
        const End *both[2] = { &from, &to };
        for (int k = 0; k < 2; k++) {
            uint32_t c = both[k]->cell;
            if (c == QI_NONE || e->cells.v[c].phen != phen || e->cells.v[c].model == QI_NONE || e->cells.v[c].model == owner)
                continue;
            err(e, loc, "%s moves the stock %s of the model %s, and the interaction is in the model %s: an interaction of "
                "a model moves the stocks of that model", in->text, e->cells.v[c].path,
                model_at(e, phen, e->cells.v[c].model)->name, mr ? mr->name : "?");
            ok = false;
        }
        if (from.cell == QI_NONE && to.cell == QI_NONE) {
            err(e, loc, "%s has no source and no sink: an interaction moves a stock, as in `sink: .energy`", in->text);
            ok = false;
        } else if (from.cell == to.cell) {
            err(e, loc, "the source and the sink of %s are the same stock, %s", in->text, e->cells.v[from.cell].path);
            ok = false;
        } else if (from.cell != QI_NONE && to.cell != QI_NONE && e->cells.v[from.cell].kind != e->cells.v[to.cell].kind) {
            err(e, loc, "%s moves %s to %s, and they have different kinds, %s and %s: a quantity keeps its kind", in->text,
                e->cells.v[from.cell].path, e->cells.v[to.cell].path, e->kinds.v[e->cells.v[from.cell].kind].name,
                e->kinds.v[e->cells.v[to.cell].kind].name);
            ok = false;
        } else if (from.cell != QI_NONE && to.cell != QI_NONE && !conserved) {
            err(e, loc, "%s has a source and a sink, so it conserves the quantity: remove `conserved: false`", in->text);
            ok = false;
        } else if ((from.cell == QI_NONE || to.cell == QI_NONE) && conserved) {
            const char *has = from.cell == QI_NONE ? "sink" : "source", *lacks = from.cell == QI_NONE ? "source" : "sink";
            err(e, loc, "%s has a %s and no %s: the quantity then comes from outside the world, or goes there. If that is "
                "the intent, write `conserved: false`. If not, give the %s", in->text, has, lacks, lacks);
            ok = false;
        }
        if (f->nerrors != errors) continue;
        /* the rate is the quantity of the stock for each second */
        uint32_t stock = from.cell != QI_NONE ? from.cell : to.cell;
        QDim second = qdim_base(Q_BASE_TIME), want;
        if (rx) {
            /* a flow: the ratio times the rate of the reaction is the quantity of the stock for each second */
            QDim flow, per;
            bool fits = qdim_div(&e->ctypes.v[stock].dim, &second, &want) == Q_OK &&
                        qdim_mul(&e->ctypes.v[ratio].dim, &e->ctypes.v[rate].dim, &flow) == Q_OK && qdim_eq(&want, &flow);
            if (!fits) {
                Buf has = { 0 }, needs = { 0 }, su = { 0 }, ru = { 0 };
                dim_unit(e, &has, &e->ctypes.v[ratio].dim);
                dim_unit(e, &su, &e->ctypes.v[stock].dim);
                dim_unit(e, &ru, &e->ctypes.v[rate].dim);
                if (qdim_div(&e->ctypes.v[stock].dim, &second, &want) == Q_OK &&
                    qdim_div(&want, &e->ctypes.v[rate].dim, &per) == Q_OK)
                    dim_unit(e, &needs, &per);
                err(e, loc, "the ratio of %s is in %s, the rate of the reaction %s is in %s, and the stock %s is in %s: the "
                    "ratio must be the quantity of the stock for each unit of the reaction, in %s", in->text,
                    has.len ? has.s : "1", rtext, ru.len ? ru.s : "1", e->cells.v[stock].path, su.len ? su.s : "1",
                    needs.len ? needs.s : "1");
                qf_buf_free(&has);
                qf_buf_free(&needs);
                qf_buf_free(&su);
                qf_buf_free(&ru);
                ok = false;
                continue;
            }
        } else if (qdim_div(&e->ctypes.v[stock].dim, &second, &want) != Q_OK || !qdim_eq(&want, &e->ctypes.v[rate].dim)) {
            Buf has = { 0 }, needs = { 0 }, su = { 0 };
            dim_unit(e, &has, &e->ctypes.v[rate].dim);
            dim_unit(e, &su, &e->ctypes.v[stock].dim);
            if (qdim_div(&e->ctypes.v[stock].dim, &second, &want) == Q_OK) dim_unit(e, &needs, &want);
            err(e, loc, "the rate of %s is %s, in %s, and the stock %s is in %s: the rate must be the quantity for each "
                "second, in %s", in->text, e->cells.v[rate].path, has.len ? has.s : "1", e->cells.v[stock].path,
                su.len ? su.s : "1", needs.len ? needs.s : "s^-1");
            qf_buf_free(&has);
            qf_buf_free(&needs);
            qf_buf_free(&su);
            ok = false;
            continue;
        }
        size_t cap = inters->n * 5;
        End rate_end = { rate, keep(e, e->cells.v[rate].path + strlen(ii->name) + 1), QI_NONE, NULL };
        End tol_end = { tol, keep(e, e->cells.v[tol].path + strlen(ii->name) + 1), QI_NONE, NULL };
        inter_bind(&ku, cap, QK_IN, rate_end);
        inter_bind(&ku, cap, QK_IN, tol_end);
        if (rx) {
            End ratio_end = { ratio, keep(e, e->cells.v[ratio].path + strlen(ii->name) + 1), QI_NONE, NULL };
            inter_bind(&ku, cap, QK_IN, ratio_end);
        }
        if (from.cell != QI_NONE)
            inter_bind(&ku, cap, e->cells.v[from.cell].phen == phen && from.mphen == QI_NONE ? QK_STOCK : QK_LINK, from);
        if (to.cell != QI_NONE)
            inter_bind(&ku, cap, e->cells.v[to.cell].phen == phen && to.mphen == QI_NONE ? QK_STOCK : QK_LINK, to);
        ku.transfers[ku.ntransfers++] =
            (QiTransfer) { keep(e, name), from.cell, to.cell, from.cell != QI_NONE && to.cell != QI_NONE, rate, tol,
                           keep(e, in->text), ratio };
    }
    if (ok && ku.ntransfers) VEC_PUSH(e->kernels, ku);
    qf_buf_free(&evname);
    qf_buf_free(&who);
}

static void phen_interactions(Elab *e, uint32_t phen, size_t conn0, size_t conn1)
{
    Front *f = e->f;
    if (e->tinter == QF_BOTTOM) return;
    const InstInfo *ii = &f->insts.v[e->insts.v[phen]];
    InterVec inters = { 0 };
    U32Vec path = { 0 };
    NodeStack stack = { 0 };
    find_inters(e, ii->name, ii->node, &path, &stack, conn0, conn1, &inters);
    VEC_FREE(path);
    VEC_FREE(stack);
    if (inters.n) qsort(inters.v, inters.n, sizeof(Inter), cmp_inter);
    /* the interactions of the phenomenon itself, and then those of each model */
    owner_interactions(e, phen, QI_NONE, &inters);
    for (uint32_t m = 0; m < nmodels(e, phen); m++) owner_interactions(e, phen, m, &inters);
    /* a reaction is a set of flows (ADR 0021 section 7) */
    InterVec reactions = { 0 };
    if (e->treact != QF_BOTTOM) {
        U32Vec rpath = { 0 };
        NodeStack rstack = { 0 };
        find_typed(e, e->treact, ii->name, ii->node, &rpath, &rstack, conn0, conn1, &reactions);
        VEC_FREE(rpath);
        VEC_FREE(rstack);
    }
    for (size_t r = 0; r < reactions.n; r++) {
        bool flows = false;
        for (size_t i = 0; i < inters.n; i++) flows |= starts_with(inters.v[i].path.v, inters.v[i].path.n, &reactions.v[r].path);
        if (!flows)
            err(e, inst_loc(ii), "the reaction %s has no flows: give it an interaction with a ratio for each stock that it "
                "changes, as in `fuel: #Interaction & { source: .wood, ratio: 1, conserved: false }`", reactions.v[r].text);
        VEC_FREE(reactions.v[r].path);
    }
    VEC_FREE(reactions);
    for (size_t i = 0; i < inters.n; i++) VEC_FREE(inters.v[i].path);
    VEC_FREE(inters);
}

/* --- maps (ADR 0021 section 5) ---------------------------------------------------------------- */

/* --- truth values (ADR 0022) ------------------------------------------------------------------- */

static const char *const truth_forms =
    "A condition has a path to a field with the type bool, a comparison of a path with an atom (== or !=), a comparison "
    "of two quantities, and the operators !, && and ||";

enum { ATOM_FALSE = 1, ATOM_TRUE = 2, ATOM_BOTH = 3 };

/* The enumeration of the type bool: false, true. */
static uint32_t use_bool(Elab *e)
{
    Front *f = e->f;
    for (size_t i = 0; i < e->usedenums.n; i++)
        if (e->usedenums.v[i] == f->tbool) return (uint32_t) i;
    QiEnum en = { keep(e, f->types.v[f->tbool].name), qf_alloc(e->mem, 2 * sizeof(char *)), 2 };
    en.atoms[0] = keep(e, "false");
    en.atoms[1] = keep(e, "true");
    VEC_PUSH(e->usedenums, f->tbool);
    VEC_PUSH(e->enums, en);
    return (uint32_t) e->usedenums.n - 1;
}

/* A cell with the type bool: a set cell with the atoms false and true. */
static bool is_truth(const Elab *e, uint32_t c)
{
    if (e->cells.v[c].domain != QI_SET) return false;
    const QiEnum *en = &e->enums.v[e->cells.v[c].kind];
    return en->natoms == 2 && !strcmp(en->atoms[0], "false") && !strcmp(en->atoms[1], "true");
}

static void emit_truth(Law *l, QiOp op, uint32_t z, uint32_t a, uint32_t b, QInterval range, uint64_t mask)
{
    emit(l, op, z, a, b);
    l->e->props.v[l->e->props.n - 1].range = range;
    l->e->props.v[l->e->props.n - 1].mask = mask;
}

static void emit_logic(Law *l, QiOp op, uint32_t z, uint32_t a, uint32_t b) { emit_truth(l, op, z, a, b, (QInterval) { 0, 0 }, 0); }

/* The cell of a result: z, or a new inner value. */
static uint32_t truth_result(Law *l, uint32_t z) { return z != QI_NONE ? z : inner_set(l, use_bool(l->e), ATOM_BOTH); }

/* z <=> c, for two cells with the type bool. Two cells of one enumeration
 * are equal. If not, two negations make them equal. */
static void truth_equal(Law *l, uint32_t z, uint32_t c)
{
    Elab *e = l->e;
    if (e->cells.v[z].kind == e->cells.v[c].kind) {
        emit_logic(l, QI_EQUAL, z, c, QI_NONE);
        return;
    }
    uint32_t t = inner_set(l, use_bool(e), ATOM_BOTH);
    if (t == QI_NONE) return;
    emit_logic(l, QI_NOT, t, c, QI_NONE);
    emit_logic(l, QI_NOT, z, t, QI_NONE);
}

/* A condition with a value that the elaborator knows. */
static uint32_t truth_constant(Law *l, bool value, uint32_t z)
{
    if (z == QI_NONE) return inner_set(l, use_bool(l->e), value ? ATOM_TRUE : ATOM_FALSE);
    uint32_t c = inner_set(l, use_bool(l->e), value ? ATOM_FALSE : ATOM_TRUE);
    if (c == QI_NONE) return QI_NONE;
    emit_logic(l, QI_NOT, z, c, QI_NONE);
    return z;
}

/* A part of a sum: an expression with its factor, after the same text on
 * the two sides of a comparison is one part. */
typedef struct { const X *x; char *text; int coef; uint32_t cell; } Term;
typedef VEC(Term) TermVec;

static void sum_terms(const X *x, int sign, TermVec *out)
{
    if (x->k == X_ARITH && (x->u.ar.op == '+' || x->u.ar.op == '-')) {
        sum_terms(x->u.ar.a, sign, out);
        sum_terms(x->u.ar.b, x->u.ar.op == '+' ? sign : -sign, out);
        return;
    }
    char *text = x_text(x);
    for (size_t i = 0; i < out->n; i++)
        if (!strcmp(out->v[i].text, text)) {
            out->v[i].coef += sign;
            free(text);
            return;
        }
    Term t = { x, text, sign, QI_NONE };
    VEC_PUSH(*out, t);
}

/* z = a op b for two cells, where z is a new inner value with no kind. */
static uint32_t sum_cells(Law *l, QiOp op, uint32_t a, uint32_t b)
{
    Elab *e = l->e;
    QType ta = e->ctypes.v[a], tb = e->ctypes.v[b], t;
    QStatus why = qtype_sum(&ta, &tb, &t);
    if (why) { misfit(l, why, QI_NONE, a, b); return QI_NONE; }
    t.kind = 0;                                      /* not state */
    uint32_t z = inner(l, &t, qi_top());
    if (z == QI_NONE) return QI_NONE;
    if (!fits(&t, &ta, &tb, op, &why)) { misfit(l, why, z, a, b); return QI_NONE; }
    emit(l, op, z, a, b);
    return z;
}

/* The sum of the parts whose factor has the sign `sign`. QI_NONE if there is
 * no such part, or with a diagnostic: then *bad is true. */
static uint32_t sum_side(Law *l, const TermVec *ts, int sign, bool *bad)
{
    uint32_t acc = QI_NONE;
    for (size_t i = 0; i < ts->n && !*bad; i++)
        for (int k = 0; k < ts->v[i].coef * sign && !*bad; k++) {
            acc = acc == QI_NONE ? ts->v[i].cell : sum_cells(l, QI_SUM, acc, ts->v[i].cell);
            *bad = acc == QI_NONE;
        }
    return *bad ? QI_NONE : acc;
}

/* The range of "x op 0" in quanta, for an order or an equation. */
static QInterval sign_range(int op)
{
    return op == 'e'   ? (QInterval) { 0, 0 }
           : op == '<' ? (QInterval) { Q_NINF, -1 }
           : op == 'l' ? (QInterval) { Q_NINF, 0 }
           : op == '>' ? (QInterval) { 1, Q_INF }
                       : (QInterval) { 0, Q_INF };
}

static bool literal_quanta(Elab *e, const XNum *num, uint32_t cell, int64_t *lo, int64_t *hi, char *why, size_t len);

/* r <=> d is in the range, or for `!=`, r <=> d is not in the range. */
static uint32_t truth_range(Law *l, int op, uint32_t d, QInterval range, uint32_t z)
{
    uint32_t r = truth_result(l, z);
    if (r == QI_NONE) return QI_NONE;
    if (op != 'n') {
        emit_truth(l, QI_IN_RANGE, r, d, QI_NONE, range, 0);
        return r;
    }
    uint32_t t = inner_set(l, use_bool(l->e), ATOM_BOTH);
    if (t == QI_NONE) return QI_NONE;
    emit_truth(l, QI_IN_RANGE, t, d, QI_NONE, range, 0);
    emit_logic(l, QI_NOT, r, t, QI_NONE);
    return r;
}

/* A comparison of two quantities (ADR 0022 section 1). The same text on the
 * two sides goes away first: `.t > .t - 1K` is `1K > 0`. A comparison of one
 * cell with one number is a range of that cell. Each other comparison is a
 * range of the difference of the two sides. */
static uint32_t truth_compare(Law *l, const X *x, uint32_t z)
{
    Elab *e = l->e;
    int op = x->u.ar.op;
    TermVec ts = { 0 };
    sum_terms(x->u.ar.a, 1, &ts);
    sum_terms(x->u.ar.b, -1, &ts);
    uint32_t r = QI_NONE;
    bool bad = false;
    char why[240];
    /* Each part has a cell, also a part that goes away: its dimension must
     * fit. A number gets its cell later, and only if the comparison needs it. */
    QDim dim0 = qdim_none();
    for (size_t i = 0; i < ts.n && !bad; i++) {
        QDim dim;
        if (ts.v[i].x->k == X_NUM) {
            int64_t value;
            int exp;
            if (!qf_literal(e->f, &ts.v[i].x->u.num, &dim, &value, &exp, why, sizeof why)) {
                err(e, l->loc, "the condition %s: %s", l->law, why);
                bad = true;
            }
        } else {
            ts.v[i].cell = ts.v[i].x->k == X_TAG ? ref_cell(l, ts.v[i].x, true) : cell_of(l, ts.v[i].x);
            bad = ts.v[i].cell == QI_NONE;
            if (!bad && e->cells.v[ts.v[i].cell].domain == QI_SET) {
                err(e, l->loc, "the condition %s has a comparison with %s, which is a set of atoms. A set of atoms stands in "
                    "a comparison with an atom, with == or !=", l->law, e->cells.v[ts.v[i].cell].path);
                bad = true;
            }
            if (!bad) dim = e->ctypes.v[ts.v[i].cell].dim;
        }
        if (!bad && !i) dim0 = dim;
        if (!bad && i && !qdim_eq(&dim0, &dim)) {
            Buf has = { 0 }, needs = { 0 };
            dim_unit(e, &has, &dim);
            dim_unit(e, &needs, &dim0);
            err(e, l->loc, "the condition %s compares %s, in %s, with %s, in %s", l->law, ts.v[0].text,
                needs.len ? needs.s : "1", ts.v[i].text, has.len ? has.s : "1");
            qf_buf_free(&has);
            qf_buf_free(&needs);
            bad = true;
        }
    }
    const Term *num = NULL, *ref = NULL;
    size_t live = 0;
    for (size_t i = 0; i < ts.n; i++) {
        if (!ts.v[i].coef) continue;
        live++;
        if (ts.v[i].x->k == X_NUM) num = &ts.v[i];
        else ref = &ts.v[i];
    }
    int64_t lo = 0, hi = 0;
    if (bad) {
    } else if (live == 2 && num && ref && num->coef == -ref->coef && (ref->coef == 1 || ref->coef == -1)) {
        /* cell op number: a range of the cell. With the cell on the right, the order turns */
        int o = ref->coef > 0 ? op : op == '<' ? '>' : op == '>' ? '<' : op == 'l' ? 'g' : op == 'g' ? 'l' : op;
        if (!literal_quanta(e, &num->x->u.num, ref->cell, &lo, &hi, why, sizeof why)) {
            err(e, l->loc, "the condition %s: %s", l->law, why);
        } else if (lo != hi && (op == 'e' || op == 'n')) {
            err(e, l->loc, "the condition %s compares %s with %s: the number is between two steps of the field, so the two are "
                "never equal. Use %s", l->law, ref->text, num->text, op == 'e' ? "<= or >=" : "< or >");
        } else {
            QInterval range = o == '<'   ? (QInterval) { Q_NINF, hi - 1 }
                              : o == 'l' ? (QInterval) { Q_NINF, lo }
                              : o == '>' ? (QInterval) { lo + 1, Q_INF }
                              : o == 'g' ? (QInterval) { hi, Q_INF }
                                         : (QInterval) { lo, lo };
            r = truth_range(l, op, ref->cell, range, z);
        }
    } else {
        for (size_t i = 0; i < ts.n && !bad; i++)
            if (ts.v[i].coef && ts.v[i].x->k == X_NUM) bad = (ts.v[i].cell = cell_of(l, ts.v[i].x)) == QI_NONE;
        uint32_t p = sum_side(l, &ts, 1, &bad), n = sum_side(l, &ts, -1, &bad);
        if (bad) {
        } else if (p == QI_NONE && n == QI_NONE) {
            /* each part went away: 0 op 0 */
            r = truth_constant(l, op == 'e' || op == 'l' || op == 'g', z);
        } else {
            /* p - n op 0. With no p, n has the other order */
            uint32_t d = p == QI_NONE ? n : n == QI_NONE ? p : sum_cells(l, QI_DIFFERENCE, p, n);
            int o = p != QI_NONE ? op : op == '<' ? '>' : op == '>' ? '<' : op == 'l' ? 'g' : op == 'g' ? 'l' : op;
            if (d != QI_NONE) r = truth_range(l, op, d, sign_range(o == 'n' ? 'e' : o), z);
        }
    }
    for (size_t i = 0; i < ts.n; i++) free(ts.v[i].text);
    VEC_FREE(ts);
    return r;
}

/* A comparison of a path with an atom: r <=> the set cell is in the atoms. */
static uint32_t truth_atom(Law *l, const X *x, uint32_t z)
{
    Elab *e = l->e;
    int op = x->u.ar.op;
    const X *ref = x->u.ar.a->k == X_ATOM ? x->u.ar.b : x->u.ar.a, *val = x->u.ar.a->k == X_ATOM ? x->u.ar.a : x->u.ar.b;
    char *xt = x_text(x);
    uint32_t r = QI_NONE, s = QI_NONE;
    if (ref->k != X_TAG || (op != 'e' && op != 'n'))
        err(e, l->loc, "the condition %s has the comparison %s. An atom stands in a comparison with one path, with == or !=",
            l->law, xt);
    else s = ref_cell(l, ref, true);
    if (s != QI_NONE && e->cells.v[s].domain != QI_SET) {
        err(e, l->loc, "the condition %s has the comparison %s, and %s is a quantity: compare it with a quantity", l->law, xt,
            e->cells.v[s].path);
    } else if (s != QI_NONE) {
        const QiEnum *en = &e->enums.v[e->cells.v[s].kind];
        const char *atom = atom_name(e, val->u.name);
        uint64_t bit = 0, all = all_atoms(en);
        for (uint32_t a = 0; a < en->natoms; a++)
            if (!strcmp(en->atoms[a], atom)) bit = (uint64_t) 1 << a;
        uint64_t mask = op == 'e' ? bit : all & ~bit;
        if (!bit) {
            Buf atoms = { 0 };
            for (uint32_t a = 0; a < en->natoms; a++) qf_buf_printf(&atoms, "%s%s", a ? ", " : "", en->atoms[a]);
            err(e, l->loc, "the condition %s has the comparison %s, and %s is not an atom of %s. Its atoms: %s", l->law, xt,
                atom, e->cells.v[s].path, atoms.s);
            qf_buf_free(&atoms);
        } else if (!mask || mask == all) {
            r = truth_constant(l, mask == all, z);   /* the field has one atom */
        } else if ((r = truth_result(l, z)) != QI_NONE) {
            emit_truth(l, QI_IN_SET, r, s, QI_NONE, (QInterval) { 0, 0 }, mask);
        }
    }
    free(xt);
    return r;
}

/* The cell with the value of a condition (ADR 0022 sections 1 to 3). z: the
 * cell that gets the result, or QI_NONE: then the result is the cell of a
 * path, or a new inner value. QI_NONE with a diagnostic. */
static uint32_t truth(Law *l, const X *x, uint32_t z)
{
    Elab *e = l->e;
    if (x->k == X_TAG) {
        uint32_t c = ref_cell(l, x, true);
        if (c == QI_NONE) return QI_NONE;
        if (!is_truth(e, c)) {
            err(e, l->loc, "the condition %s reads %s as a truth value, and it %s. %s", l->law, e->cells.v[c].path,
                e->cells.v[c].domain == QI_SET ? "does not have the type bool" : "is a quantity", truth_forms);
            return QI_NONE;
        }
        if (z == QI_NONE) return c;
        truth_equal(l, z, c);
        return z;
    }
    if (x->k == X_NOT) {
        uint32_t a = truth(l, x->u.sub, QI_NONE), r = a == QI_NONE ? QI_NONE : truth_result(l, z);
        if (r != QI_NONE) emit_logic(l, QI_NOT, r, a, QI_NONE);
        return r;
    }
    if (x->k == X_LOGIC) {
        uint32_t a = truth(l, x->u.ar.a, QI_NONE), b = a == QI_NONE ? QI_NONE : truth(l, x->u.ar.b, QI_NONE);
        uint32_t r = b == QI_NONE ? QI_NONE : truth_result(l, z);
        if (r != QI_NONE) emit_logic(l, x->u.ar.op == '&' ? QI_AND : QI_OR, r, a, b);
        return r;
    }
    if (x->k == X_CMP) return x->u.ar.a->k == X_ATOM || x->u.ar.b->k == X_ATOM ? truth_atom(l, x, z) : truth_compare(l, x, z);
    char *xt = x_text(x);
    err(e, l->loc, "the condition %s has the part %s, which is not a truth value. %s", l->law, xt, truth_forms);
    free(xt);
    return QI_NONE;
}

static int cmp_map(const void *a, const void *b) { return strcmp(((const QiMap *) a)->name, ((const QiMap *) b)->name); }

/* The quanta of a literal for a cell: floor and ceiling of its value in the
 * scale of the cell. False with a reason. */
static bool literal_quanta(Elab *e, const XNum *num, uint32_t cell, int64_t *lo, int64_t *hi, char *why, size_t len)
{
    QDim dim;
    int64_t value;
    int exp;
    if (!qf_literal(e->f, num, &dim, &value, &exp, why, len)) return false;
    if (!qdim_eq(&dim, &e->ctypes.v[cell].dim)) {
        Buf has = { 0 }, needs = { 0 };
        dim_unit(e, &has, &dim);
        dim_unit(e, &needs, &e->ctypes.v[cell].dim);
        snprintf(why, len, "the number is in %s, and %s is in %s", has.len ? has.s : "1", e->cells.v[cell].path,
                 needs.len ? needs.s : "1");
        qf_buf_free(&has);
        qf_buf_free(&needs);
        return false;
    }
    int shift = exp - e->ctypes.v[cell].scale;
    __extension__ typedef __int128 i128;
    i128 v = value;
    if (shift >= 0) {
        for (int i = 0; i < shift; i++) {
            v *= 10;
            if (v > Q_FMAX || v < Q_FMIN) { snprintf(why, len, "the number is too large for %s", e->cells.v[cell].path); return false; }
        }
        *lo = *hi = (int64_t) v;
        return true;
    }
    i128 d = 1;
    for (int i = 0; i < -shift && d <= (i128) INT64_MAX * 10; i++) d *= 10;
    i128 q = v / d, r = v % d;
    if (r < 0) { q -= 1; r += d; }                   /* floor */
    *lo = (int64_t) q;
    *hi = (int64_t) (r ? q + 1 : q);
    return true;
}

/* A condition that is not a statement about one cell of the phenomenon
 * (ADR 0022 section 3): a cell with the type bool at the field `when` of the
 * map has its value, and propagators compute the cell. The condition of the
 * world is then "the cell is true". False with a diagnostic. */
static bool when_cell(Elab *e, uint32_t phen, const char *name, const QfRecUse *use, const char *text, QiCondition *out)
{
    Front *f = e->f;
    const InstInfo *in = &f->insts.v[e->insts.v[phen]];
    const X *x = use->rec->expr;
    Loc loc = x->loc.file ? x->loc : inst_loc(in);
    Buf nb = { 0 };
    qf_buf_printf(&nb, "%s.%s.when", in->name, name);
    Sym path = qf_sym(f, nb.s);
    qf_buf_free(&nb);
    size_t errors = f->nerrors;
    if (u64map_get(&e->paths, (uint64_t) (uintptr_t) path, NULL)) {
        err(e, loc, "the condition of the map %s.%s needs the path %s for its value, and a field has that path", in->name, name,
            path);
        return false;
    }
    uint32_t z = derived_set(e, path, phen, QI_NONE, use_bool(e), ATOM_BOTH, false);
    Law l = { e, phen, use, text, path, loc, QI_NONE };
    if (truth(&l, x, z) != QI_NONE && f->nerrors == errors) {
        out->path = keep(e, path + strlen(in->name) + 1);
        out->cell = z;
        out->mask = ATOM_TRUE;
        out->text = keep(e, text);
    }
    return f->nerrors == errors;
}

/* The condition of a map (ADR 0022 section 1): a statement about one cell of
 * the phenomenon. `when` is a path to a field with the type bool, or a
 * comparison of one path with an atom or a quantity. The result has a NULL
 * path if the map has no condition. False with a diagnostic. */
static bool map_condition(Elab *e, uint32_t phen, uint32_t mapfeat, const QfNode *map, const char *name, QiCondition *out)
{
    Front *f = e->f;
    const InstInfo *in = &f->insts.v[e->insts.v[phen]];
    Loc loc = inst_loc(in);
    memset(out, 0, sizeof *out);
    out->cell = QI_NONE;
    const QfNode *wn = e->fwhen == QF_BOTTOM ? NULL : node_arc(map, e->fwhen);
    static const char *const forms = "A condition is a path to a field with the type bool, as in `when: .visible`, a "
                                     "comparison, as in `when: .temperature > 300K`, or conditions with !, && and ||";
    /* a comparison is a constraint record at the path of the field */
    QfRecUses uses = { 0 };
    qf_records(f, in->node, &in->ariths, &uses);
    const QfRecUse *use = NULL;
    for (size_t i = 0; i < uses.n && !use; i++) {
        const QfRecUse *u = &uses.v[i];
        size_t n = u->prefix.n + u->rec->path.n;
        uint32_t first = n ? (u->prefix.n ? u->prefix.v[0] : u->rec->path.v[0]) : QF_BOTTOM;
        uint32_t second = n == 2 ? (u->prefix.n == 2 ? u->prefix.v[1] : u->rec->path.v[u->rec->path.n - 1]) : QF_BOTTOM;
        if (n == 2 && first == mapfeat && second == e->fwhen) use = u;
    }
    bool ok = true;
    uint32_t cell = QI_NONE;
    Buf rel = { 0 }, text = { 0 };
    if (use) {
        const X *x = use->rec->expr, *ref = NULL, *val = NULL;
        char *xt = x_text(x);
        qf_buf_puts(&text, xt);
        free(xt);
        int op = x->k == X_CMP ? x->u.ar.op : 0;
        if (op && x->u.ar.a->k == X_TAG) { ref = x->u.ar.a; val = x->u.ar.b; }
        else if (op && x->u.ar.b->k == X_TAG) {
            /* the path is on the right: turn the comparison */
            ref = x->u.ar.b;
            val = x->u.ar.a;
            op = op == '<' ? '>' : op == '>' ? '<' : op == 'l' ? 'g' : op == 'g' ? 'l' : op;
        }
        /* Not one path with one constant: a cell at `when` has the value. A
         * quantity with != is two ranges, so it has a cell too. */
        if (!ref || (val->k != X_NUM && val->k != X_ATOM) || (val->k == X_NUM && op == 'n')) {
            ok = when_cell(e, phen, name, use, text.s, out);
            qf_buf_free(&rel);
            qf_buf_free(&text);
            qf_records_free(&uses);
            return ok;
        }
        /* the cell of the path */
        for (size_t t = 0; ok && t < use->rec->tags.n; t++) {
            if (use->rec->tags.v[t].name != ref->u.name) continue;
            rel_text(e, &rel, use->prefix.v, use->prefix.n);
            if (rel.len && use->rec->tags.v[t].path.n) qf_buf_putc(&rel, '.');
            rel_text(e, &rel, use->rec->tags.v[t].path.v, use->rec->tags.v[t].path.n);
            cell = find_cell(e, phen, rel.s ? rel.s : "", NULL);
        }
        if (ok && cell == QI_NONE) {
            err(e, loc, "the condition of the map %s.%s reads %s.%s, which is not a cell of %s", in->name, name, in->name,
                rel.s ? rel.s : "", in->name);
            ok = false;
        }
        if (ok && e->cells.v[cell].domain == QI_SET) {
            const QiEnum *en = &e->enums.v[e->cells.v[cell].kind];
            uint64_t bit = 0, all = 0;
            const char *atom = val->k == X_ATOM ? atom_name(e, val->u.name) : "";
            for (uint32_t a = 0; a < en->natoms; a++) {
                all |= (uint64_t) 1 << a;
                if (!strcmp(en->atoms[a], atom)) bit = (uint64_t) 1 << a;
            }
            if (val->k != X_ATOM || (op != 'e' && op != 'n')) {
                err(e, loc, "the condition of the map %s.%s is %s, and %s is a set of atoms: compare it with an atom, with "
                    "== or !=", in->name, name, text.s, e->cells.v[cell].path);
                ok = false;
            } else if (!bit) {
                Buf atoms = { 0 };
                for (uint32_t a = 0; a < en->natoms; a++) qf_buf_printf(&atoms, "%s%s", a ? ", " : "", en->atoms[a]);
                err(e, loc, "the condition of the map %s.%s is %s, and %s is not an atom of %s. Its atoms: %s", in->name, name,
                    text.s, atom, e->cells.v[cell].path, atoms.s);
                qf_buf_free(&atoms);
                ok = false;
            } else out->mask = op == 'e' ? bit : all & ~bit;
            if (ok && !out->mask) {
                err(e, loc, "the condition of the map %s.%s is %s, and %s has no other atom: the condition is never true",
                    in->name, name, text.s, e->cells.v[cell].path);
                ok = false;
            }
        } else if (ok) {
            char why[240];
            int64_t lo = 0, hi = 0;
            if (val->k != X_NUM) {
                err(e, loc, "the condition of the map %s.%s is %s, and %s is a quantity: compare it with a quantity", in->name,
                    name, text.s, e->cells.v[cell].path);
                ok = false;
            } else if (!literal_quanta(e, &val->u.num, cell, &lo, &hi, why, sizeof why)) {
                err(e, loc, "the condition of the map %s.%s is %s: %s", in->name, name, text.s, why);
                ok = false;
            } else if (op == 'e' && lo != hi) {
                err(e, loc, "the condition of the map %s.%s is %s: the number is between two steps of the field. Use <= or >=",
                    in->name, name, text.s);
                ok = false;
            } else {
                out->range = op == 'e'   ? (QInterval) { lo, lo }
                             : op == '<' ? (QInterval) { Q_NINF, hi - 1 }
                             : op == 'l' ? (QInterval) { Q_NINF, lo }
                             : op == '>' ? (QInterval) { lo + 1, Q_INF }
                                         : (QInterval) { hi, Q_INF };
            }
        }
    } else if (!unstated(f, wn)) {
        /* a path to a field: the same value stands at the field and at `when` */
        const char *open;
        cell = node_cell(e, phen, wn, NULL, &rel, &open);
        qf_buf_puts(&text, ".");
        qf_buf_puts(&text, rel.s ? rel.s : "");
        /* a truth value: a set cell with the atoms true and false, or one of them */
        bool truth = cell != QI_NONE && e->cells.v[cell].domain == QI_SET;
        for (uint32_t a = 0; truth && a < e->enums.v[e->cells.v[cell].kind].natoms; a++) {
            const char *atom = e->enums.v[e->cells.v[cell].kind].atoms[a];
            truth = !strcmp(atom, "true") || !strcmp(atom, "false");
        }
        if (!truth) {
            err(e, loc, "the condition of the map %s.%s is not a truth value. %s", in->name, name, forms);
            ok = false;
        } else {
            const QiEnum *en = &e->enums.v[e->cells.v[cell].kind];
            for (uint32_t a = 0; a < en->natoms; a++)
                if (!strcmp(en->atoms[a], "true")) out->mask = (uint64_t) 1 << a;
            if (!out->mask) {
                err(e, loc, "the condition of the map %s.%s is always false: remove it, and the map has no condition", in->name,
                    name);
                ok = false;
            }
        }
    }
    if (ok && cell != QI_NONE) {
        if (e->cells.v[cell].phen != phen && use) {
            /* through a connection: the world has no path for it in this phenomenon, so the cell at `when` has the value */
            memset(out, 0, sizeof *out);
            out->cell = QI_NONE;
            ok = when_cell(e, phen, name, use, text.s, out);
        } else if (e->cells.v[cell].phen != phen) {
            err(e, loc, "the condition of the map %s.%s reads %s, a field of another phenomenon: a condition reads a field "
                "of its own phenomenon. Give %s a field with a law for the value", in->name, name, e->cells.v[cell].path,
                in->name);
            ok = false;
        } else {
            /* through the port of the active model: the path as written. Else the path of the cell */
            out->path = keep(e, e->via != QI_NONE ? rel.s : e->cells.v[cell].path + strlen(in->name) + 1);
            out->cell = cell;
            out->text = keep(e, text.s ? text.s : "");
        }
    }
    qf_buf_free(&rel);
    qf_buf_free(&text);
    qf_records_free(&uses);
    return ok;
}

/* The maps of a phenomenon: each field with a type below #Map. A map names
 * two models and a map kernel. The slot "from.x" of the kernel is a cell of
 * the model `from`, "to.x" a cell of the model `to`, and another slot a cell
 * of the phenomenon. */
static void phen_maps(Elab *e, uint32_t phen)
{
    Front *f = e->f;
    if (e->tmap == QF_BOTTOM) return;
    const InstInfo *in = &f->insts.v[e->insts.v[phen]];
    Loc loc = inst_loc(in);
    size_t first = e->maps.n;
    for (uint32_t i = 0; i < in->node->narcs; i++) {
        const QfNode *v = in->node->arcs[i].val;
        if ((v->flags & NF_VALUE) || !is_subtype(f, v->type, e->tmap)) continue;
        const char *name = f->feats.v[in->node->arcs[i].feat].label;
        Buf from = { 0 }, to = { 0 }, ev = { 0 }, all = { 0 };
        bool texts = e->ffrom != QF_BOTTOM && e->fto != QF_BOTTOM && e->fevaluator != QF_BOTTOM &&
                     text_value(e, node_arc(v, e->ffrom), &from) && text_value(e, node_arc(v, e->fto), &to) &&
                     text_value(e, node_arc(v, e->fevaluator), &ev);
        model_names(e, phen, &all);
        uint32_t mf = texts ? model_named(e, phen, from.s) : QI_NONE, mt = texts ? model_named(e, phen, to.s) : QI_NONE;
        const QkKernel *k = texts ? qi_find_kernel(e->opt, ev.s) : NULL;
        QkKernel *made = NULL;
        unsigned size = 0;
        if (texts && !k && mf != QI_NONE && mt != QI_NONE) {
            /* a map of a room has a contract for each number of cells: those of the model with the cells */
            char prefix[160];
            snprintf(prefix, sizeof prefix, "%s.", !strcmp(ev.s, "native.room.abstract") ? from.s : to.s);
            size = room_cells(e, phen, prefix);
            k = made = qi_room_kernel(ev.s, size);
        }
        const QiMap *same = NULL;
        for (size_t j = first; j < e->maps.n; j++)
            if (e->maps.v[j].from == mf && e->maps.v[j].to == mt) same = &e->maps.v[j];
        if (!world_name(name))
            err(e, loc, "the map %s.%s needs a name of letters, digits and '_'", in->name, name);
        else if (!texts)
            err(e, loc, "the map %s.%s needs two models and a map kernel, as in `%s: #Map & { from: \"coarse\", to: \"fine\", "
                "evaluator: \"native.room.refine\" }`", in->name, name, name);
        else if (!nmodels(e, phen))
            err(e, loc, "%s.%s is a map, and %s has no models: a map moves the state from one model to another", in->name,
                name, in->name);
        else if (mf == QI_NONE || mt == QI_NONE)
            err(e, loc, "the map %s.%s names the model \"%s\", and %s has no model with that name. Its models: %s", in->name,
                name, mf == QI_NONE ? from.s : to.s, in->name, all.s);
        else if (mf == mt)
            err(e, loc, "the map %s.%s goes from the model %s to the same model", in->name, name, from.s);
        else if (same)
            err(e, loc, "the maps %s.%s and %s.%s both go from %s to %s: a phenomenon has one map for each pair of models",
                in->name, same->name, in->name, name, from.s, to.s);
        else if (!k && room_kernel_name(ev.s))
            err(e, loc, "the map %s.%s: the kernel %s is for a room with the cells c0, c1, and so on, and the model %s has "
                "no field c0 with an energy", in->name, name, ev.s, !strcmp(ev.s, "native.room.abstract") ? from.s : to.s);
        else if (!k)
            err(e, loc, "the map %s.%s: unknown evaluator \"%s\"", in->name, name, ev.s);
        else if (!(k->flags & QK_MAP))
            err(e, loc, "the map %s.%s: the evaluator \"%s\" is not a map kernel", in->name, name, ev.s);
        else {
            QiMap mp = { phen, keep(e, name), mf, mt, keep(e, k->name), made ? size : 0,
                         qf_alloc(e->mem, (k->nslots ? k->nslots : 1) * sizeof(QiBind)), k->nslots,
                         { NULL, QI_NONE, { 0, 0 }, 0, NULL } };
            bool ok = map_condition(e, phen, in->node->arcs[i].feat, v, name, &mp.when);
            for (uint32_t s = 0; s < k->nslots; s++) {
                const char *slot = k->slots[s].name;
                Buf rel = { 0 };
                if (!strncmp(slot, "from.", 5)) qf_buf_printf(&rel, "%s.%s", from.s, slot + 5);
                else if (!strncmp(slot, "to.", 3)) qf_buf_printf(&rel, "%s.%s", to.s, slot + 3);
                else qf_buf_puts(&rel, slot);
                e->assume = mf;                      /* the map runs before the switch takes effect */
                uint32_t c = find_cell(e, phen, rel.s, NULL);
                e->assume = QI_NONE;
                if (c == QI_NONE) {
                    err(e, loc, "the map %s.%s: the kernel %s needs %s.%s, which is not a cell", in->name, name, k->name,
                        in->name, rel.s);
                    ok = false;
                }
                mp.binds[s] = (QiBind) { keep(e, slot), k->slots[s].role, c, QI_NONE, NULL };
                qf_buf_free(&rel);
            }
            if (ok) VEC_PUSH(e->maps, mp);
        }
        qk_free(made);
        qf_buf_free(&from);
        qf_buf_free(&to);
        qf_buf_free(&ev);
        qf_buf_free(&all);
    }
    if (e->maps.n > first) qsort(e->maps.v + first, e->maps.n - first, sizeof(QiMap), cmp_map);
}

/* --- pass 4: the start ------------------------------------------------------------------------ */

/* "303.15 K": a value of a cell, for a message. */
static void put_value(Elab *e, Buf *b, uint32_t cell, QInterval v)
{
    const QiKind *k = &e->kinds.v[e->cells.v[cell].kind];
    char text[160];
    qi_format_interval(v, k->scale, text, sizeof text);
    const char *unit = qi_unit(k->dimension);
    qf_buf_printf(b, "%s%s%s", text, *unit ? " " : "", unit);
}

static void put_atoms(Elab *e, Buf *b, uint32_t cell, uint64_t mask)
{
    const QiEnum *en = &e->enums.v[e->cells.v[cell].kind];
    bool first = true;
    for (uint32_t a = 0; a < en->natoms; a++)
        if (mask >> a & 1) {
            qf_buf_printf(b, "%s%s", first ? "" : " or ", en->atoms[a]);
            first = false;
        }
    if (first) qf_buf_puts(b, "no atom");
}

/* A network of the runtime with the cells and the propagators of the laws.
 * values: the value of each cell. id: gets the cell of the network for each
 * cell. which: gets, for each propagator of the network, its index in
 * e->props. NULL if the network refuses a part: the world then gives the reason,
 * when it builds the same network. */
static QNet *law_net(Elab *e, const QInterval *values, QnCell *id, uint32_t *which)
{
    QNet *net = qn_new();
    bool ok = net != NULL;
    size_t added = 0;
    for (size_t i = 0; ok && i < e->cells.n; i++)
        ok = (e->cells.v[i].domain == QI_INTERVAL
                  ? qn_cell(net, &e->ctypes.v[i], values[i], &id[i])
                  : qn_set_cell(net, e->cells.v[i].kind + 1, e->enums.v[e->cells.v[i].kind].natoms, e->cells.v[i].mask, &id[i])) ==
             QN_OK;
    for (size_t i = 0; i < e->props.n; i++) which[i] = QI_NONE;
    for (size_t i = 0; ok && i < e->props.n; i++) {
        const QiProp *p = &e->props.v[i];
        /* the start runs the laws of the active model only (ADR 0021 section 3) */
        if (p->model != QI_NONE && p->model != e->mstart.v[p->mphen]) continue;
        which[added++] = (uint32_t) i;
        QnCell z = id[p->cell[0]], x = id[p->cell[1]], y = p->cell[2] == QI_NONE ? 0 : id[p->cell[2]];
        QStatus q = Q_OK;
        ok = qi_add_prop(net, p, e->cells.v[p->cell[0]].domain == QI_SET, z, x, y, &q) == QN_OK;
    }
    if (ok) return net;
    qn_free(net);
    return NULL;
}

/* Runs the laws. False with a diagnostic if the values contradict a law: the
 * message names the law and shows its values. */
static bool settle(Elab *e, QNet *net, const QnCell *id, const uint32_t *which)
{
    Front *f = e->f;
    if (qn_propagate(net) == QN_OK) return true;
    uint32_t culprit;
    if (qn_contradicted_by(net, &culprit) && culprit < e->props.n && which[culprit] != QI_NONE) {
        const QiProp *p = &e->props.v[which[culprit]];
        Buf vals = { 0 };
        for (int k = 0; k < 3; k++) {
            if (p->cell[k] == QI_NONE) continue;
            if (vals.len) qf_buf_puts(&vals, ", ");
            qf_buf_printf(&vals, "%s = ", e->cells.v[p->cell[k]].inner ? "an inner value" : e->cells.v[p->cell[k]].path);
            if (e->cells.v[p->cell[k]].domain == QI_SET) put_atoms(e, &vals, p->cell[k], qn_get_set(net, id[p->cell[k]]));
            else put_value(e, &vals, p->cell[k], qn_get(net, id[p->cell[k]]));
        }
        err(e, inst_loc(&f->insts.v[e->insts.v[p->phen]]), "the values of %s at the start contradict the law %s: %s (%s)",
            e->phens.v[p->phen].name, p->name, p->law, vals.s);
        qf_buf_free(&vals);
    } else {
        err(e, inst_loc(&f->insts.v[e->insts.v[0]]), "the values at the start contradict the laws");
    }
    return false;
}

/* Gives each stock its first value (ADR 0019 section 3). net: the laws
 * after they ran with the values of the start. */
static void start_stocks(Elab *e, QNet *net, const QnCell *id)
{
    Front *f = e->f;
    for (size_t i = 0; i < e->cells.n; i++) {
        QiCell *c = &e->cells.v[i];
        if (!c->stock) continue;
        QInterval v = qn_get(net, id[i]);
        bool bounded = v.lo != Q_NINF && v.hi != Q_INF;
        if (bounded && v.hi - v.lo <= 2) {
            /* The laws round outward, so an exact value can come as a range
             * of one or two steps. One step: the value is between two steps,
             * and the stock takes the lower one. Two steps: the middle. The
             * start then holds to within one step. */
            c->base = qi_point(v.hi - v.lo == 2 ? v.lo + 1 : v.lo);
            continue;
        }
        /* the fields from which a law computes the stock, and which have no exact value */
        Buf from = { 0 }, range = { 0 };
        for (size_t pi = 0; pi < e->props.n; pi++) {
            const QiProp *p = &e->props.v[pi];
            if (p->cell[0] != i && p->cell[1] != i && p->cell[2] != i) continue;
            for (int k = 0; k < 3; k++) {
                uint32_t o = p->cell[k];
                if (o == QI_NONE || o == i || e->cells.v[o].inner) continue;
                QInterval ov = qn_get(net, id[o]);
                if (ov.lo == ov.hi || (from.s && strstr(from.s, e->cells.v[o].path))) continue;
                qf_buf_printf(&from, "%s%s", from.len ? ", " : "", e->cells.v[o].path);
            }
        }
        if (v.lo != Q_NINF || v.hi != Q_INF) {
            qf_buf_puts(&range, "the laws give ");
            put_value(e, &range, (uint32_t) i, v);
        } else {
            qf_buf_puts(&range, "the laws give it no value");
        }
        Loc loc = inst_loc(&f->insts.v[e->insts.v[c->phen]]);
        if (from.len)
            err(e, loc, "the stock %s needs an exact value at the start, and %s. A law computes it from %s: give an "
                "exact value there, or give the stock a value", c->path, range.s, from.s);
        else
            err(e, loc, "the stock %s needs an exact value at the start, and %s. Give the stock a value, or give the "
                "values from which a law computes it", c->path, range.s);
        qf_buf_free(&from);
        qf_buf_free(&range);
    }
}

/* Finds the start (ADR 0019 section 3). The laws run two times, in a network
 * of the runtime, so the start has the arithmetic of the world.
 *
 * First with the values of the instances: a stock with no exact value takes
 * the value that the laws give. Then with the values that the world has: the
 * stocks, and no start value of a derived field. The world must give a
 * derived field a value again. If it does not, the start value of that field
 * has no effect, and that is an error. */
static void solve_start(Elab *e)
{
    Front *f = e->f;
    size_t n = e->cells.n, errors0 = f->nerrors;
    QnCell *id = qf_xcalloc(n + 1, sizeof *id);
    uint32_t *which = qf_xcalloc(e->props.n + 1, sizeof *which);
    QNet *net = law_net(e, e->start.v, id, which);
    if (net && settle(e, net, id, which)) start_stocks(e, net, id);
    qn_free(net);
    if (net && f->nerrors == errors0) {
        QInterval *bases = qf_xcalloc(n + 1, sizeof *bases);
        for (size_t i = 0; i < n; i++) bases[i] = e->cells.v[i].base;
        net = law_net(e, bases, id, which);
        if (net && settle(e, net, id, which))
            for (size_t i = 0; i < n; i++) {
                const QiCell *c = &e->cells.v[i];
                QInterval s = e->start.v[i], v;
                if (!c->derived || c->inner || (s.lo == c->base.lo && s.hi == c->base.hi)) continue;
                v = qn_get(net, id[i]);
                if (v.lo != c->base.lo || v.hi != c->base.hi) continue;
                Buf val = { 0 };
                put_value(e, &val, (uint32_t) i, s);
                err(e, inst_loc(&f->insts.v[e->insts.v[c->phen]]),
                    "%s has the value %s at the start, and no law gives it a value from the state after the start: the "
                    "start value has no effect. If the field is a parameter, remove @derived in its type", c->path, val.s);
                qf_buf_free(&val);
            }
        qn_free(net);
        free(bases);
    }
    free(id);
    free(which);
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
    if (x->mphen != y->mphen) return x->mphen < y->mphen ? -1 : 1;
    if (x->model != y->model) return x->model < y->model ? -1 : 1;
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
    for (size_t i = 0; i < e->maps.n; i++) {
        for (uint32_t s = 0; s < e->maps.v[i].nbinds; s++) REMAP(e->maps.v[i].binds[s].cell);
        REMAP(e->maps.v[i].when.cell);
    }
    for (size_t i = 0; i < e->kernels.n; i++) {
        for (uint32_t s = 0; s < e->kernels.v[i].nbinds; s++) {
            QiBind *bd = &e->kernels.v[i].binds[s];
            REMAP(bd->cell);
            for (uint32_t m = 0; bd->mcells && m < nmodels(e, bd->mphen); m++) REMAP(bd->mcells[m]);
        }
        for (uint32_t t = 0; t < e->kernels.v[i].ntransfers; t++) {
            REMAP(e->kernels.v[i].transfers[t].from);
            REMAP(e->kernels.v[i].transfers[t].to);
            REMAP(e->kernels.v[i].transfers[t].rate);
            REMAP(e->kernels.v[i].transfers[t].tolerance);
            REMAP(e->kernels.v[i].transfers[t].ratio);
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

    for (uint32_t p = 0; p < e->phens.n; p++) {
        uint32_t n = nmodels(e, p);
        QiPhen *ph = &e->phens.v[p];
        ph->nmodels = n;
        ph->start = e->mstart.v[p];
        ph->models = n ? qf_alloc(e->mem, n * sizeof(char *)) : NULL;
        for (uint32_t m = 0; m < n; m++) ph->models[m] = keep(e, model_at(e, p, m)->name);
    }
    ir->phens = take(e, e->phens.v, e->phens.n, sizeof(QiPhen));
    ir->nphens = (uint32_t) e->phens.n;
    ir->maps = take(e, e->maps.v, e->maps.n, sizeof(QiMap));        /* in the order of the phenomena, then of the names */
    ir->nmaps = (uint32_t) e->maps.n;
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
        /* a slot behind a port of the active model uses the cell of each model */
        for (uint32_t s = 0; s < ku->nbinds; s++) {
            const QiBind *bd = &ku->binds[s];
            QkRole r = bd->role;
            uint32_t nm = bd->mcells ? ir->phens[bd->mphen].nmodels : 0;
            for (uint32_t m = 0; m <= nm; m++) {
                uint32_t c = m < nm ? bd->mcells[m] : bd->cell;
                if (r != QK_LINK) add_dep(&deps, c, true, i, QI_READS);
                if (r == QK_OUT || r == QK_STATE) add_dep(&deps, c, true, i, QI_WRITES);
                for (uint32_t t = 0; t < ku->ntransfers; t++)
                    if (ku->transfers[t].from == bd->cell || ku->transfers[t].to == bd->cell)
                        add_dep(&deps, c, true, i, QI_TRANSFERS);
            }
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
    e.tinter = prelude_type(f, "#Interaction");
    e.fsource = feat_lookup(f, qf_sym(f, "quad.source"));
    e.fsink = feat_lookup(f, qf_sym(f, "quad.sink"));
    e.frate = feat_lookup(f, qf_sym(f, "quad.rate"));
    e.ftol = feat_lookup(f, qf_sym(f, "quad.tolerance"));
    e.fcons = feat_lookup(f, qf_sym(f, "quad.conserved"));
    e.tmodel = prelude_type(f, "#Model");
    e.tmap = prelude_type(f, "#Map");
    e.fmodel = feat_lookup(f, qf_sym(f, "quad.model"));
    e.ffrom = feat_lookup(f, qf_sym(f, "quad.from"));
    e.fto = feat_lookup(f, qf_sym(f, "quad.to"));
    e.treact = prelude_type(f, "#Reaction");
    e.fratio = feat_lookup(f, qf_sym(f, "quad.ratio"));
    e.fwhen = feat_lookup(f, qf_sym(f, "quad.when"));
    e.assume = e.via = QI_NONE;

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
        QiPhen ph = { keep(&e, in->name), keep(&e, f->types.v[in->node->type].name), NULL, 0, QI_NONE };
        VEC_PUSH(e.phens, ph);
        u64map_put(&e.phenidx, (uint64_t) e.insts.v[p] + 1, (uint64_t) p + 1);
    }

    for (uint32_t p = 0; p < e.phens.n; p++) {
        VEC_PUSH(e.model0, (uint32_t) e.models.n);
        find_models(&e, p);
    }
    VEC_PUSH(e.model0, (uint32_t) e.models.n);
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
            kernel_use(&e, p, QI_NONE);
            for (uint32_t m = 0; m < nmodels(&e, p); m++) kernel_use(&e, p, m);
            phen_interactions(&e, p, conn0.v[p], conn0.v[p + 1]);
            phen_maps(&e, p);
        }
    if (f->nerrors == errors0) solve_start(&e);
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
    VEC_FREE(e.start);
    VEC_FREE(e.aliases);
    VEC_FREE(e.ports);
    VEC_FREE(e.props);
    VEC_FREE(e.kernels);
    VEC_FREE(e.kinds);
    VEC_FREE(e.models);
    VEC_FREE(e.model0);
    VEC_FREE(e.mstart);
    VEC_FREE(e.maps);
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
