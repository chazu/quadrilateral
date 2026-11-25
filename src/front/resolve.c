#include "front/front.h"

#include <stdio.h>

/* Stage 2 of the front end (ADR 0011 section 3, ADR 0015): the resolver. It
 * reads the kinds, builds the type hierarchy, expands each type's constraint,
 * and unifies each instance's declarations into one structure.
 *
 * Borrowed from Triangle's load.c (bruggadung f425574), under ADR 0003: the
 * build of a syntax tree into a structure, type expansions, instance
 * patterns, the resolution of names across packages, and the content hash.
 * Changes from Triangle (ADR 0015):
 *   - A literal is an exact decimal with a dimension. It converts when
 *     unification gives it a kind.
 *   - An arithmetic expression or a comparison is not evaluated. It gives a
 *     constraint record, and stage 3 makes propagators from the records.
 *   - An instance has one structure, so alternatives of structures in an
 *     instance are an error. Defaults apply after unification, as in
 *     Triangle. expand.c expands comprehensions, functions and
 *     interpolations before the type hierarchy. */

/* The prelude: the types that the front end knows (ADR 0011 section 2).
 * The list types are in the root. #Phenomenon, #Port, #Interaction, #Model
 * and #Map, which the elaborator recognizes, are in the package `quad`
 * (ADR 0016, ADR 0020, ADR 0021), so their labels stay free for the content.
 * The other prelude types come with their stages. */
static const char *prelude =
    "#List: {}\n"
    "#Cons: #List & { first: _, rest: #List }\n"
    "#Null: #List\n";
static const char *prelude_quad =
    "package quad\n"
    "#Phenomenon: { evaluator: text, model: text }\n"
    "#Model: { evaluator: text }\n"
    "#Map: { from: text, to: text, evaluator: text }\n"
    "#Port: {}\n"
    "#Interaction: { source: _, sink: _, rate: _, tolerance: *1pct | %ratio, conserved: *true | bool }\n";
#define PRELUDE_FILE "<prelude>"
#define PRELUDE_PKG  "quad"

static Loc noloc;

/* --- building ------------------------------------------------------------------------- */

typedef struct { Sym name; QfNode *node; } TagEnt;

typedef struct {
    QfOp *c;
    VEC(TagEnt) tags;
    U32Vec path;
    bool ok;
    bool direct;     /* the next type name may use its expansion without copying */
    uint32_t owner;  /* type whose body is being built, or QF_BOTTOM */
    size_t decl;     /* which of its declarations */
    ArithVec ariths; /* constraint records met while building; tags resolved afterwards */
    DefVec defs;     /* defaults met while building */
    InstRefVec refs; /* reference records met while building */
    uint32_t ctx1;   /* the type a struct's labels are written against (T & { … }), + 1; 0: none */
} Build;

static bool inst_build(QfOp *c, uint32_t i);

static void build_error(Build *b, Loc loc, const char *fmt, const char *arg)
{
    if (b->ok) {
        char msg[512];
        snprintf(msg, sizeof msg, fmt, arg);
        qf_diag(b->c->f, true, loc, "%s", msg);
    }
    b->ok = false;
}

/* The feature a struct's field names, written against ctx (or QF_BOTTOM);
 * QF_BOTTOM, reported, if none or several. */
static uint32_t field_feat(Build *b, X *x, XField *fl, uint32_t ctx)
{
    Front *f = b->c->f;
    bool ambiguous;
    if (!fl->label) {
        build_error(b, fl->loc, "%s", "a comprehension or a computed label was not expanded");
        return QF_BOTTOM;
    }
    uint32_t fid = feat_resolve(f, fl->label, ctx, x->u.st.pkg, &ambiguous);
    if (fid != QF_BOTTOM) return fid;
    if (!ambiguous) {
        Buf m = { 0 };
        qf_buf_printf(&m, "unknown field '%s'", fl->label);
        size_t n = m.len;
        if (ctx != QF_BOTTOM && ctx != f->top) qf_say_fields(f, &m, ctx);
        if (m.len == n) qf_buf_puts(&m, ": no type declares a field with this name");
        build_error(b, fl->loc, "%s", m.s);
        qf_buf_free(&m);
        return QF_BOTTOM;
    }
    Buf w = { 0 };
    uint64_t v;
    u64map_get(&f->labelidx, (uint64_t) (uintptr_t) fl->label, &v);
    for (uint32_t g = (uint32_t) v - 1; g < f->feats.n && f->feats.v[g].label == fl->label; g++)
        qf_buf_printf(&w, "%s%s", w.len ? " or " : "", f->feats.v[g].full);
    char msg[512];
    snprintf(msg, sizeof msg, "two packages have a field '%s': say which (%s)", fl->label, w.s);
    build_error(b, fl->loc, "%s", msg);
    qf_buf_free(&w);
    return QF_BOTTOM;
}

static void free_ariths(ArithVec *v)
{
    for (size_t i = 0; i < v->n; i++) {
        VEC_FREE(v->v[i].path);
        for (size_t k = 0; k < v->v[i].tags.n; k++) VEC_FREE(v->v[i].tags.v[k].path);
        VEC_FREE(v->v[i].tags);
    }
    VEC_FREE(*v);
}

static void free_defaults(DefVec *v)
{
    for (size_t i = 0; i < v->n; i++) VEC_FREE(v->v[i].path);
    VEC_FREE(*v);
}

static void build_free(Build *b)
{
    VEC_FREE(b->tags);
    VEC_FREE(b->path);
    free_ariths(&b->ariths);
    free_defaults(&b->defs);
    qf_refs_free(&b->refs);
}

static void reset_failure(QfOp *c)
{
    c->failed = false;
    c->depth = 0;
}

/* The location of the field at `path` in an expression, through
 * conjunctions and nested structures. */
static bool find_field_loc(const Front *f, X *x, const uint32_t *path, size_t n, Loc *out)
{
    if (!n) return false;
    if (x->k == X_AND) return find_field_loc(f, x->u.bin.a, path, n, out) || find_field_loc(f, x->u.bin.b, path, n, out);
    if (x->k == X_DEFAULT) return find_field_loc(f, x->u.sub, path, n, out);
    if (x->k != X_STRUCT) return false;
    for (int i = 0; i < x->u.st.n; i++) {
        XField *fl = &x->u.st.fields[i];
        if (!fl->label || !label_names(f, fl->label, path[0])) continue;
        if (n == 1 || find_field_loc(f, fl->value, path + 1, n - 1, out)) {
            if (n == 1) *out = fl->loc;
            return true;
        }
    }
    return false;
}

/* Where a type, or its nearest ancestor that does, declares `path`. */
static bool type_field_loc(const Front *f, uint32_t t, const uint32_t *path, size_t n, Loc *out, Sym *who)
{
    U32Vec queue = { 0 };
    U64Map seen = { 0 };
    VEC_PUSH(queue, t);
    bool found = false;
    for (size_t i = 0; i < queue.n && !found; i++) {
        const TypeInfo *ti = &f->types.v[queue.v[i]];
        for (size_t k = 0; k < ti->decls.n && !found; k++) found = find_field_loc(f, ti->decls.v[k]->value, path, n, out);
        if (found) { *who = ti->name; break; }
        for (size_t k = 0; k < ti->parents.n; k++)
            if (!u64map_get(&seen, ti->parents.v[k], NULL)) {
                u64map_put(&seen, ti->parents.v[k], 1);
                VEC_PUSH(queue, ti->parents.v[k]);
            }
    }
    VEC_FREE(queue);
    u64map_free(&seen);
    return found;
}

/* A failure as one text, in the words of the author: "voltage: 12 A is not
 * of the kind %voltage". At the root of a value, the text has no path. */
static void failure_text(Buf *b, const QfFailure *fl)
{
    if (strcmp(fl->path, "(root)")) qf_buf_printf(b, "%s: ", fl->path);
    qf_buf_puts(b, fl->text);
}

/* Reports a conflict at `loc`, naming where the other side was declared:
 * an earlier declaration of the type being built, or `other` (a type whose
 * constraint is being added) and its ancestors. */
static bool unify_at_other(Build *b, QfNode *x, QfNode *y, Loc loc, const char *what, uint32_t other)
{
    QfOp *c = b->c;
    Front *f = c->f;
    path_set(c, b->path.v, b->path.n);
    c->failed = false;
    if (unify1(c, x, y)) return true;
    if (b->ok) {
        Buf where = { 0 }, text = { 0 };
        Loc ol;
        Sym who = NULL;
        if (other != QF_BOTTOM && type_field_loc(f, other, c->failpath, (size_t) c->faildepth, &ol, &who))
            qf_buf_printf(&where, ", with %s at %s:%d", who, ol.file, ol.line);
        else if (b->owner != QF_BOTTOM)
            for (size_t k = 0; k < b->decl && !where.len; k++)
                if (find_field_loc(f, f->types.v[b->owner].decls.v[k]->value, c->failpath, (size_t) c->faildepth, &ol))
                    qf_buf_printf(&where, ", with the declaration at %s:%d", ol.file, ol.line);
        QfFailure *fl = fail_info(c);
        size_t pl = strlen(fl->path);
        failure_text(&text, fl);
        if (what[0] == '.' && !strncmp(what + 1, fl->path, pl) && what[1 + pl] == '.') {
            /* a reference whose path goes through a field without its next name */
            const char *nx = what + 2 + pl;
            qf_diag(f, true, loc, "the reference %s: %s is %s, which has no field %.*s", what, fl->path, fl->left,
                    (int) strcspn(nx, ".["), nx);
        } else {
            /* `what` names a reference or an inheritance. A written value has no name */
            qf_diag(f, true, loc, "%s%s%s%s%s", what[0] == '.' ? "the reference " : "", what, *what ? ": " : "", text.s,
                    where.s ? where.s : "");
        }
        qf_buf_free(&where);
        qf_buf_free(&text);
    }
    b->ok = false;
    return false;
}

static bool unify_at(Build *b, QfNode *x, QfNode *y, Loc loc, const char *what)
{
    return unify_at_other(b, x, y, loc, what, QF_BOTTOM);
}

/* --- defaults: the records ------------------------------------------------------------------------ */

/* A default at the place of the build. rest: the one other alternative, or NULL. */
static void record_default(Build *b, X *value, X *rest)
{
    DefaultRec d = { { 0 }, value, b->owner == QF_BOTTOM ? QF_CONTRIB_INSTANCE : b->owner, rest, NULL };
    for (size_t i = 0; i < b->path.n; i++) VEC_PUSH(d.path, b->path.v[i]);
    VEC_PUSH(b->defs, d);
}

/* Adds a copy of a default, with a prefix before its path. One default comes
 * once, although it can come through two parents or two paths of a walk. */
static void add_default(DefVec *defs, const DefaultRec *d, const uint32_t *prefix, size_t plen, uint32_t contributor)
{
    DefaultRec n = { { 0 }, d->value, contributor, d->rest, d->who };
    for (size_t i = 0; i < plen; i++) VEC_PUSH(n.path, prefix[i]);
    for (size_t i = 0; i < d->path.n; i++) VEC_PUSH(n.path, d->path.v[i]);
    for (size_t i = 0; i < defs->n; i++) {
        const DefaultRec *o = &defs->v[i];
        if (o->value == n.value && o->contributor == n.contributor && o->path.n == n.path.n &&
            (!n.path.n || !memcmp(o->path.v, n.path.v, n.path.n * sizeof(uint32_t)))) {
            VEC_FREE(n.path);
            return;
        }
    }
    VEC_PUSH(*defs, n);
}

/* The defaults that an instance states, moved to the path where a copy of
 * the instance now stands. In an instance, they are less specific than its
 * own defaults and more specific than the defaults of a type. */
static void include_defaults(Build *b, const InstInfo *ri)
{
    for (size_t i = 0; i < ri->defaults.n; i++) {
        DefaultRec d = ri->defaults.v[i];
        if (!d.who) d.who = ri->name;
        add_default(&b->defs, &d, b->path.v, b->path.n, b->owner == QF_BOTTOM ? QF_CONTRIB_NAMED : b->owner);
    }
}

static QfNode *build_raw(Build *b, X *x);

/* The constraint records that an instance states, moved to the path where a
 * copy of the instance now stands. */
