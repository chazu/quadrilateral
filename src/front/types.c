#include "front/front.h"

#include <ctype.h>

/* Builds the type hierarchy: types from declarations and atoms, anonymous
 * enums, completion to a bounded complete partial order (generated meet types),
 * deterministic IDs, codes for meets, and feature appropriateness.
 *
 * Borrowed from Triangle's types.c (bruggadung f425574), under ADR 0003.
 * Changes from Triangle (ADR 0015): numbers are `number`, one dimension type
 * for each dimension, and the kinds that source declares, in place of a fixed
 * table of dimensions. The TDL front end and components are gone. Operations
 * after load use op_meet, which keeps its cache in the operation's context. */

static Loc noloc;

uint32_t type_lookup(const Front *c, Sym name)
{
    uint64_t v;
    if (u64map_get(&c->typeidx, (uint64_t) (uintptr_t) name, &v)) return (uint32_t) v - 1;
    return QF_BOTTOM;
}

uint32_t feat_lookup(const Front *c, Sym name)
{
    uint64_t v;
    if (u64map_get(&c->featidx, (uint64_t) (uintptr_t) name, &v)) return (uint32_t) v - 1;
    return QF_BOTTOM;
}

static uint32_t type_add(Front *c, Sym name, TypeKind kind)
{
    TypeInfo t = { 0 };
    t.name = name;
    t.kind = kind;
    const char *pound = strstr(name, ".#");      /* tools.#Wrench is the tools package's */
    if (!pound) pound = strstr(name, ".%");      /* and so is money.%coin */
    if (pound && pound > name && (isalpha((unsigned char) name[0]) || name[0] == '_')) {
        bool ident = true;
        for (const char *p = name; p < pound && ident; p++) ident = isalnum((unsigned char) *p) || *p == '_';
        if (ident) {
            size_t len = (size_t) (pound - name);      /* strndup is not C11, nor in every libc */
            char *pkg = qf_xmalloc(len + 1);
            memcpy(pkg, name, len);
            pkg[len] = 0;
            t.home = qf_sym(c, pkg);
            free(pkg);
        }
    }
    t.bit = -1;          /* no code until compute_codes runs */
    VEC_PUSH(c->types, t);
    uint32_t id = (uint32_t) c->types.n - 1;
    u64map_put(&c->typeidx, (uint64_t) (uintptr_t) name, id + 1);
    return id;
}

static uint32_t type_get(Front *c, Sym name, TypeKind kind)
{
    uint32_t t = type_lookup(c, name);
    return t != QF_BOTTOM ? t : type_add(c, name, kind);
}

static void add_parent(Front *c, uint32_t child, uint32_t parent)
{
    TypeInfo *t = &c->types.v[child];
    for (size_t i = 0; i < t->parents.n; i++)
        if (t->parents.v[i] == parent) return;
    VEC_PUSH(t->parents, parent);
}

/* The type of a dimension: below `number`, made on first use. Only while the
 * hierarchy is being built, before the types are renumbered. */
uint32_t qf_dim_type(Front *c, uint32_t dimix)
{
    uint32_t t = type_lookup(c, c->dims.v[dimix].name);
    if (t != QF_BOTTOM) return t;
    t = type_add(c, c->dims.v[dimix].name, TK_DIMT);
    c->types.v[t].dimix = dimix;
    add_parent(c, t, c->tnumber);
    return t;
}

static int cmp_sym(const void *a, const void *b)
{
    return name_cmp(*(Sym const *) a, *(Sym const *) b);
}

/* A qualified name's package and plain name: tools.#Wrench, tools.wrench_250.
 * Anything else is plain. */
static const char *plain_part(const char *s, size_t *pkglen)
{
    *pkglen = 0;
    if (!(isalpha((unsigned char) s[0]) || s[0] == '_')) return s;
    const char *p = s;
    while (isalnum((unsigned char) *p) || *p == '_') p++;
    if (*p != '.' || !(p[1] == '#' || p[1] == '_' || isalpha((unsigned char) p[1]))) return s;
    *pkglen = (size_t) (p - s);
    return p + 1;
}

int name_cmp(const char *a, const char *b)
{
    size_t la, lb;
    const char *pa = plain_part(a, &la), *pb = plain_part(b, &lb);
    int r = strcmp(pa, pb);
    if (r || (la == lb && !memcmp(a, b, la))) return r;
    if (!la || !lb) return la ? 1 : -1;               /* the root's first */
    r = memcmp(a, b, la < lb ? la : lb);
    return r ? r : la < lb ? -1 : 1;
}

Sym type_pkg(const Front *c, uint32_t t)
{
    return c->types.v[t].home;
}

static Sym qualified_name(Front *c, Sym pkg, Sym name)
{
    Buf b = { 0 };
    qf_buf_printf(&b, "%s.%s", pkg, name);
    Sym q = qf_sym(c, b.s);
    qf_buf_free(&b);
    return q;
}

typedef struct { Sym label; Sym pkg; U32Vec intros; } NewFeat;
typedef struct { uint32_t type; uint32_t label; Sym pkg; } DeclPair;

static int cmp_newfeat(const void *a, const void *b)
{
    const NewFeat *x = a, *y = b;
    int r = strcmp(x->label, y->label);
    if (r) return r;
    if (!x->pkg || !y->pkg) return x->pkg ? 1 : y->pkg ? -1 : 0;
    return strcmp(x->pkg, y->pkg);
}

/* t is below a type that introduces f: t has the field. */
static bool below_intro(const Front *c, uint32_t t, uint32_t f)
{
    const U32Vec *in = &c->feats.v[f].intros;
    for (size_t i = 0; i < in->n; i++)
        if (is_subtype(c, t, in->v[i])) return true;
    return false;
}

bool introduces(const Front *c, uint32_t t, uint32_t f)
{
    const U32Vec *in = &c->feats.v[f].intros;
    for (size_t i = 0; i < in->n; i++)
        if (in->v[i] == t) return true;
    return false;
}

uint32_t feat_resolve(const Front *c, Sym label, uint32_t ctx, Sym pkg, bool *ambiguous)
{
    if (ambiguous) *ambiguous = false;
    if (!label) return QF_BOTTOM;
    if (strchr(label, '.')) return feat_lookup(c, label);      /* pkg.label: that package's */
    uint64_t v;
    if (!u64map_get(&c->labelidx, (uint64_t) (uintptr_t) label, &v)) return QF_BOTTOM;
    uint32_t first = (uint32_t) v - 1, end = first + 1;
    while (end < c->feats.n && c->feats.v[end].label == label) end++;
    if (end - first == 1) return first;
    /* several: the one the type it is written against has */
    if (ctx != QF_BOTTOM && ctx < c->types.n) {
        uint32_t found = QF_BOTTOM, n = 0;
        for (uint32_t f = first; f < end; f++)
            if (below_intro(c, ctx, f)) { found = f; n++; }
        if (n == 1) return found;
    }
    /* else the package's own, else the root's */
    for (uint32_t f = first; f < end; f++) if (pkg && c->feats.v[f].pkg == pkg) return f;
    for (uint32_t f = first; f < end; f++) if (!c->feats.v[f].pkg) return f;
    if (ambiguous) *ambiguous = true;
    return QF_BOTTOM;
}

bool label_names(const Front *c, Sym label, uint32_t f)
{
    return f < c->feats.n && (label == c->feats.v[f].label || label == c->feats.v[f].full);
}

/* --- codes ----------------------------------------------------------------------- */

/* Codes are descendant sets over "bits". Leaf types (no subtypes, one parent)
 * get no bit, as in PET: the many string types would otherwise make every
 * code enormous. Their subsumption follows the parent chain instead. */

static inline bool code_has_bit(const uint64_t *code, int32_t bit)
{
    return bit >= 0 && ((code[bit >> 6] >> (bit & 63)) & 1);
}

static void compute_codes(Front *c)
{
    size_t n = c->types.n;
    for (size_t i = 0; i < n; i++) {
        TypeInfo *t = &c->types.v[i];
        free(t->code);
        t->code = NULL;
        t->children.n = 0;
    }
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < c->types.v[i].parents.n; j++)
            VEC_PUSH(c->types.v[c->types.v[i].parents.v[j]].children, (uint32_t) i);
    c->nbits = 0;
    for (size_t i = 0; i < n; i++) {
        TypeInfo *t = &c->types.v[i];
        t->bit = (t->children.n == 0 && t->parents.n == 1) ? -1 : (int32_t) c->nbits++;
    }
    c->bittype = qf_xrealloc(c->bittype, (c->nbits ? c->nbits : 1) * sizeof(uint32_t));
    c->codewords = (c->nbits + 63) / 64;
    if (!c->codewords) c->codewords = 1;
    for (size_t i = 0; i < n; i++) {
        TypeInfo *t = &c->types.v[i];
        if (t->bit < 0) continue;
        c->bittype[t->bit] = (uint32_t) i;
        t->code = qf_xcalloc(c->codewords, 8);
    }
    /* propagate upward from the lowest non-leaf types */
    uint32_t *order = qf_xmalloc(n * sizeof(uint32_t));
    uint32_t *indeg = qf_xcalloc(n, sizeof(uint32_t));
    size_t head = 0, tail = 0;
    for (size_t i = 0; i < n; i++) {
        if (c->types.v[i].bit < 0) continue;
        for (size_t k = 0; k < c->types.v[i].children.n; k++)
            if (c->types.v[c->types.v[i].children.v[k]].bit >= 0) indeg[i]++;
        if (!indeg[i]) order[tail++] = (uint32_t) i;
    }
    while (head < tail) {
        uint32_t t = order[head++];
        TypeInfo *ti = &c->types.v[t];
        ti->code[ti->bit >> 6] |= 1ULL << (ti->bit & 63);
        ti->wlo = (uint32_t) (ti->bit >> 6);
        ti->whi = ti->wlo + 1;
        for (size_t k = 0; k < ti->children.n; k++) {
            TypeInfo *ch = &c->types.v[ti->children.v[k]];
            if (ch->bit < 0) continue;
            for (size_t w = ch->wlo; w < ch->whi; w++) ti->code[w] |= ch->code[w];
            if (ch->wlo < ti->wlo) ti->wlo = ch->wlo;
            if (ch->whi > ti->whi) ti->whi = ch->whi;
        }
        for (size_t k = 0; k < ti->parents.n; k++) {
            uint32_t p = ti->parents.v[k];
            if (--indeg[p] == 0) order[tail++] = p;
        }
    }
    free(order);
    free(indeg);
}

