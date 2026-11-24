#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "front/front.h"

/* Comprehensions, computed labels and built-in functions (quad-ijx.9).
 *
 * A comprehension is expanded when content loads, into what it stands for:
 * the items of a list, or the fields of a struct. Its sources and conditions
 * must be known by then: data written in the content (a list, a struct, the
 * value of a let, a field of this or of another declaration), or the atoms
 * of an enum. A function call becomes the value that it gives. The rest of
 * the front end never sees a comprehension or a call.
 *
 * The method and most of the code are Triangle's (load.c and builtins.c,
 * ADR 0003). Two things are different here:
 *   - A number is an exact decimal with a unit (ADR 0015). The arithmetic of
 *     this file is exact, and it refuses a result that is not.
 *   - A comparison in the place of a value is a law (ADR 0016). It becomes
 *     true or false here only if both sides are known when content loads.
 *     The same holds for an arithmetic of two numbers: 2 * 12 is 24. */

__extension__ typedef __int128 i128;

typedef struct Env { const CompVar *var; X *value; const struct Env *up; } Env;
typedef struct { Front *f; X *root; bool ok; size_t made; int refdepth; } Exp;
typedef VEC(XField) FieldVec;

enum { EXPAND_MAX = 100000 };      /* the items and fields that the comprehensions of one declaration can make */

static void exp_error(Exp *e, Loc loc, const char *msg)
{
    if (e->ok) qf_diag(e->f, true, loc, "%s", msg);
    e->ok = false;
}

/* --- numbers known when content loads ------------------------------------------------------ */

/* v × 10^e in the coherent unit of the dimension, with no factor of ten in v. */
typedef struct { QDim dim; int64_t v; int e; } Num;

static Num num_norm(Num n)
{
    if (!n.v) n.e = 0;
    while (n.v && n.v % 10 == 0) { n.v /= 10; n.e++; }
    return n;
}

static bool num_of(const Front *f, const X *x, Num *out)
{
    char err[400];
    if (!x || x->k != X_NUM || !qf_literal(f, &x->u.num, &out->dim, &out->v, &out->e, err, sizeof err)) return false;
    *out = num_norm(*out);
    return true;
}

/* a and b with one exponent. False if a value does not fit. */
static bool num_align(Num *a, Num *b)
{
    while (a->e > b->e) { if (__builtin_mul_overflow(a->v, (int64_t) 10, &a->v)) return false; a->e--; }
    while (b->e > a->e) { if (__builtin_mul_overflow(b->v, (int64_t) 10, &b->v)) return false; b->e--; }
    return true;
}

/* -1, 0 or 1. The two numbers have one dimension. */
static int num_cmp(Num a, Num b)
{
    if (!a.v || !b.v || (a.v < 0) != (b.v < 0)) return (a.v > b.v) - (a.v < b.v);
    /* the same sign: compare the magnitudes, with the smaller exponent for both */
    i128 x = a.v, y = b.v;
    for (int k = a.e; k > b.e && x < (i128) 1 << 100 && x > -((i128) 1 << 100); k--) x *= 10;
    for (int k = b.e; k > a.e && y < (i128) 1 << 100 && y > -((i128) 1 << 100); k--) y *= 10;
    return (x > y) - (x < y);
}

/* The text of a unit for a dimension, which qf_literal reads back: m^2*kg*s^-2. NULL for no dimension. */
static Sym dim_unit(Front *f, const QDim *d)
{
    if (qdim_is_none(d)) return NULL;
    Buf b = { 0 };
    qf_dim_format(f, &b, d);
    for (size_t i = 0; i < b.len; i++)
        if (b.s[i] == ' ') b.s[i] = '*';
    Sym s = qf_sym(f, b.s);
    qf_buf_free(&b);
    return s;
}

static X *num_x(Front *f, Num n, Loc loc)
{
    n = num_norm(n);
    X *x = x_new(f, X_NUM, loc);
    x->u.num.mant = n.v;
    x->u.num.frac = -n.e;
    x->u.num.unit = dim_unit(f, &n.dim);
    return x;
}

static X *int_x(Front *f, int64_t v, Loc loc)
{
    Num n = { qdim_none(), v, 0 };
    return num_x(f, n, loc);
}

/* a op b, exactly. False if the dimensions do not fit the operator, or the result is not exact or does not fit. */
static bool num_arith(int op, Num a, Num b, Num *out)
{
    Num r = a;
    if (op == '+' || op == '-') {
        if (!qdim_eq(&a.dim, &b.dim) || !num_align(&a, &b)) return false;
        r.e = a.e;
        if (op == '+' ? __builtin_add_overflow(a.v, b.v, &r.v) : __builtin_sub_overflow(a.v, b.v, &r.v)) return false;
    } else if (op == '*') {
        if (qdim_mul(&a.dim, &b.dim, &r.dim) != Q_OK || __builtin_mul_overflow(a.v, b.v, &r.v)) return false;
        r.e = a.e + b.e;
    } else if (op == '/') {
        if (!b.v || qdim_div(&a.dim, &b.dim, &r.dim) != Q_OK) return false;
        i128 n = a.v;
        int k = 0;
        while (n % b.v && k < 18) { n *= 10; k++; }
        if (n % b.v || n / b.v > INT64_MAX || n / b.v < INT64_MIN) return false;       /* 1 / 3 has no exact decimal */
        r.v = (int64_t) (n / b.v);
        r.e = a.e - b.e - k;
    } else {
        return false;
    }
    *out = num_norm(r);
    return true;
}

static bool num_whole(Num n, int64_t *out)
{
    if (!qdim_is_none(&n.dim) || n.e < 0) return false;
    int64_t v = n.v;
    for (int k = 0; k < n.e; k++)
        if (__builtin_mul_overflow(v, (int64_t) 10, &v)) return false;
    *out = v;
    return true;
}

/* --- data written in the content ------------------------------------------------------------ */

static void build_declidx(Front *f)
{
    if (f->declidx_built) return;
    for (size_t i = 0; i < f->decls.n; i++)
        if (!u64map_get(&f->declidx, (uint64_t) (uintptr_t) f->decls.v[i].label, NULL))
            u64map_put(&f->declidx, (uint64_t) (uintptr_t) f->decls.v[i].label, i + 1);
    f->declidx_built = true;
}