static void include_ariths(Build *b, const InstInfo *ri)
{
    for (size_t i = 0; i < ri->ariths.n; i++) {
        const ArithRec *src = &ri->ariths.v[i];
        ArithRec r = { { 0 }, src->expr, { 0 } };
        for (size_t k = 0; k < b->path.n; k++) VEC_PUSH(r.path, b->path.v[k]);
        for (size_t k = 0; k < src->path.n; k++) VEC_PUSH(r.path, src->path.v[k]);
        for (size_t t = 0; t < src->tags.n; t++) {
            TagPath tp = { src->tags.v[t].name, { 0 } };
            for (size_t k = 0; k < b->path.n; k++) VEC_PUSH(tp.path, b->path.v[k]);
            for (size_t k = 0; k < src->tags.v[t].path.n; k++) VEC_PUSH(tp.path, src->tags.v[t].path.v[k]);
            VEC_PUSH(r.tags, tp);
        }
        VEC_PUSH(b->ariths, r);
    }
}

/* The reference record of a copy of an instance, or of its part at `sub`,
 * that stands at the current path. The copy also holds a copy of each
 * reference that the instance has below `sub`. */
static void include_refs(Build *b, const InstInfo *ri, const uint32_t *sub, size_t nsub)
{
    InstRef r = { { 0 }, ri->name, { 0 } };
    for (size_t k = 0; k < b->path.n; k++) VEC_PUSH(r.path, b->path.v[k]);
    for (size_t k = 0; k < nsub; k++) VEC_PUSH(r.sub, sub[k]);
    VEC_PUSH(b->refs, r);
    for (size_t i = 0; i < ri->refs.n; i++) {
        const InstRef *src = &ri->refs.v[i];
        /* a reference at `sub` itself says what the part is a copy of: this record replaces it */
        if (src->path.n <= nsub || (nsub && memcmp(src->path.v, sub, nsub * sizeof(uint32_t)))) continue;
        InstRef in = { { 0 }, src->inst, { 0 } };
        for (size_t k = 0; k < b->path.n; k++) VEC_PUSH(in.path, b->path.v[k]);
        for (size_t k = nsub; k < src->path.n; k++) VEC_PUSH(in.path, src->path.v[k]);
        for (size_t k = 0; k < src->sub.n; k++) VEC_PUSH(in.sub, src->sub.v[k]);
        VEC_PUSH(b->refs, in);
    }
}

static void copy_ref(InstRefVec *dst, const InstRef *src)
{
    for (size_t i = 0; i < dst->n; i++) {        /* through two parents: once */
        const InstRef *d = &dst->v[i];
        if (d->inst == src->inst && d->path.n == src->path.n && d->sub.n == src->sub.n &&
            (!d->path.n || !memcmp(d->path.v, src->path.v, d->path.n * sizeof(uint32_t))) &&
            (!d->sub.n || !memcmp(d->sub.v, src->sub.v, d->sub.n * sizeof(uint32_t)))) return;
    }
    InstRef r = { { 0 }, src->inst, { 0 } };
    for (size_t k = 0; k < src->path.n; k++) VEC_PUSH(r.path, src->path.v[k]);
    for (size_t k = 0; k < src->sub.n; k++) VEC_PUSH(r.sub, src->sub.v[k]);
    VEC_PUSH(*dst, r);
}

/* A path into another declaration, as in wrench_19.size or #Wrench.size: a
 * copy of the declaration, at the path. The longest prefix that names a type
 * or an instance is the declaration, so a package's names come first. */
static QfNode *name_path(Build *b, X *x)
{
    QfOp *c = b->c;
    Front *f = c->f;
    const char *s = x->u.name;
    size_t n = strlen(s);
    for (size_t end = n; end-- > 1;) {
        if (s[end] != '.' && s[end] != '[') continue;
        if (s[end] == '.' && (s[end + 1] == '#' || s[end + 1] == '%')) continue;     /* pkg.#Type is one name */
        Sym name = qf_intern(f, s, end);
        QfNode *node = NULL;
        uint32_t t = type_lookup(f, name);
        uint64_t v;
        uint32_t from = QF_BOTTOM;               /* the instance that the path goes into */
        U32Vec sub = { 0 };
        if (t != QF_BOTTOM && f->types.v[t].kind != TK_ATOM) {
            QfNode *e = type_expansion(c, t);
            if (!e) { build_error(b, x->loc, "cannot expand %s", name); return NULL; }
            node = copy_raw(c, &c->scratch, e);
        } else if (u64map_get(&f->instidx, (uint64_t) (uintptr_t) name, &v)) {
            if (!inst_build(c, (uint32_t) v - 1)) {
                build_error(b, x->loc, "instance %s could not be resolved", name);
                return NULL;
            }
            node = copy_raw(c, &c->scratch, f->insts.v[v - 1].pattern);
            from = (uint32_t) v - 1;
        } else continue;
        /* the rest: .name and [k] */
        for (const char *p = s + end; *p && node;) {
            char seg[256];
            QfNode *cur = deref(c, node);
            if (cur->flags & NF_VALUE) {
                char msg[600];
                snprintf(msg, sizeof msg, "%s is a value, with no fields (in %s)", name, s);
                build_error(b, x->loc, "%s", msg);
                VEC_FREE(sub);
                return NULL;
            }
            if (*p == '.') {
                const char *e = ++p;
                while (*e && *e != '.' && *e != '[') e++;
                snprintf(seg, sizeof seg, "%.*s", (int) (e - p), p);
                p = e;
                uint32_t ft = feat_resolve(f, qf_sym(f, seg), cur->type, NULL, NULL);
                QfNode *next = ft == QF_BOTTOM ? NULL : node_arc(cur, ft);
                if (!next) {
                    char msg[600];
                    snprintf(msg, sizeof msg, "%s has no field %s (in %s)", name, seg, s);
                    build_error(b, x->loc, "%s", msg);
                    VEC_FREE(sub);
                    return NULL;
                }
                VEC_PUSH(sub, ft);
                node = next;
            } else {
                long k = strtol(p + 1, NULL, 10);
                while (*p && *p != ']') p++;
                if (*p) p++;
                for (long i = 0; i <= k; i++) {
                    QfNode *at = deref(c, node);
                    QfNode *next = (at->flags & NF_VALUE) ? NULL : node_arc(at, i == k ? f->ffirst : f->frest);
                    if (!next) {
                        char msg[600];
                        snprintf(msg, sizeof msg, "%s has no item %ld (in %s)", name, k, s);
                        build_error(b, x->loc, "%s", msg);
                        VEC_FREE(sub);
                        return NULL;
                    }
                    VEC_PUSH(sub, i == k ? f->ffirst : f->frest);
                    node = next;
                }
            }
        }
        if (node && from != QF_BOTTOM) include_refs(b, &f->insts.v[from], sub.v, sub.n);
        VEC_FREE(sub);
        return node;
    }
    return NULL;
}

/* A struct does not get a type from its fields alone (ADR 0015 section 5).
 * The place where it stands must already have the type that introduces its
 * fields: the declared type of the field, or a type that the author names
 * (#Tool & { ... }). So the expected type goes down while a tree is built:
 * ctx1 holds it. */

/* The type that a field has by declaration in a structure of type t: the
 * type of its node in the expansion of t. Top if nothing says. */
static uint32_t expected_field(QfOp *c, uint32_t t, uint32_t fid)
{
    Front *f = c->f;
    if (t == QF_BOTTOM || t == f->top || is_value(f, t)) return f->top;
    if (f->types.v[t].xstate == 1) {
        /* t is being expanded: its supertypes say */
        uint32_t r = f->top;
        for (size_t k = 0; k < f->types.v[t].parents.n; k++) {
            uint32_t m = op_meet(c, r, expected_field(c, f->types.v[t].parents.v[k], fid));
            if (m != QF_BOTTOM) r = m;
        }
        return r;
    }
    QfNode *e = type_expansion(c, t);
    if (!e || (e->flags & NF_VALUE)) return f->top;
    QfNode *v = node_arc(e, fid);
    return v ? v->type : f->top;
}

/* The type that an item has by declaration in a list of type lt: the type of
 * `first` in the cell type of the list. */
static uint32_t expected_item(QfOp *c, uint32_t lt)
{
    Front *f = c->f;
    if (lt == QF_BOTTOM || lt == f->top || is_value(f, lt) || !is_subtype(f, lt, f->tlist)) return f->top;
    if (is_subtype(f, lt, f->tcons)) return expected_field(c, lt, f->ffirst);
    for (size_t k = 0; k < f->types.v[lt].children.n; k++) {
        uint32_t ch = f->types.v[lt].children.v[k];
        if (is_subtype(f, ch, f->tcons)) return expected_field(c, ch, f->ffirst);
    }
    return f->top;
}

/* The meet of t and the types that the conjuncts of x name: #Tool & { ... }
 * and { ... } & #Tool give the struct the same expected type. */
static uint32_t chain_type(Build *b, X *x, uint32_t t)
{
    QfOp *c = b->c;
    Front *f = c->f;
    if (x->k == X_AND) return chain_type(b, x->u.bin.b, chain_type(b, x->u.bin.a, t));
    if (x->k != X_NAME) return t;
    uint32_t named = type_lookup(f, x->u.name);
    uint64_t v;
    if (named != QF_BOTTOM && f->types.v[named].kind == TK_ATOM) return t;
    if (named == QF_BOTTOM) {
        if (!u64map_get(&f->instidx, (uint64_t) (uintptr_t) x->u.name, &v)) return t;
        InstInfo *in = &f->insts.v[v - 1];
        if (in->pstate == 1 || in->pstate == 3 || !inst_build(c, (uint32_t) v - 1)) return t;
        named = f->insts.v[v - 1].pattern->type;
    }
    uint32_t m = op_meet(c, t, named);
    return m != QF_BOTTOM ? m : t;
}

/* The type of a written struct, or QF_BOTTOM with a diagnostic. A struct
 * does not get a type from its fields (ADR 0015 section 5): the place where
 * it stands must have a type with every field of the struct.
 *
 * base: the meet of the types that alone introduce a field of the struct.
 * e: the expected type. feats: the features of its fields. A field name
 * that several unrelated types have (ADR 0017) says nothing about the type. */
static uint32_t struct_type(Build *b, X *x, uint32_t base, uint32_t e, const uint32_t *feats, int n)
{
    QfOp *c = b->c;
    Front *f = c->f;
    if (x->u.st.refs || !n) return base;         /* the parser made it for a reference; or no fields */
    if (e != f->top && is_value(f, e)) return base;
    int odd = 0;                                 /* the first field that the place does not have */
    if (e != f->top) {
        odd = -1;
        for (int i = 0; i < n && odd < 0; i++)
            if (!approp(f, e, feats[i])) odd = i;
        if (odd < 0) return base;
        if (f->types.v[e].isdisj) {
            /* the fields choose among the alternatives: the most general type below e with every field */
            U32Vec cands = { 0 };
            for (uint32_t u = 0; u < f->types.n; u++) {
                if (u == e || !is_subtype(f, u, e)) continue;
                bool all = true;
                for (int i = 0; i < n && all; i++) all = approp(f, u, feats[i]);
                if (all) VEC_PUSH(cands, u);
            }
            uint32_t pick = QF_BOTTOM;
            size_t top = 0;
            for (size_t i = 0; i < cands.n; i++) {
                bool maximal = true;
                for (size_t j = 0; j < cands.n && maximal; j++)
                    maximal = i == j || !is_subtype(f, cands.v[i], cands.v[j]);
                if (maximal) { pick = cands.v[i]; top++; }
            }
            VEC_FREE(cands);
            if (top == 1) return pick;
            if (top > 1) {
                char msg[600];
                snprintf(msg, sizeof msg, "the fields of this struct are fields of more than one alternative of %s: "
                         "state its type before it, with '&'", f->types.v[e].name);
                build_error(b, x->loc, "%s", msg);
                return QF_BOTTOM;
            }
        }
    }
    const U32Vec *in = &f->feats.v[feats[odd]].intros;
    const char *tn = f->types.v[base].name;
    char msg[900], fix[400];
    if (in->n > 1) {
        /* several types have this field: the author names one of them */
        Buf who = { 0 };
        for (size_t k = 0; k < in->n && k < 4; k++) qf_buf_printf(&who, "%s%s", k ? ", " : "", f->types.v[in->v[k]].name);
        snprintf(fix, sizeof fix, "state its type before it, with '&'. The types with these fields: %s%s", who.s,
                 in->n > 4 ? ", ..." : "");
        qf_buf_free(&who);
        tn = "more than one type";
    } else {
        if (base == f->top) tn = f->types.v[in->v[0]].name;
        if (!strncmp(tn, "glb[", 4)) snprintf(fix, sizeof fix, "state its type before it, with '&'");
        else snprintf(fix, sizeof fix, "write %s & { ... }", tn);
    }
    if (e == f->top)
        snprintf(msg, sizeof msg, "this struct stands where no type is known, and a struct does not get a type from "
                 "its fields. Its fields are fields of %s: %s", tn, fix);
    else
        snprintf(msg, sizeof msg, "this struct has fields of %s, but it stands where the type is %s: %s", tn,
                 f->types.v[e].name, fix);
    build_error(b, x->loc, "%s", msg);
    return QF_BOTTOM;
}

