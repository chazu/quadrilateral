#include "front/front.h"

/* Helpers for syntax trees that the resolver uses. From Triangle's xtree.c
 * (bruggadung f425574), under ADR 0003. */

char *x_text(const X *x)
{
    Buf b = { 0 };
    qf_print(&b, x);
    return b.s ? b.s : qf_xcalloc(1, 1);
}

X *x_new(Front *c, XKind k, Loc loc)
{
    X *x = qf_alloc(c->arena, sizeof(X));
    x->k = k;
    x->loc = loc;
    return x;
}

X *x_name(Front *c, Sym name, Loc loc)
{
    X *x = x_new(c, X_NAME, loc);
    x->u.name = name;
    return x;
}

X *x_and(Front *c, X *a, X *b)
{
    if (!a) return b;
    if (!b) return a;
    X *x = x_new(c, X_AND, a->loc);
    x->u.bin.a = a;
    x->u.bin.b = b;
    return x;
}

void x_flatten(X *x, XKind k, XPtrVec *out)
{
    if (x->k == k) { x_flatten(x->u.bin.a, k, out); x_flatten(x->u.bin.b, k, out); }
    else VEC_PUSH(*out, x);
}

static bool kind_name(Sym s)
{
    const char *dot = strchr(s, '.');
    return s[0] == '%' || (dot && dot[1] == '%');
}

/* A numeric expression: a number, a bound, a kind, `number`, or a
 * conjunction or an arithmetic of those. */
bool x_numeric(const X *x)
{
    switch (x->k) {
    case X_DEFAULT: return x_numeric(x->u.sub);
    case X_NUM: return true;
    case X_BOUND: return !x->u.bound.of;
    case X_NAME: return kind_name(x->u.name) || !strcmp(x->u.name, "number");
    case X_AND: return x_numeric(x->u.bin.a) && x_numeric(x->u.bin.b);
    case X_ARITH: return x_numeric(x->u.ar.a) && x_numeric(x->u.ar.b);
    default: return false;
    }
}

bool x_numeric_or(X *x)
{
    XPtrVec ds = { 0 };
    x_flatten(x, X_OR, &ds);
    bool ok = ds.n > 1;
    for (size_t i = 0; i < ds.n && ok; i++) ok = x_numeric(ds.v[i]);
    VEC_FREE(ds);
    return ok;
}

/* `*d | rest`: a disjunction of one default and one other alternative. */
bool x_default_rest(X *x)
{
    XPtrVec ds = { 0 };
    x_flatten(x, X_OR, &ds);
    bool ok = ds.n == 2 && (ds.v[0]->k == X_DEFAULT) != (ds.v[1]->k == X_DEFAULT);
    VEC_FREE(ds);
    return ok;
}

/* A disjunction of text: `text` itself among the alternatives, the rest
 * string literals or regular expressions, which it covers. The hull is
 * `text`, so it is no disjunction of structures. */
bool x_text_or(X *x)
{
    XPtrVec ds = { 0 };
    x_flatten(x, X_OR, &ds);
    bool text = false, ok = ds.n > 1;
    for (size_t i = 0; i < ds.n && ok; i++) {
        X *a = ds.v[i]->k == X_DEFAULT ? ds.v[i]->u.sub : ds.v[i];
        if (a->k == X_NAME && strcmp(a->u.name, "text") == 0) text = true;
        else if (a->k != X_ATOM && a->k != X_REGEX) ok = false;
    }
    VEC_FREE(ds);
    return ok && text;
}
