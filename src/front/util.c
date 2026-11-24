#include "front/front.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The arena, the buffers, the symbol table and the diagnostics of the front
 * end. Borrowed from Triangle's util.c (bruggadung f425574), under ADR 0003. */

void qf_oom(void)
{
    fputs("quadrilateral: the front end is out of memory\n", stderr);
    abort();
}

void *qf_grow(void *v, size_t *cap, size_t elem)
{
    size_t n = *cap ? *cap * 2 : 8;
    void *q = realloc(v, n * elem);
    if (!q) qf_oom();
    *cap = n;
    return q;
}

/* --- arena ------------------------------------------------------------------- */

#define CHUNK_SIZE (256 * 1024)

typedef struct Chunk {
    struct Chunk *prev;
    size_t        used, cap;
    char          data[];
} Chunk;

struct QfArena { Chunk *top; Chunk *spare; };   /* spare: one released chunk, kept for reuse */

void *qf_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) qf_oom();
    return p;
}

void *qf_xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) qf_oom();
    return p;
}

void *qf_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) qf_oom();
    return q;
}

QfArena *qf_arena_new(void)
{
    return qf_xcalloc(1, sizeof(QfArena));
}

void qf_arena_free(QfArena *a)
{
    if (!a) return;
    Chunk *ch = a->top;
    while (ch) {
        Chunk *prev = ch->prev;
        free(ch);
        ch = prev;
    }
    free(a->spare);
    free(a);
}

void *qf_alloc_raw(QfArena *a, size_t n)
{
    n = (n + 15) & ~(size_t) 15;
    Chunk *ch = a->top;
    if (!ch || ch->used + n > ch->cap) {
        Chunk *nc;
        if (a->spare && a->spare->cap >= n) {
            nc = a->spare;
            a->spare = NULL;
        } else {
            size_t cap = n > CHUNK_SIZE ? n : CHUNK_SIZE;
            nc = qf_xmalloc(sizeof(Chunk) + cap);
            nc->cap = cap;
        }
        nc->prev = ch;
        nc->used = 0;
        a->top = nc;
        ch = nc;
    }
    void *p = ch->data + ch->used;
    ch->used += n;
    return p;
}

void *qf_alloc(QfArena *a, size_t n)
{
    void *p = qf_alloc_raw(a, n);
    memset(p, 0, n);
    return p;
}

QfMark qf_arena_mark(QfArena *a)
{
    QfMark m = { a->top, a->top ? a->top->used : 0 };
    return m;
}

void qf_arena_release(QfArena *a, QfMark m)
{
    while (a->top && a->top != (Chunk *) m.chunk) {
        Chunk *prev = a->top->prev;
        /* keep one standard chunk, so an operation that crosses a chunk
         * boundary doesn't allocate and free one every time */
        if (!a->spare && a->top->cap == CHUNK_SIZE) a->spare = a->top;
        else free(a->top);
        a->top = prev;
    }
    if (a->top) a->top->used = m.used;
}

/* --- buffers ------------------------------------------------------------------ */

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + extra + 1) cap *= 2;
        char *s = realloc(b->s, cap);
        if (!s) qf_oom();
        b->s = s;
        b->cap = cap;
    }
}

void qf_buf_putc(Buf *b, char c)
{
    buf_reserve(b, 1);
    b->s[b->len++] = c;
    b->s[b->len] = 0;
}

void qf_buf_puts(Buf *b, const char *s)
{
    size_t n = strlen(s);
    buf_reserve(b, n);
    memcpy(b->s + b->len, s, n + 1);
    b->len += n;
}

void qf_buf_putn(Buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void qf_buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    buf_reserve(b, (size_t) n);
    vsnprintf(b->s + b->len, (size_t) n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t) n;
}

void qf_buf_free(Buf *b)
{
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}

/* --- symbols ------------------------------------------------------------------- */

static uint64_t str_hash(const char *s, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;   /* FNV-1a */
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char) s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* The table is only searched, never iterated, so its order cannot change a
 * result (ADR 0004). */
static Sym sym_intern(SymTable *t, const char *s, size_t len)
{
    if (t->count * 2 >= t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 256;
        Sym *ns = calloc(ncap, sizeof(Sym));
        if (!ns) qf_oom();
        for (size_t i = 0; i < t->cap; i++) {
            Sym e = t->slots[i];
            if (!e) continue;
            size_t j = str_hash(e, strlen(e)) & (ncap - 1);
            while (ns[j]) j = (j + 1) & (ncap - 1);
            ns[j] = e;
        }
        free(t->slots);
        t->slots = ns;
        t->cap = ncap;
    }
    size_t j = str_hash(s, len) & (t->cap - 1);
    while (t->slots[j]) {
        if (strlen(t->slots[j]) == len && memcmp(t->slots[j], s, len) == 0) return t->slots[j];
        j = (j + 1) & (t->cap - 1);
    }
    char *copy = malloc(len + 1);
    if (!copy) qf_oom();
    memcpy(copy, s, len);
    copy[len] = 0;
    t->slots[j] = copy;
    t->count++;
    return copy;
}