/* Hashes the non-zero words of a code that lie in [lo, hi): zero words don't
 * count, so any range covering the non-zero words gives the same hash. */
static uint64_t code_hash_range(const uint64_t *code, size_t lo, size_t hi)
{
    uint64_t h = 0x9e3779b97f4a7c15ULL;
    for (size_t w = lo; w < hi; w++)
        if (code[w]) h = qf_hash64(h ^ code[w]) + w;
    return h;
}

static void index_codes(Front *c)
{
    u64map_free(&c->codeidx);
    for (size_t i = 0; i < c->types.n; i++) {
        const TypeInfo *t = &c->types.v[i];
        if (t->bit >= 0) u64map_put(&c->codeidx, code_hash_range(t->code, t->wlo, t->whi), i + 1);
    }
}

/* The type whose code equals `code`, whose non-zero words lie in [lo, hi). */
static uint32_t type_with_code_range(const Front *c, const uint64_t *code, size_t lo, size_t hi)
{
    uint64_t v;
    if (!u64map_get(&c->codeidx, code_hash_range(code, lo, hi), &v)) return QF_BOTTOM;
    const TypeInfo *t = &c->types.v[v - 1];
    size_t a = lo < t->wlo ? lo : t->wlo, b = hi > t->whi ? hi : t->whi;
    for (size_t w = a; w < b; w++) {
        uint64_t x = w >= lo && w < hi ? code[w] : 0, y = w >= t->wlo && w < t->whi ? t->code[w] : 0;
        if (x != y) return QF_BOTTOM;   /* a 64-bit hash collision would make a meet look missing */
    }
    return (uint32_t) v - 1;
}

bool is_subtype(const Front *c, uint32_t a, uint32_t b)
{
    if (a == b) return true;
    const TypeInfo *tb = &c->types.v[b];
    if (tb->bit < 0) return false;
    while (c->types.v[a].bit < 0) {
        a = c->types.v[a].parents.v[0];
        if (a == b) return true;
    }
    return code_has_bit(tb->code, c->types.v[a].bit);
}

static uint32_t meet_slow(Front *c, uint32_t a, uint32_t b);

uint32_t meet(Front *c, uint32_t a, uint32_t b)
{
    if (a == b) return a;
    if (a == QF_BOTTOM || b == QF_BOTTOM) return QF_BOTTOM;
    uint64_t key = a < b ? ((uint64_t) a << 32 | b) : ((uint64_t) b << 32 | a);
    size_t slot = (size_t) (qf_hash64(key) & 4095);
    if (c->meetcache[slot].key == key + 1) return c->meetcache[slot].val;
    uint32_t r = meet_slow(c, a, b);
    c->meetcache[slot].key = key + 1;
    c->meetcache[slot].val = r;
    return r;
}

/* Forgets cached meets; the hierarchy has changed. */
void meet_cache_clear(Front *c)
{
    memset(c->meetcache, 0, sizeof c->meetcache);
    u64map_free(&c->meetmemo);
}

/* The meet from the codes. It changes nothing, so two operations can ask at
 * the same time. */
uint32_t meet_pure(const Front *c, uint32_t a, uint32_t b)
{
    if (a == b) return a;
    if (a == QF_BOTTOM || b == QF_BOTTOM) return QF_BOTTOM;
    if (is_subtype(c, a, b)) return a;
    if (is_subtype(c, b, a)) return b;
    if (c->types.v[a].bit < 0 || c->types.v[b].bit < 0) return QF_BOTTOM;
    const TypeInfo *ta = &c->types.v[a], *tb = &c->types.v[b];
    /* the meet's code is the intersection, and it lies where both codes do */
    uint32_t lo = ta->wlo > tb->wlo ? ta->wlo : tb->wlo, hi = ta->whi < tb->whi ? ta->whi : tb->whi;
    uint64_t small[8], *tmp = small;
    if (hi <= lo) return QF_BOTTOM;
    if (hi - lo > 8) tmp = qf_xmalloc((hi - lo) * 8);
    bool any = false;
    for (uint32_t w = lo; w < hi; w++) {
        tmp[w - lo] = ta->code[w] & tb->code[w];
        any |= tmp[w - lo] != 0;
    }
    uint32_t r = any ? type_with_code_range(c, tmp - lo, lo, hi) : QF_BOTTOM;
    if (tmp != small) free(tmp);
    return r;
}

static uint32_t meet_slow(Front *c, uint32_t a, uint32_t b)
{
    if (is_subtype(c, a, b)) return a;
    if (is_subtype(c, b, a)) return b;
    if (c->types.v[a].bit < 0 || c->types.v[b].bit < 0) return QF_BOTTOM;
    uint64_t key = a < b ? ((uint64_t) a << 32 | b) : ((uint64_t) b << 32 | a);
    uint64_t v;
    if (u64map_get(&c->meetmemo, key, &v)) return (uint32_t) v;
    uint32_t r = meet_pure(c, a, b);
    u64map_put(&c->meetmemo, key, r);
    return r;
}

bool approp(const Front *c, uint32_t t, uint32_t f)
{
    const U32Vec *fs = &c->types.v[t].feats;
    size_t lo = 0, hi = fs->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (fs->v[mid] == f) return true;
        if (fs->v[mid] < f) lo = mid + 1; else hi = mid;
    }
    return false;
}

bool is_value(const Front *c, uint32_t t)
{
    if (t >= c->types.n) return false;
    TypeKind k = c->types.v[t].kind;
    return k == TK_NUMBER || k == TK_DIMT || k == TK_KIND || k == TK_TEXT || k == TK_CONSTRAINT;
}

bool is_leaf(const Front *c, uint32_t t)
{
    return c->types.v[t].children.n == 0 && c->types.v[t].feats.n == 0;
}

/* --- anonymous enums ----------------------------------------------------------------- */

static void or_atoms(X *x, SymVec *out, bool *ok)
{
    if (x->k == X_OR) {
        or_atoms(x->u.bin.a, out, ok);
        or_atoms(x->u.bin.b, out, ok);
    } else if (x->k == X_DEFAULT) {
        or_atoms(x->u.sub, out, ok);
    } else if (x->k == X_ATOM) {
        VEC_PUSH(*out, x->u.name);
    } else {
        *ok = false;
    }
}

Sym anon_name(Front *c, X *orx)
{
    SymVec atoms = { 0 };
    bool ok = true;
    or_atoms(orx, &atoms, &ok);
    Sym r = NULL;
    if (ok && atoms.n) {
        qsort(atoms.v, atoms.n, sizeof(Sym), cmp_sym);
        size_t k = 0;
        for (size_t i = 0; i < atoms.n; i++)
            if (!k || atoms.v[k - 1] != atoms.v[i]) atoms.v[k++] = atoms.v[i];
        atoms.n = k;
        if (k >= 2) {
            Buf b = { 0 };
            for (size_t i = 0; i < k; i++) {
                if (i) qf_buf_puts(&b, " | ");
                qf_buf_puts(&b, atoms.v[i]);
            }
            r = qf_sym(c, b.s);
            qf_buf_free(&b);
        }
    }
    VEC_FREE(atoms);
    return r;
}

static void disjuncts(X *x, XPtrVec *out)
{
    if (x->k == X_OR) { disjuncts(x->u.bin.a, out); disjuncts(x->u.bin.b, out); }
    else VEC_PUSH(*out, x);
}

static void scan_expr(Front *c, X *x, bool typebody);

/* Records a disjunction of structures in a type body. Its types are made once
 * the declared hierarchy is known (make_disjunctions). */
static void register_disj(Front *c, X *x, Sym owner)
{
    if (u64map_get(&c->disjidx, (uint64_t) (uintptr_t) x, NULL)) return;
    DisjInfo d = { 0 };
    d.orx = x;
    d.owner = owner;
    d.def = -1;
    d.loc = x->loc;
    XPtrVec ds = { 0 };
    x_flatten(x, X_OR, &ds);
    for (size_t i = 0; i < ds.n; i++) {
        X *a = ds.v[i];
        if (a->k == X_DEFAULT) {                 /* one at most: scan_expr checks it */
            d.def = (int) i;
            a = a->u.sub;
        }
        VEC_PUSH(d.altx, a);
        scan_expr(c, a, true);
    }
    VEC_FREE(ds);
    VEC_PUSH(c->disjs, d);
    u64map_put(&c->disjidx, (uint64_t) (uintptr_t) x, c->disjs.n);
}