/* The attribute @derived on a field (ADR 0019): the laws compute the field
 * from the state. A type gives the mark, to a field with a quantity that is
 * not conserved. b->path is the path of the field. */
static void mark_derived(Build *b, const XField *fl, const QfNode *value)
{
    Front *f = b->c->f;
    bool marked = false;
    for (int a = 0; a < fl->nattrs; a++) marked |= !strcmp(fl->attrs[a].name, "derived");
    if (!marked) return;
    const TypeInfo *ti = &f->types.v[value->type];
    if (b->owner == QF_BOTTOM)
        qf_diag(f, true, fl->loc, "@derived on %s is in an instance: a type marks the fields that the laws compute. "
                "Write the mark in the type", fl->label);
    else if (ti->kind != TK_KIND && ti->kind != TK_DIMT)
        qf_diag(f, true, fl->loc, "@derived on %s: a derived field is a quantity, with a kind or a dimension", fl->label);
    else if (ti->kind == TK_KIND && f->kinds.v[ti->kindix].conserved)
        qf_diag(f, true, fl->loc, "@derived on %s: its kind %s is conserved, so the field is a stock. A stock is state, "
                "and the laws do not compute it", fl->label, f->kinds.v[ti->kindix].name);
    else {
        DerivedMark m = { b->owner, { 0 }, fl->loc };
        for (size_t i = 0; i < b->path.n; i++) VEC_PUSH(m.path, b->path.v[i]);
        VEC_PUSH(f->derived, m);
    }
}

static QfNode *build_struct_into(Build *b, QfNode *r, X *x)
{
    QfOp *c = b->c;
    /* written against: what the struct is conjoined with, else what it is
     * built into (a type's body, its own type) */
    uint32_t ctx = b->ctx1 ? b->ctx1 - 1 : deref(c, r)->type;
    uint32_t ctx1 = b->ctx1;
    for (int i = 0; i < x->u.st.n && b->ok; i++) {
        XField *fl = &x->u.st.fields[i];
        uint32_t fid = field_feat(b, x, fl, ctx);
        if (fid == QF_BOTTOM) break;
        VEC_PUSH(b->path, fid);
        uint32_t expect = expected_field(c, ctx, fid);
        b->ctx1 = expect != c->f->top ? expect + 1 : 0;      /* a field's value: the declared type of the field */
        QfNode *child = build_raw(b, fl->value);
        b->ctx1 = ctx1;
        if (child) mark_derived(b, fl, deref(c, child));
        b->path.n--;
        if (!child) break;
        QfNode *rd = deref(c, r);
        QfNode *existing = carc_find(c, rd, fid);
        if (existing) {
            VEC_PUSH(b->path, fid);
            unify_at(b, existing, child, fl->loc, "field");
            b->path.n--;
        } else {
            /* attach as a temporary arc; copy_out sorts and checks closedness */
            carc_add(c, rd, fid, child);
        }
    }
    return r;
}

/* A literal as a node: an exact decimal of its dimension's type. */
static QfNode *literal_node(Build *b, const X *x, const XNum *num, int op)
{
    QfOp *c = b->c;
    Front *f = c->f;
    QDim dim;
    int64_t v;
    int e;
    char err[400];
    if (!qf_literal(f, num, &dim, &v, &e, err, sizeof err)) { build_error(b, x->loc, "%s", err); return NULL; }
    uint32_t di = qf_dim_find(f, &dim);
    if (di == QF_BOTTOM || f->dims.v[di].type == QF_BOTTOM) {
        build_error(b, x->loc, "%s", "the dimension of this number has no type");
        return NULL;
    }
    QfDec d = { v, v, e, false, false };
    if (op == B_GE || op == B_GT) { d.hi = Q_INF; d.lo_open = op == B_GT; }
    if (op == B_LE || op == B_LT) { d.lo = Q_NINF; d.hi_open = op == B_LT; }
    d = qf_dec_norm(d);
    QfNode *n = leaf_new(c, &c->scratch, f->dims.v[di].type, d.lo, d.hi);
    n->exp = d.exp;
    if (d.lo_open) n->flags |= NF_LO_OPEN;
    if (d.hi_open) n->flags |= NF_HI_OPEN;
    return n;
}

/* The tags and references of an expression, each once. */
static void x_tags(const X *x, SymVec *out)
{
    if (!x) return;
    switch (x->k) {
    case X_TAG:
        for (size_t i = 0; i < out->n; i++) if (out->v[i] == x->u.name) return;
        VEC_PUSH(*out, x->u.name);
        break;
    case X_ARITH: case X_CMP: case X_LOGIC:
        x_tags(x->u.ar.a, out);
        x_tags(x->u.ar.b, out);
        break;
    case X_NOT:
        x_tags(x->u.sub, out);
        break;
    default:
        break;
    }
}

/* What an expression of a constraint record can hold: numbers, references,
 * and operators. Stage 3 gives it a meaning. */
static bool check_expr(Build *b, const X *x, const X *parent)
{
    Front *f = b->c->f;
    switch (x->k) {
    case X_NUM: {
        QDim dim;
        int64_t v;
        int e;
        char err[400];
        if (qf_literal(f, &x->u.num, &dim, &v, &e, err, sizeof err)) return true;
        build_error(b, x->loc, "%s", err);
        return false;
    }
    case X_TAG:
        return true;
    case X_ARITH: case X_CMP: case X_LOGIC:
        return check_expr(b, x->u.ar.a, x) && check_expr(b, x->u.ar.b, x);
    case X_NOT:
        return check_expr(b, x->u.sub, x);
    case X_NAME:
        if (qf_unit_symbol(f, x->u.name) && parent && parent->k == X_ARITH && parent->u.ar.op == '/' &&
            parent->u.ar.b == x && parent->u.ar.a->k == X_NUM && !parent->u.ar.a->u.num.unit) {
            char msg[400];
            snprintf(msg, sizeof msg, "unknown name '%s'. '%s' is a unit: for a value for each %s, write the unit "
                     "directly after the number with a negative exponent, for example 0.1%s^-1",
                     x->u.name, x->u.name, x->u.name, x->u.name);
            build_error(b, x->loc, "%s", msg);
        } else if ((x->u.name[0] == '%' || strstr(x->u.name, ".%")) && qf_kind_lookup(f, x->u.name) == QF_BOTTOM) {
            /* %energy / %time with a kind that the content does not declare */
            char msg[400];
            snprintf(msg, sizeof msg, "unknown kind %s. Declare it: %s: { dimension: \"J\" }", x->u.name, x->u.name);
            build_error(b, x->loc, "%s", msg);
        } else if (x->u.name[0] == '%' || strstr(x->u.name, ".%")) {
            build_error(b, x->loc, "the kind %s stands in an expression with a value. A product or a quotient of kinds, "
                        "such as %%energy / %%time, has kinds only", x->u.name);
        } else {
            build_error(b, x->loc, "the name '%s' cannot stand in an expression: a value of the structure is a "
                        "reference, for example .size", x->u.name);
        }
        return false;
    case X_CALL:
        build_error(b, x->loc, "the function '%s' was not expanded", x->u.call.name);
        return false;
    default:
        build_error(b, x->loc, "%s", "an expression can hold numbers, references and operators");
        return false;
    }
}

static bool record_arith(Build *b, X *x)
{
    if (!check_expr(b, x, NULL)) return false;
    ArithRec r = { { 0 }, x, { 0 } };
    for (size_t i = 0; i < b->path.n; i++) VEC_PUSH(r.path, b->path.v[i]);
    VEC_PUSH(b->ariths, r);
    return true;
}

static bool free_text(const Front *f, uint32_t t)
{
    return t == QF_BOTTOM || (f->types.v[t].kind == TK_ATOM && f->types.v[t].parents.n == 1 &&
                              f->types.v[t].parents.v[0] == f->atomroot);
}