/* A field written in a literal: through conjunctions, and not in the structs that place references. */
static X *data_field(X *x, Sym label)
{
    if (!x) return NULL;
    if (x->k == X_AND) {
        X *a = data_field(x->u.bin.a, label);
        return a ? a : data_field(x->u.bin.b, label);
    }
    if (x->k != X_STRUCT || x->u.st.refs) return NULL;
    for (int i = 0; i < x->u.st.n; i++)
        if (x->u.st.fields[i].label == label) return x->u.st.fields[i].value;
    return NULL;
}

static X *data_item(X *x, long k)
{
    if (!x) return NULL;
    if (x->k == X_AND) {
        X *a = data_item(x->u.bin.a, k);
        return a ? a : data_item(x->u.bin.b, k);
    }
    return x->k == X_LIST && k >= 0 && k < x->u.list.n ? x->u.list.items[k] : NULL;
}

static X *data_of(Exp *e, X *x, int depth);
static bool expand_decl(Front *f, Decl *d, Loc from);
static X *expand(Exp *e, X *x, const Env *env);

/* The literal at a path, ".a[0].b" or "a[0].b", in x. */
static X *data_at(Exp *e, X *x, const char *path, int depth)
{
    const char *s = path;
    while (x && *s) {
        x = data_of(e, x, depth + 1);
        if (*s == '.') s++;
        if (*s == '[') {
            long k = strtol(s + 1, NULL, 10);
            while (*s && *s != ']') s++;
            if (*s) s++;
            x = data_item(x, k);
        } else {
            size_t n = strcspn(s, ".[");
            x = data_field(x, qf_intern(e->f, s, n));
            s += n;
        }
    }
    return x;
}

/* The atoms that an enum lists, as a list. NULL if the declaration is not an enum. */
static X *enum_list(Exp *e, const Decl *d, Loc loc)
{
    if (d->kind != L_DEF && d->kind != L_ATOM) return NULL;
    XPtrVec ds = { 0 };
    x_flatten(d->value, X_OR, &ds);
    bool atoms = ds.n > 1;
    for (size_t i = 0; i < ds.n && atoms; i++) {
        X *a = ds.v[i]->k == X_DEFAULT ? ds.v[i]->u.sub : ds.v[i];
        if (a->k != X_ATOM) atoms = false;
        ds.v[i] = a;
    }
    X *l = NULL;
    if (atoms) {
        l = x_new(e->f, X_LIST, loc);
        l->u.list.n = (int) ds.n;
        l->u.list.items = qf_alloc(e->f->arena, ds.n * sizeof(X *));
        memcpy(l->u.list.items, ds.v, ds.n * sizeof(X *));
    }
    VEC_FREE(ds);
    return l;
}

/* A tag or a reference anywhere in x, or NULL. */
static const char *x_tag_in(const X *x)
{
    if (!x) return NULL;
    const char *t = NULL;
    switch (x->k) {
    case X_TAG: return x->u.name;
    case X_STRUCT:
        for (int i = 0; i < x->u.st.n && !t; i++) t = x_tag_in(x->u.st.fields[i].value);
        return t;
    case X_LIST:
        for (int i = 0; i < x->u.list.n && !t; i++) t = x_tag_in(x->u.list.items[i]);
        return t ? t : x_tag_in(x->u.list.elem);
    case X_AND: case X_OR: return (t = x_tag_in(x->u.bin.a)) ? t : x_tag_in(x->u.bin.b);
    case X_ARITH: case X_CMP: case X_LOGIC: return (t = x_tag_in(x->u.ar.a)) ? t : x_tag_in(x->u.ar.b);
    case X_CALL:
        for (int i = 0; i < x->u.call.n && !t; i++) t = x_tag_in(x->u.call.args[i]);
        return t;
    case X_DEFAULT: case X_NOT: return x_tag_in(x->u.sub);
    case X_INTERP:
        for (int i = 0; i < x->u.in.n && !t; i++) t = x_tag_in(x->u.in.parts[i]);
        return t;
    default: return NULL;
    }
}

/* What x stands for as data: the literal of a reference or of a name, the atoms of an enum, or x itself. */
static X *data_of(Exp *e, X *x, int depth)
{
    if (!x || depth > 64) return x;
    if (x->k == X_TAG && x->u.name[0] == '.') {
        X *v = data_at(e, e->root, x->u.name, depth + 1);
        if (!v) {
            char msg[300];
            snprintf(msg, sizeof msg, "%s is not written in this declaration: a comprehension, a function or an "
                     "interpolation reads only values that the content writes", x->u.name);
            exp_error(e, x->loc, msg);
            return x;
        }
        /* the field as it will be: its own comprehensions expanded */
        if (++e->refdepth > 32) {
            exp_error(e, x->loc, "the references that comprehensions read go around in a circle");
            return x;
        }
        v = expand(e, v, NULL);
        e->refdepth--;
        return data_of(e, v, depth + 1);
    }
    if (x->k == X_NAME) {
        build_declidx(e->f);
        const char *s = x->u.name;
        size_t n = strlen(s);
        for (size_t end = n; end > 0; end--) {
            if (end < n && s[end] != '.' && s[end] != '[') continue;
            if (end < n && s[end] == '.' && (s[end + 1] == '#' || s[end + 1] == '%')) continue;
            uint64_t di;
            if (!u64map_get(&e->f->declidx, (uint64_t) (uintptr_t) qf_intern(e->f, s, end), &di)) continue;
            Decl *d = &e->f->decls.v[di - 1];
            if (d->expstate != 2 && !expand_decl(e->f, d, x->loc)) { e->ok = false; return x; }
            if (end == n) {
                X *l = enum_list(e, d, x->loc);
                if (l || d->kind != L_INST) return l ? l : x;
            }
            X *root = e->root;                       /* its references are its own */
            e->root = d->value;
            X *v = end == n ? data_of(e, d->value, depth + 1) : data_at(e, d->value, s + end, depth + 1);
            if (v && end < n) v = data_of(e, v, depth + 1);
            e->root = root;
            const char *tag = v ? x_tag_in(v) : NULL;
            if (tag) {
                char msg[400];
                snprintf(msg, sizeof msg, "%s holds %s, which belongs to %s: a comprehension reads values as written, so "
                         "the values of another declaration must be plain values", x->u.name, tag, d->label);
                exp_error(e, x->loc, msg);
                return x;
            }
            return v ? v : x;
        }
    }
    return x;
}

