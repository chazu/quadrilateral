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

struct QfArena { Chunk *top; };

QfArena *qf_arena_new(void)
{
    QfArena *a = calloc(1, sizeof *a);
    if (!a) qf_oom();
    return a;
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
    free(a);
}

void *qf_alloc(QfArena *a, size_t n)
{
    n = (n + 15) & ~(size_t) 15;
    Chunk *ch = a->top;
    if (!ch || ch->used + n > ch->cap) {
        size_t cap = n > CHUNK_SIZE ? n : CHUNK_SIZE;
        Chunk *nc = malloc(sizeof(Chunk) + cap);
        if (!nc) qf_oom();
        nc->cap = cap;
        nc->prev = ch;
        nc->used = 0;
        a->top = nc;
        ch = nc;
    }
    void *p = ch->data + ch->used;
    ch->used += n;
    memset(p, 0, n);
    return p;
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
    return f;
}

void qf_free(Front *f)
{
    if (!f) return;
    for (size_t i = 0; i < f->diags.n; i++) free(f->diags.v[i].msg);
    VEC_FREE(f->diags);
    VEC_FREE(f->lets);
    for (size_t i = 0; i < f->syms.cap; i++) free((void *) f->syms.slots[i]);
    free(f->syms.slots);
    qf_arena_free(f->arena);
    free(f);
}