/* Registers atom literals, anonymous enums, and (in type bodies) disjunctions
 * of structures found anywhere in an expression. */
static void scan_expr(Front *c, X *x, bool typebody)
{
    if (!x) return;
    switch (x->k) {
    case X_ATOM:
        if (type_lookup(c, x->u.name) == QF_BOTTOM) {
            uint32_t t = type_add(c, x->u.name, TK_ATOM);
            (void) t;
        }
        break;
    case X_OR: {
        Sym an = anon_name(c, x);
        if (an && type_lookup(c, an) == QF_BOTTOM) {
            uint32_t t = type_add(c, an, TK_ANON);
            SymVec atoms = { 0 };
            bool ok = true;
            or_atoms(x, &atoms, &ok);
            for (size_t i = 0; i < atoms.n; i++) {
                bool dup = false;
                for (size_t j = 0; j < c->types.v[t].members.n; j++)
                    if (c->types.v[t].members.v[j] == atoms.v[i]) dup = true;
                if (!dup) VEC_PUSH(c->types.v[t].members, atoms.v[i]);
            }
            VEC_FREE(atoms);
        }
        XPtrVec alts = { 0 };
        x_flatten(x, X_OR, &alts);
        int ndef = 0;
        for (size_t i = 0; i < alts.n; i++)
            if (alts.v[i]->k == X_DEFAULT && ++ndef == 2)
                qf_diag(c, true, alts.v[i]->loc, "more than one default in a disjunction");
        VEC_FREE(alts);
        if (ndef > 1) break;
        if (!an && typebody) {
            if (x_numeric_or(x)) {
                /* *0 | >=0 & <=1000 is the range, with the default 0 */
                if (!x_default_rest(x))
                    qf_diag(c, true, x->loc, "a disjunction of numbers is not a type; use a range, or an enum of atoms");
            } else if (!x_text_or(x)) {
                register_disj(c, x, NULL);
                break;
            }
        }
        /* only the whole chain is an enum; its disjuncts are scanned individually */
        XPtrVec ds = { 0 };
        disjuncts(x, &ds);
        for (size_t i = 0; i < ds.n; i++) scan_expr(c, ds.v[i], typebody);
        VEC_FREE(ds);
        break;
    }
    case X_AND:
        scan_expr(c, x->u.bin.a, typebody);
        scan_expr(c, x->u.bin.b, typebody);
        break;
    case X_DEFAULT:
        scan_expr(c, x->u.sub, typebody);
        break;
    case X_STRUCT:
        for (int i = 0; i < x->u.st.n; i++) scan_expr(c, x->u.st.fields[i].value, typebody);
        break;
    case X_LIST:
        for (int i = 0; i < x->u.list.n; i++) scan_expr(c, x->u.list.items[i], typebody);
        if (x->u.list.elem) {
            scan_expr(c, x->u.list.elem, typebody);
            VEC_PUSH(c->listelems, x->u.list.elem);
        }
        break;
    case X_NUM: case X_BOUND: case X_ARITH: case X_NAME:
        /* a literal's unit, or arithmetic of kinds: its dimension gets a type */
        qf_dim_scan(c, x);
        if (x->k == X_ARITH) { scan_expr(c, x->u.ar.a, typebody); scan_expr(c, x->u.ar.b, typebody); }
        break;
    case X_CMP: case X_LOGIC:
        scan_expr(c, x->u.ar.a, typebody);
        scan_expr(c, x->u.ar.b, typebody);
        break;
    case X_NOT:
        scan_expr(c, x->u.sub, typebody);
        break;
    default:
        break;
    }
}

static void conjuncts(X *x, XPtrVec *out)
{
    if (x->k == X_AND) {
        conjuncts(x->u.bin.a, out);
        conjuncts(x->u.bin.b, out);
    } else {
        VEC_PUSH(*out, x);
    }
}

typedef VEC(SymVec) SymSets;

/* Collects the sets of named types conjoined in each AND chain. The top-level
 * chain of an instance is handled across all its declarations instead. */
static void conj_sets(Front *c, X *x, bool skip_root, SymSets *out)
{
    if (!x) return;
    if (x->k == X_AND) {
        XPtrVec cj = { 0 };
        conjuncts(x, &cj);
        SymVec names = { 0 };
        for (size_t j = 0; j < cj.n; j++) {
            X *y = cj.v[j];
            if (y->k == X_NAME) {
                uint32_t t = type_lookup(c, y->u.name);
                if (t != QF_BOTTOM && c->types.v[t].kind == TK_DEF) VEC_PUSH(names, y->u.name);
            } else {
                conj_sets(c, y, false, out);
            }
        }
        if (names.n >= 2 && !skip_root) VEC_PUSH(*out, names);
        else VEC_FREE(names);
        VEC_FREE(cj);
        return;
    }
    switch (x->k) {
    case X_OR: conj_sets(c, x->u.bin.a, false, out); conj_sets(c, x->u.bin.b, false, out); break;
    case X_DEFAULT: conj_sets(c, x->u.sub, false, out); break;
    case X_STRUCT:
        for (int i = 0; i < x->u.st.n; i++) conj_sets(c, x->u.st.fields[i].value, false, out);
        break;
    case X_LIST:
        for (int i = 0; i < x->u.list.n; i++) conj_sets(c, x->u.list.items[i], false, out);
        break;
    default: break;
    }
}

/* --- topological renumbering ---------------------------------------------------------- */

static void renumber(Front *c)
{
    size_t n = c->types.n;
    uint32_t *indeg = qf_xcalloc(n, sizeof(uint32_t));
    for (size_t i = 0; i < n; i++) indeg[i] = (uint32_t) c->types.v[i].parents.n;
    /* Kahn's algorithm with a ready set ordered by name. */
    uint32_t *heap = qf_xmalloc(n * sizeof(uint32_t));
    size_t hn = 0;
#define LESS(a, b) (name_cmp(c->types.v[a].name, c->types.v[b].name) < 0)
#define PUSH(x) do { size_t i_ = hn++; heap[i_] = (x); \
        while (i_ && LESS(heap[i_], heap[(i_ - 1) / 2])) { \
            uint32_t t_ = heap[i_]; heap[i_] = heap[(i_ - 1) / 2]; heap[(i_ - 1) / 2] = t_; i_ = (i_ - 1) / 2; } } while (0)
    for (size_t i = 0; i < n; i++) if (!indeg[i]) PUSH((uint32_t) i);
    uint32_t *order = qf_xmalloc(n * sizeof(uint32_t));
    uint32_t *newid = qf_xmalloc(n * sizeof(uint32_t));
    size_t k = 0;
    while (hn) {
        uint32_t t = heap[0];
        heap[0] = heap[--hn];
        size_t i = 0;
        for (;;) {
            size_t l = 2 * i + 1, r = l + 1, m = i;
            if (l < hn && LESS(heap[l], heap[m])) m = l;
            if (r < hn && LESS(heap[r], heap[m])) m = r;
            if (m == i) break;
            uint32_t tmp = heap[i]; heap[i] = heap[m]; heap[m] = tmp;
            i = m;
        }
        order[k] = t;
        newid[t] = (uint32_t) k++;
        for (size_t j = 0; j < c->types.v[t].children.n; j++) {
            uint32_t ch = c->types.v[t].children.v[j];
            if (--indeg[ch] == 0) PUSH(ch);
        }
    }
#undef LESS
#undef PUSH
    TypeInfo *nt = qf_xmalloc(n * sizeof(TypeInfo));
    for (size_t i = 0; i < n; i++) {
        nt[i] = c->types.v[order[i]];
        for (size_t j = 0; j < nt[i].parents.n; j++) nt[i].parents.v[j] = newid[nt[i].parents.v[j]];
    }
    free(c->types.v);
    c->types.v = nt;
    c->types.cap = n;
    u64map_free(&c->typeidx);
    for (size_t i = 0; i < n; i++)
        u64map_put(&c->typeidx, (uint64_t) (uintptr_t) c->types.v[i].name, i + 1);
    free(indeg);
    free(heap);
    free(order);
    free(newid);
}

static bool has_cycle(Front *c)
{
    size_t n = c->types.n;
    uint8_t *state = qf_xcalloc(n, 1);
    uint32_t *stack = qf_xmalloc(n * 2 * sizeof(uint32_t));
    bool cyc = false;
    for (size_t s = 0; s < n && !cyc; s++) {
        if (state[s]) continue;
        size_t sp = 0;
        stack[sp++] = (uint32_t) s;
        stack[sp++] = 0;
        state[s] = 1;
        while (sp && !cyc) {
            uint32_t t = stack[sp - 2], i = stack[sp - 1];
            if (i < c->types.v[t].parents.n) {
                stack[sp - 1]++;
                uint32_t p = c->types.v[t].parents.v[i];
                if (state[p] == 1) {
                    qf_diag(c, true, c->types.v[p].decls.n ? c->types.v[p].decls.v[0]->loc : noloc,
                            "the type hierarchy has a cycle through %s", c->types.v[p].name);
                    cyc = true;
                } else if (!state[p]) {
                    state[p] = 1;
                    stack[sp++] = p;
                    stack[sp++] = 0;
                }
            } else {
                state[t] = 2;
                sp -= 2;
            }
        }
    }
    free(state);
    free(stack);
    return cyc;
}