static X *atom_x(Front *f, const char *s, size_t n, Loc loc)
{
    X *a = x_new(f, X_ATOM, loc);
    a->u.name = qf_string_canonical(f, s, n);
    return a;
}

static X *bool_x(Front *f, bool v, Loc loc)
{
    X *a = x_new(f, X_ATOM, loc);
    a->u.name = qf_sym(f, v ? "true" : "false");
    return a;
}

/* The keys and the values that a source gives: the indexes and the items of a list, the labels and the values of a struct. */
static bool comp_items(Exp *e, X *src, Loc loc, XPtrVec *keys, XPtrVec *vals)
{
    /* expanded: the data of another declaration can have comprehensions of its own */
    X *x = expand(e, data_of(e, src, 0), NULL);
    if (!e->ok) return false;
    XPtrVec cj = { 0 };
    x_flatten(x, X_AND, &cj);
    X *list = NULL;
    bool structs = false;
    for (size_t i = 0; i < cj.n; i++) {
        X *k = data_of(e, cj.v[i], 0);
        if (k->k == X_LIST) list = k;
        else if (k->k == X_STRUCT && !k->u.st.refs) structs = true;
    }
    if (list) {
        if (list->u.list.open || list->u.list.elem) {
            exp_error(e, loc, "a comprehension cannot go through an open list: not all of its items are written");
            VEC_FREE(cj);
            return false;
        }
        for (int i = 0; i < list->u.list.n; i++) {
            VEC_PUSH(*keys, int_x(e->f, i, loc));
            VEC_PUSH(*vals, list->u.list.items[i]);
        }
    } else if (structs) {
        for (size_t i = 0; i < cj.n; i++) {
            X *k = data_of(e, cj.v[i], 0);
            if (k->k != X_STRUCT || k->u.st.refs) continue;
            for (int fi = 0; fi < k->u.st.n; fi++) {
                XField *fl = &k->u.st.fields[fi];
                if (!fl->label) continue;
                VEC_PUSH(*keys, atom_x(e->f, fl->label, strlen(fl->label), loc));
                VEC_PUSH(*vals, fl->value);
            }
        }
    } else {
        char *t = x_text(src);
        char msg[300];
        snprintf(msg, sizeof msg, "a comprehension goes through a list, a struct or an enum that the content writes: not %s", t);
        free(t);
        exp_error(e, loc, msg);
    }
    VEC_FREE(cj);
    return e->ok;
}

/* A condition that is known when content loads: 1 or 0. -1 if it is not
 * known. loud: -1 comes with a diagnostic that says why. */
static int cond_eval(Exp *e, X *x, bool loud)
{
    if (!e->ok) return -1;
    switch (x->k) {
    case X_LOGIC: {
        int a = cond_eval(e, x->u.ar.a, loud);
        if (a < 0) return -1;
        if (x->u.ar.op == '&' ? !a : a) return a;
        return cond_eval(e, x->u.ar.b, loud);
    }
    case X_NOT: {
        int a = cond_eval(e, x->u.sub, loud);
        return a < 0 ? -1 : !a;
    }
    case X_CMP: {
        /* a reference is data only for a condition of a comprehension: in the place of a value it is a law */
        X *a = loud ? data_of(e, x->u.ar.a, 0) : x->u.ar.a, *b = loud ? data_of(e, x->u.ar.b, 0) : x->u.ar.b;
        int op = x->u.ar.op;
        Num na, nb;
        if (num_of(e->f, a, &na) && num_of(e->f, b, &nb)) {
            if (!qdim_eq(&na.dim, &nb.dim)) {
                if (loud) exp_error(e, x->loc, "a comparison of numbers with different units");
                return -1;
            }
            int c = num_cmp(na, nb);
            switch (op) {
            case 'e': return c == 0;
            case 'n': return c != 0;
            case '<': return c < 0;
            case 'l': return c <= 0;
            case '>': return c > 0;
            default:  return c >= 0;
            }
        }
        if (a->k == X_ATOM && b->k == X_ATOM && (op == 'e' || op == 'n')) return (a->u.name == b->u.name) == (op == 'e');
        if (loud) {
            char *ta = x_text(a), *tb = x_text(b);
            char msg[400];
            snprintf(msg, sizeof msg, "a comparison in a comprehension is of values that are known when content loads: "
                     "numbers, or strings with == and != (not %s and %s)", ta, tb);
            free(ta);
            free(tb);
            exp_error(e, x->loc, msg);
        }
        return -1;
    }
    default: {
        X *v = loud ? data_of(e, x, 0) : x;
        if (v->k == X_ATOM && !strcmp(v->u.name, "true")) return 1;
        if (v->k == X_ATOM && !strcmp(v->u.name, "false")) return 0;
        if (loud) {
            char *t = x_text(v);
            char msg[300];
            snprintf(msg, sizeof msg, "a condition is true or false, known when content loads: not %s", t);
            free(t);
            exp_error(e, x->loc, msg);
        }
        return -1;
    }
    }
}

/* The string of an interpolation, "Nails-\(.count)": each part is a value
 * that is known when content loads. A number has its text as the content
 * writes it. x has its parts expanded. NULL with a diagnostic. */
static X *interp_eval(Exp *e, X *x)
{
    Buf b = { 0 };
    for (int i = 0; i <= x->u.in.n && e->ok; i++) {
        qf_buf_puts(&b, x->u.in.lits[i]);
        if (i == x->u.in.n) break;
        X *v = data_of(e, x->u.in.parts[i], 0);
        if (!e->ok) break;
        Num n;
        if (v->k == X_ATOM && v->u.name[0] == '"') {
            Buf s = { 0 };
            qf_string_decode(v->u.name, &s);
            qf_buf_puts(&b, s.s ? s.s : "");
            qf_buf_free(&s);
        } else if (v->k == X_ATOM && (!strcmp(v->u.name, "true") || !strcmp(v->u.name, "false"))) {
            qf_buf_puts(&b, v->u.name);
        } else if (num_of(e->f, v, &n)) {
            qf_print(&b, v);
        } else {
            char *t = x_text(x->u.in.parts[i]);
            char msg[400];
            snprintf(msg, sizeof msg, "an interpolation takes a number, a string, or true or false that is known when "
                     "content loads: not %s", t);
            free(t);
            exp_error(e, x->u.in.parts[i]->loc, msg);
        }
    }
    X *r = e->ok ? atom_x(e->f, b.s ? b.s : "", b.len, x->loc) : NULL;
    qf_buf_free(&b);
    return r;
}