Sym qf_intern(Front *f, const char *s, size_t len) { return sym_intern(&f->syms, s, len); }

/* --- strings ---------------------------------------------------------------------- */

void qf_string_escape_into(Buf *b, const char *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char) bytes[i];
        switch (ch) {
        case '"':  qf_buf_puts(b, "\\\""); break;
        case '\\': qf_buf_puts(b, "\\\\"); break;
        case '\n': qf_buf_puts(b, "\\n"); break;
        case '\t': qf_buf_puts(b, "\\t"); break;
        case '\r': qf_buf_puts(b, "\\r"); break;
        default:
            if (ch < 0x20 || ch == 0x7f) qf_buf_printf(b, "\\u%04x", ch);
            else qf_buf_putc(b, (char) ch);
        }
    }
}

Sym qf_string_canonical(Front *f, const char *bytes, size_t n)
{
    Buf b = { 0 };
    qf_buf_putc(&b, '"');
    qf_string_escape_into(&b, bytes, n);
    qf_buf_putc(&b, '"');
    Sym r = qf_intern(f, b.s, b.len);
    qf_buf_free(&b);
    return r;
}

/* --- diagnostics ------------------------------------------------------------------- */

void qf_diag(Front *f, bool error, Loc loc, const char *fmt, ...)
{
    Buf b = { 0 };
    if (loc.file) qf_buf_printf(&b, "%s:%d:%d: ", loc.file, loc.line, loc.col);
    qf_buf_puts(&b, error ? "error: " : "warning: ");
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *msg = malloc((size_t) n + 1);
    if (!msg) qf_oom();
    va_start(ap, fmt);
    vsnprintf(msg, (size_t) n + 1, fmt, ap);
    va_end(ap);
    qf_buf_puts(&b, msg);
    free(msg);
    Diag d = { error, b.s, loc };
    VEC_PUSH(f->diags, d);
    if (error) f->nerrors++;
}

/* --- the front end --------------------------------------------------------------------- */

Front *qf_new(void)
{
    Front *f = calloc(1, sizeof *f);
    if (!f) qf_oom();
    f->arena = qf_arena_new();
    f->store.arena = qf_arena_new();
    f->tick_scale = -6;          /* the default tick of a world: 1 µs */
    return f;
}

static void free_records(ArithVec *v)
{
    for (size_t i = 0; i < v->n; i++) {
        VEC_FREE(v->v[i].path);
        for (size_t k = 0; k < v->v[i].tags.n; k++) VEC_FREE(v->v[i].tags.v[k].path);
        VEC_FREE(v->v[i].tags);
    }
    VEC_FREE(*v);
}

void qf_free(Front *f)
{
    if (!f) return;
    for (size_t i = 0; i < f->diags.n; i++) free(f->diags.v[i].msg);
    VEC_FREE(f->diags);
    VEC_FREE(f->lets);
    /* stage 2 */
    qf_op_free(f->op);
    VEC_FREE(f->decls);
    VEC_FREE(f->kinds);
    VEC_FREE(f->dims);
    VEC_FREE(f->bases);
    for (size_t t = 0; t < f->types.n; t++) {
        TypeInfo *ti = &f->types.v[t];
        VEC_FREE(ti->parents);
        VEC_FREE(ti->children);
        free(ti->code);
        VEC_FREE(ti->feats);
        VEC_FREE(ti->decls);
        VEC_FREE(ti->members);
        free_records(&ti->ariths);
        for (size_t k = 0; k < ti->defaults.n; k++) VEC_FREE(ti->defaults.v[k].path);
        VEC_FREE(ti->defaults);
        qf_refs_free(&ti->refs);
    }
    VEC_FREE(f->types);
    for (size_t i = 0; i < f->feats.n; i++) VEC_FREE(f->feats.v[i].intros);
    VEC_FREE(f->feats);
    for (size_t i = 0; i < f->derived.n; i++) VEC_FREE(f->derived.v[i].path);
    VEC_FREE(f->derived);
    for (size_t i = 0; i < f->insts.n; i++) {
        InstInfo *in = &f->insts.v[i];
        VEC_FREE(in->decls);
        for (size_t k = 0; k < in->tags.n; k++) VEC_FREE(in->tags.v[k].path);
        VEC_FREE(in->tags);
        free_records(&in->ariths);
        for (size_t k = 0; k < in->defaults.n; k++) VEC_FREE(in->defaults.v[k].path);
        VEC_FREE(in->defaults);
        qf_refs_free(&in->refs);
    }
    VEC_FREE(f->insts);
    u64map_free(&f->declidx);
    for (size_t i = 0; i < f->disjs.n; i++) {
        VEC_FREE(f->disjs.v[i].altx);
        VEC_FREE(f->disjs.v[i].alts);
    }
    VEC_FREE(f->disjs);
    VEC_FREE(f->listelems);
    for (size_t i = 0; i < f->buildlog.n; i++) free(f->buildlog.v[i]);
    VEC_FREE(f->buildlog);
    free(f->bittype);
    u64map_free(&f->kindidx);
    u64map_free(&f->typeidx);
    u64map_free(&f->codeidx);
    u64map_free(&f->meetmemo);
    u64map_free(&f->featidx);
    u64map_free(&f->labelidx);
    u64map_free(&f->plainidx);
    u64map_free(&f->instidx);
    u64map_free(&f->disjidx);
    u64map_free(&f->cmpidx);
    qf_arena_free(f->store.arena);
    for (size_t i = 0; i < f->syms.cap; i++) free((void *) f->syms.slots[i]);
    free(f->syms.slots);
    qf_arena_free(f->arena);
    free(f);
}