/* --- completion --------------------------------------------------------------------------- */

static void ancestors(Front *c, uint32_t m, uint32_t *mark, uint32_t stamp, U32Vec *out)
{
    out->n = 0;
    VEC_PUSH(*out, m);
    mark[m] = stamp;
    for (size_t i = 0; i < out->n; i++) {
        TypeInfo *t = &c->types.v[out->v[i]];
        for (size_t k = 0; k < t->parents.n; k++) {
            uint32_t p = t->parents.v[k];
            if (mark[p] != stamp) { mark[p] = stamp; VEC_PUSH(*out, p); }
        }
    }
}

static bool complete(Front *c)
{
    /* After the first round, a pair of older types keeps its meet: new meet
     * types only add codes below meets that already exist. So later rounds
     * only look at pairs involving a type made in the previous round. */
    size_t newfrom = 0;
    for (int round = 0; round < 64; round++) {
        compute_codes(c);
        index_codes(c);
        size_t n = c->types.n;
        VEC(uint64_t *) needed = { 0 };
        U64Map seen = { 0 };              /* hash of an intersection already handled */
        uint32_t *mark = qf_xcalloc(n, sizeof(uint32_t));
        uint32_t stamp = 0;
        U32Vec anc = { 0 };
        uint64_t *inter = qf_xcalloc(c->codewords, 8);
        uint32_t *news = qf_xmalloc(n * sizeof(uint32_t));
        U64Map pairs = { 0 };               /* pairs already examined this round */
        /* An incomparable pair whose codes meet needs a type with exactly the
         * intersection; if none exists, a meet type is made for it. */
#define EXAMINE(a_, b_) do { \
            uint32_t xa = (a_), xb = (b_); \
            if (is_subtype(c, xa, xb) || is_subtype(c, xb, xa)) break; \
            const TypeInfo *ta = &c->types.v[xa], *tb = &c->types.v[xb]; \
            size_t lo = ta->wlo > tb->wlo ? ta->wlo : tb->wlo, hi = ta->whi < tb->whi ? ta->whi : tb->whi; \
            bool any = false; \
            for (size_t w = lo; w < hi; w++) any |= (inter[w] = ta->code[w] & tb->code[w]) != 0; \
            if (any) { \
                uint64_t h = code_hash_range(inter, lo, hi); \
                if (!u64map_get(&seen, h, NULL)) { \
                    u64map_put(&seen, h, 1); \
                    if (type_with_code_range(c, inter, lo, hi) == QF_BOTTOM) { \
                        uint64_t *copy = qf_xcalloc(c->codewords, 8); \
                        memcpy(copy + lo, inter + lo, (hi - lo) * 8); \
                        VEC_PUSH(needed, copy); \
                    } \
                } \
            } \
            for (size_t w = lo; w < hi; w++) inter[w] = 0; \
        } while (0)
        if (!round) {
            /* every pair of ancestors of a type with several parents */
            for (size_t m = 0; m < n; m++) {
                if (c->types.v[m].parents.n < 2) continue;
                ancestors(c, (uint32_t) m, mark, ++stamp, &anc);
                for (size_t i = 1; i < anc.n; i++)
                    for (size_t j = i + 1; j < anc.n; j++) {
                        uint32_t a = anc.v[i], b = anc.v[j];
                        uint64_t pk = a < b ? ((uint64_t) a << 32 | b) : ((uint64_t) b << 32 | a);
                        if (u64map_get(&pairs, pk, NULL)) continue;
                        u64map_put(&pairs, pk, 1);
                        EXAMINE(a, b);
                    }
            }
        } else {
            /* each new type with every type sharing a descendant with it: the
             * ancestors of its descendants that have several parents */
            U32Vec **anccache = qf_xcalloc(n, sizeof(U32Vec *));
            uint32_t *pmark = qf_xcalloc(n, sizeof(uint32_t));
            uint32_t pstamp = 0;
            for (uint32_t a = (uint32_t) newfrom; a < n; a++) {
                pstamp++;
                pmark[a] = pstamp;
                const uint64_t *ca = c->types.v[a].code;
                for (size_t w = 0; ca && w < c->codewords; w++)
                    for (uint64_t word = ca[w]; word; word &= word - 1) {
                        uint32_t x = c->bittype[w * 64 + (size_t) __builtin_ctzll(word)];
                        if (c->types.v[x].parents.n < 2) continue;
                        if (!anccache[x]) {
                            ancestors(c, x, mark, ++stamp, &anc);
                            anccache[x] = qf_xcalloc(1, sizeof(U32Vec));
                            for (size_t i = 1; i < anc.n; i++) VEC_PUSH(*anccache[x], anc.v[i]);
                        }
                        const U32Vec *ax = anccache[x];
                        for (size_t i = 0; i < ax->n; i++) {
                            uint32_t b = ax->v[i];
                            if (pmark[b] == pstamp) continue;
                            pmark[b] = pstamp;
                            if (b >= newfrom && b < a) continue;      /* that pair was seen from b */
                            EXAMINE(a, b);
                        }
                    }
            }
            for (size_t i = 0; i < n; i++) if (anccache[i]) { VEC_FREE(*anccache[i]); free(anccache[i]); }
            free(anccache);
            free(pmark);
        }
#undef EXAMINE
        free(inter);
        free(news);
        u64map_free(&pairs);
        u64map_free(&seen);
        if (!needed.n) { free(mark); VEC_FREE(anc); return true; }
        c->nglb += needed.n;
        newfrom = n;                        /* types from here on are this round's */
        /* new glb types are added below and may be reached as ancestors */
        free(mark);
        mark = qf_xcalloc(n + needed.n, sizeof(uint32_t));
        stamp = 0;
        U32Vec created = { 0 };
        for (size_t k = 0; k < needed.n; k++) {
            uint64_t *in = needed.v[k];
            SymVec maxes = { 0 };
            U32Vec maxids = { 0 };
            for (uint32_t bit = 0; bit < c->nbits; bit++) {
                if (!code_has_bit(in, (int32_t) bit)) continue;
                uint32_t x = c->bittype[bit];
                bool maximal = true;
                for (size_t q = 0; q < c->types.v[x].parents.n && maximal; q++)
                    if (code_has_bit(in, c->types.v[c->types.v[x].parents.v[q]].bit)) maximal = false;
                if (maximal) { VEC_PUSH(maxes, c->types.v[x].name); VEC_PUSH(maxids, x); }
            }
            qsort(maxes.v, maxes.n, sizeof(Sym), cmp_sym);
            Buf b = { 0 };
            for (int suffix = 1;; suffix++) {
                b.len = 0;
                qf_buf_puts(&b, "glb[");
                for (size_t i = 0; i < maxes.n; i++) {
                    if (i) qf_buf_putc(&b, ',');
                    qf_buf_puts(&b, maxes.v[i]);
                }
                qf_buf_putc(&b, ']');
                if (suffix > 1) qf_buf_printf(&b, "#%d", suffix);
                if (type_lookup(c, qf_sym(c, b.s)) == QF_BOTTOM) break;
            }
            uint32_t g = type_add(c, qf_sym(c, b.s), TK_GLB);
            VEC_PUSH(created, g);
            qf_buf_free(&b);
            for (size_t i = 0; i < maxes.n; i++) VEC_PUSH(c->types.v[g].members, maxes.v[i]);
            /* parents: the minimal common ancestors of the maximal members, outside the set */
            U32Vec cands = { 0 };
            ancestors(c, maxids.v[0], mark, ++stamp, &anc);
            for (size_t i = 0; i < anc.n; i++) {
                uint32_t u = anc.v[i];
                if (u == g || code_has_bit(in, c->types.v[u].bit)) continue;
                bool all = true;
                for (size_t q = 1; q < maxids.n && all; q++) all = is_subtype(c, maxids.v[q], u);
                if (all) VEC_PUSH(cands, u);
            }
            SymVec pnames = { 0 };
            for (size_t i = 0; i < cands.n; i++) {
                bool minimal = true;
                for (size_t j = 0; j < cands.n && minimal; j++)
                    if (i != j && is_subtype(c, cands.v[j], cands.v[i])) minimal = false;
                if (minimal) VEC_PUSH(pnames, c->types.v[cands.v[i]].name);
            }
            qsort(pnames.v, pnames.n, sizeof(Sym), cmp_sym);
            for (size_t i = 0; i < pnames.n; i++) add_parent(c, g, type_lookup(c, pnames.v[i]));
            for (size_t i = 0; i < maxids.n; i++) add_parent(c, maxids.v[i], g);
            VEC_FREE(cands);
            VEC_FREE(pnames);
            VEC_FREE(maxes);
            VEC_FREE(maxids);
        }
        /* Meet types made in the same round don't see each other's codes yet:
         * place each under the minimal other new ones whose sets contain its set. */
        size_t W = c->codewords;
        /* candidates for containing set i are the sets holding its rarest bit */
        U64Map bitsets = { 0 };              /* bit → index into holders + 1 */
        VEC(U32Vec) holders = { 0 };
        for (size_t i = 0; i < needed.n; i++)
            for (size_t w = 0; w < W; w++)
                for (uint64_t word = needed.v[i][w]; word; word &= word - 1) {
                    uint64_t bit = w * 64 + (uint64_t) __builtin_ctzll(word), hv;
                    if (!u64map_get(&bitsets, bit, &hv)) {
                        U32Vec e = { 0 };
                        VEC_PUSH(holders, e);
                        hv = holders.n;
                        u64map_put(&bitsets, bit, hv);
                    }
                    VEC_PUSH(holders.v[hv - 1], (uint32_t) i);
                }
        for (size_t i = 0; i < needed.n; i++) {
            U32Vec sup = { 0 };
            const U32Vec *cand = NULL;
            for (size_t w = 0; w < W; w++)
                for (uint64_t word = needed.v[i][w]; word; word &= word - 1) {
                    uint64_t hv;
                    u64map_get(&bitsets, w * 64 + (uint64_t) __builtin_ctzll(word), &hv);
                    if (!cand || holders.v[hv - 1].n < cand->n) cand = &holders.v[hv - 1];
                }
            for (size_t ci = 0; cand && ci < cand->n; ci++) {
                size_t j = cand->v[ci];
                if (i == j) continue;
                bool subset = true, proper = false;
                for (size_t w = 0; w < W && subset; w++) {
                    uint64_t a = needed.v[i][w], bb = needed.v[j][w];
                    if (a & ~bb) subset = false;
                    if (bb & ~a) proper = true;
                }
                if (subset && proper) VEC_PUSH(sup, (uint32_t) j);
            }
            for (size_t x = 0; x < sup.n; x++) {
                bool minimal = true;
                for (size_t y = 0; y < sup.n && minimal; y++) {
                    if (x == y) continue;
                    bool sub = true;          /* needed[sup[y]] ⊊ needed[sup[x]] ? */
                    for (size_t w = 0; w < W && sub; w++)
                        if (needed.v[sup.v[y]][w] & ~needed.v[sup.v[x]][w]) sub = false;
                    if (sub) minimal = false;
                }
                if (minimal) add_parent(c, created.v[i], created.v[sup.v[x]]);
            }
            VEC_FREE(sup);
        }
        for (size_t k = 0; k < holders.n; k++) VEC_FREE(holders.v[k]);
        VEC_FREE(holders);
        u64map_free(&bitsets);
        for (size_t k = 0; k < needed.n; k++) free(needed.v[k]);
        VEC_FREE(created);
        VEC_FREE(needed);
        free(mark);
        VEC_FREE(anc);
    }
    qf_diag(c, true, noloc, "type hierarchy completion did not converge");
    return false;
}