/* A computed label: a name, from a string. */
static Sym label_of(Exp *e, X *x, Loc loc)
{
    Buf b = { 0 };
    X *v = data_of(e, x, 0);
    const char *err = NULL;
    char why[300];
    if (!e->ok) return NULL;
    if (v->k == X_ATOM && v->u.name[0] == '"') qf_string_decode(v->u.name, &b);
    else {
        char *t = x_text(v);
        snprintf(why, sizeof why, "a computed label is a string: not %s", t);
        free(t);
        err = why;
    }
    bool name = !err && b.len && (isalpha((unsigned char) b.s[0]) || b.s[0] == '_');
    for (size_t i = 0; name && i < b.len; i++) name = isalnum((unsigned char) b.s[i]) || b.s[i] == '_';
    Sym r = NULL;
    if (err) exp_error(e, loc, err);
    else if (!name) {
        char msg[300];
        snprintf(msg, sizeof msg, "a computed label must be the name of a field: not \"%.*s\"", (int) b.len, b.s ? b.s : "");
        exp_error(e, loc, msg);
    } else r = qf_sym(e->f, b.s);
    qf_buf_free(&b);
    return r;
}

/* --- built-in functions -------------------------------------------------------------------------- */

typedef struct { const char *name; int min, max; const char *sig; } Builtin;

static const Builtin builtins[] = {
    { "len", 1, 1, "len(list | struct | string)" },
    { "div", 2, 2, "div(whole, whole)" },
    { "mod", 2, 2, "mod(whole, whole)" },
    { "quo", 2, 2, "quo(whole, whole)" },
    { "rem", 2, 2, "rem(whole, whole)" },
    { "math.Abs", 1, 1, "math.Abs(number)" },
    { "math.Floor", 1, 1, "math.Floor(number)" },
    { "math.Ceil", 1, 1, "math.Ceil(number)" },
    { "math.Round", 1, 1, "math.Round(number)" },
    { "math.Trunc", 1, 1, "math.Trunc(number)" },
    { "math.Min", 2, 2, "math.Min(number, number)" },
    { "math.Max", 2, 2, "math.Max(number, number)" },
    { "math.Clamp", 3, 3, "math.Clamp(number, lowest, highest)" },
    { "strings.ToUpper", 1, 1, "strings.ToUpper(string)" },
    { "strings.ToLower", 1, 1, "strings.ToLower(string)" },
    { "strings.TrimSpace", 1, 1, "strings.TrimSpace(string)" },
    { "strings.Contains", 2, 2, "strings.Contains(string, string)" },
    { "strings.HasPrefix", 2, 2, "strings.HasPrefix(string, string)" },
    { "strings.HasSuffix", 2, 2, "strings.HasSuffix(string, string)" },
    { "strings.Index", 2, 2, "strings.Index(string, string)" },
    { "strings.Join", 2, 2, "strings.Join([...string], string)" },
    { "strings.Split", 2, 2, "strings.Split(string, string)" },
    { "strings.Replace", 4, 4, "strings.Replace(string, old, new, whole)" },
    { "strings.Repeat", 2, 2, "strings.Repeat(string, whole)" },
    { "list.Sum", 1, 1, "list.Sum([...number])" },
    { "list.Product", 1, 1, "list.Product([...number])" },
    { "list.Max", 1, 1, "list.Max([...number])" },
    { "list.Min", 1, 1, "list.Min([...number])" },
    { "list.Contains", 2, 2, "list.Contains(list, value)" },
    { "list.Concat", 1, 1, "list.Concat([...list])" },
    { "list.Range", 3, 3, "list.Range(whole, whole, whole)" },
    { "list.Slice", 3, 3, "list.Slice(list, whole, whole)" },
    { "list.Take", 2, 2, "list.Take(list, whole)" },
    { "list.Drop", 2, 2, "list.Drop(list, whole)" },
    { "list.Repeat", 2, 2, "list.Repeat(list, whole)" },
    { "list.SortStrings", 1, 1, "list.SortStrings([...string])" },
};

typedef struct { Exp *e; const X *call; char err[400]; } Call;

static X *fail(Call *k, const char *why)
{
    snprintf(k->err, sizeof k->err, "%s: %s", k->call->u.call.name, why);
    return NULL;
}

static bool as_num(Call *k, X *x, Num *n) { return num_of(k->e->f, data_of(k->e, x, 0), n); }

static bool as_int(Call *k, X *x, int64_t *v)
{
    Num n;
    return as_num(k, x, &n) && num_whole(n, v);
}

/* The bytes of a string. */
static bool as_str(Call *k, X *x, Buf *out)
{
    x = data_of(k->e, x, 0);
    if (x->k != X_ATOM || x->u.name[0] != '"') return false;
    qf_string_decode(x->u.name, out);
    if (!out->s) qf_buf_puts(out, "");
    return true;
}

static X *as_list(Call *k, X *x)
{
    x = data_of(k->e, x, 0);
    if (x->k == X_AND) {
        XPtrVec cj = { 0 };
        x_flatten(x, X_AND, &cj);
        X *l = NULL;
        for (size_t i = 0; i < cj.n && !l; i++) {
            X *d = data_of(k->e, cj.v[i], 0);
            if (d->k == X_LIST) l = d;
        }
        VEC_FREE(cj);
        x = l ? l : x;
    }
    if (x->k != X_LIST || x->u.list.open || x->u.list.elem) return NULL;
    return x;
}