static QfNode *build_raw(Build *b, X *x)
{
    QfOp *c = b->c;
    Front *f = c->f;
    if (!b->ok) return NULL;
    switch (x->k) {
    case X_TOP:
        return node_new(c, &c->scratch, f->top);
    case X_NAME: {
        uint32_t t = type_lookup(f, x->u.name);
        if (t != QF_BOTTOM && f->types.v[t].kind != TK_ATOM) {
            if (f->types.v[t].xstate == 1) return type_instance(c, t, &c->scratch);
            QfNode *e = type_expansion(c, t);
            if (!e) { build_error(b, x->loc, "cannot expand %s", x->u.name); return NULL; }
            /* The first type name of a top-level definition may be used as is:
             * it occurs once in the operation, so unification only touches its
             * scratch, and copy_out can share its unchanged parts. Only in a
             * top-level operation: a nested one (a lazy expansion inside a
             * unification) could overwrite the scratch of shared nodes that
             * the outer operation is using. */
            if (b->direct && c->opdepth == 1) { b->direct = false; return e; }
            return copy_raw(c, &c->scratch, e);
        }
        uint64_t v;
        if (u64map_get(&f->instidx, (uint64_t) (uintptr_t) x->u.name, &v)) {
            if (!inst_build(c, (uint32_t) v - 1)) {
                build_error(b, x->loc, "instance %s could not be resolved", x->u.name);
                return NULL;
            }
            /* a name unifies a copy of its constraints, its records among them */
            include_ariths(b, &f->insts.v[v - 1]);
            include_refs(b, &f->insts.v[v - 1], NULL, 0);
            include_defaults(b, &f->insts.v[v - 1]);
            return copy_raw(c, &c->scratch, f->insts.v[v - 1].pattern);
        }
        if (strchr(x->u.name, '.') || strchr(x->u.name, '[')) {
            QfNode *n = name_path(b, x);
            if (n || !b->ok) return n;
        }
        if (x->u.name[0] == '%' || strstr(x->u.name, ".%")) {
            char msg[400];
            snprintf(msg, sizeof msg, "unknown kind %s. Declare it: %s: { dimension: \"J\" }", x->u.name, x->u.name);
            build_error(b, x->loc, "%s", msg);
        } else
            build_error(b, x->loc, "unknown name %s", x->u.name);
        return NULL;
    }
    case X_ATOM: {
        uint32_t t = type_lookup(f, x->u.name);
        if (free_text(f, t)) {
            /* a string no enum lists is free text */
            int64_t q = (int64_t) (intptr_t) x->u.name;
            return leaf_new(c, &c->scratch, f->ttext, q, q);
        }
        return node_new(c, &c->scratch, t);
    }
    case X_NUM:
        return literal_node(b, x, &x->u.num, -1);
    case X_BOUND: {
        const X *of = x->u.bound.of;
        if (of && of->k != X_NUM) {
            build_error(b, x->loc, "%s", "a bound needs a number: >=10mm");
            return NULL;
        }
        return literal_node(b, x, of ? &of->u.num : &x->u.bound.n, x->u.bound.op);
    }
    case X_INTERP:                           /* expand.c gives each one its string */
        build_error(b, x->loc, "%s", "an interpolation was not expanded");
        return NULL;
    case X_REGEX: {
        char why[200] = "";
        const RxSet *set = rx_single(f, x->u.rx.pat, x->u.rx.neg, why, sizeof why);
        if (!set) {
            char msg[400];
            snprintf(msg, sizeof msg, "the pattern %s is not a regular expression: %s", x->u.rx.pat, why);
            build_error(b, x->loc, "%s", msg);
            return NULL;
        }
        return leaf_new(c, &c->scratch, f->ttext, Q_NINF, (int64_t) (intptr_t) set);
    }
    case X_TAG: {
        for (size_t i = 0; i < b->tags.n; i++)
            if (b->tags.v[i].name == x->u.name) return b->tags.v[i].node;
        TagEnt te = { x->u.name, node_new(c, &c->scratch, f->top) };
        VEC_PUSH(b->tags, te);
        return te.node;
    }
    case X_STRUCT: {
        uint32_t t = f->top;
        uint32_t *fts = qf_alloc_raw(c->scratch.arena, sizeof(uint32_t) * (size_t) (x->u.st.n ? x->u.st.n : 1));
        for (int i = 0; i < x->u.st.n; i++) {
            uint32_t ft = field_feat(b, x, &x->u.st.fields[i], b->ctx1 ? b->ctx1 - 1 : QF_BOTTOM);
            if (ft == QF_BOTTOM) return NULL;
            fts[i] = ft;
            if (f->feats.v[ft].intros.n != 1) continue;      /* a name of several types gives no type */
            uint32_t m = op_meet(c, t, f->feats.v[ft].intros.v[0]);
            if (m == QF_BOTTOM) {
                build_error(b, x->u.st.fields[i].loc, "no type has all of these fields (at '%s')", x->u.st.fields[i].label);
                return NULL;
            }
            t = m;
        }
        t = struct_type(b, x, t, b->ctx1 ? b->ctx1 - 1 : f->top, fts, x->u.st.n);
        if (t == QF_BOTTOM) return NULL;
        QfNode *r = node_new(c, &c->scratch, t);
        build_struct_into(b, r, x);
        if (!b->ok) return NULL;
        const char *what = "";
        if (x->u.st.refs) {                  /* a reference's place: name the reference */
            X *tag = x;
            while (tag->k == X_STRUCT) tag = tag->u.st.fields[0].value;
            what = tag->u.name;
        }
        if (t != f->top && f->types.v[t].xstate == 1) {
            r->flags |= NF_LAZY;             /* inside t's own constraint */
        } else if (t != f->top) {
            QfNode *e = type_expansion(c, t);
            if (!e) { build_error(b, x->loc, "cannot expand %s", f->types.v[t].name); return NULL; }
            unify_at(b, r, copy_raw(c, &c->scratch, e), x->loc, what);
        }
        return b->ok ? r : NULL;
    }
    case X_AND: {
        /* every conjunct is written against the types that the chain names,
         * so the order of the conjuncts does not count */
        uint32_t ctx1 = b->ctx1;
        uint32_t chain = chain_type(b, x, ctx1 ? ctx1 - 1 : f->top);
        b->ctx1 = chain != f->top ? chain + 1 : 0;
        QfNode *l = build_raw(b, x->u.bin.a);
        if (!l) { b->ctx1 = ctx1; return NULL; }
        /* and the right is written against the left: T & { … } */
        uint32_t lt = deref(c, l)->type;
        uint32_t both = b->ctx1 ? op_meet(c, b->ctx1 - 1, lt) : lt;
        b->ctx1 = 1 + (both != QF_BOTTOM ? both : lt);
        QfNode *r = build_raw(b, x->u.bin.b);
        b->ctx1 = ctx1;
        if (!r) return NULL;
        X *rb = x->u.bin.b;
        if (rb->k == X_STRUCT && rb->u.st.refs) {      /* a reference's place: name the reference */
            X *tag = rb;
            while (tag->k == X_STRUCT) tag = tag->u.st.fields[0].value;
            unify_at(b, l, r, tag->loc, tag->u.name);
        } else {
            unify_at(b, l, r, x->loc, "");
        }
        return b->ok ? l : NULL;
    }
    case X_ARITH: {
        /* a product or a quotient of kinds is a dimension: %energy / %time */
        QDim dim;
        if (qf_x_dim(f, x, &dim)) {
            uint32_t di = qf_dim_find(f, &dim);
            if (di == QF_BOTTOM || f->dims.v[di].type == QF_BOTTOM) {
                build_error(b, x->loc, "%s", "the dimension of this expression has no type");
                return NULL;
            }
            return node_new(c, &c->scratch, f->dims.v[di].type);
        }
        /* a constraint on this field. Nothing here evaluates it. */
        if (!record_arith(b, x)) return NULL;
        return node_new(c, &c->scratch, f->top);
    }
    case X_CMP: case X_LOGIC: case X_NOT: {
        /* a comparison that stands as a value: laws: [ .e == .c * .t ] */
        if (!record_arith(b, x)) return NULL;
        /* Two comparisons with the same text are the same value, so one law
         * that two declarations state unifies with itself. */
        char *text = x_text(x);
        uint64_t key = (uint64_t) (uintptr_t) qf_sym(f, text), v;
        free(text);
        if (!u64map_get(&f->cmpidx, key, &v)) {
            v = (uint64_t) (uintptr_t) x;
            u64map_put(&f->cmpidx, key, v);
        }
        int64_t q = (int64_t) v;
        return leaf_new(c, &c->scratch, f->tconstraint, q, q);
    }
    case X_CALL: case X_COMP: case X_VAR:
        build_error(b, x->loc, "%s", "a comprehension or a function was not expanded");
        return NULL;
    case X_LET:                              /* the parser replaces every use */
        build_error(b, x->loc, "%s", "a let's use was not resolved");
        return NULL;
    case X_DEFAULT:
        /* *v with no alternative: the field stays open, and v is its default */
        record_default(b, x->u.sub, NULL);
        return node_new(c, &c->scratch, f->top);
    case X_OR: {
        uint64_t di;
        if (u64map_get(&f->disjidx, (uint64_t) (uintptr_t) x, &di)) {
            /* a disjunction of structures in a type body: its type, whose
             * alternatives are subtypes */
            DisjInfo *d = &f->disjs.v[di - 1];
            if (d->def >= 0)
                record_default(b, d->collapse ? d->altx.v[d->def] : x_name(f, d->alts.v[d->def], x->loc), NULL);
            X *named = x_name(f, d->collapse ? d->collapse : d->dname, x->loc);
            if (d->collapse && f->types.v[type_lookup(f, d->collapse)].kind == TK_ATOM) named->k = X_ATOM;
            return build_raw(b, named);
        }
        XPtrVec ds = { 0 };
        x_flatten(x, X_OR, &ds);
        bool all_atoms = true;
        X *def = NULL, *rest = NULL;
        size_t nrest = 0;
        for (size_t i = 0; i < ds.n; i++) {
            X *a = ds.v[i];
            if (a->k == X_DEFAULT) {             /* one at most: the type stage checks it */
                def = a = a->u.sub;
            } else {
                rest = a;
                nrest++;
            }
            if (a->k != X_ATOM) all_atoms = false;
        }
        VEC_FREE(ds);
        bool text = !all_atoms && x_text_or(x);
        /* *0 | >=0 & <=1000: the other alternative gives the values, and 0 is the default */
        bool one = def && nrest == 1 && !all_atoms && !text;
        if (def) record_default(b, def, one ? rest : NULL);
        if (all_atoms) {
            Sym an = anon_name(f, x);
            if (!an) {                           /* every disjunct is the same atom */
                X *d = x;
                while (d->k == X_OR) d = d->u.bin.a;
                return build_raw(b, d->k == X_DEFAULT ? d->u.sub : d);
            }
            uint32_t t = type_lookup(f, an);
            if (t == QF_BOTTOM) { build_error(b, x->loc, "unknown enum %s", an); return NULL; }
            return node_new(c, &c->scratch, t);
        }
        if (text) {
            /* text, and strings it covers: text (a string an enum lists is
             * no free text, so not among it) */
            XPtrVec ts = { 0 };
            x_flatten(x, X_OR, &ts);
            for (size_t i = 0; i < ts.n; i++) {
                X *a = ts.v[i]->k == X_DEFAULT ? ts.v[i]->u.sub : ts.v[i];
                if (a->k == X_REGEX) {                 /* text covers it, and the pattern must still be good */
                    char why[200] = "";
                    if (!rx_single(f, a->u.rx.pat, a->u.rx.neg, why, sizeof why)) {
                        char msg[400];
                        snprintf(msg, sizeof msg, "the pattern %s is not a regular expression: %s", a->u.rx.pat, why);
                        build_error(b, a->loc, "%s", msg);
                        VEC_FREE(ts);
                        return NULL;
                    }
                    continue;
                }
                uint32_t t = a->k == X_ATOM ? type_lookup(f, a->u.name) : QF_BOTTOM;
                if (!free_text(f, t)) {
                    build_error(b, a->loc, "%s is an enum's atom, not text: it can't be an alternative to text", a->u.name);
                    VEC_FREE(ts);
                    return NULL;
                }
            }
            VEC_FREE(ts);
            return leaf_new(c, &c->scratch, f->ttext, Q_NINF, Q_INF);
        }
        if (one) return build_raw(b, rest);
        if (x_numeric_or(x)) {
            build_error(b, x->loc, "%s", "a disjunction of numbers is not a type; use a range, or an enum of atoms");
            return NULL;
        }
        build_error(b, x->loc, "%s", "a disjunction of structures stands in a type's body. An instance has one "
                    "structure: give the alternatives to a type, and name the type here");
        return NULL;
    }
    case X_LIST: {
        if (f->tcons == QF_BOTTOM) { build_error(b, x->loc, "%s", "list types are missing"); return NULL; }
        size_t base = b->path.n;
        QfNode *tail;
        if (x->u.list.elem) {
            uint32_t lt = type_lookup(f, list_type_name(f, x->u.list.elem, false));
            QfNode *e = lt == QF_BOTTOM ? NULL : type_expansion(c, lt);
            if (!e) { build_error(b, x->loc, "%s", "this typed list does not occur in the content"); return NULL; }
            tail = copy_raw(c, &c->scratch, e);
        } else {
            QfNode *e = type_expansion(c, x->u.list.open ? f->tlist : f->tnull);
            if (!e) { build_error(b, x->loc, "%s", "list types are missing"); return NULL; }
            tail = copy_raw(c, &c->scratch, e);
        }
        QfNode *conse = type_expansion(c, f->tcons);
        if (!conse) { build_error(b, x->loc, "%s", "list types are missing"); return NULL; }
        /* an item is written against the element type of the list */
        uint32_t listctx = b->ctx1;
        uint32_t expect = expected_item(c, x->u.list.elem ? deref(c, tail)->type : listctx ? listctx - 1 : f->top);
        /* Build from the end, tracking paths for records inside items. */
        QfNode **cells = qf_xmalloc(sizeof(QfNode *) * (size_t) (x->u.list.n ? x->u.list.n : 1));
        for (int i = 0; i < x->u.list.n && b->ok; i++) {
            for (int k = 0; k < i; k++) VEC_PUSH(b->path, f->frest);
            VEC_PUSH(b->path, f->ffirst);
            b->ctx1 = expect != f->top ? expect + 1 : 0;
            QfNode *item = build_raw(b, x->u.list.items[i]);
            b->ctx1 = listctx;
            b->path.n = base;
            QfNode *cell = copy_raw(c, &c->scratch, conse);
            cells[i] = cell;
            if (item) {
                for (int k = 0; k < i; k++) VEC_PUSH(b->path, f->frest);
                VEC_PUSH(b->path, f->ffirst);
                unify_at(b, node_arc(cell, f->ffirst), item, x->loc, "list item");
                b->path.n = base;
            }
        }
        QfNode *next = tail;
        for (int i = x->u.list.n - 1; i >= 0 && b->ok; i--) {
            unify_at(b, node_arc(cells[i], f->frest), next, x->loc, "list");
            next = cells[i];
        }
        free(cells);
        return b->ok ? next : NULL;
    }
    }
    return NULL;
}

/* --- tags and constraint records -------------------------------------------------------------- */

static void find_tag_paths(QfNode *root, QfNode *target, U32Vec *cur, U32Vec *out, U64Map *seen)
{
    if (out->n || !root) return;
    if (root == target) {
        for (size_t i = 0; i < cur->n; i++) VEC_PUSH(*out, cur->v[i]);
        if (!out->n) VEC_PUSH(*out, QF_BOTTOM);  /* root itself; marked */
        return;
    }
    uint64_t k = (uint64_t) (uintptr_t) root;
    if (u64map_get(seen, k, NULL)) return;
    u64map_put(seen, k, 1);
    if (root->flags & NF_VALUE) return;
    for (uint32_t i = 0; i < root->narcs && !out->n; i++) {
        VEC_PUSH(*cur, root->arcs[i].feat);
        find_tag_paths(root->arcs[i].val, target, cur, out, seen);
        cur->n--;
    }
}

/* Where a tag of the build stands in `out`, the structure that copy_out just
 * made. An empty path is the root. False if it stands nowhere. */
static bool tag_path(Build *b, QfNode *out, QfNode *tagnode, U32Vec *path)
{
    QfNode *mapped = last_copy(b->c, tagnode);
    U32Vec cur = { 0 };
    U64Map seen = { 0 };
    find_tag_paths(out, mapped, &cur, path, &seen);
    VEC_FREE(cur);
    u64map_free(&seen);
    if (!path->n) return false;
    if (path->n == 1 && path->v[0] == QF_BOTTOM) path->n = 0;
    return true;
}

/* Turns a build's records into records of `out`, the structure it built:
 * each tag the expression uses gets its path there. False (with a
 * diagnostic) if a tag is bound nowhere else. */