/* --- building ------------------------------------------------------------------------------- */

static uint32_t inst_get(Front *c, Sym name)
{
    uint64_t v;
    if (u64map_get(&c->instidx, (uint64_t) (uintptr_t) name, &v)) return (uint32_t) v - 1;
    InstInfo in = { 0 };
    in.name = name;
    VEC_PUSH(c->insts, in);
    u64map_put(&c->instidx, (uint64_t) (uintptr_t) name, c->insts.n);
    return (uint32_t) c->insts.n - 1;
}

/* Where type t's own body declares a field, or its first declaration. */

/* --- disjunctions of structures ------------------------------------------------------------ */

static bool body_declares(const TypeInfo *ti, Sym lab)
{
    for (size_t k = 0; k < ti->decls.n; k++) {
        XPtrVec cj = { 0 };
        conjuncts(ti->decls.v[k]->value, &cj);
        bool found = false;
        for (size_t j = 0; j < cj.n && !found; j++)
            if (cj.v[j]->k == X_STRUCT)
                for (int f = 0; f < cj.v[j]->u.st.n && !found; f++) found = cj.v[j]->u.st.fields[f].label == lab;
        VEC_FREE(cj);
        if (found) return true;
    }
    return false;
}

/* How many alternatives of disjunction d declare a field. */
static int alt_siblings_declaring(Front *c, uint32_t d, Sym lab)
{
    int n = 0;
    for (size_t k = 0; k < c->types.v[d].children.n; k++) {
        const TypeInfo *ch = &c->types.v[c->types.v[d].children.v[k]];
        if (ch->kind == TK_ALT && ch->parents.v[0] == d && body_declares(ch, lab)) n++;
    }
    return n;
}

/* The types an alternative is known to lie below: its named conjuncts. A
 * structure in an alternative declares its own fields. It does not get a
 * supertype from a named type that declares the same field (ADR 0017). */
static void alt_bases(Front *c, X *a, U32Vec *out)
{
    XPtrVec cj = { 0 };
    conjuncts(a, &cj);
    for (size_t j = 0; j < cj.n; j++) {
        X *x = cj.v[j];
        if (x->k == X_NAME || x->k == X_ATOM) {
            uint32_t t = type_lookup(c, x->u.name);
            if (t == QF_BOTTOM) qf_diag(c, true, x->loc, "unknown name %s", x->u.name);
            else VEC_PUSH(*out, t);
        } else if (x->k != X_STRUCT && x->k != X_TOP) {
            qf_diag(c, true, x->loc, "an alternative may conjoin only names and structures");
        }
    }
    VEC_FREE(cj);
}

/* Each disjunction of structures in a type body becomes a type (anonymous, or
 * the named type it defines) with one anonymous subtype per alternative. An
 * alternative's type lies below the disjunction and below its own bases; the
 * disjunction lies below the minimal common named ancestors of the bases. */
Sym list_type_name(Front *c, const X *elem, bool cons)
{
    char *t = x_text(elem);
    Buf b = { 0 };
    if (cons) qf_buf_printf(&b, "[%s, ...%s]", t, t);
    else qf_buf_printf(&b, "[...%s]", t);
    Sym s = qf_sym(c, b.s);
    qf_buf_free(&b);
    free(t);
    return s;
}

/* A typed open list [...X] gets two types: the list `[...X]` below #List,
 * and its cell `[X, ...X]` below #Cons and the list, whose first is X and
 * whose rest is the list again. #Null is below every typed list. */
static void make_list_types(Front *c)
{
    uint32_t tlist = type_lookup(c, qf_sym(c, "#List"));
    uint32_t tcons = type_lookup(c, qf_sym(c, "#Cons"));
    uint32_t tnull = type_lookup(c, qf_sym(c, "#Null"));
    if (tlist == QF_BOTTOM || tcons == QF_BOTTOM || tnull == QF_BOTTOM) return;
    /* in name order, so #Null's parents do not depend on declaration order */
    for (size_t i = 1; i < c->listelems.n; i++)
        for (size_t j = i; j > 0 && strcmp(list_type_name(c, c->listelems.v[j - 1], false),
                                           list_type_name(c, c->listelems.v[j], false)) > 0; j--) {
            X *x = c->listelems.v[j]; c->listelems.v[j] = c->listelems.v[j - 1]; c->listelems.v[j - 1] = x;
        }
    for (size_t i = 0; i < c->listelems.n; i++) {
        X *elem = c->listelems.v[i];
        Sym ln = list_type_name(c, elem, false);
        if (type_lookup(c, ln) != QF_BOTTOM) continue;
        uint32_t lt = type_add(c, ln, TK_DEF);
        add_parent(c, lt, tlist);
        add_parent(c, tnull, lt);
        uint32_t ct = type_add(c, list_type_name(c, elem, true), TK_DEF);
        add_parent(c, ct, tcons);
        add_parent(c, ct, lt);
        X *body = x_new(c, X_STRUCT, elem->loc);
        body->u.st.n = 2;
        body->u.st.fields = qf_alloc(c->arena, 2 * sizeof(XField));
        body->u.st.fields[0] = (XField) { qf_sym(c, "first"), elem, NULL, 0, elem->loc, NULL };
        body->u.st.fields[1] = (XField) { qf_sym(c, "rest"), x_name(c, ln, elem->loc), NULL, 0, elem->loc, NULL };
        Decl *sd = qf_alloc(c->arena, sizeof(Decl));
        sd->label = c->types.v[ct].name;
        sd->kind = L_DEF;
        sd->value = body;
        sd->loc = elem->loc;
        VEC_PUSH(c->types.v[ct].decls, sd);
    }
}