static X *list_x(Call *k, X **items, size_t n)
{
    X *x = x_new(k->e->f, X_LIST, k->call->loc);
    x->u.list.n = (int) n;
    x->u.list.items = qf_alloc(k->e->f->arena, (n ? n : 1) * sizeof(X *));
    if (n) memcpy(x->u.list.items, items, n * sizeof(X *));
    return x;
}

/* Two values that are known when content loads are equal: 1 or 0, or -1 if they are not such values. */
static int equal(Call *k, X *a, X *b)
{
    Num na, nb;
    if (as_num(k, a, &na) && as_num(k, b, &nb)) return qdim_eq(&na.dim, &nb.dim) && num_cmp(na, nb) == 0;
    a = data_of(k->e, a, 0);
    b = data_of(k->e, b, 0);
    if (a->k == X_ATOM && b->k == X_ATOM) return a->u.name == b->u.name;
    return -1;
}

static const char *find_bytes(const char *h, size_t hn, const char *n, size_t nn)
{
    if (!nn) return h;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == n[0] && !memcmp(h + i, n, nn)) return h + i;
    return NULL;
}

static X *call_len(Call *k, X **a)
{
    X *x = data_of(k->e, a[0], 0);
    Buf b = { 0 };
    if (as_str(k, x, &b)) {
        size_t n = b.len;
        qf_buf_free(&b);
        return int_x(k->e->f, (int64_t) n, k->call->loc);
    }
    X *l = as_list(k, x);
    if (l) return int_x(k->e->f, l->u.list.n, k->call->loc);
    XPtrVec cj = { 0 };
    x_flatten(x, X_AND, &cj);
    int64_t n = 0;
    bool any = false;
    for (size_t i = 0; i < cj.n; i++) {
        X *s = data_of(k->e, cj.v[i], 0);
        if (s->k != X_STRUCT || s->u.st.refs) continue;
        any = true;
        for (int fi = 0; fi < s->u.st.n; fi++)
            if (s->u.st.fields[fi].label) n++;
    }
    VEC_FREE(cj);
    if (!any) return fail(k, "takes a closed list, a struct or a string that is known when content loads");
    return int_x(k->e->f, n, k->call->loc);
}

static X *call_intdiv(Call *k, X **a, char op)
{
    int64_t x, y;
    if (!as_int(k, a[0], &x) || !as_int(k, a[1], &y)) return fail(k, "takes two whole numbers that are known when content loads");
    if (y == 0) return fail(k, "division by zero");
    if (x == INT64_MIN || y == INT64_MIN) return fail(k, "the number is too large");
    /* div and mod are Euclidean, as in CUE: 0 <= mod < |y|, and x = y * div + mod. quo and rem go toward zero */
    int64_t m = y < 0 ? -y : y;
    int64_t r = ((x % m) + m) % m;
    Loc loc = k->call->loc;
    switch (op) {
    case 'd': return int_x(k->e->f, (x - r) / y, loc);
    case 'm': return int_x(k->e->f, r, loc);
    case 'q': return int_x(k->e->f, x / y, loc);
    default:  return int_x(k->e->f, x % y, loc);
    }
}

static X *call_math(Call *k, const char *fn, X **a)
{
    Front *f = k->e->f;
    Loc loc = k->call->loc;
    Num n, lo, hi;
    if (!as_num(k, a[0], &n)) return fail(k, "takes a number that is known when content loads");
    if (!strcmp(fn, "Abs")) {
        if (n.v == INT64_MIN) return fail(k, "the number is too large");
        n.v = n.v < 0 ? -n.v : n.v;
        return num_x(f, n, loc);
    }
    if (!strcmp(fn, "Min") || !strcmp(fn, "Max")) {
        if (!as_num(k, a[1], &lo) || !qdim_eq(&n.dim, &lo.dim)) return fail(k, "takes two numbers with one unit");
        int c = num_cmp(n, lo);
        return num_x(f, (fn[1] == 'i' ? c <= 0 : c >= 0) ? n : lo, loc);
    }
    if (!strcmp(fn, "Clamp")) {
        if (!as_num(k, a[1], &lo) || !as_num(k, a[2], &hi) || !qdim_eq(&n.dim, &lo.dim) || !qdim_eq(&n.dim, &hi.dim))
            return fail(k, "takes three numbers with one unit");
        if (num_cmp(lo, hi) > 0) return fail(k, "the lowest value is above the highest");
        return num_x(f, num_cmp(n, lo) < 0 ? lo : num_cmp(n, hi) > 0 ? hi : n, loc);
    }
    /* Floor, Ceil, Round, Trunc: a whole number of the coherent unit */
    if (n.e >= 0) return num_x(f, n, loc);
    i128 d = 1;
    for (int i = 0; i < -n.e && i < 38; i++) d *= 10;
    i128 q = n.v / d, r = n.v % d;                 /* toward zero, and r has the sign of the number */
    if (!strcmp(fn, "Floor")) { if (r < 0) q -= 1; }
    else if (!strcmp(fn, "Ceil")) { if (r > 0) q += 1; }
    else if (!strcmp(fn, "Round")) { if ((r < 0 ? -r : r) * 2 >= d) q += n.v < 0 ? -1 : 1; }     /* half away from zero */
    Num w = { n.dim, (int64_t) q, 0 };
    return num_x(f, w, loc);
}