static bool resolve_ariths(Build *b, QfNode *out, ArithVec *dst, const char *who)
{
    Front *f = b->c->f;
    bool ok = true;
    for (size_t i = 0; i < b->ariths.n; i++) {
        ArithRec r = { { 0 }, b->ariths.v[i].expr, { 0 } };
        for (size_t k = 0; k < b->ariths.v[i].path.n; k++) VEC_PUSH(r.path, b->ariths.v[i].path.v[k]);
        if (b->ariths.v[i].tags.n) {             /* copied from an instance by name: its tags have their places */
            for (size_t t = 0; t < b->ariths.v[i].tags.n; t++) {
                const TagPath *src = &b->ariths.v[i].tags.v[t];
                TagPath tp = { src->name, { 0 } };
                for (size_t k = 0; k < src->path.n; k++) VEC_PUSH(tp.path, src->path.v[k]);
                VEC_PUSH(r.tags, tp);
            }
            VEC_PUSH(*dst, r);
            continue;
        }
        SymVec names = { 0 };
        x_tags(r.expr, &names);
        for (size_t t = 0; t < names.n; t++) {
            TagPath tp = { names.v[t], { 0 } };
            bool found = false;
            for (size_t q = 0; q < b->tags.n && !found; q++)
                if (b->tags.v[q].name == names.v[t]) found = tag_path(b, out, b->tags.v[q].node, &tp.path);
            if (!found) {
                qf_diag(f, true, r.expr->loc, "%s: %s in an expression is not bound to a field", who, names.v[t]);
                ok = false;
            }
            VEC_PUSH(r.tags, tp);
        }
        VEC_FREE(names);
        VEC_PUSH(*dst, r);
    }
    return ok;
}

static void copy_arith(ArithVec *dst, const ArithRec *src)
{
    for (size_t i = 0; i < dst->n; i++)          /* through two parents: once */
        if (dst->v[i].expr == src->expr && dst->v[i].path.n == src->path.n &&
            (!src->path.n || !memcmp(dst->v[i].path.v, src->path.v, src->path.n * sizeof(uint32_t)))) return;
    ArithRec r = { { 0 }, src->expr, { 0 } };
    for (size_t k = 0; k < src->path.n; k++) VEC_PUSH(r.path, src->path.v[k]);
    for (size_t t = 0; t < src->tags.n; t++) {
        TagPath tp = { src->tags.v[t].name, { 0 } };
        for (size_t k = 0; k < src->tags.v[t].path.n; k++) VEC_PUSH(tp.path, src->tags.v[t].path.v[k]);
        VEC_PUSH(r.tags, tp);
    }
    VEC_PUSH(*dst, r);
}

/* --- type expansions ------------------------------------------------------------------------ */

static void conjuncts(X *x, XPtrVec *out)
{
    if (x->k == X_AND) { conjuncts(x->u.bin.a, out); conjuncts(x->u.bin.b, out); }
    else VEC_PUSH(*out, x);
}

QfNode *type_instance(QfOp *c, uint32_t t, QfStore *a)
{
    if (c->f->types.v[t].xstate == 1) {
        /* t's constraint mentions t: a lazy node, completed when used */
        QfNode *n = node_new(c, a, t);
        n->flags |= NF_LAZY;
        return n;
    }
    QfNode *e = type_expansion(c, t);
    return e ? copy_raw(c, a, e) : NULL;
}

QfNode *type_expansion(QfOp *c, uint32_t t)
{
    Front *f = c->f;
    TypeInfo *ti = &f->types.v[t];
    if (ti->xstate == 2) return ti->expansion;
    if (ti->xstate == 1 || f->loaded) return NULL;
    if (ti->kind == TK_TOP || is_value(f, t) || ti->kind == TK_ATOMROOT || ti->kind == TK_ATOM || ti->kind == TK_ANON) {
        ti->expansion = node_new(c, &f->store, t);
        ti->xstate = 2;
        return ti->expansion;
    }
    ti->xstate = 1;

    /* Save the failure state of any operation in progress. The paths have no
     * bound (a list's length is a path's depth), so their copies are on the
     * heap; a type is expanded once, so this is rare. */
    int saveddepth = c->depth;
    bool savedfailed = c->failed;
    QfFailure savedfail = *fail_info(c);
    int savedfaildepth = c->faildepth;
    uint32_t *savedpath = qf_xmalloc(sizeof(uint32_t) * ((size_t) saveddepth + 1));
    uint32_t *savedfailpath = qf_xmalloc(sizeof(uint32_t) * ((size_t) savedfaildepth + 1));
    if (saveddepth) memcpy(savedpath, c->pathstack, sizeof(uint32_t) * (size_t) saveddepth);
    if (savedfaildepth) memcpy(savedfailpath, c->failpath, sizeof(uint32_t) * (size_t) savedfaildepth);
    QfMark m = qf_arena_mark(c->scratch.arena);
    uint32_t mn = c->scratch.n;
    uint64_t saved = op_begin(c);

    Build b = { 0 };
    b.c = c;
    b.ok = true;
    b.owner = t;
    QfNode *r = node_new(c, &c->scratch, t);
    for (size_t k = 0; k < ti->decls.n && b.ok; k++) {
        b.decl = k;
        XPtrVec cj = { 0 };
        conjuncts(ti->decls.v[k]->value, &cj);
        for (size_t j = 0; j < cj.n && b.ok; j++)
            if (cj.v[j]->k == X_STRUCT) build_struct_into(&b, r, cj.v[j]);
        VEC_FREE(cj);
        ti = &f->types.v[t];
    }
    Loc loc = ti->decls.n ? ti->decls.v[0]->loc : noloc;
    for (size_t k = 0; k < ti->parents.n && b.ok; k++) {
        uint32_t p = ti->parents.v[k];
        QfNode *e = type_expansion(c, p);
        ti = &f->types.v[t];
        if (!e) {
            if (f->types.v[p].xstate == 1)
                qf_diag(f, true, loc, "%s inherits from %s, whose constraint depends on %s", ti->name,
                        f->types.v[p].name, ti->name);
            b.ok = false;
            break;
        }
        Buf what = { 0 };
        qf_buf_printf(&what, "%s inheriting from %s", ti->name, f->types.v[p].name);
        /* the conflict is reported at this type's own declaration of the path */
        Loc here = loc;
        b.owner = QF_BOTTOM;
        /* the first parent's expansion is used as is, so the result shares it */
        if (!unify_at_other(&b, r, k == 0 && c->opdepth == 1 ? e : copy_raw(c, &c->scratch, e), here, what.s, p)) {
            /* re-point the diagnostic at this type's declaration of the path, if any */
            for (size_t q = 0; q < ti->decls.n; q++)
                if (find_field_loc(f, ti->decls.v[q]->value, c->failpath, (size_t) c->faildepth, &here)) {
                    Diag *dg = &f->diags.v[f->diags.n - 1];
                    char *old = dg->msg;
                    const char *rest = strstr(old, ": error: ");
                    Buf nb = { 0 };
                    if (rest) qf_buf_printf(&nb, "%s:%d:%d%s", here.file, here.line, here.col, rest);
                    else qf_buf_printf(&nb, "%s:%d:%d: %s", here.file, here.line, here.col, old);
                    dg->msg = nb.s;
                    dg->loc = here;
                    free(old);
                    break;
                }
        }
        qf_buf_free(&what);
    }
    QfNode *out = NULL;
    if (b.ok) {
        bool closed;
        out = copy_out(c, &f->store, r, &closed);
        if (!closed) {
            qf_diag(f, true, loc, "%s: its type does not allow one of its fields", ti->name);
            out = NULL;
        }
    }
    if (out) {
        /* records: the type's own, with tags placed in its expansion, and its parents' */
        if (!resolve_ariths(&b, out, &ti->ariths, ti->name)) out = NULL;
        ti = &f->types.v[t];
        for (size_t k = 0; k < ti->parents.n; k++) {
            const ArithVec *pa = &f->types.v[ti->parents.v[k]].ariths;
            for (size_t i = 0; i < pa->n; i++) copy_arith(&ti->ariths, &pa->v[i]);
        }
        /* defaults: the type's own, and its parents' */
        for (size_t i = 0; i < b.defs.n; i++) add_default(&ti->defaults, &b.defs.v[i], NULL, 0, b.defs.v[i].contributor);
        for (size_t k = 0; k < ti->parents.n; k++) {
            const DefVec *pd = &f->types.v[ti->parents.v[k]].defaults;
            for (size_t i = 0; i < pd->n; i++) add_default(&ti->defaults, &pd->v[i], NULL, 0, pd->v[i].contributor);
        }
        for (size_t i = 0; i < b.refs.n; i++) copy_ref(&ti->refs, &b.refs.v[i]);
        for (size_t k = 0; k < ti->parents.n; k++) {
            const InstRefVec *pr = &f->types.v[ti->parents.v[k]].refs;
            for (size_t i = 0; i < pr->n; i++) copy_ref(&ti->refs, &pr->v[i]);
        }
    }
    build_free(&b);

    op_end(c, saved);
    qf_arena_release(c->scratch.arena, m);
    c->scratch.n = mn;
    /* (each array only grows, so each still has room for what it held) */
    if (saveddepth) memcpy(c->pathstack, savedpath, sizeof(uint32_t) * (size_t) saveddepth);
    c->depth = saveddepth;
    c->failed = savedfailed;
    c->fail_ = savedfail;
    c->failfmt = true;
    if (savedfaildepth) memcpy(c->failpath, savedfailpath, sizeof(uint32_t) * (size_t) savedfaildepth);
    c->faildepth = savedfaildepth;
    free(savedpath);
    free(savedfailpath);

    ti = &f->types.v[t];
    ti->expansion = out;
    ti->xstate = 2;              /* do not retry; errors are already reported */
    return out;
}

/* --- instances ------------------------------------------------------------------------------- */

static bool inst_build(QfOp *c, uint32_t i)
{
    Front *f = c->f;
    InstInfo *in = &f->insts.v[i];
    if (in->pstate == 2) return true;
    if (in->pstate == 3) return false;
    if (in->pstate == 1) {
        qf_diag(f, true, in->decls.v[0]->loc, "instance %s refers to itself", in->name);
        return false;
    }
    in->pstate = 1;
    QfMark m = qf_arena_mark(c->scratch.arena);
    uint32_t mn = c->scratch.n;
    uint64_t saved = op_begin(c);
    int saveddepth = c->depth;
    bool savedfailed = c->failed;
    Build b = { 0 };
    b.c = c; b.ok = true; b.owner = QF_BOTTOM;
    b.direct = true;
    QfNode *r = NULL;
    size_t nd = in->decls.n;
    /* the types that its declarations name: each declaration is written against all of them */
    uint32_t root = f->top;
    for (size_t k = 0; k < nd; k++) root = chain_type(&b, f->insts.v[i].decls.v[k]->value, root);
    for (size_t k = 0; k < nd && b.ok; k++) {
        Decl *d = f->insts.v[i].decls.v[k];
        b.ctx1 = root != f->top ? root + 1 : 0;
        QfNode *x = build_raw(&b, d->value);
        if (!x) break;
        if (!r) { r = x; continue; }
        reset_failure(c);
        if (!unify1(c, r, x)) {
            Buf text = { 0 };
            failure_text(&text, fail_info(c));
            qf_diag(f, true, d->loc, "%s: %s, with its other declarations", f->insts.v[i].name, text.s);
            qf_buf_free(&text);
            b.ok = false;
        }
    }
    in = &f->insts.v[i];
    if (b.ok && r) {
        bool closed;
        QfNode *pat = copy_out(c, &f->store, r, &closed);
        if (!closed) {
            qf_diag(f, true, in->decls.v[0]->loc, "%s: its type does not allow one of its fields", in->name);
            b.ok = false;
        } else {
            in->node = in->pattern = pat;        /* resolve() gives the node its defaults, after every instance */
            for (size_t q = 0; q < b.defs.n; q++) add_default(&in->defaults, &b.defs.v[q], NULL, 0, b.defs.v[q].contributor);
            for (size_t t = 0; t < b.tags.n; t++) {
                TagPath tp = { b.tags.v[t].name, { 0 } };
                tag_path(&b, pat, b.tags.v[t].node, &tp.path);
                VEC_PUSH(in->tags, tp);
            }
            if (!resolve_ariths(&b, pat, &in->ariths, in->name)) b.ok = false;
            for (size_t q = 0; q < b.refs.n; q++) copy_ref(&in->refs, &b.refs.v[q]);
        }
    } else {
        b.ok = false;
    }
    bool ok = b.ok;
    build_free(&b);
    op_end(c, saved);
    qf_arena_release(c->scratch.arena, m);
    c->scratch.n = mn;
    c->depth = saveddepth;
    c->failed = savedfailed;
    c->failfmt = false;
    f->insts.v[i].pstate = ok ? 2 : 3;
    return ok;
}

/* --- packages -------------------------------------------------------------------------------- */

#define PLAIN_AMBIGUOUS ((uint64_t) 1)