/* --- hashing ------------------------------------------------------------------- */

uint64_t qf_hash64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

uint64_t qf_fnv1a(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* --- symbols ------------------------------------------------------------------- */

/* --- u64 map ----------------------------------------------------------------------- */

static void u64map_grow(U64Map *m)
{
    size_t ncap = m->cap ? m->cap * 2 : 64;
    U64Map n = { qf_xcalloc(ncap, 8), qf_xcalloc(ncap, 8), qf_xcalloc(ncap, 1), ncap, 0 };
    for (size_t i = 0; i < m->cap; i++)
        if (m->used[i]) u64map_put(&n, m->keys[i], m->vals[i]);
    u64map_free(m);
    *m = n;
}

bool u64map_get(const U64Map *m, uint64_t key, uint64_t *val)
{
    if (!m->cap) return false;
    size_t j = qf_hash64(key) & (m->cap - 1);
    while (m->used[j]) {
        if (m->keys[j] == key) {
            if (val) *val = m->vals[j];
            return true;
        }
        j = (j + 1) & (m->cap - 1);
    }
    return false;
}

void u64map_put(U64Map *m, uint64_t key, uint64_t val)
{
    if ((m->count + 1) * 2 > m->cap) u64map_grow(m);
    size_t j = qf_hash64(key) & (m->cap - 1);
    while (m->used[j]) {
        if (m->keys[j] == key) {
            m->vals[j] = val;
            return;
        }
        j = (j + 1) & (m->cap - 1);
    }
    m->used[j] = 1;
    m->keys[j] = key;
    m->vals[j] = val;
    m->count++;
}

void u64map_clear(U64Map *m)
{
    if (m->cap) memset(m->used, 0, m->cap);
    m->count = 0;
}

void u64map_free(U64Map *m)
{
    free(m->keys);
    free(m->vals);
    free(m->used);
    memset(m, 0, sizeof *m);
}

/* --- strings ---------------------------------------------------------------------- */

/* Decodes a canonical string (the only escapes are those qf_string_canonical
 * writes); a name without quotes, such as true, is copied as it is. */
void qf_string_decode(const char *q, Buf *out)
{
    qf_buf_puts(out, "");
    if (*q != '"') { qf_buf_puts(out, q); return; }
    for (q++; *q && !(q[0] == '"' && q[1] == 0); q++) {
        if (*q != '\\' || !q[1]) { qf_buf_putc(out, *q); continue; }
        q++;
        switch (*q) {
        case 'n': qf_buf_putc(out, '\n'); break;
        case 't': qf_buf_putc(out, '\t'); break;
        case 'r': qf_buf_putc(out, '\r'); break;
        case 'u': {
            unsigned v = 0;
            for (int k = 1; k <= 4 && q[k]; k++) {
                char h = q[k];
                v = v * 16 + (unsigned) (h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
            }
            qf_buf_putc(out, (char) v);       /* canonical \u escapes are control bytes only */
            q += 4;
            break;
        }
        default: qf_buf_putc(out, *q);        /* \" and \\ */
        }
    }
}

/* --- diagnostics ------------------------------------------------------------------- */

Sym qf_sym(Front *f, const char *s)
{
    return qf_intern(f, s, strlen(s));
}