static X *call_strings(Call *k, const char *fn, X **a, int n)
{
    Front *f = k->e->f;
    Loc loc = k->call->loc;
    Buf s = { 0 }, t = { 0 }, u = { 0 }, out = { 0 };
    X *r = NULL;
    if (!strcmp(fn, "Join")) {
        X *l = as_list(k, a[0]);
        bool ok = l && as_str(k, a[1], &t);
        for (int i = 0; ok && i < l->u.list.n; i++) {
            s.len = 0;
            if (!as_str(k, l->u.list.items[i], &s)) { ok = false; break; }
            if (i) qf_buf_puts(&out, t.s);
            qf_buf_puts(&out, s.s);
        }
        r = ok ? atom_x(f, out.s ? out.s : "", out.len, loc) : fail(k, "takes a closed list of strings and a string");
    } else if (!as_str(k, a[0], &s)) {
        r = fail(k, "takes a string that is known when content loads");
    } else if (!strcmp(fn, "ToUpper") || !strcmp(fn, "ToLower")) {
        for (size_t i = 0; i < s.len; i++)
            s.s[i] = (char) (fn[2] == 'U' ? toupper((unsigned char) s.s[i]) : tolower((unsigned char) s.s[i]));
        r = atom_x(f, s.s, s.len, loc);
    } else if (!strcmp(fn, "TrimSpace")) {
        size_t b = 0, e = s.len;
        while (b < e && isspace((unsigned char) s.s[b])) b++;
        while (e > b && isspace((unsigned char) s.s[e - 1])) e--;
        r = atom_x(f, s.s + b, e - b, loc);
    } else if (!strcmp(fn, "Repeat")) {
        int64_t times;
        if (!as_int(k, a[1], &times) || times < 0 || (s.len && (uint64_t) times > EXPAND_MAX / s.len))
            r = fail(k, "takes a string and a count that is not negative and not too large");
        else {
            for (int64_t i = 0; i < times; i++) qf_buf_puts(&out, s.s);
            r = atom_x(f, out.s ? out.s : "", out.len, loc);
        }
    } else if (!as_str(k, a[1], &t)) {
        r = fail(k, "takes strings that are known when content loads");
    } else if (!strcmp(fn, "Contains")) {
        r = bool_x(f, find_bytes(s.s, s.len, t.s, t.len) != NULL, loc);
    } else if (!strcmp(fn, "HasPrefix")) {
        r = bool_x(f, t.len <= s.len && !memcmp(s.s, t.s, t.len), loc);
    } else if (!strcmp(fn, "HasSuffix")) {
        r = bool_x(f, t.len <= s.len && !memcmp(s.s + s.len - t.len, t.s, t.len), loc);
    } else if (!strcmp(fn, "Index")) {
        const char *at = find_bytes(s.s, s.len, t.s, t.len);
        r = int_x(f, at ? (int64_t) (at - s.s) : -1, loc);
    } else if (!strcmp(fn, "Split")) {
        XPtrVec parts = { 0 };
        if (!t.len) {
            for (size_t i = 0; i < s.len; i++) VEC_PUSH(parts, atom_x(f, s.s + i, 1, loc));
        } else {
            const char *p = s.s, *end = s.s + s.len, *at;
            while ((at = find_bytes(p, (size_t) (end - p), t.s, t.len))) {
                VEC_PUSH(parts, atom_x(f, p, (size_t) (at - p), loc));
                p = at + t.len;
            }
            VEC_PUSH(parts, atom_x(f, p, (size_t) (end - p), loc));
        }
        r = list_x(k, parts.v, parts.n);
        VEC_FREE(parts);
    } else if (!strcmp(fn, "Replace") && n == 4) {
        int64_t count;
        if (!as_str(k, a[2], &u) || !as_int(k, a[3], &count)) r = fail(k, "takes three strings and a count");
        else if (!t.len) r = fail(k, "the text to replace is empty");
        else {
            const char *p = s.s, *end = s.s + s.len, *at;
            int64_t done = 0;
            while ((count < 0 || done < count) && (at = find_bytes(p, (size_t) (end - p), t.s, t.len))) {
                for (const char *c = p; c < at; c++) qf_buf_putc(&out, *c);
                qf_buf_puts(&out, u.s);
                p = at + t.len;
                done++;
            }
            for (const char *c = p; c < end; c++) qf_buf_putc(&out, *c);
            r = atom_x(f, out.s ? out.s : "", out.len, loc);
        }
    } else {
        r = fail(k, "is not a built-in function");
    }
    qf_buf_free(&s);
    qf_buf_free(&t);
    qf_buf_free(&u);
    qf_buf_free(&out);
    return r;
}

static int cmp_atom_name(const void *a, const void *b)
{
    return strcmp((*(X *const *) a)->u.name, (*(X *const *) b)->u.name);
}