/* The name as its package qualifies it: #Wrench in tools is tools.#Wrench,
 * wrench_250 is tools.wrench_250, %coin is tools.%coin. */
static Sym qualify(Front *c, Sym pkg, Sym name)
{
    Buf b = { 0 };
    qf_buf_printf(&b, "%s.%s", pkg, name);
    Sym q = qf_sym(c, b.s);
    qf_buf_free(&b);
    return q;
}

/* A plain name means the package's own if it declares one, else the root's,
 * else the prelude's, else the one package's that declares it; where two do
 * and none of the above has it, it stays plain, and is unknown. So a package
 * that declares its own #Phenomenon does not take the prelude's name from
 * the other packages. */
static Sym resolve_plain(Front *c, Sym name, Sym pkg, const U64Map *declared, const U64Map *root)
{
    if (pkg) {
        Sym q = qualify(c, pkg, name);
        if (u64map_get(declared, (uint64_t) (uintptr_t) q, NULL)) return q;
    }
    if (u64map_get(root, (uint64_t) (uintptr_t) name, NULL) || qf_builtin_kind(name)) return name;
    Sym pq = qualify(c, qf_sym(c, PRELUDE_PKG), name);
    if (u64map_get(declared, (uint64_t) (uintptr_t) pq, NULL)) return pq;
    uint64_t v;
    if (u64map_get(&c->plainidx, (uint64_t) (uintptr_t) name, &v) && v != PLAIN_AMBIGUOUS) return (Sym) (uintptr_t) v;
    return name;
}

/* Qualified names are taken as written; plain ones, as resolve_plain says. */
static void resolve_names(Front *c, X *x, Sym pkg, const U64Map *declared, const U64Map *root)
{
    if (!x) return;
    switch (x->k) {
    case X_NAME: {
        const char *s = x->u.name;
        size_t first = strcspn(s, ".[");
        if (!s[first]) {
            x->u.name = resolve_plain(c, x->u.name, pkg, declared, root);
            break;
        }
        /* a path, wrench_19.size: its first name resolved, unless the name
         * already starts with a package's */
        if (s[first] == '.' && (s[first + 1] == '#' || s[first + 1] == '%')) break;
        for (size_t end = strlen(s); end > first; end--) {
            if (s[end] && s[end] != '.' && s[end] != '[') continue;
            if (u64map_get(declared, (uint64_t) (uintptr_t) qf_intern(c, s, end), NULL)) return;
        }
        Sym h = qf_intern(c, s, first);
        Sym r = resolve_plain(c, h, pkg, declared, root);
        if (r != h) {
            Buf b = { 0 };
            qf_buf_printf(&b, "%s%s", r, s + first);
            x->u.name = qf_sym(c, b.s);
            qf_buf_free(&b);
        }
        break;
    }
    case X_STRUCT:
        for (int i = 0; i < x->u.st.n; i++) {
            resolve_names(c, x->u.st.fields[i].value, pkg, declared, root);
            resolve_names(c, x->u.st.fields[i].dlabel, pkg, declared, root);
        }
        break;
    case X_COMP:
        for (int i = 0; i < x->u.comp.n; i++) resolve_names(c, x->u.comp.clauses[i].expr, pkg, declared, root);
        resolve_names(c, x->u.comp.body, pkg, declared, root);
        break;
    case X_CMP: case X_LOGIC: case X_ARITH:
        resolve_names(c, x->u.ar.a, pkg, declared, root);
        resolve_names(c, x->u.ar.b, pkg, declared, root);
        break;
    case X_NOT: case X_DEFAULT:
        resolve_names(c, x->u.sub, pkg, declared, root);
        break;
    case X_CALL:
        for (int i = 0; i < x->u.call.n; i++) resolve_names(c, x->u.call.args[i], pkg, declared, root);
        break;
    case X_BOUND:
        resolve_names(c, x->u.bound.of, pkg, declared, root);
        break;
    case X_LIST:
        for (int i = 0; i < x->u.list.n; i++) resolve_names(c, x->u.list.items[i], pkg, declared, root);
        resolve_names(c, x->u.list.elem, pkg, declared, root);
        break;
    case X_AND:
    case X_OR:
        resolve_names(c, x->u.bin.a, pkg, declared, root);
        resolve_names(c, x->u.bin.b, pkg, declared, root);
        break;
    case X_INTERP:
        for (int i = 0; i < x->u.in.n; i++) resolve_names(c, x->u.in.parts[i], pkg, declared, root);
        break;
    default:
        break;
    }
}

/* A let's name may not be a type's or an instance's: inside its scope, it
 * would hide that one without a word. It also may not be a unit symbol: with
 * a let s, 10m/s would have two readings (ADR 0014 section 2). */
static bool check_lets(Front *c)
{
    bool ok = true;
    for (size_t i = 0; i < c->lets.n; i++) {
        LetName *l = &c->lets.v[i];
        Sym q = l->pkg ? qualify(c, l->pkg, l->name) : NULL;
        const char *what = NULL;
        uint32_t t = type_lookup(c, l->name);
        if (t != QF_BOTTOM && c->types.v[t].kind != TK_ATOM) what = "a built-in type";
        else if (u64map_get(&c->instidx, (uint64_t) (uintptr_t) l->name, NULL) ||
                 (q && u64map_get(&c->instidx, (uint64_t) (uintptr_t) q, NULL)))
            what = "an instance";
        if (what) {
            qf_diag(c, true, l->loc, "%s %s: %s has this name, and %s may not hide it", l->var ? "for" : "let", l->name,
                    what, l->var ? "a for's variable" : "a let");
            ok = false;
        } else if (qf_unit_symbol(c, l->name)) {
            qf_diag(c, true, l->loc, "%s %s: '%s' is a unit symbol. Give the %s another name, because a number "
                    "directly before /%s or *%s takes it as a unit", l->var ? "for" : "let", l->name, l->name,
                    l->var ? "variable" : "let", l->name, l->name);
            ok = false;
        }
    }
    return ok;
}

/* Types, kinds and instances declared in a package take its name; references
 * to them are resolved, in the root too. Atoms stay global; features are
 * resolved once there are features (feat_resolve). */
static void resolve_packages(Front *c)
{
    U64Map declared = { 0 }, root = { 0 };
    bool any = false;
    for (size_t i = 0; i < c->decls.n; i++) {
        Decl *d = &c->decls.v[i];
        if (d->kind != L_ATOM && !d->pkg && !strchr(d->label, '.')) u64map_put(&root, (uint64_t) (uintptr_t) d->label, 1);
    }
    for (size_t i = 0; i < c->decls.n; i++) {
        Decl *d = &c->decls.v[i];
        if (d->kind == L_ATOM || strchr(d->label, '.')) {
            any |= d->kind != L_ATOM;
            continue;
        }
        /* a type the root declares is the root's: a package's file that
         * declares it adds to it; the fields the file declares are its
         * package's */
        if (!d->pkg || (d->kind == L_DEF && u64map_get(&root, (uint64_t) (uintptr_t) d->label, NULL))) continue;
        Sym plain = d->label;
        d->label = qualify(c, d->pkg, d->label);
        u64map_put(&declared, (uint64_t) (uintptr_t) d->label, 1);
        uint64_t v;
        if (!u64map_get(&c->plainidx, (uint64_t) (uintptr_t) plain, &v))
            u64map_put(&c->plainidx, (uint64_t) (uintptr_t) plain, (uint64_t) (uintptr_t) d->label);
        else if (v != (uint64_t) (uintptr_t) d->label)
            u64map_put(&c->plainidx, (uint64_t) (uintptr_t) plain, PLAIN_AMBIGUOUS);
        any = true;
    }
    /* adding to another package's type, written pkg.#Type: it must have one */
    for (size_t i = 0; i < c->decls.n; i++) {
        Decl *d = &c->decls.v[i];
        if (d->kind == L_ATOM || !strchr(d->label, '.') || u64map_get(&declared, (uint64_t) (uintptr_t) d->label, NULL)) continue;
        qf_diag(c, true, d->loc, "%s adds to a type its package doesn't declare: declare it in its package's files", d->label);
    }
    for (size_t i = 0; any && i < c->decls.n; i++)
        resolve_names(c, c->decls.v[i].value, c->decls.v[i].pkg, &declared, &root);
    u64map_free(&declared);
    u64map_free(&root);
}

/* The plain name of a qualified one, tools.#Wrench or tools.wrench: past the
 * package's dot. */
static const char *past_package(const char *s)
{
    const char *d = strchr(s, '.');
    return d ? d + 1 : s;
}

/* What a packaged type or instance is called where only one is so named, and
 * the root has none: its plain name, which also finds it. */
static void name_plainly(Front *c)
{
    for (uint32_t t = 0; t < c->types.n; t++) {
        TypeInfo *ti = &c->types.v[t];
        if (ti->kind != TK_DEF || !type_pkg(c, t)) continue;
        Sym plain = qf_sym(c, past_package(ti->name));
        uint64_t v;
        if (!u64map_get(&c->plainidx, (uint64_t) (uintptr_t) plain, &v) || v != (uint64_t) (uintptr_t) ti->name) continue;
        if (u64map_get(&c->typeidx, (uint64_t) (uintptr_t) plain, NULL)) continue;
        u64map_put(&c->typeidx, (uint64_t) (uintptr_t) plain, t + 1);
        ti->name = plain;
    }
    for (uint32_t i = 0; i < c->insts.n; i++) {
        InstInfo *in = &c->insts.v[i];
        if (!in->decls.n || !in->decls.v[0]->pkg) continue;
        Sym plain = qf_sym(c, past_package(in->name));
        uint64_t v;
        if (!u64map_get(&c->plainidx, (uint64_t) (uintptr_t) plain, &v) || v != (uint64_t) (uintptr_t) in->name) continue;
        if (u64map_get(&c->instidx, (uint64_t) (uintptr_t) plain, NULL)) continue;
        u64map_put(&c->instidx, (uint64_t) (uintptr_t) plain, i + 1);
        in->name = plain;
    }
}

/* --- what this stage does not have ------------------------------------------------------------ */

/* The type hierarchy reads the declarations before any structure is built,
 * and it expects labels on fields. So source that needs a later stage is
 * found here, before the hierarchy. */
/* The front end knows the names of its attributes (ADR 0019). `field`: the
 * attributes are those of a field, where @derived can stand. */
static void check_attrs(Front *c, const Attr *attrs, int n, bool field, Loc loc)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(attrs[i].name, "derived") != 0)
            qf_diag(c, true, loc, "unknown attribute @%s. The attribute of this stage is @derived, on a field of a type",
                    attrs[i].name);
        else if (!field)
            qf_diag(c, true, loc, "@derived marks a field of a type: write it after the value of the field");
        else if (attrs[i].args && attrs[i].args[0])
            qf_diag(c, true, loc, "@derived has no arguments");
    }
}

static void check_stage(Front *c, const X *x)
{
    if (!x) return;
    switch (x->k) {
    case X_STRUCT:
        check_attrs(c, x->u.st.attrs, x->u.st.nattrs, false, x->loc);
        for (int i = 0; i < x->u.st.n; i++) {
            const XField *fl = &x->u.st.fields[i];
            check_attrs(c, fl->attrs, fl->nattrs, true, fl->loc);
            check_stage(c, fl->dlabel);
            check_stage(c, fl->value);
        }
        break;
    case X_LIST:
        for (int i = 0; i < x->u.list.n; i++) check_stage(c, x->u.list.items[i]);
        check_stage(c, x->u.list.elem);
        break;
    case X_AND: case X_OR:
        check_stage(c, x->u.bin.a);
        check_stage(c, x->u.bin.b);
        break;
    case X_ARITH: case X_CMP: case X_LOGIC:
        check_stage(c, x->u.ar.a);
        check_stage(c, x->u.ar.b);
        break;
    case X_NOT: case X_DEFAULT:
        check_stage(c, x->u.sub);
        break;
    case X_BOUND:
        check_stage(c, x->u.bound.of);
        break;
    default:
        break;
    }
}

/* --- the text form of the result, and the content hash ----------------------------------------- */

static int cmp_instp(const void *a, const void *b)
{
    return strcmp((*(const InstInfo *const *) a)->name, (*(const InstInfo *const *) b)->name);
}

static int cmp_kindp(const void *a, const void *b)
{
    return strcmp((*(const KindInfo *const *) a)->name, (*(const KindInfo *const *) b)->name);
}