static void make_disjunctions(Front *c)
{
    if (!c->disjs.n) return;
    size_t nbefore = c->types.n;
    for (size_t i = 0; i < c->disjs.n; i++) {
        DisjInfo *d = &c->disjs.v[i];
        size_t na = d->altx.n;
        U32Vec *bases = qf_xcalloc(na, sizeof(U32Vec));
        bool simple = true;               /* every alternative is a bare name or atom */
        for (size_t k = 0; k < na; k++) {
            alt_bases(c, d->altx.v[k], &bases[k]);
            if (d->altx.v[k]->k != X_NAME && d->altx.v[k]->k != X_ATOM) simple = false;
        }
        if (simple && !d->owner) {
            for (size_t k = 0; k < na && !d->collapse; k++) {
                bool covers = bases[k].n == 1;
                for (size_t j = 0; j < na && covers; j++)
                    covers = bases[j].n == 1 && is_subtype(c, bases[j].v[0], bases[k].v[0]);
                if (covers) d->collapse = c->types.v[bases[k].v[0]].name;
            }
        }
        if (!d->collapse) {
            /* the disjunction's own type */
            char **texts = qf_xmalloc(sizeof(char *) * na);
            for (size_t k = 0; k < na; k++) texts[k] = x_text(d->altx.v[k]);
            uint32_t dt;
            if (d->owner) {
                dt = type_lookup(c, d->owner);
                d->dname = d->owner;
            } else {
                char **sorted = qf_xmalloc(sizeof(char *) * na);
                memcpy(sorted, texts, sizeof(char *) * na);
                qsort(sorted, na, sizeof(char *), cmp_sym);
                Buf nb = { 0 };
                qf_buf_putc(&nb, '(');
                for (size_t k = 0; k < na; k++) {
                    if (k && strcmp(sorted[k], sorted[k - 1]) == 0) continue;
                    if (nb.len > 1) qf_buf_puts(&nb, " | ");
                    qf_buf_puts(&nb, sorted[k]);
                }
                qf_buf_putc(&nb, ')');
                d->dname = qf_sym(c, nb.s);
                qf_buf_free(&nb);
                free(sorted);
                dt = type_lookup(c, d->dname);
                if (dt == QF_BOTTOM) dt = type_add(c, d->dname, TK_DISJ);
            }
            bool fresh = !c->types.v[dt].isdisj;
            c->types.v[dt].isdisj = true;
            if (fresh) {
                /* parents: minimal common named ancestors of every alternative's bases */
                U32Vec cands = { 0 };
                for (uint32_t u = 0; u < nbefore; u++) {
                    TypeKind uk = c->types.v[u].kind;
                    if (u == dt || uk == TK_ALT || uk == TK_DISJ || uk == TK_ANON || uk == TK_ATOM) continue;
                    if (d->owner && is_subtype(c, u, dt)) continue;
                    bool all = true;
                    for (size_t k = 0; k < na && all; k++) {
                        bool any = !bases[k].n && u == c->top;
                        for (size_t q = 0; q < bases[k].n && !any; q++) any = is_subtype(c, bases[k].v[q], u);
                        all = any;
                    }
                    if (all) VEC_PUSH(cands, u);
                }
                SymVec pn = { 0 };
                for (size_t a = 0; a < cands.n; a++) {
                    bool minimal = true;
                    for (size_t b = 0; b < cands.n && minimal; b++)
                        if (a != b && is_subtype(c, cands.v[b], cands.v[a])) minimal = false;
                    if (minimal) VEC_PUSH(pn, c->types.v[cands.v[a]].name);
                }
                qsort(pn.v, pn.n, sizeof(Sym), cmp_sym);
                c->types.v[dt].parents.n = 0;
                for (size_t a = 0; a < pn.n; a++) add_parent(c, dt, type_lookup(c, pn.v[a]));
                if (!pn.n) add_parent(c, dt, c->top);
                VEC_FREE(cands);
                VEC_FREE(pn);
            }
            /* one type per alternative */
            for (size_t k = 0; k < na; k++) {
                Buf nb = { 0 };
                qf_buf_printf(&nb, "%s/%s", d->dname, texts[k]);
                Sym an = qf_sym(c, nb.s);
                qf_buf_free(&nb);
                VEC_PUSH(d->alts, an);
                if (type_lookup(c, an) != QF_BOTTOM) continue;
                uint32_t at = type_add(c, an, TK_ALT);
                add_parent(c, at, dt);
                for (size_t q = 0; q < bases[k].n; q++) add_parent(c, at, bases[k].v[q]);
                Decl *sd = qf_alloc(c->arena, sizeof(Decl));
                sd->label = an;
                sd->kind = L_DEF;
                sd->value = d->altx.v[k];
                sd->loc = d->altx.v[k]->loc;
                VEC_PUSH(c->types.v[at].decls, sd);
            }
            for (size_t k = 0; k < na; k++) free(texts[k]);
            free(texts);
        }
        for (size_t k = 0; k < na; k++) VEC_FREE(bases[k]);
        free(bases);
    }
}

typedef VEC(DeclPair) DeclPairs;
typedef struct { Sym label; Sym pkg; U32Vec types; } IntroGroup;
typedef VEC(IntroGroup) IntroGroups;

/* Each label that the body of a type declares, in `fnames`, and for each
 * label the types that declare it, with the package of the declaring file. */
static DeclPairs *label_declarers(Front *c, SymVec *fnames_out)
{
    /* features: introduced by the top-level fields of each type's own body */
    SymVec fnames = { 0 };
    VEC(DeclPair) declpairs = { 0 };      /* a type declaring a label, in a package's file */
    U64Map labidx = { 0 };                /* label Sym → index into fnames + 1 */
    U64Map altonly = { 0 };               /* labels only alternatives declare */
    /* named types first; then alternatives, where a field several alternatives
     * of one disjunction declare (and no named type does) is the disjunction's */
    for (int pass = 0; pass < 2; pass++)
    for (uint32_t t = 0; t < c->types.n; t++) {
        TypeInfo *ti = &c->types.v[t];
        if (ti->kind != (pass ? TK_ALT : TK_DEF)) continue;
        for (size_t k = 0; k < ti->decls.n; k++) {
            XPtrVec cj = { 0 };
            conjuncts(ti->decls.v[k]->value, &cj);
            for (size_t j = 0; j < cj.n; j++) {
                X *x = cj.v[j];
                if (x->k != X_STRUCT || x->u.st.refs) continue;     /* a reference's place introduces nothing */
                if (pass) {
                    for (int f = 0; f < x->u.st.n; f++) {
                        Sym lab = x->u.st.fields[f].label;
                        uint64_t li;
                        if (strchr(lab, '.')) continue;     /* pkg.label narrows that package's */
                        uint32_t declarer = t;
                        if (!u64map_get(&labidx, (uint64_t) (uintptr_t) lab, &li)) {
                            uint32_t d = ti->parents.v[0];
                            if (alt_siblings_declaring(c, d, lab) >= 2) declarer = d;
                            VEC_PUSH(fnames, lab);
                            li = fnames.n;
                            u64map_put(&labidx, (uint64_t) (uintptr_t) lab, li);
                            u64map_put(&altonly, (uint64_t) (uintptr_t) lab, 1);
                        } else if (u64map_get(&altonly, (uint64_t) (uintptr_t) lab, NULL) &&
                                   alt_siblings_declaring(c, ti->parents.v[0], lab) >= 2) {
                            declarer = ti->parents.v[0];
                        }
                        DeclPair dp = { declarer, (uint32_t) (li - 1), ti->decls.v[k]->pkg };
                        VEC_PUSH(declpairs, dp);
                    }
                    continue;
                }
                for (int f = 0; f < x->u.st.n; f++) {
                    Sym lab = x->u.st.fields[f].label;
                    uint64_t li;
                    if (strchr(lab, '.')) continue;         /* pkg.label narrows that package's */
                    if (!u64map_get(&labidx, (uint64_t) (uintptr_t) lab, &li)) {
                        VEC_PUSH(fnames, lab);
                        li = fnames.n;
                        u64map_put(&labidx, (uint64_t) (uintptr_t) lab, li);
                    }
                    DeclPair dp = { t, (uint32_t) (li - 1), ti->decls.v[k]->pkg };
                    VEC_PUSH(declpairs, dp);
                }
            }
            VEC_FREE(cj);
        }
    }
    /* each label's declarers, by type and the package of the file that
     * declares it there; the most general of them in a package introduces
     * the label's feature there (CONTENT_LANGUAGE.md decision 12) */
    DeclPairs *declarers = qf_xcalloc(fnames.n ? fnames.n : 1, sizeof *declarers);
    for (size_t i = 0; i < declpairs.n; i++) {
        DeclPair dp = declpairs.v[i];
        bool dup = false;
        for (size_t q = 0; q < declarers[dp.label].n && !dup; q++)
            dup = declarers[dp.label].v[q].type == dp.type && declarers[dp.label].v[q].pkg == dp.pkg;
        if (!dup) VEC_PUSH(declarers[dp.label], dp);
    }
    VEC_FREE(declpairs);
    u64map_free(&labidx);
    u64map_free(&altonly);
    *fnames_out = fnames;
    return declarers;
}

/* For each label and each package: the types that declare the label there
 * and have no supertype that declares it. They introduce the feature. There
 * can be several, because unrelated types can each have a field with the
 * same name (ADR 0017). */
static void intro_groups(Front *c, IntroGroups *out)
{
    SymVec fnames = { 0 };
    DeclPairs *declarers = label_declarers(c, &fnames);
    for (size_t li = 0; li < fnames.n; li++) {
        DeclPair *d = declarers[li].v;
        size_t nd = declarers[li].n;
        /* a declaration narrows the field when a supertype declares it, or
         * the type's own package does where another package adds to it */
        bool *narrows = qf_xcalloc(nd ? nd : 1, sizeof(bool));
        for (size_t i = 0; i < nd; i++)
            for (size_t j = 0; j < nd && !narrows[i]; j++) {
                if (i == j) continue;
                if (d[j].type != d[i].type) narrows[i] = is_subtype(c, d[i].type, d[j].type);
                else narrows[i] = d[j].pkg == type_pkg(c, d[i].type) && d[i].pkg != d[j].pkg;
            }
        bool *taken = qf_xcalloc(nd ? nd : 1, sizeof(bool));
        for (size_t i = 0; i < nd; i++) {
            if (taken[i] || narrows[i]) continue;
            IntroGroup g = { fnames.v[li], d[i].pkg, { 0 } };
            for (size_t j = i; j < nd; j++)
                if (!taken[j] && !narrows[j] && d[j].pkg == g.pkg) {
                    taken[j] = true;
                    bool seen = false;
                    for (size_t q = 0; q < g.types.n && !seen; q++) seen = g.types.v[q] == d[j].type;
                    if (!seen) VEC_PUSH(g.types, d[j].type);
                }
            VEC_PUSH(*out, g);
        }
        free(taken);
        free(narrows);
        VEC_FREE(declarers[li]);
    }
    free(declarers);
    VEC_FREE(fnames);
}