static X *call_list(Call *k, const char *fn, X **a)
{
    Front *f = k->e->f;
    Loc loc = k->call->loc;
    if (!strcmp(fn, "Range")) {
        int64_t from, to, step;
        if (!as_int(k, a[0], &from) || !as_int(k, a[1], &to) || !as_int(k, a[2], &step))
            return fail(k, "takes three whole numbers that are known when content loads");
        if (step == 0) return fail(k, "the step is zero");
        XPtrVec out = { 0 };
        for (int64_t v = from; step > 0 ? v < to : v > to; v += step) {
            if (out.n >= EXPAND_MAX) { VEC_FREE(out); return fail(k, "the list is too long"); }
            VEC_PUSH(out, int_x(f, v, loc));
            if (step > 0 ? v > INT64_MAX - step : v < INT64_MIN - step) break;
        }
        X *r = list_x(k, out.v, out.n);
        VEC_FREE(out);
        return r;
    }
    X *l = as_list(k, a[0]);
    if (!l) return fail(k, "takes a closed list that is known when content loads");
    int n = l->u.list.n;
    X **it = l->u.list.items;
    if (!strcmp(fn, "Sum") || !strcmp(fn, "Product") || !strcmp(fn, "Max") || !strcmp(fn, "Min")) {
        Num acc = { qdim_none(), fn[0] == 'P' ? 1 : 0, 0 };
        for (int i = 0; i < n; i++) {
            Num v;
            if (!as_num(k, it[i], &v)) return fail(k, "takes a list of numbers that are known when content loads");
            if (fn[0] == 'P') {
                if (!num_arith('*', acc, v, &acc)) return fail(k, "the product is too large");
            } else if (i == 0) {
                acc = v;
            } else if (!qdim_eq(&acc.dim, &v.dim)) {
                return fail(k, "the numbers have different units");
            } else if (fn[0] == 'S') {
                if (!num_arith('+', acc, v, &acc)) return fail(k, "the sum is too large");
            } else if (fn[1] == 'a' ? num_cmp(v, acc) > 0 : num_cmp(v, acc) < 0) {
                acc = v;
            }
        }
        if (!n && fn[0] == 'M') return fail(k, "the list is empty");
        return num_x(f, acc, loc);
    }
    if (!strcmp(fn, "Contains")) {
        for (int i = 0; i < n; i++) {
            int eq = equal(k, it[i], a[1]);
            if (eq < 0) return fail(k, "compares numbers and strings that are known when content loads");
            if (eq) return bool_x(f, true, loc);
        }
        return bool_x(f, false, loc);
    }
    if (!strcmp(fn, "Concat")) {
        XPtrVec out = { 0 };
        for (int i = 0; i < n; i++) {
            X *sub = as_list(k, it[i]);
            if (!sub) { VEC_FREE(out); return fail(k, "takes a list of closed lists"); }
            for (int j = 0; j < sub->u.list.n; j++) VEC_PUSH(out, sub->u.list.items[j]);
        }
        X *r = list_x(k, out.v, out.n);
        VEC_FREE(out);
        return r;
    }
    if (!strcmp(fn, "SortStrings")) {
        for (int i = 0; i < n; i++) {
            X *s = data_of(k->e, it[i], 0);
            if (s->k != X_ATOM || s->u.name[0] != '"') return fail(k, "takes a list of strings");
        }
        X *r = list_x(k, it, (size_t) n);
        for (int i = 0; i < n; i++) r->u.list.items[i] = data_of(k->e, it[i], 0);
        if (n) qsort(r->u.list.items, (size_t) n, sizeof(X *), cmp_atom_name);
        return r;
    }
    int64_t p, q = 0;
    if (!as_int(k, a[1], &p) || (!strcmp(fn, "Slice") && !as_int(k, a[2], &q)))
        return fail(k, "takes a list and whole numbers that are known when content loads");
    if (!strcmp(fn, "Slice")) {
        if (p < 0 || q < p || q > n) return fail(k, "the range is not inside the list");
        return list_x(k, it + p, (size_t) (q - p));
    }
    if (!strcmp(fn, "Take") || !strcmp(fn, "Drop")) {
        if (p < 0) return fail(k, "the count is negative");
        if (p > n) p = n;
        return fn[0] == 'T' ? list_x(k, it, (size_t) p) : list_x(k, it + p, (size_t) (n - p));
    }
    if (!strcmp(fn, "Repeat")) {
        if (p < 0 || (n && p > EXPAND_MAX / n)) return fail(k, "the count is negative or too large");
        XPtrVec out = { 0 };
        for (int64_t r = 0; r < p; r++)
            for (int i = 0; i < n; i++) VEC_PUSH(out, it[i]);
        X *r = list_x(k, out.v, out.n);
        VEC_FREE(out);
        return r;
    }
    return fail(k, "is not a built-in function");
}

/* The value of a call, or NULL with the reason in k->err. */
static X *builtin_call(Call *k, X **args)
{
    const char *name = k->call->u.call.name;
    int n = k->call->u.call.n;
    const Builtin *b = NULL;
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0] && !b; i++)
        if (!strcmp(builtins[i].name, name)) b = &builtins[i];
    if (!b) {
        snprintf(k->err, sizeof k->err, "unknown function %s: the built-in functions are len, div, mod, quo, rem, and the "
                 "functions of math, strings and list", name);
        return NULL;
    }
    if (n < b->min || n > b->max) {
        snprintf(k->err, sizeof k->err, "%s takes %d argument%s: %s", name, b->min, b->min == 1 ? "" : "s", b->sig);
        return NULL;
    }
    if (!strcmp(name, "len")) return call_len(k, args);
    if (!strcmp(name, "div") || !strcmp(name, "mod") || !strcmp(name, "quo") || !strcmp(name, "rem"))
        return call_intdiv(k, args, name[0]);
    if (!strncmp(name, "math.", 5)) return call_math(k, name + 5, args);
    if (!strncmp(name, "strings.", 8)) return call_strings(k, name + 8, args, n);
    return call_list(k, name + 5, args);
}

/* --- the expansion --------------------------------------------------------------------------------- */

static void comp_run(Exp *e, X *comp, int i, const Env *env, XPtrVec *items, FieldVec *fields)
{
    if (!e->ok) return;
    if (i == comp->u.comp.n) {
        if (++e->made > EXPAND_MAX) {
            char msg[128];
            snprintf(msg, sizeof msg, "the comprehensions here make more than %d items or fields", EXPAND_MAX);
            exp_error(e, comp->loc, msg);
            return;
        }
        X *b = expand(e, comp->u.comp.body, env);
        if (!e->ok) return;
        if (comp->u.comp.list) { VEC_PUSH(*items, b); return; }
        if (b->k != X_STRUCT) { exp_error(e, b->loc, "the body of a comprehension among fields is a struct"); return; }
        if (b->u.st.nattrs) { exp_error(e, b->loc, "the fields of a comprehension cannot have the attributes of a struct"); return; }
        for (int fi = 0; fi < b->u.st.n; fi++) VEC_PUSH(*fields, b->u.st.fields[fi]);
        return;
    }
    CompClause *cl = &comp->u.comp.clauses[i];
    if (cl->kind == C_IF) {
        int v = cond_eval(e, expand(e, cl->expr, env), true);
        if (v > 0) comp_run(e, comp, i + 1, env, items, fields);
        return;
    }
    XPtrVec keys = { 0 }, vals = { 0 };
    if (comp_items(e, expand(e, cl->expr, env), cl->loc, &keys, &vals))
        for (size_t k = 0; k < vals.n && e->ok; k++) {
            Env ke = { cl->key, keys.v[k], env };
            Env ve = { cl->val, vals.v[k], cl->key ? &ke : env };
            comp_run(e, comp, i + 1, &ve, items, fields);
        }
    VEC_FREE(keys);
    VEC_FREE(vals);
}

static X *x_copy(Exp *e, X *x)
{
    X *n = x_new(e->f, x->k, x->loc);
    *n = *x;
    return n;
}

/* x with its comprehensions expanded, its computed labels made names, its
 * calls made values, and the variables of env replaced by their values. x
 * itself does not change: its parts can be shared (the value of a let, the
 * body of one comprehension). */
