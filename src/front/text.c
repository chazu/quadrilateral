#include <stdlib.h>
#include <string.h>

#include "front/front.h"

/* Constraints on text with regular expressions: the =~ and !~ of a text
 * value that is not one string yet (quad-ijx.11). A value has the set of the
 * constraints that it must meet. The unification of two values takes the
 * union of their sets, and one string must meet each constraint of the set.
 * The sets are interned for each Front, so the unifier compares them by
 * pointer, and they live as long as the Front. Triangle's text.c (ADR 0003). */

const RxSet *text_rx(int64_t lo, int64_t hi)
{
    return lo == Q_NINF && hi != Q_INF ? (const RxSet *) (intptr_t) hi : NULL;
}

static int cmp_item(const RxItem *a, const RxItem *b)
{
    int d = strcmp(a->pat, b->pat);
    return d ? d : (int) a->neg - (int) b->neg;
}

static uint64_t set_hash(const RxItem *v, uint32_t n)
{
    uint64_t h = 0x7E57;
    for (uint32_t i = 0; i < n; i++) {
        h = qf_fnv1a(h, v[i].pat, strlen(v[i].pat));
        h = qf_hash64(h ^ (uint64_t) v[i].neg);
    }
    return h ? h : 1;
}

/* The interned set with these items, which are in order and have no repeat. */
static const RxSet *intern_set(Front *f, const RxItem *v, uint32_t n)
{
    if (!n) return NULL;
    uint64_t h = set_hash(v, n), k = h, idx;
    while (u64map_get(&f->rxsetidx, k, &idx)) {
        const RxSet *s = f->rxsets.v[idx - 1];
        if (s->hash == h && s->n == n) {
            uint32_t i = 0;
            while (i < n && s->v[i].pat == v[i].pat && s->v[i].neg == v[i].neg) i++;
            if (i == n) return s;
        }
        k = qf_hash64(k);
    }
    RxSet *s = qf_xmalloc(sizeof *s);
    s->n = n;
    s->hash = h;
    s->v = qf_xmalloc(n * sizeof *s->v);
    memcpy(s->v, v, n * sizeof *v);
    VEC_PUSH(f->rxsets, s);
    u64map_put(&f->rxsetidx, k, f->rxsets.n);
    return s;
}

const RxSet *rx_single(Front *f, Sym pat, bool neg, char *err, size_t errlen)
{
    uint64_t v;
    Regex *re;
    if (u64map_get(&f->rxcache, (uint64_t) (uintptr_t) pat, &v)) re = (Regex *) (uintptr_t) v;
    else {
        Buf b = { 0 };
        qf_string_decode(pat, &b);
        re = regex_compile(b.s ? b.s : "", b.len, err, errlen);
        qf_buf_free(&b);
        if (!re) return NULL;
        u64map_put(&f->rxcache, (uint64_t) (uintptr_t) pat, (uint64_t) (uintptr_t) re);
        VEC_PUSH(f->regexes, re);
    }
    RxItem it = { pat, neg, re };
    return intern_set(f, &it, 1);
}

const RxSet *rx_union(Front *f, const RxSet *a, const RxSet *b)
{
    if (!a || a == b) return b ? b : a;
    if (!b) return a;
    RxItem *v = qf_xmalloc((a->n + b->n) * sizeof *v);
    uint32_t i = 0, j = 0, n = 0;
    while (i < a->n || j < b->n) {
        int d = i == a->n ? 1 : j == b->n ? -1 : cmp_item(&a->v[i], &b->v[j]);
        if (d <= 0) v[n++] = a->v[i++];
        else v[n++] = b->v[j++];
        if (d == 0) j++;
    }
    const RxSet *r = intern_set(f, v, n);
    free(v);
    return r;
}

const RxSet *rx_common(Front *f, const RxSet *a, const RxSet *b)
{
    if (!a || !b) return NULL;
    if (a == b) return a;
    RxItem *v = qf_xmalloc((a->n < b->n ? a->n : b->n) * sizeof *v + 1);
    uint32_t i = 0, j = 0, n = 0;
    while (i < a->n && j < b->n) {
        int d = cmp_item(&a->v[i], &b->v[j]);
        if (d == 0) { v[n++] = a->v[i]; i++; j++; }
        else if (d < 0) i++;
        else j++;
    }
    const RxSet *r = intern_set(f, v, n);
    free(v);
    return r;
}

static bool item_accepts(const RxItem *it, const char *s, size_t n)
{
    return regex_match(it->re, s, n) != it->neg;
}

bool rx_accepts(const RxSet *set, Sym text)
{
    if (!set) return true;
    Buf b = { 0 };
    qf_string_decode(text, &b);
    bool ok = true;
    for (uint32_t i = 0; i < set->n && ok; i++) ok = item_accepts(&set->v[i], b.s ? b.s : "", b.len);
    qf_buf_free(&b);
    return ok;
}

const RxSet *rx_satisfied(Front *f, const RxSet *set, Sym text)
{
    if (!set) return NULL;
    Buf b = { 0 };
    qf_string_decode(text, &b);
    RxItem *v = qf_xmalloc(set->n * sizeof *v);
    uint32_t n = 0;
    for (uint32_t i = 0; i < set->n; i++)
        if (item_accepts(&set->v[i], b.s ? b.s : "", b.len)) v[n++] = set->v[i];
    qf_buf_free(&b);
    const RxSet *r = intern_set(f, v, n);
    free(v);
    return r;
}

bool rx_subset(const RxSet *a, const RxSet *b)
{
    if (!a || a == b) return true;
    if (!b) return false;
    uint32_t j = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        while (j < b->n && cmp_item(&b->v[j], &a->v[i]) < 0) j++;
        if (j == b->n || cmp_item(&b->v[j], &a->v[i]) != 0) return false;
    }
    return true;
}

void rx_print(Buf *b, const RxSet *s)
{
    for (uint32_t i = 0; s && i < s->n; i++) {
        if (i) qf_buf_puts(b, " & ");
        qf_buf_puts(b, s->v[i].neg ? "!~" : "=~");
        qf_buf_puts(b, s->v[i].pat);
    }
}

void rx_free_all(Front *f)
{
    for (size_t i = 0; i < f->rxsets.n; i++) {
        free(f->rxsets.v[i]->v);
        free(f->rxsets.v[i]);
    }
    VEC_FREE(f->rxsets);
    for (size_t i = 0; i < f->regexes.n; i++) regex_free(f->regexes.v[i]);
    VEC_FREE(f->regexes);
    u64map_free(&f->rxsetidx);
    u64map_free(&f->rxcache);
}