static int cmp_strp(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

/* The defaults that a type has, as sorted lines. */
static void dump_defaults(const Front *f, Buf *b, const DefVec *v)
{
    VEC(char *) lines = { 0 };
    for (size_t i = 0; i < v->n; i++) {
        Buf l = { 0 };
        qf_buf_puts(&l, "  default at ");
        path_format(f, &l, v->v[i].path.v, v->v[i].path.n);
        qf_buf_puts(&l, ": ");
        qf_print(&l, v->v[i].value);
        qf_buf_printf(&l, " from %s", f->types.v[v->v[i].contributor].name);
        VEC_PUSH(lines, l.s);
    }
    if (lines.n) qsort(lines.v, lines.n, sizeof(char *), cmp_strp);
    for (size_t i = 0; i < lines.n; i++) {
        qf_buf_printf(b, "%s\n", lines.v[i]);
        free(lines.v[i]);
    }
    VEC_FREE(lines);
}

/* The records of one owner, as sorted lines: the order in which a load met
 * them must not count. */
static void dump_records(const Front *f, Buf *b, const ArithVec *v)
{
    VEC(char *) lines = { 0 };
    for (size_t i = 0; i < v->n; i++) {
        Buf l = { 0 };
        qf_buf_puts(&l, "  record at ");
        path_format(f, &l, v->v[i].path.v, v->v[i].path.n);
        qf_buf_puts(&l, ": ");
        qf_print(&l, v->v[i].expr);
        for (size_t t = 0; t < v->v[i].tags.n; t++) {
            qf_buf_printf(&l, "%s %s = ", t ? "," : " where", v->v[i].tags.v[t].name);
            path_format(f, &l, v->v[i].tags.v[t].path.v, v->v[i].tags.v[t].path.n);
        }
        VEC_PUSH(lines, l.s);
    }
    if (lines.n) qsort(lines.v, lines.n, sizeof(char *), cmp_strp);
    for (size_t i = 0; i < lines.n; i++) {
        qf_buf_printf(b, "%s\n", lines.v[i]);
        free(lines.v[i]);
    }
    VEC_FREE(lines);
}

/* The reference records of one owner, as sorted lines. */
static void dump_refs(const Front *f, Buf *b, const InstRefVec *v)
{
    VEC(char *) lines = { 0 };
    for (size_t i = 0; i < v->n; i++) {
        Buf l = { 0 };
        qf_buf_puts(&l, "  reference at ");
        path_format(f, &l, v->v[i].path.v, v->v[i].path.n);
        qf_buf_printf(&l, ": %s", v->v[i].inst);
        if (v->v[i].sub.n) {
            qf_buf_putc(&l, '.');
            path_format(f, &l, v->v[i].sub.v, v->v[i].sub.n);
        }
        VEC_PUSH(lines, l.s);
    }
    if (lines.n) qsort(lines.v, lines.n, sizeof(char *), cmp_strp);
    for (size_t i = 0; i < lines.n; i++) {
        qf_buf_printf(b, "%s\n", lines.v[i]);
        free(lines.v[i]);
    }
    VEC_FREE(lines);
}

void qf_dump(const Front *f, Buf *b)
{
    static const char *const rounding[] = { "half-even", "floor", "ceiling", "zero" };
    qf_buf_puts(b, "");
    /* kinds in name order */
    const KindInfo **ks = qf_xmalloc(sizeof *ks * (f->kinds.n ? f->kinds.n : 1));
    for (size_t i = 0; i < f->kinds.n; i++) ks[i] = &f->kinds.v[i];
    if (f->kinds.n) qsort(ks, f->kinds.n, sizeof *ks, cmp_kindp);
    for (size_t i = 0; i < f->kinds.n; i++) {
        const KindInfo *k = ks[i];
        qf_buf_printf(b, "kind %s: ", k->name);
        qf_dim_format(f, b, &f->dims.v[k->dimix].dim);
        qf_buf_printf(b, ", scale %d%s, %s%s%s\n", k->scale, k->conserved ? ", conserved" : "", rounding[k->rounding & 3],
                      k->base ? ", base " : "", k->base ? k->base : "");
    }
    free(ks);
    /* types in the order of their ids: the hierarchy numbers them by its
     * shape and their names, not by the order of declarations */
    for (uint32_t t = 0; t < f->types.n; t++) {
        const TypeInfo *ti = &f->types.v[t];
        qf_buf_printf(b, "type %s <", ti->name);
        for (size_t k = 0; k < ti->parents.n; k++) qf_buf_printf(b, "%s%s", k ? ", " : " ", f->types.v[ti->parents.v[k]].name);
        qf_buf_puts(b, " > ");
        if (ti->expansion) qf_node_print(f, b, ti->expansion);
        qf_buf_putc(b, '\n');
        dump_records(f, b, &ti->ariths);
        dump_defaults(f, b, &ti->defaults);
        dump_refs(f, b, &ti->refs);
    }
    for (uint32_t ft = 0; ft < f->feats.n; ft++) {
        qf_buf_printf(b, "feature %s of ", f->feats.v[ft].full);
        for (size_t k = 0; k < f->feats.v[ft].intros.n; k++)
            qf_buf_printf(b, "%s%s", k ? ", " : "", f->types.v[f->feats.v[ft].intros.v[k]].name);
        qf_buf_putc(b, '\n');
    }
    /* instances in name order */
    const InstInfo **is = qf_xmalloc(sizeof *is * (f->insts.n ? f->insts.n : 1));
    for (size_t i = 0; i < f->insts.n; i++) is[i] = &f->insts.v[i];
    if (f->insts.n) qsort(is, f->insts.n, sizeof *is, cmp_instp);
    for (size_t i = 0; i < f->insts.n; i++) {
        qf_buf_printf(b, "instance %s = ", is[i]->name);
        if (is[i]->node) qf_node_print(f, b, is[i]->node);
        qf_buf_putc(b, '\n');
        dump_records(f, b, &is[i]->ariths);
        dump_refs(f, b, &is[i]->refs);
    }
    free(is);
}

/* --- the records of a structure ----------------------------------------------------------------- */

static void records_walk(const Front *f, const QfNode *n, U32Vec *path, U64Map *seen, QfRecUses *out)
{
    uint64_t k = (uint64_t) (uintptr_t) n;
    if (u64map_get(seen, k, NULL)) return;       /* a shared node: its first path */
    u64map_put(seen, k, 1);
    const ArithVec *v = &f->types.v[n->type].ariths;
    for (size_t i = 0; i < v->n; i++) {
        QfRecUse u = { &v->v[i], { 0 } };
        for (size_t p = 0; p < path->n; p++) VEC_PUSH(u.prefix, path->v[p]);
        VEC_PUSH(*out, u);
    }
    if (n->flags & NF_VALUE) return;
    for (uint32_t i = 0; i < n->narcs; i++) {
        VEC_PUSH(*path, n->arcs[i].feat);
        records_walk(f, n->arcs[i].val, path, seen, out);
        path->n--;
    }
}

void qf_records(const Front *f, const QfNode *root, const ArithVec *own, QfRecUses *out)
{
    for (size_t i = 0; own && i < own->n; i++) {
        QfRecUse u = { &own->v[i], { 0 } };
        VEC_PUSH(*out, u);
    }
    U32Vec path = { 0 };
    U64Map seen = { 0 };
    records_walk(f, root, &path, &seen, out);
    VEC_FREE(path);
    u64map_free(&seen);
}

void qf_records_free(QfRecUses *v)
{
    for (size_t i = 0; i < v->n; i++) VEC_FREE(v->v[i].prefix);
    VEC_FREE(*v);
}

/* --- the references of a structure ---------------------------------------------------------------- */

static void refs_walk(const Front *f, const QfNode *n, U32Vec *path, U64Map *seen, InstRefVec *out)
{
    uint64_t k = (uint64_t) (uintptr_t) n;
    if (u64map_get(seen, k, NULL)) return;       /* a shared node: its first path */
    u64map_put(seen, k, 1);
    const InstRefVec *v = &f->types.v[n->type].refs;
    for (size_t i = 0; i < v->n; i++) {
        InstRef r = { { 0 }, v->v[i].inst, { 0 } };
        for (size_t p = 0; p < path->n; p++) VEC_PUSH(r.path, path->v[p]);
        for (size_t p = 0; p < v->v[i].path.n; p++) VEC_PUSH(r.path, v->v[i].path.v[p]);
        for (size_t p = 0; p < v->v[i].sub.n; p++) VEC_PUSH(r.sub, v->v[i].sub.v[p]);
        copy_ref(out, &r);
        VEC_FREE(r.path);
        VEC_FREE(r.sub);
    }
    if (n->flags & NF_VALUE) return;
    for (uint32_t i = 0; i < n->narcs; i++) {
        VEC_PUSH(*path, n->arcs[i].feat);
        refs_walk(f, n->arcs[i].val, path, seen, out);
        path->n--;
    }
}

void qf_refs(const Front *f, const QfNode *root, const InstRefVec *own, InstRefVec *out)
{
    for (size_t i = 0; own && i < own->n; i++) copy_ref(out, &own->v[i]);
    U32Vec path = { 0 };
    U64Map seen = { 0 };
    refs_walk(f, root, &path, &seen, out);
    VEC_FREE(path);
    u64map_free(&seen);
}

void qf_refs_free(InstRefVec *v)
{
    for (size_t i = 0; i < v->n; i++) {
        VEC_FREE(v->v[i].path);
        VEC_FREE(v->v[i].sub);
    }
    VEC_FREE(*v);
}

/* --- loading ----------------------------------------------------------------------------------- */

static int cmp_diag(const void *a, const void *b)
{
    const Diag *x = a, *y = b;
    int r = !x->loc.file || !y->loc.file ? (x->loc.file != NULL) - (y->loc.file != NULL) : strcmp(x->loc.file, y->loc.file);
    if (r) return r;
    if (x->loc.line != y->loc.line) return x->loc.line - y->loc.line;
    if (x->loc.col != y->loc.col) return x->loc.col - y->loc.col;
    return strcmp(x->msg, y->msg);
}

void qf_sort_diags(Front *f)
{
    if (f->diags.n) qsort(f->diags.v, f->diags.n, sizeof(Diag), cmp_diag);
}

/* --- defaults: the resolution (quad-ijx.10) ------------------------------------------------------------
 *
 * The method is Triangle's resolve_defaults. A default applies after every
 * value is unified. For one path, the most specific defaults apply first: an
 * instance's own, then those of the instances that it names, then those of
 * the types, a subtype before its supertype. A default never changes a value.
 * Defaults from types with no order between them that conflict do not apply,
 * and a warning names them. A default that conflicts with one of those does
 * not apply either. The result does not depend on the order of files or
 * declarations. */

static int cmp_path(const U32Vec *a, const U32Vec *b)
{
    size_t n = a->n < b->n ? a->n : b->n;
    for (size_t i = 0; i < n; i++)
        if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    return a->n < b->n ? -1 : a->n > b->n;
}

/* The defaults of the type of each node, with full paths. A node that stands
 * for the constraint of a recursive type gives the defaults of its type, and
 * not those of its fields. */
static void defaults_walk(const Front *f, const QfNode *n, U32Vec *path, U64Map *seen, DefVec *out)
{
    uint64_t k = (uint64_t) (uintptr_t) n;
    if (u64map_get(seen, k, NULL)) return;       /* a shared node: its first path */
    u64map_put(seen, k, 1);
    const DefVec *v = &f->types.v[n->type].defaults;
    for (size_t i = 0; i < v->n; i++) add_default(out, &v->v[i], path->v, path->n, v->v[i].contributor);
    if (n->flags & NF_VALUE) return;
    for (uint32_t i = 0; i < n->narcs; i++) {
        VEC_PUSH(*path, n->arcs[i].feat);
        defaults_walk(f, n->arcs[i].val, path, seen, out);
        path->n--;
    }
}

static void collect_defaults(const Front *f, const QfNode *root, const DefVec *own, DefVec *out)
{
    U32Vec path = { 0 };
    U64Map seen = { 0 };
    defaults_walk(f, root, &path, &seen, out);
    VEC_FREE(path);
    u64map_free(&seen);
    for (size_t k = 0; own && k < own->n; k++) add_default(out, &own->v[k], NULL, 0, own->v[k].contributor);
}

/* A chain of nodes of the top type that has `value` at `path`. */
static QfNode *wrap_path(QfOp *c, const U32Vec *path, QfNode *value)
{
    QfNode *n = value;
    for (size_t i = path->n; i-- > 0 && n;) n = qf_build_struct(c, &c->scratch, c->f->top, &path->v[i], &n, 1);
    return n;
}

/* Unifies `root` with each default at its path. Gives the new root in `out`,
 * or NULL if they do not all fit. why: gets the reason, if a default does not
 * fit. An error in the value of a default gives a diagnostic: check_defaults
 * builds each value first, so the resolution meets no such error. */
static QfNode *try_defaults(QfOp *c, QfNode *root, DefaultRec *const *ds, size_t n, QfStore *out, Buf *why)
{
    QfMark m = qf_arena_mark(c->scratch.arena);
    uint32_t mn = c->scratch.n;
    uint64_t saved = op_begin(c);
    int saveddepth = c->depth;
    bool savedfailed = c->failed;
    Build b = { 0 };
    b.c = c; b.ok = true; b.owner = QF_BOTTOM;
    QfNode *r = NULL;
    bool ok = true;
    for (size_t i = 0; i < n && ok; i++) {
        path_set(c, NULL, 0);
        QfNode *v = build_raw(&b, ds[i]->value);
        QfNode *w = v ? wrap_path(c, &ds[i]->path, v) : NULL;
        if (!w) { ok = false; break; }
        path_set(c, NULL, 0);
        reset_failure(c);
        ok = unify1(c, root, w);
        if (!ok && why) failure_text(why, fail_info(c));
    }
    if (ok) {
        bool closed;
        r = copy_out(c, out, root, &closed);
        if (!closed) r = NULL;
    }
    build_free(&b);
    op_end(c, saved);
    qf_arena_release(c->scratch.arena, m);
    c->scratch.n = mn;
    c->depth = saveddepth;
    c->failed = savedfailed;
    c->failfmt = false;
    return r;
}

static const char *contributor_name(const Front *f, const DefaultRec *d)
{
    return d->contributor == QF_CONTRIB_INSTANCE ? "the instance" : d->contributor == QF_CONTRIB_NAMED ? d->who
                                                                                                    : f->types.v[d->contributor].name;
}

/* a is strictly more specific than b */
static bool more_specific(const Front *f, uint32_t a, uint32_t b)
{
    if (a == b) return false;
    if (a == QF_CONTRIB_INSTANCE || b == QF_CONTRIB_INSTANCE) return a == QF_CONTRIB_INSTANCE;
    if (a == QF_CONTRIB_NAMED || b == QF_CONTRIB_NAMED) return a == QF_CONTRIB_NAMED;
    return is_subtype(f, a, b);
}

static int cmp_defptr(const void *a, const void *b)
{
    const DefaultRec *x = *(DefaultRec *const *) a, *y = *(DefaultRec *const *) b;
    int d = cmp_path(&x->path, &y->path);
    /* qsort is not stable: the records of one path keep the order of the collection */
    return d ? d : (x > y) - (x < y);
}

/* The structure of an instance with its defaults. *count gets the number of
 * defaults that the structure had. */
static QfNode *resolve_defaults(QfOp *c, QfNode *root, const InstInfo *in, size_t *count)
{
    Front *f = c->f;
    DefVec all = { 0 };
    collect_defaults(f, root, &in->defaults, &all);
    *count = all.n;
    DefaultRec **order = qf_xmalloc(sizeof *order * (all.n ? all.n : 1));
    for (size_t k = 0; k < all.n; k++) order[k] = &all.v[k];
    if (all.n) qsort(order, all.n, sizeof *order, cmp_defptr);       /* by path, then as collected */

    for (size_t g0 = 0, g1; g0 < all.n; g0 = g1) {
        for (g1 = g0; g1 < all.n && cmp_path(&order[g1]->path, &order[g0]->path) == 0;) g1++;
        size_t gn = g1 - g0, left = gn;
        bool *done = qf_xmalloc(gn);
        memset(done, 0, gn);
        VEC(DefaultRec *) dropped = { 0 };
        DefaultRec **ready = qf_xmalloc(sizeof *ready * gn), **compat = qf_xmalloc(sizeof *compat * gn);
        while (left) {
            /* the defaults that no default more specific waits before */
            size_t nr = 0;
            for (size_t a = 0; a < gn; a++) {
                bool isready = !done[a];
                for (size_t o = 0; o < gn && isready; o++)
                    if (!done[o] && more_specific(f, order[g0 + o]->contributor, order[g0 + a]->contributor)) isready = false;
                if (isready) ready[nr++] = order[g0 + a];
            }
            for (size_t a = 0; a < gn; a++)
                for (size_t q = 0; q < nr; q++)
                    if (order[g0 + a] == ready[q]) { done[a] = true; left--; }
            /* A default that conflicts with a default that did not apply
             * does not apply either. So a default that a more specific default
             * overrides stays overridden. Triangle has this rule only for the
             * more specific default. Then a supertype that states the value of
             * one side of a conflict decides the conflict. */
            size_t kept = 0;
            for (size_t q = 0; q < nr; q++) {
                bool overridden = false;
                for (size_t k = 0; k < dropped.n && !overridden; k++) {
                    DefaultRec *pair[2] = { ready[q], dropped.v[k] };
                    overridden = !try_defaults(c, root, pair, 2, &f->store, NULL);
                }
                if (!overridden) ready[kept++] = ready[q];
            }
            nr = kept;
            if (!nr) continue;
            QfNode *res = try_defaults(c, root, ready, nr, &f->store, NULL);
            if (res) { root = res; continue; }
            /* not all together: a default that does not fit alone loses to a value */
            size_t nc = 0;
            for (size_t q = 0; q < nr; q++)
                if (try_defaults(c, root, &ready[q], 1, &f->store, NULL)) compat[nc++] = ready[q];
            if (nc == 1) {
                root = try_defaults(c, root, compat, 1, &f->store, NULL);
            } else if (nc > 1) {
                Buf names = { 0 }, pth = { 0 };
                const char **ns = qf_xmalloc(sizeof *ns * nc);       /* in the order of the names, not of the load */
                for (size_t q = 0; q < nc; q++) ns[q] = contributor_name(f, compat[q]);
                qsort(ns, nc, sizeof *ns, cmp_strp);
                for (size_t q = 0; q < nc; q++) qf_buf_printf(&names, "%s%s", q ? ", " : "", ns[q]);
                free(ns);
                path_format(f, &pth, order[g0]->path.v, order[g0]->path.n);
                qf_diag(f, false, in->decls.v[0]->loc, "%s: the defaults for %s from %s conflict, so none of them "
                        "applies. Give %s a value", in->name, pth.s, names.s, pth.s);
                for (size_t q = 0; q < nc; q++) VEC_PUSH(dropped, compat[q]);
                qf_buf_free(&names);
                qf_buf_free(&pth);
            }
        }
        VEC_FREE(dropped);
        free(done);
        free(ready);
        free(compat);
    }
    free(order);
    free_defaults(&all);
    return root;
}

/* Builds the value of a default, and for `*d | rest` checks that d is a value
 * of rest. False with a diagnostic if the value has an error or does not fit. */
static bool default_probe(Front *f, const DefaultRec *d, const char *who)
{
    QfOp *c = f->op;
    QfMark m = qf_arena_mark(c->scratch.arena);
    uint32_t mn = c->scratch.n;
    uint64_t saved = op_begin(c);
    Build b = { 0 };
    b.c = c; b.ok = true; b.owner = QF_BOTTOM;
    path_set(c, NULL, 0);
    QfNode *v = build_raw(&b, d->value), *r = v && d->rest ? build_raw(&b, d->rest) : NULL;
    bool ok = v && (!d->rest || r);
    reset_failure(c);
    if (r && !unify1(c, r, v)) {
        Buf pth = { 0 };
        path_format(f, &pth, d->path.v, d->path.n);
        qf_diag(f, true, d->value->loc, "%s: the default for %s is not a value of its other alternative: %s. In "
                "*d | rest, the default d must be one of the values of rest", who, pth.s, fail_info(c)->text);
        qf_buf_free(&pth);
        ok = false;
    }
    build_free(&b);
    op_end(c, saved);
    qf_arena_release(c->scratch.arena, m);
    c->scratch.n = mn;
    c->depth = 0;
    c->failed = false;
    c->failfmt = false;
    return ok;
}

/* An error for each default that can never apply: a default that is not a
 * value of its other alternative, and a default of a type that does not fit
 * the field in that type. A default of an instance can lose to a value of the
 * instance, and that is not an error. */
static void check_defaults(Front *f)
{
    QfStore *tmp = qf_store_new();
    for (uint32_t t = 0; t < f->types.n; t++) {
        for (size_t i = 0; i < f->types.v[t].defaults.n; i++) {
            DefaultRec *d = &f->types.v[t].defaults.v[i];
            if (d->contributor != t || !f->types.v[t].expansion) continue;
            Buf why = { 0 }, pth = { 0 };
            if (default_probe(f, d, f->types.v[t].name) && !try_defaults(f->op, f->types.v[t].expansion, &d, 1, tmp, &why)) {
                path_format(f, &pth, d->path.v, d->path.n);
                qf_diag(f, true, d->value->loc, "%s: the default for %s can never apply: %s", f->types.v[t].name, pth.s,
                        why.s ? why.s : "its type does not allow the value");
            }
            qf_buf_free(&why);
            qf_buf_free(&pth);
        }
    }
    for (uint32_t i = 0; i < f->insts.n; i++)
        for (size_t k = 0; k < f->insts.v[i].defaults.n; k++)
            if (f->insts.v[i].defaults.v[k].contributor == QF_CONTRIB_INSTANCE)
                default_probe(f, &f->insts.v[i].defaults.v[k], f->insts.v[i].name);
    qf_store_free(tmp);
}

/* Gives each instance its structure with defaults. A default can give a
 * node a type with defaults of its own, so the step repeats while the
 * structure gets more defaults. */
static void apply_defaults(Front *f)
{
    for (uint32_t i = 0; i < f->insts.n; i++) {
        if (f->insts.v[i].pstate != 2 || !f->insts.v[i].pattern) continue;
        QfNode *root = f->insts.v[i].pattern;
        size_t had = 0, has = 0;
        for (int round = 0; round < 64; round++) {
            root = resolve_defaults(f->op, root, &f->insts.v[i], &had);
            DefVec now = { 0 };
            collect_defaults(f, root, &f->insts.v[i].defaults, &now);
            has = now.n;
            free_defaults(&now);
            if (has <= had) break;
        }
        f->insts.v[i].node = root;
    }
}

static bool resolve(Front *c)
{
    size_t errors0 = c->nerrors;
    for (size_t i = 0; i < c->decls.n; i++) {
        check_attrs(c, c->decls.v[i].attrs, c->decls.v[i].nattrs, false, c->decls.v[i].loc);
        if (c->decls.v[i].kind != L_KIND) check_stage(c, c->decls.v[i].value);
    }
    if (c->nerrors > errors0) return false;
    resolve_packages(c);
    if (c->nerrors > errors0 || !qf_kinds_build(c) || !qf_expand_all(c) || !types_build(c) || !check_lets(c)) return false;
    name_plainly(c);
    c->op = qf_op_new(c);
    for (uint32_t t = 0; t < c->types.n; t++) type_expansion(c->op, t);
    for (uint32_t i = 0; i < c->insts.n; i++) inst_build(c->op, i);
    if (c->nerrors > errors0) return false;
    check_defaults(c);
    if (c->nerrors > errors0) return false;
    apply_defaults(c);
    Buf b = { 0 };
    qf_dump(c, &b);
    c->hash = qf_fnv1a(0xcbf29ce484222325ULL, b.s, b.len);
    qf_buf_free(&b);
    return true;
}

bool qf_load(Front *c, const char *const *names, const char *const *sources, size_t n)
{
    if (c->loaded) {
        qf_diag(c, true, noloc, "this front end already has a content: a load happens once");
        return false;
    }
    bool ok = qf_parse(c, PRELUDE_FILE, prelude, &c->decls);
    ok &= qf_parse(c, PRELUDE_FILE, prelude_quad, &c->decls);
    size_t own = c->decls.n;
    for (size_t i = 0; i < n; i++) ok &= qf_parse(c, names[i], sources[i], &c->decls);
    /* the package of the prelude has only the prelude's declarations */
    Sym quad = qf_sym(c, PRELUDE_PKG);
    Sym said = NULL;
    for (size_t i = own; i < c->decls.n; i++) {
        const Decl *d = &c->decls.v[i];
        if (d->pkg != quad || d->loc.file == said) continue;
        said = d->loc.file;
        qf_diag(c, true, d->loc, "the package quad is the prelude: a content cannot declare in it");
        ok = false;
    }
    ok = ok && resolve(c);
    c->loaded = true;            /* also after an error: the content does not change again */
    qf_sort_diags(c);
    return ok;
}

uint32_t qf_instance(Front *f, const char *name)
{
    Sym s = qf_sym(f, name);
    uint64_t v;
    return u64map_get(&f->instidx, (uint64_t) (uintptr_t) s, &v) ? (uint32_t) v - 1 : QF_BOTTOM;
}