bool types_build(Front *c)
{
    size_t errors0 = c->nerrors;

    /* builtins */
    c->top = type_add(c, qf_sym(c, "_"), TK_TOP);
    c->atomroot = type_add(c, qf_sym(c, "atom"), TK_ATOMROOT);
    add_parent(c, c->atomroot, c->top);
    /* number, with the dimension types and the kinds below it (ADR 0015
     * section 2); text holds free strings; a constraint is a comparison that
     * stands as a value */
    c->tnumber = type_add(c, qf_sym(c, "number"), TK_NUMBER);
    add_parent(c, c->tnumber, c->top);
    add_parent(c, type_add(c, qf_sym(c, "text"), TK_TEXT), c->top);
    add_parent(c, type_add(c, qf_sym(c, "constraint"), TK_CONSTRAINT), c->top);
    for (uint32_t k = 0; k < c->kinds.n; k++) {
        uint32_t t = type_add(c, c->kinds.v[k].name, TK_KIND);
        c->types.v[t].kindix = k;
        c->types.v[t].dimix = c->kinds.v[k].dimix;
        add_parent(c, t, qf_dim_type(c, c->kinds.v[k].dimix));
    }
    {
        /* bool is the enum of true and false */
        uint32_t tb = type_add(c, qf_sym(c, "bool"), TK_DEF);
        add_parent(c, tb, c->atomroot);
        add_parent(c, type_add(c, qf_sym(c, "false"), TK_ATOM), tb);
        add_parent(c, type_add(c, qf_sym(c, "true"), TK_ATOM), tb);
    }

    /* declarations */
    for (size_t i = 0; i < c->decls.n; i++) {
        Decl *d = &c->decls.v[i];
        if (d->kind == L_KIND) continue;       /* qf_kinds_build read it */
        if (d->kind == L_DEF && type_lookup(c, d->label) != QF_BOTTOM &&
            c->types.v[type_lookup(c, d->label)].kind != TK_DEF) {
            qf_diag(c, true, d->loc, "%s is a built-in type", d->label);
            continue;
        }
        if (d->kind == L_DEF || d->kind == L_ATOM) {
            uint32_t t = type_get(c, d->label, d->kind == L_DEF ? TK_DEF : TK_ATOM);
            VEC_PUSH(c->types.v[t].decls, d);
        } else {
            if (type_lookup(c, d->label) != QF_BOTTOM) {
                qf_diag(c, true, d->loc, "instance name %s is a built-in type", d->label);
                continue;
            }
            uint32_t in = inst_get(c, d->label);
            VEC_PUSH(c->insts.v[in].decls, d);
        }
    }
    for (size_t i = 0; i < c->decls.n; i++) {
        X *v = c->decls.v[i].value;
        if (c->decls.v[i].kind == L_KIND) continue;
        bool typebody = c->decls.v[i].kind != L_INST;
        if (typebody && v->k == X_OR && !anon_name(c, v) && c->decls.v[i].kind == L_DEF) {
            /* a named type defined as a disjunction of structures */
            register_disj(c, v, c->decls.v[i].label);
        } else if (typebody && v->k == X_OR) {
            /* an enum declaration lists its atoms; it is not an anonymous enum */
            XPtrVec ds = { 0 };
            disjuncts(v, &ds);
            for (size_t k = 0; k < ds.n; k++) scan_expr(c, ds.v[k], true);
            VEC_FREE(ds);
        } else {
            scan_expr(c, v, typebody);
        }
    }


    /* parents from declarations */
    size_t nnamed = c->types.n;
    SymVec *listers = qf_xcalloc(nnamed, sizeof *listers);   /* named enums listing each atom */
    bool *enumtype = qf_xcalloc(nnamed, 1);
    for (uint32_t t = 0; t < nnamed; t++) {
        TypeInfo *ti = &c->types.v[t];
        if (ti->kind != TK_DEF && ti->kind != TK_ATOM) continue;
        Loc ploc = noloc;
        bool have_parents = false;
        for (size_t k = 0; k < ti->decls.n; k++) {
            Decl *d = ti->decls.v[k];
            XPtrVec cj = { 0 };
            conjuncts(d->value, &cj);
            U32Vec parents = { 0 };
            for (size_t j = 0; j < cj.n; j++) {
                X *x = cj.v[j];
                if (x->k == X_NAME && ti->kind == TK_DEF) {
                    uint32_t p = type_lookup(c, x->u.name);
                    if (p == QF_BOTTOM || c->types.v[p].kind != TK_DEF) {
                        qf_diag(c, true, x->loc, "%s is not a type that %s can inherit from",
                             x->u.name, ti->name);
                        continue;
                    }
                    VEC_PUSH(parents, p);
                } else if (x->k == X_STRUCT && ti->kind == TK_DEF) {
                    /* body: handled when features are inferred */
                } else if (x->k == X_TOP && ti->kind == TK_DEF) {
                    /* _: the default parent */
                } else if (x->k == X_OR && u64map_get(&c->disjidx, (uint64_t) (uintptr_t) x, NULL)) {
                    /* a disjunction of structures: parents come from make_disjunctions */
                } else if (x->k == X_OR || x->k == X_ATOM) {
                    SymVec atoms = { 0 };
                    bool ok = true;
                    or_atoms(x, &atoms, &ok);
                    if (!ok) {
                        qf_diag(c, true, x->loc, "only atoms may be listed in the enum %s", ti->name);
                    } else {
                        enumtype[t] = true;
                        for (size_t a = 0; a < atoms.n; a++) {
                            uint32_t at = type_lookup(c, atoms.v[a]);
                            bool dup = false;
                            for (size_t q = 0; q < listers[at].n; q++)
                                if (listers[at].v[q] == ti->name) dup = true;
                            if (!dup) VEC_PUSH(listers[at], ti->name);
                        }
                    }
                    VEC_FREE(atoms);
                } else {
                    qf_diag(c, true, x->loc, "unsupported construct in the definition of %s", ti->name);
                }
            }
            if (parents.n) {
                if (have_parents) {
                    qf_diag(c, true, d->loc, "supertypes of %s are already declared at %s:%d; "
                         "additions may not add supertypes", ti->name, ploc.file, ploc.line);
                } else {
                    have_parents = true;
                    ploc = d->loc;
                    for (size_t j = 0; j < parents.n; j++) add_parent(c, t, parents.v[j]);
                }
            }
            VEC_FREE(parents);
            VEC_FREE(cj);
        }
    }
    for (uint32_t t = 0; t < nnamed; t++) {
        TypeInfo *ti = &c->types.v[t];
        if (ti->kind == TK_ATOM && ti->parents.n) {
            /* a built-in atom */
        } else if (ti->kind == TK_ATOM) {
            if (listers[t].n) {
                qsort(listers[t].v, listers[t].n, sizeof(Sym), cmp_sym);
                for (size_t k = 0; k < listers[t].n; k++) add_parent(c, t, type_lookup(c, listers[t].v[k]));
            } else {
                /* a string no enum lists: free text, built as a text value */
                add_parent(c, t, c->atomroot);
            }
        } else if (ti->kind == TK_DEF && !ti->parents.n) {
            add_parent(c, t, enumtype[t] ? c->atomroot : c->top);
        }
        VEC_FREE(listers[t]);
    }
    free(listers);
    free(enumtype);
    if (c->nerrors > errors0 || has_cycle(c)) return false;

    /* conjunction types: content may conjoin unrelated types (mixins), so each
     * such conjunction gets a generated common subtype */
    compute_codes(c);
    {
        SymSets sets = { 0 };
        for (size_t i = 0; i < c->decls.n; i++)
            conj_sets(c, c->decls.v[i].value, true, &sets);   /* a type's own chain is the type */
        for (size_t i = 0; i < c->insts.n; i++) {     /* an instance's root, across additions */
            SymVec root = { 0 };
            for (size_t k = 0; k < c->insts.v[i].decls.n; k++) {
                XPtrVec cj = { 0 };
                conjuncts(c->insts.v[i].decls.v[k]->value, &cj);
                for (size_t j = 0; j < cj.n; j++)
                    if (cj.v[j]->k == X_NAME) {
                        uint32_t t = type_lookup(c, cj.v[j]->u.name);
                        if (t != QF_BOTTOM && c->types.v[t].kind == TK_DEF) VEC_PUSH(root, cj.v[j]->u.name);
                    }
                VEC_FREE(cj);
            }
            VEC_PUSH(sets, root);
        }
        for (size_t i = 0; i < sets.n; i++) {
            SymVec *s = &sets.v[i];
            qsort(s->v, s->n, sizeof(Sym), cmp_sym);
            size_t k = 0;
            for (size_t j = 0; j < s->n; j++) if (!k || s->v[k - 1] != s->v[j]) s->v[k++] = s->v[j];
            s->n = k;
            bool incomparable = false;
            for (size_t a = 0; a < s->n; a++)
                for (size_t b = a + 1; b < s->n; b++) {
                    uint32_t ta = type_lookup(c, s->v[a]), tb = type_lookup(c, s->v[b]);
                    if (!is_subtype(c, ta, tb) && !is_subtype(c, tb, ta)) incomparable = true;
                }
            if (incomparable) {
                Buf nb = { 0 };
                for (size_t j = 0; j < s->n; j++) {
                    if (j) qf_buf_puts(&nb, " & ");
                    qf_buf_puts(&nb, s->v[j]);
                }
                Sym name = qf_sym(c, nb.s);
                qf_buf_free(&nb);
                if (type_lookup(c, name) == QF_BOTTOM) {
                    uint32_t g = type_add(c, name, TK_GLB);
                    for (size_t j = 0; j < s->n; j++) add_parent(c, g, type_lookup(c, s->v[j]));
                }
            }
            VEC_FREE(*s);
        }
        VEC_FREE(sets);
    }

    compute_codes(c);
    make_disjunctions(c);
    make_list_types(c);

    /* anonymous enums: parents are the minimal common named ancestors of
     * members. They are taken in name order, so that an atom's parents (and
     * its primary parent) do not depend on the order of declarations. */
    compute_codes(c);
    U32Vec anons = { 0 };
    for (uint32_t t = 0; t < c->types.n; t++) if (c->types.v[t].kind == TK_ANON) VEC_PUSH(anons, t);
    for (size_t i = 1; i < anons.n; i++)
        for (size_t j = i; j > 0 && strcmp(c->types.v[anons.v[j - 1]].name, c->types.v[anons.v[j]].name) > 0; j--) {
            uint32_t x = anons.v[j]; anons.v[j] = anons.v[j - 1]; anons.v[j - 1] = x;
        }
    for (size_t ai = 0; ai < anons.n; ai++) {
        uint32_t t = anons.v[ai];
        TypeInfo *ti = &c->types.v[t];
        U32Vec cands = { 0 };
        for (uint32_t u = 0; u < nnamed; u++) {
            bool all = true;
            for (size_t k = 0; k < ti->members.n && all; k++)
                if (!is_subtype(c, type_lookup(c, ti->members.v[k]), u)) all = false;
            if (all) VEC_PUSH(cands, u);
        }
        SymVec pn = { 0 };
        for (size_t i = 0; i < cands.n; i++) {
            bool minimal = true;
            for (size_t j = 0; j < cands.n && minimal; j++)
                if (i != j && is_subtype(c, cands.v[j], cands.v[i])) minimal = false;
            if (minimal) VEC_PUSH(pn, c->types.v[cands.v[i]].name);
        }
        qsort(pn.v, pn.n, sizeof(Sym), cmp_sym);
        for (size_t i = 0; i < pn.n; i++) add_parent(c, t, type_lookup(c, pn.v[i]));
        for (size_t k = 0; k < ti->members.n; k++) add_parent(c, type_lookup(c, ti->members.v[k]), t);
        VEC_FREE(cands);
        VEC_FREE(pn);
    }
    VEC_FREE(anons);

    if (!complete(c)) return false;
    /* generated meet types are added as parents in the order they were made,
     * which follows declaration order: put them after the declared parents,
     * in name order */
    for (uint32_t t = 0; t < c->types.n; t++) {
        U32Vec *ps = &c->types.v[t].parents;
        size_t k = 0;
        for (size_t i = 0; i < ps->n; i++)
            if (c->types.v[ps->v[i]].kind != TK_GLB) ps->v[k++] = ps->v[i];
        size_t first_glb = k;
        if (first_glb == ps->n) continue;
        U32Vec glbs = { 0 };
        for (size_t i = 0; i < ps->n; i++)
            if (c->types.v[ps->v[i]].kind == TK_GLB) VEC_PUSH(glbs, ps->v[i]);
        for (size_t i = 1; i < glbs.n; i++)
            for (size_t j = i; j > 0 && strcmp(c->types.v[glbs.v[j - 1]].name, c->types.v[glbs.v[j]].name) > 0; j--) {
                uint32_t x = glbs.v[j]; glbs.v[j] = glbs.v[j - 1]; glbs.v[j - 1] = x;
            }
        for (size_t i = 0; i < glbs.n; i++) ps->v[k++] = glbs.v[i];
        VEC_FREE(glbs);
    }
    renumber(c);
    compute_codes(c);
    index_codes(c);
    meet_cache_clear(c);
    c->top = type_lookup(c, qf_sym(c, "_"));
    c->atomroot = type_lookup(c, qf_sym(c, "atom"));
    c->tnumber = type_lookup(c, qf_sym(c, "number"));
    c->ttext = type_lookup(c, qf_sym(c, "text"));
    c->tconstraint = type_lookup(c, qf_sym(c, "constraint"));
    for (uint32_t d = 0; d < c->dims.n; d++) c->dims.v[d].type = type_lookup(c, c->dims.v[d].name);
    for (uint32_t k = 0; k < c->kinds.n; k++) c->kinds.v[k].type = type_lookup(c, c->kinds.v[k].name);
    c->tbool = type_lookup(c, qf_sym(c, "bool"));

    /* features: the top-level fields of the body of a type declare them */
    IntroGroups groups = { 0 };
    intro_groups(c, &groups);
    VEC(NewFeat) made = { 0 };
    for (size_t gi = 0; gi < groups.n; gi++) {
        IntroGroup *g = &groups.v[gi];
        for (size_t i = 1; i < g->types.n; i++)          /* ascending */
            for (size_t j = i; j > 0 && g->types.v[j - 1] > g->types.v[j]; j--) {
                uint32_t x = g->types.v[j]; g->types.v[j] = g->types.v[j - 1]; g->types.v[j - 1] = x;
            }
        NewFeat nf = { g->label, g->pkg, g->types };     /* the feature owns the list now */
        VEC_PUSH(made, nf);
    }
    VEC_FREE(groups);
    /* numbered by label, then package, the root's first */
    qsort(made.v, made.n, sizeof(NewFeat), cmp_newfeat);
    for (size_t i = 0; i < made.n; i++) {
        FeatInfo fi = { 0 };
        fi.label = made.v[i].label;
        fi.pkg = made.v[i].pkg;
        fi.intros = made.v[i].intros;
        fi.full = fi.pkg ? qualified_name(c, fi.pkg, fi.label) : fi.label;
        VEC_PUSH(c->feats, fi);
        u64map_put(&c->featidx, (uint64_t) (uintptr_t) fi.full, i + 1);
        if (!i || made.v[i - 1].label != fi.label) u64map_put(&c->labelidx, (uint64_t) (uintptr_t) fi.label, i + 1);
    }
    /* each prints as its label where the label alone finds it: the root's, or
     * the only one */
    for (size_t i = 0; i < c->feats.n; i++) {
        FeatInfo *fi = &c->feats.v[i];
        bool alone = (!i || c->feats.v[i - 1].label != fi->label) &&
                     (i + 1 == c->feats.n || c->feats.v[i + 1].label != fi->label);
        bool plain = alone || !fi->pkg;
        fi->name = plain ? fi->label : fi->full;
        if (plain) u64map_put(&c->featidx, (uint64_t) (uintptr_t) fi->label, i + 1);
    }
    VEC_FREE(made);
    if (c->nerrors > errors0) return false;

    for (uint32_t t = 0; t < c->types.n; t++)
        for (uint32_t f = 0; f < c->feats.n; f++)
            if (below_intro(c, t, f)) VEC_PUSH(c->types.v[t].feats, f);

    /* the build log: every type the loader made, in ID order */
    for (uint32_t t = 0; t < c->types.n; t++) {
        TypeInfo *ti = &c->types.v[t];
        const char *what = ti->kind == TK_GLB ? (strchr(ti->name, '&') && strncmp(ti->name, "glb[", 4) ? "conjunction type"
                                                                                                    : "meet type")
                         : ti->kind == TK_DISJ ? "disjunction type" : ti->kind == TK_ALT ? "alternative type"
                         : ti->kind == TK_ANON ? "anonymous enum" : NULL;
        if (!what && ti->kind == TK_DEF && ti->name[0] == '[') what = "typed list type";
        if (!what) continue;
        Buf b = { 0 };
        qf_buf_printf(&b, "%s %s, below ", what, ti->name);
        for (size_t k = 0; k < ti->parents.n; k++) qf_buf_printf(&b, "%s%s", k ? ", " : "", c->types.v[ti->parents.v[k]].name);
        VEC_PUSH(c->buildlog, b.s);
    }
    c->tlist = type_lookup(c, qf_sym(c, "#List"));
    c->tcons = type_lookup(c, qf_sym(c, "#Cons"));
    c->tnull = type_lookup(c, qf_sym(c, "#Null"));
    c->ffirst = feat_lookup(c, qf_sym(c, "first"));
    c->frest = feat_lookup(c, qf_sym(c, "rest"));
    return c->nerrors == errors0;
}