static X *expand(Exp *e, X *x, const Env *env)
{
    if (!x || !e->ok) return x;
    Front *f = e->f;
    switch (x->k) {
    case X_VAR: {
        const Env *b = env;
        while (b && b->var != x->u.var.var) b = b->up;
        if (!b) { exp_error(e, x->loc, "the variable of a comprehension stands outside its comprehension"); return x; }
        if (!x->u.var.path) return b->value;
        X *v = data_at(e, b->value, x->u.var.path, 0);
        if (!v) {
            char msg[300];
            snprintf(msg, sizeof msg, "%s%s: its value has no such field or item", x->u.var.var->name, x->u.var.path);
            exp_error(e, x->loc, msg);
            return x;
        }
        return v;
    }
    case X_STRUCT: {
        FieldVec out = { 0 };
        for (int i = 0; i < x->u.st.n && e->ok; i++) {
            XField *fl = &x->u.st.fields[i];
            X *fc = field_comp(fl);
            if (fc) { comp_run(e, fc, 0, env, NULL, &out); continue; }
            XField g = *fl;
            if (fl->dlabel) {
                g.label = label_of(e, expand(e, fl->dlabel, env), fl->loc);
                g.dlabel = NULL;
            }
            g.value = expand(e, fl->value, env);
            VEC_PUSH(out, g);
        }
        X *n = x_copy(e, x);
        n->u.st.n = (int) out.n;
        n->u.st.fields = qf_alloc(f->arena, (out.n ? out.n : 1) * sizeof(XField));
        if (out.n) memcpy(n->u.st.fields, out.v, out.n * sizeof(XField));
        VEC_FREE(out);
        return n;
    }
    case X_LIST: {
        XPtrVec out = { 0 };
        for (int i = 0; i < x->u.list.n && e->ok; i++) {
            X *it = x->u.list.items[i];
            if (it->k == X_COMP) comp_run(e, it, 0, env, &out, NULL);
            else VEC_PUSH(out, expand(e, it, env));
        }
        X *n = x_copy(e, x);
        n->u.list.n = (int) out.n;
        n->u.list.items = qf_alloc(f->arena, (out.n ? out.n : 1) * sizeof(X *));
        if (out.n) memcpy(n->u.list.items, out.v, out.n * sizeof(X *));
        n->u.list.elem = expand(e, x->u.list.elem, env);
        VEC_FREE(out);
        return n;
    }
    case X_AND: case X_OR: {
        X *n = x_copy(e, x);
        n->u.bin.a = expand(e, x->u.bin.a, env);
        n->u.bin.b = expand(e, x->u.bin.b, env);
        return n;
    }
    case X_ARITH: {
        X *n = x_copy(e, x);
        n->u.ar.a = expand(e, x->u.ar.a, env);
        n->u.ar.b = expand(e, x->u.ar.b, env);
        /* two numbers: the value now, if it is exact. If not, the expression stays for the laws */
        Num a, b, r;
        if (num_of(f, n->u.ar.a, &a) && num_of(f, n->u.ar.b, &b) && num_arith(x->u.ar.op, a, b, &r)) return num_x(f, r, x->loc);
        return n;
    }
    case X_CMP: case X_LOGIC: case X_NOT: {
        X *n = x_copy(e, x);
        if (x->k == X_NOT) n->u.sub = expand(e, x->u.sub, env);
        else {
            n->u.ar.a = expand(e, x->u.ar.a, env);
            n->u.ar.b = expand(e, x->u.ar.b, env);
        }
        /* true or false now, if both sides are known. If not, it is a law */
        int v = cond_eval(e, n, false);
        return v < 0 ? n : bool_x(f, v, x->loc);
    }
    case X_DEFAULT: {
        X *n = x_copy(e, x);
        n->u.sub = expand(e, x->u.sub, env);
        return n;
    }
    case X_INTERP: {
        X *n = x_copy(e, x);
        n->u.in.parts = qf_alloc(f->arena, (size_t) (x->u.in.n ? x->u.in.n : 1) * sizeof(X *));
        for (int i = 0; i < x->u.in.n; i++) n->u.in.parts[i] = expand(e, x->u.in.parts[i], env);
        X *text = e->ok ? interp_eval(e, n) : NULL;
        return text ? text : x;
    }
    case X_BOUND: {                                /* a bound by a named value: the number now */
        if (!x->u.bound.of) return x;
        X *v = data_of(e, expand(e, x->u.bound.of, env), 0);
        if (!e->ok) return x;
        Num num;
        if (!num_of(f, v, &num)) {
            Buf w = { 0 };
            qf_print(&w, x->u.bound.of);
            char msg[300];
            snprintf(msg, sizeof msg, "the value of a bound must be a number that the content writes, and %s is not one",
                     w.s ? w.s : "it");
            qf_buf_free(&w);
            exp_error(e, x->loc, msg);
            return x;
        }
        X *n = x_copy(e, x);
        n->u.bound.n = v->u.num;
        n->u.bound.of = NULL;
        return n;
    }
    case X_COMP:
        exp_error(e, x->loc, "a comprehension stands in a list, or among the fields of a struct");
        return x;
    case X_CALL: {
        X *args[8];
        if (x->u.call.n > 8) { exp_error(e, x->loc, "a function takes 8 arguments at most"); return x; }
        for (int i = 0; i < x->u.call.n; i++) args[i] = expand(e, x->u.call.args[i], env);
        if (!e->ok) return x;
        Call k = { e, x, { 0 } };
        X *r = builtin_call(&k, args);
        if (!r && e->ok) exp_error(e, x->loc, k.err);
        return r ? r : x;
    }
    default:
        return x;
    }
}

/* Expands a declaration, once. A declaration that a comprehension reads is expanded first. */
static bool expand_decl(Front *f, Decl *d, Loc from)
{
    if (d->expstate == 2) return true;
    if (d->expstate == 1) {
        qf_diag(f, true, from, "the comprehensions of %s read a declaration whose comprehensions read %s back", d->label,
                d->label);
        return false;
    }
    d->expstate = 1;
    Exp e = { f, d->value, true, 0, 0 };
    X *v = expand(&e, d->value, NULL);
    d->value = v;                            /* the vector of declarations does not move here */
    d->expstate = 2;
    return e.ok;
}

bool qf_expand_all(Front *f)
{
    bool ok = true;
    for (size_t i = 0; i < f->decls.n; i++)
        if (f->decls.v[i].kind != L_KIND) ok &= expand_decl(f, &f->decls.v[i], f->decls.v[i].loc);
    return ok;
}
