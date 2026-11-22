#include "front/front.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The lexer and the parser of Quadrilateral source (ADR 0011 section 1,
 * ADR 0014).
 *
 * Borrowed from Triangle's parse.c (bruggadung f425574), under ADR 0003.
 * Changes from Triangle: a number keeps its unit as text, the unit can be
 * compound (1000J/K), and a number can have an exponent (1e-5); an attribute can have any name; the parser does not
 * check function names or regular expressions; ref(n) and @embed are gone. */

typedef enum {
    T_EOF, T_IDENT, T_DEF, T_STRING, T_NUMBER, T_TAG, T_ATTR,
    T_COLON, T_COMMA, T_LBRACE, T_RBRACE, T_LBRACK, T_RBRACK, T_LPAREN, T_RPAREN,
    T_AND, T_OR, T_STAR, T_GE, T_LE, T_GT, T_LT, T_ELLIPSIS, T_PLUS, T_MINUS, T_SLASH,
    T_MATCH, T_NMATCH, T_REF, T_ASSIGN,
    T_EQ, T_NE, T_ANDAND, T_OROR, T_NOT,          /* only in an if's condition */
} TokKind;

typedef struct {
    TokKind k;
    const char *s;     /* start in source */
    size_t len;
    Loc loc;
    Sym attr_name;     /* T_ATTR */
    char *attr_args;
    Sym str;           /* T_STRING: its canonical spelling (qf_string_canonical); NULL if interpolated */
    int ninterp;       /* T_STRING: its \(...) parts, between ninterp + 1 literal pieces */
    const char **ilits;
    struct IPart *iparts;
} Tok;

struct IPart { const char *src; size_t len; Loc loc; };   /* an interpolation's source, where it starts */

typedef struct { int line; const char *s; size_t len; } Comment;   /* a comment alone on its line */

/* A let's binding. Its uses are X_LET nodes until the parser replaces each
 * with the bound expression (resolve_lets). */
struct LetBind {
    Sym name;
    X *value;            /* NULL until its expression is parsed */
    Loc loc;
    int state;           /* 0, 1 while being resolved, 2 resolved */
};

typedef VEC(struct LetBind *) Scope;   /* the lets of one struct, or of a file */

typedef struct Parser Parser;
struct Parser {
    Front *c;
    Sym file;
    Sym pkg;
    VEC(Tok) toks;
    size_t pos;
    bool error;
    VEC(Comment) comments;
    struct { const char **v; size_t n, cap; } ilits;     /* scratch while lexing an interpolated string */
    struct { struct IPart *v; size_t n, cap; } iparts;
    VEC(Scope *) scopes;                                 /* the lets in scope, innermost last; NULL for none */
    Parser *outer;                                       /* an interpolation's parser: the string's */
    bool has_let, has_ref;                               /* the source has a let, a reference: work to do */
    bool has_comp;                                       /* a comprehension or a computed label */
    VEC(CompVar *) vars;                                 /* comprehension variables in scope, innermost last */
};

/* The length of the exponent at s (e-8, E+3, e27), or 0. */
static size_t exp_len(const char *s)
{
    if (*s != 'e' && *s != 'E') return 0;
    size_t n = s[1] == '+' || s[1] == '-' ? 2 : 1;
    if (!isdigit((unsigned char) s[n])) return 0;
    while (isdigit((unsigned char) s[n])) n++;
    return n;
}

/* The length of the unit at s, or 0 (ADR 0014). A unit is terms joined by
 * '*' or '/'. A term is a symbol, then an optional exponent (^2, ^-1). A '*'
 * or a '/' continues the unit only when a letter follows it at once, so
 * 10mm*2 and 10m / x are arithmetic. *bad: a '^' has no digits. */
static size_t unit_len(const char *s, bool *bad)
{
    const char *s0 = s;
    *bad = false;
    for (bool more = isalpha((unsigned char) *s); more;) {
        while (isalnum((unsigned char) *s) || *s == '_') s++;
        if (*s == '^') {
            s++;
            if (*s == '-') s++;
            if (!isdigit((unsigned char) *s)) { *bad = true; break; }
            while (isdigit((unsigned char) *s)) s++;
        }
        more = (*s == '*' || *s == '/') && isalpha((unsigned char) s[1]);
        if (more) s++;
    }
    return (size_t) (s - s0);
}

/* Where the unit of a number token starts: after the digits and the exponent. */
static const char *unit_start(const Tok *t)
{
    const char *s = t->s, *end = t->s + t->len;
    if (*s == '-') s++;
    while (s < end && (isdigit((unsigned char) *s) || *s == '.')) s++;
    return s < end ? s + exp_len(s) : s;
}

static bool comp_at(Parser *p, size_t i);

/* An author who knows SI notation writes 20 W/K. The name then reads as a
 * label, and the error is at the name or at the token after it. If a name
 * follows a number that has no unit, on the same line, the hint shows the
 * attached form. */
static void spaced_unit_hint(Parser *p, Loc loc, Buf *msg)
{
    size_t i = 0;
    while (i < p->toks.n && (p->toks.v[i].loc.line != loc.line || p->toks.v[i].loc.col != loc.col)) i++;
    if (i == p->toks.n) return;
    for (size_t j = i; j >= 1 && j + 1 >= i; j--) {
        const Tok *name = &p->toks.v[j], *num = name - 1;
        if (name->k != T_IDENT || num->k != T_NUMBER || name->loc.line != num->loc.line) continue;
        if (unit_start(num) != num->s + num->len || comp_at(p, j)) continue;
        bool bad;
        int n = (int) unit_len(name->s, &bad);
        if (!n) continue;
        qf_buf_printf(msg, ". If '%.*s' is a unit, write it directly after the number: %.*s%.*s",
                      n, name->s, (int) num->len, num->s, n, name->s);
        return;
    }
}

static void perr(Parser *p, Loc loc, const char *text)
{
    if (p->error) return;
    p->error = true;
    Buf msg = { 0 };
    qf_buf_puts(&msg, text);
    spaced_unit_hint(p, loc, &msg);
    qf_diag(p->c, true, loc, "%s", msg.s);
    qf_buf_free(&msg);
}

/* --- lexer --------------------------------------------------------------------- */

static bool operand_end(TokKind k)
{
    return k == T_NUMBER || k == T_IDENT || k == T_DEF || k == T_TAG || k == T_STRING || k == T_RPAREN ||
           k == T_RBRACE || k == T_RBRACK || k == T_REF;
}

/* A literal piece of an interpolated string, kept with the content. */
static const char *piece(Parser *p, const char *s, size_t n)
{
    char *r = qf_alloc(p->c->arena, n + 1);
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* #"..."#: one or more #s, then a quote */
static bool raw_start(const char *s)
{
    while (*s == '#') s++;
    return *s == '"';
}

static bool hashes_at(const char *s, int n)
{
    for (int k = 0; k < n; k++) if (s[k] != '#') return false;
    return true;
}

static void put_utf8(Buf *b, uint32_t cp)
{
    if (cp < 0x80) qf_buf_putc(b, (char) cp);
    else if (cp < 0x800) { qf_buf_putc(b, (char) (0xC0 | cp >> 6)); qf_buf_putc(b, (char) (0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        qf_buf_putc(b, (char) (0xE0 | cp >> 12));
        qf_buf_putc(b, (char) (0x80 | ((cp >> 6) & 0x3F)));
        qf_buf_putc(b, (char) (0x80 | (cp & 0x3F)));
    } else {
        qf_buf_putc(b, (char) (0xF0 | cp >> 18));
        qf_buf_putc(b, (char) (0x80 | ((cp >> 12) & 0x3F)));
        qf_buf_putc(b, (char) (0x80 | ((cp >> 6) & 0x3F)));
        qf_buf_putc(b, (char) (0x80 | (cp & 0x3F)));
    }
}

/* Decodes the escape whose letter *s points at (past the backslash) into
 * out, advancing *s past it. Returns NULL, or an error message. CUE's escapes
 * for strings: \a \b \f \n \r \t \v \\ \/ \" \uXXXX \UXXXXXXXX. */
static const char *string_escape(const char **sp, Buf *out)
{
    const char *s = *sp;
    char e = *s++;
    uint32_t cp = 0;
    switch (e) {
    case 'a': qf_buf_putc(out, '\a'); break;
    case 'b': qf_buf_putc(out, '\b'); break;
    case 'f': qf_buf_putc(out, '\f'); break;
    case 'n': qf_buf_putc(out, '\n'); break;
    case 'r': qf_buf_putc(out, '\r'); break;
    case 't': qf_buf_putc(out, '\t'); break;
    case 'v': qf_buf_putc(out, '\v'); break;
    case '\\': case '/': case '"': qf_buf_putc(out, e); break;
    case 'u': case 'U': {
        int n = e == 'u' ? 4 : 8;
        for (int k = 0; k < n; k++, s++) {
            if (!isxdigit((unsigned char) *s)) return e == 'u' ? "\\u takes four hex digits" : "\\U takes eight hex digits";
            cp = cp * 16 + (uint32_t) (isdigit((unsigned char) *s) ? *s - '0' : (*s | 0x20) - 'a' + 10);
        }
        if (cp == 0) return "a string may not contain U+0000";
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return "not a Unicode scalar value";
        put_utf8(out, cp);
        break;
    }
    case '(': return "string interpolation is not supported";
    default: return "unknown escape in a string; the escapes are \\a \\b \\f \\n \\r \\t \\v \\\\ \\/ \\\" \\u \\U";
    }
    *sp = s;
    return NULL;
}

static bool lex_at(Parser *p, const char *src, int line, int col);

static bool name_start(char ch) { return isalpha((unsigned char) ch) || ch == '_'; }

/* The length of the path segments at s: .name and [n], as many as follow. */
static size_t path_segments(const char *s)
{
    const char *t = s;
    for (;;) {
        if (t[0] == '.' && name_start(t[1])) {
            t++;
            while (isalnum((unsigned char) *t) || *t == '_') t++;
        } else if (t[0] == '[' && isdigit((unsigned char) t[1])) {
            const char *u = t + 1;
            while (isdigit((unsigned char) *u)) u++;
            if (*u != ']') break;
            t = u + 1;
        } else break;
    }
    return (size_t) (t - s);
}

static bool lex(Parser *p, const char *src)
{
    return lex_at(p, src, 1, 1);
}

static bool lex_at(Parser *p, const char *src, int line, int col)
{
    const char *s = src;
#define ADV() do { if (*s == '\n') { line++; col = 1; } else col++; s++; } while (0)
    while (*s) {
        if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') { ADV(); continue; }
        if (s[0] == '/' && s[1] == '/') {
            Comment cm = { line, s + 2, 0 };
            bool alone = !p->toks.n || p->toks.v[p->toks.n - 1].loc.line != line;
            while (*s && *s != '\n') ADV();
            cm.len = (size_t) (s - cm.s);
            if (alone) VEC_PUSH(p->comments, cm);
            continue;
        }
        Tok t = { 0 };
        t.s = s;
        t.loc = (Loc) { p->file, line, col };
        char ch = *s;
        if (isalpha((unsigned char) ch) || ch == '_' ||
            (ch == '#' && (isalpha((unsigned char) s[1]) || s[1] == '_'))) {
            t.k = ch == '#' ? T_DEF : T_IDENT;
            ADV();
            while (isalnum((unsigned char) *s) || *s == '_') ADV();
            /* a qualified name, one package deep: pkg.#Type, pkg.blueprint */
            if (ch != '#' && *s == '.' && s[1] == '#') {
                ADV();
                t.k = T_DEF;
                ADV();
                if (!name_start(*s)) {
                    perr(p, t.loc, "expected a name after the package");
                    return false;
                }
                while (isalnum((unsigned char) *s) || *s == '_') ADV();
            }
            /* then a path into it: wrench_19.size, tools.wrench_250.head.size, steps[0] */
            for (size_t n = path_segments(s); n; n--) ADV();
        } else if (ch == '.' && name_start(s[1])) {
            /* a reference: a path from the root of the declaration it is in */
            t.k = T_REF;
            p->has_ref = true;
            for (size_t n = path_segments(s); n; n--) ADV();
        } else if (ch == '$') {
            t.k = T_TAG;
            ADV();
            if (!isalnum((unsigned char) *s) && *s != '_') {
                perr(p, t.loc, "expected a tag name after '$'");
                return false;
            }
            while (isalnum((unsigned char) *s) || *s == '_') ADV();
        } else if (ch == '-' && p->toks.n && operand_end(p->toks.v[p->toks.n - 1].k)) {
            /* after an operand, '-' subtracts */
            t.k = T_MINUS;
            ADV();
        } else if (isdigit((unsigned char) ch) || (ch == '-' && isdigit((unsigned char) s[1]))) {
            t.k = T_NUMBER;
            ADV();
            while (isdigit((unsigned char) *s) || *s == '.') {
                if (*s == '.' && s[1] == '.') break;
                ADV();
            }
            for (size_t n = exp_len(s); n; n--) ADV();   /* 5.67e-8 */
            /* A unit follows the number with no space (ADR 0014): 250mm,
             * 1000J/K, 5kg*m^2/s^2. */
            bool bad;
            const char *unit = s;
            size_t ulen = unit_len(s, &bad);
            if (bad) {
                perr(p, t.loc, "expected digits after '^' in a unit");
                return false;
            }
            for (size_t n = ulen; n; n--) ADV();
            /* In 10m/sqrt(4), 10m/v.size and 10m/xs[0], the operator joined
             * a name to the unit. No valid source has this form. */
            if ((memchr(unit, '/', ulen) || memchr(unit, '*', ulen)) &&
                (*s == '(' || *s == '[' || (*s == '.' && name_start(s[1])))) {
                char msg[256];
                snprintf(msg, sizeof msg, "'%c' directly after the unit '%.*s'. To multiply or divide by a name, "
                         "put spaces around the operator", *s, (int) (ulen < 80 ? ulen : 80), unit);
                perr(p, t.loc, msg);
                return false;
            }
        } else if (ch == '"' || (ch == '#' && raw_start(s))) {
            /* "..." with escapes, or #"..."# raw, as in CUE: with n #s, only
             * \ followed by n #s escapes, and "#...# (n of them) closes */
            t.k = T_STRING;
            int hashes = 0;
            while (*s == '#') { hashes++; ADV(); }
            ADV();
            Buf raw = { 0 };
            qf_buf_puts(&raw, "");
            for (;;) {
                if (!*s || *s == '\n') { qf_buf_free(&raw); perr(p, t.loc, "unterminated string"); return false; }
                if (*s == '"' && hashes_at(s + 1, hashes)) {
                    ADV();
                    for (int k = 0; k < hashes; k++) ADV();
                    break;
                }
                if (*s != '\\' || !hashes_at(s + 1, hashes)) { qf_buf_putc(&raw, *s); ADV(); continue; }
                Loc eloc = { p->file, line, col };
                ADV();
                for (int k = 0; k < hashes; k++) ADV();
                if (*s == '(') {
                    /* \(expr): the literal so far is a piece; the expression runs to its ')' */
                    ADV();
                    VEC_PUSH(p->ilits, piece(p, raw.s, raw.len));
                    raw.len = 0;
                    raw.s[0] = 0;
                    struct IPart part = { s, 0, { p->file, line, col } };
                    int depth = 1;
                    while (*s && *s != '\n' && depth) {
                        if (*s == '"') {                        /* a string inside: skip it whole */
                            ADV();
                            while (*s && *s != '"' && *s != '\n') { if (*s == '\\' && s[1]) ADV(); ADV(); }
                            if (*s == '"') ADV();
                            continue;
                        }
                        if (*s == '(') depth++;
                        if (*s == ')' && --depth == 0) break;
                        ADV();
                    }
                    if (*s != ')') { qf_buf_free(&raw); perr(p, eloc, "unterminated interpolation: \\( without its )"); return false; }
                    part.len = (size_t) (s - part.src);
                    ADV();
                    VEC_PUSH(p->iparts, part);
                    continue;
                }
                const char *before = s;
                const char *err = string_escape(&s, &raw);
                if (err) {
                    qf_buf_free(&raw);
                    TokKind before_tok = p->toks.n ? p->toks.v[p->toks.n - 1].k : T_EOF;
                    if (before_tok == T_MATCH || before_tok == T_NMATCH) {
                        char msg[200];
                        snprintf(msg, sizeof msg, "%s (in a regex, write a raw string such as #\"\\d+\"#, or double the backslash)", err);
                        perr(p, eloc, msg);
                    } else perr(p, eloc, err);
                    return false;
                }
                col += (int) (s - before);      /* an escape never spans lines */
            }
            if (p->iparts.n) {
                VEC_PUSH(p->ilits, piece(p, raw.s, raw.len));
                t.ninterp = (int) p->iparts.n;
                t.ilits = qf_alloc(p->c->arena, (p->iparts.n + 1) * sizeof *t.ilits);
                memcpy(t.ilits, p->ilits.v, (p->iparts.n + 1) * sizeof *t.ilits);
                t.iparts = qf_alloc(p->c->arena, p->iparts.n * sizeof *t.iparts);
                memcpy(t.iparts, p->iparts.v, p->iparts.n * sizeof *t.iparts);
                p->ilits.n = p->iparts.n = 0;
            } else t.str = qf_string_canonical(p->c, raw.s, raw.len);
            qf_buf_free(&raw);
        } else if (ch == '@') {
            t.k = T_ATTR;
            ADV();
            const char *n0 = s;
            while (isalnum((unsigned char) *s) || *s == '_') ADV();
            if (s == n0) { perr(p, t.loc, "expected an attribute name after '@'"); return false; }
            t.attr_name = qf_intern(p->c, n0, (size_t) (s - n0));
            if (*s == '(') {
                ADV();
                const char *a0 = s;
                int depth = 1;
                while (*s && depth) {
                    if (*s == '(') depth++;
                    if (*s == ')') depth--;
                    if (depth) ADV();
                }
                if (!*s) { perr(p, t.loc, "unterminated attribute"); return false; }
                size_t n = (size_t) (s - a0);
                t.attr_args = qf_alloc(p->c->arena, n + 1);
                memcpy(t.attr_args, a0, n);
                ADV();
            } else {
                t.attr_args = qf_alloc(p->c->arena, 1);
            }
        } else {
            ADV();
            switch (ch) {
            case ':': t.k = T_COLON; break;
            case ',': t.k = T_COMMA; break;
            case '{': t.k = T_LBRACE; break;
            case '}': t.k = T_RBRACE; break;
            case '[': t.k = T_LBRACK; break;
            case ']': t.k = T_RBRACK; break;
            case '(': t.k = T_LPAREN; break;
            case ')': t.k = T_RPAREN; break;
            case '&': if (*s == '&') { ADV(); t.k = T_ANDAND; } else t.k = T_AND; break;
            case '|': if (*s == '|') { ADV(); t.k = T_OROR; } else t.k = T_OR; break;
            case '*': t.k = T_STAR; break;
            case '+': t.k = T_PLUS; break;
            case '/': t.k = T_SLASH; break;
            case '>': if (*s == '=') { ADV(); t.k = T_GE; } else t.k = T_GT; break;
            case '<': if (*s == '=') { ADV(); t.k = T_LE; } else t.k = T_LT; break;
            case '=': case '!':
                if (*s == '~') { ADV(); t.k = ch == '=' ? T_MATCH : T_NMATCH; break; }
                if (*s == '=') { ADV(); t.k = ch == '=' ? T_EQ : T_NE; break; }
                t.k = ch == '=' ? T_ASSIGN : T_NOT;
                break;
            case '.':
                if (s[0] == '.' && s[1] == '.') { ADV(); ADV(); t.k = T_ELLIPSIS; break; }
                /* fallthrough */
            default: {
                char msg[160];
                snprintf(msg, sizeof msg, "unexpected character '%c'%s", ch, ch != '^' ? "" :
                         ". A '^' is valid only in a unit, and a unit follows its number with no space: 5kg*m^2");
                perr(p, t.loc, msg);
                return false;
            }
            }
        }
        t.len = (size_t) (s - t.s);
        VEC_PUSH(p->toks, t);
    }
    Tok e = { 0 };
    e.k = T_EOF;
    e.loc = (Loc) { p->file, line, col };
    VEC_PUSH(p->toks, e);
    for (size_t i = 0; i + 2 < p->toks.n && !p->has_let; i++)
        if ((p->toks.v[i].k == T_IDENT && p->toks.v[i].len == 3 && !memcmp(p->toks.v[i].s, "let", 3) &&
             p->toks.v[i + 1].k == T_IDENT && p->toks.v[i + 2].k == T_ASSIGN) ||
            (p->toks.v[i].k == T_IDENT && p->toks.v[i].len > 1 && p->toks.v[i].s[0] == '_' && p->toks.v[i + 1].k == T_COLON))
            p->has_let = true;
    return true;
#undef ADV
}

/* --- parser ------------------------------------------------------------------- */

static Tok *peek(Parser *p, size_t k)
{
    size_t i = p->pos + k;
    if (i >= p->toks.n) i = p->toks.n - 1;
    return &p->toks.v[i];
}

static Tok *next(Parser *p)
{
    Tok *t = peek(p, 0);
    if (p->pos < p->toks.n - 1) p->pos++;
    return t;
}

static bool expect(Parser *p, TokKind k, const char *what)
{
    if (peek(p, 0)->k != k) {
        char msg[96];
        snprintf(msg, sizeof msg, "expected %s", what);
        perr(p, peek(p, 0)->loc, msg);
        return false;
    }
    next(p);
    return true;
}

static X *xnew(Parser *p, XKind k, Loc loc)
{
    X *x = qf_alloc(p->c->arena, sizeof(X));
    x->k = k;
    x->loc = loc;
    if (k == X_STRUCT) x->u.st.pkg = p->pkg;     /* its labels are first the package's */
    return x;
}

static Sym tok_sym(Parser *p, Tok *t)
{
    if (t->k == T_STRING) return t->str;
    return qf_intern(p->c, t->s, t->len);
}

static bool qualified(Tok *t)
{
    return memchr(t->s, '.', t->len) != NULL || memchr(t->s, '[', t->len) != NULL;
}

/* pkg.#Type: a package's type, and nothing past it. */
static bool package_type(const Tok *t)
{
    const char *dot = memchr(t->s, '.', t->len);
    return t->k == T_DEF && t->s[0] != '#' && dot && dot[1] == '#' &&
           !memchr(dot + 1, '.', (size_t) (t->s + t->len - dot - 1)) && !memchr(t->s, '[', t->len);
}

static bool tok_is(const Tok *t, const char *word)
{
    size_t n = strlen(word);
    return t->k == T_IDENT && t->len == n && memcmp(t->s, word, n) == 0;
}

/* --- lets ------------------------------------------------------------------------ */

static struct LetBind *let_lookup(Parser *p, Sym name)
{
    for (Parser *q = p; q; q = q->outer)
        for (size_t i = q->scopes.n; i-- > 0;)
            for (size_t k = 0; k < q->scopes.v[i]->n; k++)
                if (q->scopes.v[i]->v[k]->name == name) return q->scopes.v[i]->v[k];
    return NULL;
}

/* A hidden field, _name: (CUE's) is a let by another spelling. */
static bool hidden_at(Parser *p, size_t i)
{
    Tok *t = &p->toks.v[i];
    return i + 1 < p->toks.n && t->k == T_IDENT && t->len > 1 && t->s[0] == '_' && !qualified(t) &&
           p->toks.v[i + 1].k == T_COLON;
}

static bool let_at(Parser *p, size_t i)
{
    if (!p->has_let) return false;
    if (hidden_at(p, i)) return true;
    return i + 2 < p->toks.n && tok_is(&p->toks.v[i], "let") && p->toks.v[i + 1].k == T_IDENT &&
           p->toks.v[i + 2].k == T_ASSIGN;
}

/* A scope's lets are known before any of it is parsed, so a use may come
 * before its let, as fields may. `from` is the scope's first token. */
static Scope *open_scope(Parser *p, size_t from)
{
    if (!p->has_let) return NULL;            /* nothing to bind, as in most sources */
    Scope *sc = calloc(1, sizeof *sc);
    if (!sc) qf_oom();
    int depth = 0;
    for (size_t i = from; i < p->toks.n && !p->error; i++) {
        TokKind k = p->toks.v[i].k;
        if (k == T_EOF) break;
        if (k == T_LBRACE || k == T_LBRACK || k == T_LPAREN) depth++;
        else if (k == T_RBRACE || k == T_RBRACK || k == T_RPAREN) { if (depth-- == 0) break; }
        else if (depth == 0 && let_at(p, i)) {
            bool hidden = hidden_at(p, i);
            Tok *n = &p->toks.v[hidden ? i : i + 1];
            if (qualified(n)) { perr(p, n->loc, "a let binds a plain name"); break; }
            Sym name = tok_sym(p, n);
            static const char *const reserved[] = { "_", "true", "false", "let", "package" };
            for (size_t r = 0; r < sizeof reserved / sizeof *reserved; r++)
                if (!strcmp(name, reserved[r])) {
                    char msg[96];
                    snprintf(msg, sizeof msg, "'%s' can't be a let's name", name);
                    perr(p, n->loc, msg);
                }
            struct LetBind *other = NULL;
            for (size_t k2 = 0; k2 < sc->n && !other; k2++) if (sc->v[k2]->name == name) other = sc->v[k2];
            if (!other) other = let_lookup(p, name);
            if (other) {
                char msg[200];
                snprintf(msg, sizeof msg, "let %s: the name is already bound, by the let at %d:%d", name,
                         other->loc.line, other->loc.col);
                perr(p, n->loc, msg);
                break;
            }
            struct LetBind *lb = qf_alloc(p->c->arena, sizeof *lb);
            memset(lb, 0, sizeof *lb);
            lb->name = name;
            lb->loc = n->loc;
            VEC_PUSH(*sc, lb);
            LetName ln = { name, p->pkg, n->loc, false };
            VEC_PUSH(p->c->lets, ln);
            i += hidden ? 1 : 2;
        }
    }
    VEC_PUSH(p->scopes, sc);
    return sc;
}

static void close_scope(Parser *p, Scope *sc)
{
    if (!sc) return;
    p->scopes.n--;
    VEC_FREE(*sc);
    free(sc);
}

static X *parse_expr(Parser *p);

/* let name = expr, its binding already made by open_scope. */
static bool parse_let(Parser *p)
{
    bool hidden = hidden_at(p, p->pos);
    if (!hidden) next(p);
    Tok *n = next(p);
    next(p);
    struct LetBind *lb = let_lookup(p, tok_sym(p, n));
    X *v = parse_expr(p);
    if (!v) return false;
    if (lb) lb->value = v;
    return true;
}

/* Replaces each use of a let with a copy of the expression it binds. The copy
 * shares the expression's parts: it is built afresh wherever it stands, so
 * values copy, while its tags and references, like any, share. */
static bool resolve_lets(Parser *p, X *x)
{
    if (!x || p->error) return !p->error;
    switch (x->k) {
    case X_LET: {
        struct LetBind *lb = x->u.let;
        if (lb->state == 1) {
            char msg[128];
            snprintf(msg, sizeof msg, "let %s refers to itself", lb->name);
            perr(p, lb->loc, msg);
            return false;
        }
        if (lb->state == 0) {
            lb->state = 1;
            if (!resolve_lets(p, lb->value)) return false;
            lb->state = 2;
        }
        *x = *lb->value;
        return true;
    }
    case X_STRUCT:
        for (int i = 0; i < x->u.st.n; i++) {
            XField *f = &x->u.st.fields[i];
            if (!resolve_lets(p, f->value) || !resolve_lets(p, f->dlabel)) return false;
        }
        return true;
    case X_COMP:
        for (int i = 0; i < x->u.comp.n; i++) if (!resolve_lets(p, x->u.comp.clauses[i].expr)) return false;
        return resolve_lets(p, x->u.comp.body);
    case X_CMP: case X_LOGIC:
        return resolve_lets(p, x->u.ar.a) && resolve_lets(p, x->u.ar.b);
    case X_NOT:
        return resolve_lets(p, x->u.sub);
    case X_CALL:
        for (int i = 0; i < x->u.call.n; i++) if (!resolve_lets(p, x->u.call.args[i])) return false;
        return true;
    case X_BOUND:
        return resolve_lets(p, x->u.bound.of);
    case X_LIST:
        for (int i = 0; i < x->u.list.n; i++) if (!resolve_lets(p, x->u.list.items[i])) return false;
        return resolve_lets(p, x->u.list.elem);
    case X_AND: case X_OR:
        return resolve_lets(p, x->u.bin.a) && resolve_lets(p, x->u.bin.b);
    case X_DEFAULT:
        return resolve_lets(p, x->u.sub);
    case X_ARITH:
        return resolve_lets(p, x->u.ar.a) && resolve_lets(p, x->u.ar.b);
    case X_INTERP:
        for (int i = 0; i < x->u.in.n; i++) if (!resolve_lets(p, x->u.in.parts[i])) return false;
        return true;
    default:
        return true;
    }
}

/* --- references ------------------------------------------------------------------ */

/* A reference .a.b is a tag named ".a.b" (no tag a user writes starts with
 * '.'), and the declaration gains the conjunct { a: { b: .a.b } }, which puts
 * that tag at the path. The conjunct's structs are marked, so a type's
 * declaration introduces no features through them. */

typedef struct { Parser *p; Buf path; XPtrVec refs; } RefWalk;

static void collect_refs(RefWalk *w, X *x)
{
    if (!x || w->p->error) return;
    size_t keep = w->path.len;
    switch (x->k) {
    case X_TAG: {
        if (x->u.name[0] != '.') return;
        size_t n = strlen(x->u.name);
        const char *at = w->path.s ? w->path.s : "";
        if (w->path.len > n && !memcmp(at, x->u.name, n) && (at[n] == '.' || at[n] == '[')) {
            char msg[300];
            snprintf(msg, sizeof msg, "%s refers to a field that contains it (%s), which would make the structure cyclic",
                     x->u.name, at);
            perr(w->p, x->loc, msg);
            return;
        }
        for (size_t i = 0; i < w->refs.n; i++) if (w->refs.v[i]->u.name == x->u.name) return;
        VEC_PUSH(w->refs, x);
        return;
    }
    case X_STRUCT:
        for (int i = 0; i < x->u.st.n; i++) {
            XField *f = &x->u.st.fields[i];
            collect_refs(w, f->dlabel);
            if (field_comp(f)) { collect_refs(w, f->value); continue; }   /* its fields are this struct's */
            qf_buf_printf(&w->path, ".%s", f->label ? f->label : "*");
            collect_refs(w, f->value);
            w->path.len = keep;
            if (w->path.s) w->path.s[keep] = 0;
        }
        return;
    case X_COMP:
        for (int i = 0; i < x->u.comp.n; i++) collect_refs(w, x->u.comp.clauses[i].expr);
        if (x->u.comp.list) qf_buf_puts(&w->path, "[*]");
        if (x->u.comp.list || x->u.comp.body->k != X_STRUCT) collect_refs(w, x->u.comp.body);
        else {                                  /* its fields stand where the comprehension does */
            X *b = x->u.comp.body;
            for (int i = 0; i < b->u.st.n; i++) {
                XField *f = &b->u.st.fields[i];
                size_t k2 = w->path.len;
                collect_refs(w, f->dlabel);
                if (field_comp(f)) { collect_refs(w, f->value); continue; }
                qf_buf_printf(&w->path, ".%s", f->label ? f->label : "*");
                collect_refs(w, f->value);
                w->path.len = k2;
                if (w->path.s) w->path.s[k2] = 0;
            }
        }
        w->path.len = keep;
        if (w->path.s) w->path.s[keep] = 0;
        return;
    case X_CMP: case X_LOGIC:
        collect_refs(w, x->u.ar.a);
        collect_refs(w, x->u.ar.b);
        return;
    case X_NOT:
        collect_refs(w, x->u.sub);
        return;
    case X_CALL:
        for (int i = 0; i < x->u.call.n; i++) collect_refs(w, x->u.call.args[i]);
        return;
    case X_BOUND:
        collect_refs(w, x->u.bound.of);
        return;
    case X_LIST:
        for (int i = 0; i < x->u.list.n; i++) {
            qf_buf_printf(&w->path, "[%d]", i);
            collect_refs(w, x->u.list.items[i]);
            w->path.len = keep;
            if (w->path.s) w->path.s[keep] = 0;
        }
        collect_refs(w, x->u.list.elem);
        return;
    case X_AND: case X_OR:
        collect_refs(w, x->u.bin.a);
        collect_refs(w, x->u.bin.b);
        return;
    case X_DEFAULT:
        collect_refs(w, x->u.sub);
        return;
    case X_ARITH:
        collect_refs(w, x->u.ar.a);
        collect_refs(w, x->u.ar.b);
        return;
    case X_INTERP:
        for (int i = 0; i < x->u.in.n; i++) collect_refs(w, x->u.in.parts[i]);
        return;
    default:
        return;
    }
}

static X *one_field(Parser *p, const char *label, size_t n, X *value, Loc loc)
{
    X *s = xnew(p, X_STRUCT, loc);
    s->u.st.fields = qf_alloc(p->c->arena, sizeof(XField));
    memset(s->u.st.fields, 0, sizeof(XField));
    s->u.st.fields[0].label = qf_intern(p->c, label, n);
    s->u.st.fields[0].value = value;
    s->u.st.fields[0].loc = loc;
    s->u.st.n = 1;
    s->u.st.refs = true;
    return s;
}

/* { a: { b: <tag> } } for the tag .a.b; a list index [n] is n rests, then first. */
static X *ref_conjunct(Parser *p, X *tag)
{
    const char *segs[64];
    size_t lens[64];
    int nseg = 0;
    for (const char *s = tag->u.name; *s && nseg < 64;) {
        if (*s == '.') {
            const char *e = ++s;
            while (isalnum((unsigned char) *e) || *e == '_') e++;
            segs[nseg] = s;
            lens[nseg++] = (size_t) (e - s);
            s = e;
        } else if (*s == '[') {
            const char *e = ++s;
            while (isdigit((unsigned char) *e)) e++;
            segs[nseg] = s - 1;               /* starts with '[' */
            lens[nseg++] = (size_t) (e - s);
            s = e + 1;
        } else break;
    }
    if (nseg == 64) { perr(p, tag->loc, "a reference's path is too long"); return NULL; }
    X *inner = xnew(p, X_TAG, tag->loc);
    inner->u.name = tag->u.name;
    for (int i = nseg - 1; i >= 0; i--) {
        if (segs[i][0] != '[') { inner = one_field(p, segs[i], lens[i], inner, tag->loc); continue; }
        long k = strtol(segs[i] + 1, NULL, 10);
        if (lens[i] > 6 || k > 100000) { perr(p, tag->loc, "a list index in a reference is too large"); return NULL; }
        inner = one_field(p, "first", 5, inner, tag->loc);
        while (k-- > 0) inner = one_field(p, "rest", 4, inner, tag->loc);
    }
    return inner;
}

/* Whether an expression has comprehensions or computed labels to expand. */
static bool x_has_comp(const X *x)
{
    if (!x) return false;
    switch (x->k) {
    case X_COMP: return true;
    case X_STRUCT:
        for (int i = 0; i < x->u.st.n; i++)
            if (x->u.st.fields[i].dlabel || x_has_comp(x->u.st.fields[i].value)) return true;
        return false;
    case X_LIST:
        for (int i = 0; i < x->u.list.n; i++) if (x_has_comp(x->u.list.items[i])) return true;
        return x_has_comp(x->u.list.elem);
    case X_AND: case X_OR: return x_has_comp(x->u.bin.a) || x_has_comp(x->u.bin.b);
    case X_DEFAULT: return x_has_comp(x->u.sub);
    case X_ARITH: return x_has_comp(x->u.ar.a) || x_has_comp(x->u.ar.b);
    case X_CMP: case X_LOGIC: case X_NOT: return true;     /* evaluated by a later stage */
    case X_INTERP:
        for (int i = 0; i < x->u.in.n; i++) if (x_has_comp(x->u.in.parts[i])) return true;
        return false;
    case X_CALL: return true;
    case X_BOUND: return x->u.bound.of != NULL;
    default: return false;
    }
}

/* Finishes a declaration's or a runtime value's expression: lets replaced by
 * what they bind, and each reference given its place. */
static X *finish_expr(Parser *p, X *x)
{
    if (!x) return NULL;
    if (!p->has_ref && !p->has_let) return x;
    if (!resolve_lets(p, x)) return NULL;
    if (!p->has_ref) return x;
    RefWalk w = { p, { 0 }, { 0 } };
    collect_refs(&w, x);
    for (size_t i = 0; i < w.refs.n && !p->error; i++) {
        X *cj = ref_conjunct(p, w.refs.v[i]);
        if (!cj) break;
        X *a = xnew(p, X_AND, x->loc);
        a->u.bin.a = x;
        a->u.bin.b = cj;
        x = a;
    }
    qf_buf_free(&w.path);
    VEC_FREE(w.refs);
    return p->error ? NULL : x;
}

/* A label is a feature's name, or its package's and its name, where two
 * packages each have one: larder,
 * cooking.larder. */
static bool plain_label(Parser *p, Tok *t)
{
    if (!qualified(t)) return true;
    const char *dot = memchr(t->s, '.', t->len);
    if (dot && dot > t->s && dot + 1 < t->s + t->len && !memchr(dot + 1, '.', (size_t) (t->s + t->len - dot - 1)) &&
        !memchr(t->s, '[', t->len))
        return true;
    perr(p, t->loc, "a field label is a name, or a package's and a name (cooking.larder)");
    return false;
}

static void parse_attrs(Parser *p, Attr **attrs, int *n)
{
    VEC(Attr) v = { 0 };
    while (peek(p, 0)->k == T_ATTR) {
        Tok *t = next(p);
        Attr a = { t->attr_name, t->attr_args };
        VEC_PUSH(v, a);
    }
    if (v.n) {
        *attrs = qf_alloc(p->c->arena, v.n * sizeof(Attr));
        memcpy(*attrs, v.v, v.n * sizeof(Attr));
    }
    *n = (int) v.n;
    VEC_FREE(v);
}

/* A field value, allowing the path shorthand `a: b: c`. */
static X *parse_field_value(Parser *p, Attr **attrs, int *nattrs)
{
    if (peek(p, 0)->k == T_IDENT && peek(p, 1)->k == T_COLON) {
        Tok *lab = next(p);
        if (!plain_label(p, lab)) return NULL;
        next(p);
        XField f = { 0 };
        f.label = tok_sym(p, lab);
        f.loc = lab->loc;
        f.value = parse_field_value(p, &f.attrs, &f.nattrs);
        if (!f.value) return NULL;
        X *x = xnew(p, X_STRUCT, lab->loc);
        x->u.st.fields = qf_alloc(p->c->arena, sizeof(XField));
        x->u.st.fields[0] = f;
        x->u.st.n = 1;
        *attrs = NULL;
        *nattrs = 0;
        return x;
    }
    X *v = parse_expr(p);
    if (!v) return NULL;
    parse_attrs(p, attrs, nattrs);
    return v;
}

/* --- comprehensions --------------------------------------------------------------- */

static X *parse_interpolation(Parser *p, Tok *t);

static CompVar *var_lookup(Parser *p, const char *s, size_t n)
{
    for (Parser *q = p; q; q = q->outer)
        for (size_t i = q->vars.n; i-- > 0;)
            if (strlen(q->vars.v[i]->name) == n && !memcmp(q->vars.v[i]->name, s, n)) return q->vars.v[i];
    return NULL;
}

/* for x in, for k, v in; if (not a field called if) */
static bool comp_at(Parser *p, size_t i)
{
    Tok *t = &p->toks.v[i];
    if (t->k != T_IDENT || (t->len != 3 && t->len != 2)) return false;     /* for, if: most tokens are neither */
    if (i + 2 < p->toks.n && tok_is(t, "for") && p->toks.v[i + 1].k == T_IDENT &&
        (p->toks.v[i + 2].k == T_COMMA || p->toks.v[i + 2].k == T_IDENT))     /* for x in; for x of: an error there */
        return true;
    if (i + 1 < p->toks.n && tok_is(t, "if")) {
        TokKind k = p->toks.v[i + 1].k;
        return k != T_COLON && k != T_COMMA && k != T_RBRACK && k != T_RBRACE && k != T_EOF && k != T_AND &&
               k != T_OR && k != T_RPAREN;
    }
    return false;
}

static CompVar *bind_var(Parser *p, Tok *n)
{
    if (n->k != T_IDENT || qualified(n)) { perr(p, n->loc, "expected a variable's name"); return NULL; }
    Sym name = tok_sym(p, n);
    CompVar *other = var_lookup(p, n->s, n->len);
    struct LetBind *lb = let_lookup(p, name);
    if (other || lb) {
        char msg[200];
        Loc o = other ? other->loc : lb->loc;
        snprintf(msg, sizeof msg, "%s: the name is already bound, by the %s at %d:%d", name, other ? "for" : "let",
                 o.line, o.col);
        perr(p, n->loc, msg);
        return NULL;
    }
    static const char *const reserved[] = { "_", "true", "false", "let", "for", "in", "if", "package" };
    for (size_t r = 0; r < sizeof reserved / sizeof *reserved; r++)
        if (!strcmp(name, reserved[r])) {
            char msg[96];
            snprintf(msg, sizeof msg, "'%s' can't be a variable's name", name);
            perr(p, n->loc, msg);
            return NULL;
        }
    CompVar *v = qf_alloc(p->c->arena, sizeof *v);
    v->name = name;
    v->loc = n->loc;
    LetName ln = { name, p->pkg, n->loc, true };
    VEC_PUSH(p->c->lets, ln);
    return v;
}

/* An if's condition: || over && over !, over comparisons of sums. */
static X *parse_sum(Parser *p);

static X *cond_binary(Parser *p, XKind k, int op, Loc loc, X *a, X *b)
{
    X *x = xnew(p, k, loc);
    x->u.ar.a = a;
    x->u.ar.b = b;
    x->u.ar.op = op;
    return x;
}

/* Comparisons and logic: CUE's, between arithmetic and &. They are
 * evaluated by a later stage, like functions. An equation that is a law,
 * such as .energy == .capacity * .temperature, has the same tree, and the
 * elaborator gives it its meaning (ADR 0014). The parser enters
 * them only when one of their tokens follows a sum, so other content parses
 * as it always did. */
static bool cond_tok(TokKind k)
{
    return k == T_EQ || k == T_NE || k == T_LT || k == T_LE || k == T_GT || k == T_GE || k == T_ANDAND || k == T_OROR;
}

/* A comparison, given its left side. */
static X *cmp_after(Parser *p, X *a)
{
    Tok *op = peek(p, 0);
    int o = op->k == T_EQ ? 'e' : op->k == T_NE ? 'n' : op->k == T_LT ? '<' : op->k == T_LE ? 'l'
          : op->k == T_GT ? '>' : op->k == T_GE ? 'g' : 0;
    if (!a || !o) return a;
    next(p);
    X *b = parse_sum(p);
    if (!b) return NULL;
    p->has_comp = true;
    return cond_binary(p, X_CMP, o, op->loc, a, b);
}

static X *parse_cmp(Parser *p) { return cmp_after(p, parse_sum(p)); }

/* && over comparisons, then || over those, given the first sum. */
static X *cond_after(Parser *p, X *a)
{
    a = cmp_after(p, a);
    if (!a || (peek(p, 0)->k != T_ANDAND && peek(p, 0)->k != T_OROR)) return a;
    p->has_comp = true;
    while (a && peek(p, 0)->k == T_ANDAND) {
        Tok *op = next(p);
        X *b = parse_cmp(p);
        a = b ? cond_binary(p, X_LOGIC, '&', op->loc, a, b) : NULL;
    }
    while (a && peek(p, 0)->k == T_OROR) {
        Tok *op = next(p);
        X *b = parse_cmp(p);
        while (b && peek(p, 0)->k == T_ANDAND) {
            Tok *op2 = next(p);
            X *c = parse_cmp(p);
            b = c ? cond_binary(p, X_LOGIC, '&', op2->loc, b, c) : NULL;
        }
        a = b ? cond_binary(p, X_LOGIC, '|', op->loc, a, b) : NULL;
    }
    return a;
}

static X *parse_struct(Parser *p);

/* Does the '{' at token i open a struct (fields), rather than one value? */
static bool struct_body_at(Parser *p, size_t i)
{
    Tok *k = &p->toks.v[i + 1];
    if (k->k == T_RBRACE || k->k == T_ATTR || let_at(p, i + 1) || comp_at(p, i + 1)) return true;
    if (k->k == T_IDENT && p->toks.v[i + 2].k == T_COLON) return true;
    if (k->k == T_STRING && k->ninterp && p->toks.v[i + 2].k == T_COLON) return true;
    if (k->k == T_LPAREN) {                   /* (label): */
        int depth = 0;
        for (size_t j = i + 1; j < p->toks.n; j++) {
            TokKind m = p->toks.v[j].k;
            if (m == T_LPAREN || m == T_LBRACE || m == T_LBRACK) depth++;
            else if ((m == T_RPAREN || m == T_RBRACE || m == T_RBRACK) && --depth == 0)
                return m == T_RPAREN && p->toks.v[j + 1].k == T_COLON;
            else if (m == T_EOF) return false;
        }
    }
    return false;
}

/* for k, v in src  if cond ... { body }. In a list the body is an item: a
 * struct, or one value in braces ({ x * 2 }); in a struct, its fields. */
static X *parse_comprehension(Parser *p, bool in_list)
{
    Tok *start = peek(p, 0);
    X *x = xnew(p, X_COMP, start->loc);
    x->u.comp.list = in_list;
    VEC(CompClause) cl = { 0 };
    size_t vars0 = p->vars.n;
    while (!p->error && comp_at(p, p->pos)) {
        Tok *kw = next(p);
        CompClause c = { 0 };
        c.loc = kw->loc;
        if (tok_is(kw, "for")) {
            c.kind = C_FOR;
            Tok *a = next(p), *b = NULL;
            if (peek(p, 0)->k == T_COMMA) { next(p); b = next(p); }
            if (!tok_is(peek(p, 0), "in")) { perr(p, peek(p, 0)->loc, "expected 'in' after for's variables"); break; }
            next(p);
            c.expr = parse_expr(p);           /* before its variables are bound: the source can't see them */
            if (!c.expr) break;
            if (b) {
                c.key = bind_var(p, a);
                if (!c.key) break;
                VEC_PUSH(p->vars, c.key);
            }
            c.val = bind_var(p, b ? b : a);
            if (!c.val) break;
            VEC_PUSH(p->vars, c.val);
        } else {
            c.kind = C_IF;
            c.expr = parse_expr(p);
            if (!c.expr) break;
        }
        VEC_PUSH(cl, c);
    }
    X *body = NULL;
    if (!p->error) {
        if (peek(p, 0)->k != T_LBRACE) perr(p, peek(p, 0)->loc, "expected '{' after a comprehension's clauses");
        else if (!in_list || struct_body_at(p, p->pos)) body = parse_struct(p);
        else {
            next(p);
            body = parse_expr(p);
            if (body && !expect(p, T_RBRACE, "'}' after a comprehension's value")) body = NULL;
        }
    }
    p->vars.n = vars0;
    if (!body) { VEC_FREE(cl); p->error = true; return NULL; }
    x->u.comp.n = (int) cl.n;
    x->u.comp.clauses = qf_alloc(p->c->arena, (cl.n ? cl.n : 1) * sizeof *cl.v);
    if (cl.n) memcpy(x->u.comp.clauses, cl.v, cl.n * sizeof *cl.v);
    x->u.comp.body = body;
    VEC_FREE(cl);
    p->has_comp = true;
    return x;
}

static X *parse_struct(Parser *p)
{
    Tok *open = next(p);
    X *x = xnew(p, X_STRUCT, open->loc);
    VEC(XField) fields = { 0 };
    VEC(Attr) dattrs = { 0 };
    Scope *scope = open_scope(p, p->pos);
    while (peek(p, 0)->k != T_RBRACE && !p->error) {
        if (peek(p, 0)->k == T_EOF) { perr(p, open->loc, "unterminated struct"); break; }
        if (let_at(p, p->pos)) {
            if (!parse_let(p)) break;
            continue;
        }
        if (peek(p, 0)->k == T_ATTR) {
            Tok *t = next(p);
            Attr a = { t->attr_name, t->attr_args };
            VEC_PUSH(dattrs, a);
            continue;
        }
        if (peek(p, 0)->k == T_COMMA) { next(p); continue; }
        if (comp_at(p, p->pos)) {
            XField f = { 0 };
            f.loc = peek(p, 0)->loc;
            f.value = parse_comprehension(p, false);
            if (!f.value) break;
            VEC_PUSH(fields, f);
            continue;
        }
        if (peek(p, 0)->k == T_LPAREN || (peek(p, 0)->k == T_STRING && peek(p, 0)->ninterp && peek(p, 1)->k == T_COLON)) {
            /* a computed label: (k): v, "\(k)_x": v */
            XField f = { 0 };
            Tok *open = peek(p, 0);
            f.loc = open->loc;
            next(p);
            if (open->k == T_LPAREN) {
                f.dlabel = parse_expr(p);
                if (!f.dlabel || !expect(p, T_RPAREN, "')' after a computed label")) break;
            } else {
                f.dlabel = parse_interpolation(p, open);
                if (!f.dlabel) break;
            }
            if (!expect(p, T_COLON, "':' after a computed label")) break;
            f.value = parse_field_value(p, &f.attrs, &f.nattrs);
            if (!f.value) break;
            p->has_comp = true;
            VEC_PUSH(fields, f);
            continue;
        }
        Tok *lab = peek(p, 0);
        if (lab->k == T_REF) { perr(p, lab->loc, "a field's label is a plain name; a reference (.a.b) is a value"); break; }
        if (lab->k != T_IDENT) { perr(p, lab->loc, "expected a field label"); break; }
        if (!plain_label(p, lab)) break;
        next(p);
        if (peek(p, 0)->k == T_ASSIGN) { perr(p, peek(p, 0)->loc, "expected ':' after field label ('=' only binds a let)"); break; }
        if (!expect(p, T_COLON, "':' after field label")) break;
        XField f = { 0 };
        f.label = tok_sym(p, lab);
        f.loc = lab->loc;
        f.value = parse_field_value(p, &f.attrs, &f.nattrs);
        if (!f.value) break;
        VEC_PUSH(fields, f);
    }
    close_scope(p, scope);
    if (p->error) { VEC_FREE(fields); VEC_FREE(dattrs); return NULL; }
    next(p);
    x->u.st.n = (int) fields.n;
    if (fields.n) {
        x->u.st.fields = qf_alloc(p->c->arena, fields.n * sizeof(XField));
        memcpy(x->u.st.fields, fields.v, fields.n * sizeof(XField));
    }
    x->u.st.nattrs = (int) dattrs.n;
    if (dattrs.n) {
        x->u.st.attrs = qf_alloc(p->c->arena, dattrs.n * sizeof(Attr));
        memcpy(x->u.st.attrs, dattrs.v, dattrs.n * sizeof(Attr));
    }
    VEC_FREE(fields);
    VEC_FREE(dattrs);
    return x;
}

static X *parse_list(Parser *p)
{
    Tok *open = next(p);
    X *x = xnew(p, X_LIST, open->loc);
    XPtrVec items = { 0 };
    while (peek(p, 0)->k != T_RBRACK) {
        if (peek(p, 0)->k == T_ELLIPSIS) {
            next(p);
            x->u.list.open = true;
            if (peek(p, 0)->k != T_RBRACK) {          /* [...X]: the rest are all X */
                x->u.list.elem = parse_expr(p);
                if (!x->u.list.elem) goto fail;
            }
            break;
        }
        X *item = comp_at(p, p->pos) ? parse_comprehension(p, true) : parse_expr(p);
        if (!item) goto fail;
        VEC_PUSH(items, item);
        if (peek(p, 0)->k == T_COMMA) next(p);
        else if (peek(p, 0)->k != T_RBRACK) { perr(p, peek(p, 0)->loc, "expected ',' or ']'"); goto fail; }
    }
    if (!expect(p, T_RBRACK, "']'")) goto fail;
    x->u.list.n = (int) items.n;
    if (items.n) {
        x->u.list.items = qf_alloc(p->c->arena, items.n * sizeof(X *));
        memcpy(x->u.list.items, items.v, items.n * sizeof(X *));
    }
    VEC_FREE(items);
    return x;
fail:
    VEC_FREE(items);
    return NULL;
}

/* A number's digits, its exponent and its unit: mant × 10^-frac. The parser
 * keeps the unit as text. The kind of the field converts the value later (ADR 0014). */
static bool number(Parser *p, Tok *t, XNum *out)
{
    const char *s = t->s, *end = t->s + t->len, *err = NULL;
    bool neg = *s == '-', point = false;
    if (neg) s++;
    int64_t mant = 0;
    int frac = 0;
    for (; s < end && (isdigit((unsigned char) *s) || *s == '.') && !err; s++) {
        if (*s == '.') {
            if (point) err = "malformed number";
            point = true;
            continue;
        }
        int d = *s - '0';
        if (mant > (INT64_MAX - d) / 10) err = "number out of range";
        else mant = mant * 10 + d;
        if (point) frac++;
    }
    if (!err && point && !frac) err = "malformed number";
    /* An exponent moves the decimal point, exactly: 1.5e-3 is 15 × 10^-4,
     * and 1e5 is 1 × 10^5 (frac is -5). A later stage rejects a value that
     * the kind of the field cannot hold. */
    size_t elen = s < end ? exp_len(s) : 0;
    if (!err && elen) {
        const char *e = s + 1, *eend = s + elen;
        bool eneg = *e == '-';
        int exp = 0;
        if (*e == '+' || *e == '-') e++;
        for (; e < eend; e++) if (exp <= 9999) exp = exp * 10 + (*e - '0');
        if (exp > 9999) err = "number out of range";
        frac += eneg ? exp : -exp;
        s = eend;
    }
    if (err) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s: %.*s", err, (int) t->len, t->s);
        perr(p, t->loc, msg);
        return false;
    }
    out->mant = neg ? -mant : mant;
    out->frac = frac;
    out->unit = s < end ? qf_intern(p->c, s, (size_t) (end - s)) : NULL;
    return true;
}

static void parser_free(Parser *p);

/* An interpolated string: each \(...) is parsed as an expression where it
 * stands in the source, so its diagnostics point there. */
static X *parse_interpolation(Parser *p, Tok *t)
{
    X *x = xnew(p, X_INTERP, t->loc);
    x->u.in.n = t->ninterp;
    x->u.in.lits = t->ilits;
    x->u.in.parts = qf_alloc(p->c->arena, (size_t) t->ninterp * sizeof(X *));
    for (int i = 0; i < t->ninterp; i++) {
        struct IPart *ip = &t->iparts[i];
        char *src = malloc(ip->len + 1);
        if (!src) qf_oom();
        memcpy(src, ip->src, ip->len);
        src[ip->len] = 0;
        Parser q = { .c = p->c, .file = p->file, .pkg = p->pkg, .outer = p };
        X *e = NULL;
        if (lex_at(&q, src, ip->loc.line, ip->loc.col)) {
            if (peek(&q, 0)->k == T_EOF) perr(&q, ip->loc, "an empty interpolation: \\()");
            else {
                e = parse_expr(&q);
                if (e && peek(&q, 0)->k != T_EOF) { perr(&q, peek(&q, 0)->loc, "unexpected input in an interpolation"); e = NULL; }
            }
        }
        bool bad = q.error;
        if (q.has_ref) p->has_ref = true;
        if (q.has_comp) p->has_comp = true;
        parser_free(&q);
        free(src);
        if (bad || !e) { p->error = true; return NULL; }
        x->u.in.parts[i] = e;
    }
    return x;
}

static X *parse_primary(Parser *p)
{
    Tok *t = peek(p, 0);
    switch (t->k) {
    case T_IDENT: {
        next(p);
        if (t->len == 1 && t->s[0] == '_') return xnew(p, X_TOP, t->loc);
        if ((t->len == 4 && memcmp(t->s, "true", 4) == 0) || (t->len == 5 && memcmp(t->s, "false", 5) == 0)) {
            X *x = xnew(p, X_ATOM, t->loc);
            x->u.name = tok_sym(p, t);
            return x;
        }
        if (peek(p, 0)->k == T_LPAREN && peek(p, 0)->s == t->s + t->len) {
            /* a function: len(xs), strings.ToUpper(s). A later stage checks
             * its name and the number of its arguments. */
            next(p);
            XPtrVec args = { 0 };
            while (peek(p, 0)->k != T_RPAREN) {
                X *a = parse_expr(p);
                if (!a) { VEC_FREE(args); return NULL; }
                VEC_PUSH(args, a);
                if (peek(p, 0)->k == T_COMMA) next(p);
                else if (peek(p, 0)->k != T_RPAREN) { perr(p, peek(p, 0)->loc, "expected ',' or ')'"); VEC_FREE(args); return NULL; }
            }
            next(p);
            X *x = xnew(p, X_CALL, t->loc);
            x->u.call.name = tok_sym(p, t);
            x->u.call.n = (int) args.n;
            x->u.call.args = qf_alloc(p->c->arena, (args.n ? args.n : 1) * sizeof(X *));
            if (args.n) memcpy(x->u.call.args, args.v, args.n * sizeof(X *));
            VEC_FREE(args);
            p->has_comp = true;                 /* evaluated with comprehensions */
            return x;
        }
        Sym name = tok_sym(p, t);
        size_t head = 0;
        CompVar *cv = NULL;
        if (p->vars.n || p->outer) {            /* inside a comprehension */
            while (head < t->len && t->s[head] != '.' && t->s[head] != '[') head++;
            cv = var_lookup(p, t->s, head);
        }
        if (cv) {
            /* a comprehension's variable, perhaps then a path: x, x.size, x[0] */
            X *x = xnew(p, X_VAR, t->loc);
            x->u.var.var = cv;
            x->u.var.path = head < t->len ? qf_intern(p->c, t->s + head, t->len - head) : NULL;
            return x;
        }
        struct LetBind *lb = qualified(t) ? NULL : let_lookup(p, name);
        X *x = xnew(p, lb ? X_LET : X_NAME, t->loc);
        if (lb) x->u.let = lb;
        else x->u.name = name;
        return x;
    }
    case T_REF: {
        next(p);
        X *x = xnew(p, X_TAG, t->loc);
        x->u.name = tok_sym(p, t);
        return x;
    }
    case T_DEF: {
        next(p);
        X *x = xnew(p, X_NAME, t->loc);
        x->u.name = tok_sym(p, t);
        return x;
    }
    case T_STRING: {
        next(p);
        if (t->ninterp) return parse_interpolation(p, t);
        X *x = xnew(p, X_ATOM, t->loc);
        x->u.name = tok_sym(p, t);
        return x;
    }
    case T_NUMBER: {
        next(p);
        X *x = xnew(p, X_NUM, t->loc);
        if (!number(p, t, &x->u.num)) return NULL;
        return x;
    }
    case T_TAG: {
        next(p);
        X *x = xnew(p, X_TAG, t->loc);
        x->u.name = tok_sym(p, t);
        return x;
    }
    case T_LBRACE: return parse_struct(p);
    case T_LBRACK: return parse_list(p);
    case T_LPAREN: {
        next(p);
        X *x = parse_expr(p);
        if (!x || !expect(p, T_RPAREN, "')'")) return NULL;
        return x;
    }
    default:
        perr(p, t->loc, t->k == T_ASSIGN ? "unexpected '='; did you mean ':' or '=~'?" : "expected a value");
        return NULL;
    }
}

static X *parse_unary(Parser *p)
{
    Tok *t = peek(p, 0);
    if (t->k == T_NOT) {
        next(p);
        X *sub = parse_unary(p);
        if (!sub) return NULL;
        X *x = xnew(p, X_NOT, t->loc);
        x->u.sub = sub;
        p->has_comp = true;
        return x;
    }
    if (t->k == T_STAR) {
        next(p);
        X *sub = parse_unary(p);
        if (!sub) return NULL;
        X *x = xnew(p, X_DEFAULT, t->loc);
        x->u.sub = sub;
        return x;
    }
    if (t->k == T_GE || t->k == T_LE || t->k == T_GT || t->k == T_LT) {
        next(p);
        Tok *n = peek(p, 0);
        int op = t->k == T_GE ? B_GE : t->k == T_LE ? B_LE : t->k == T_GT ? B_GT : B_LT;
        if (n->k == T_IDENT || n->k == T_REF || n->k == T_LPAREN) {
            /* a bound by a value named (a let, a field, a path, a sum): read
             * by a later stage, as comprehensions read data */
            X *of = parse_primary(p);
            if (!of) return NULL;
            X *x = xnew(p, X_BOUND, t->loc);
            x->u.bound.op = op;
            x->u.bound.of = of;
            p->has_comp = true;
            return x;
        }
        if (n->k != T_NUMBER) { perr(p, n->loc, "expected a number after a bound"); return NULL; }
        next(p);
        X *x = xnew(p, X_BOUND, t->loc);
        x->u.bound.op = op;
        if (!number(p, n, &x->u.bound.n)) return NULL;
        return x;
    }
    if (t->k == T_MATCH || t->k == T_NMATCH) {
        next(p);
        Tok *str = peek(p, 0);
        if (str->k != T_STRING) { perr(p, str->loc, "expected a string after =~ or !~"); return NULL; }
        if (str->ninterp) { perr(p, str->loc, "a regex can't be interpolated"); return NULL; }
        next(p);
        X *x = xnew(p, X_REGEX, t->loc);   /* a later stage compiles the pattern */
        x->u.rx.pat = str->str;
        x->u.rx.neg = t->k == T_NMATCH;
        return x;
    }
    return parse_primary(p);
}

/* Arithmetic binds tighter than &: products, then sums. A '*' after an
 * operand multiplies; at the start of a term it marks a default. */
static X *parse_product(Parser *p)
{
    X *a = parse_unary(p);
    while (a && (peek(p, 0)->k == T_STAR || peek(p, 0)->k == T_SLASH)) {
        Tok *op = next(p);
        X *b = parse_unary(p);
        if (!b) return NULL;
        X *x = xnew(p, X_ARITH, op->loc);
        x->u.ar.a = a;
        x->u.ar.b = b;
        x->u.ar.op = op->k == T_STAR ? '*' : '/';
        a = x;
    }
    return a;
}

static X *parse_sum(Parser *p)
{
    X *a = parse_product(p);
    while (a && (peek(p, 0)->k == T_PLUS || peek(p, 0)->k == T_MINUS)) {
        Tok *op = next(p);
        X *b = parse_product(p);
        if (!b) return NULL;
        X *x = xnew(p, X_ARITH, op->loc);
        x->u.ar.a = a;
        x->u.ar.b = b;
        x->u.ar.op = op->k == T_PLUS ? '+' : '-';
        a = x;
    }
    return a;
}

static X *parse_conj(Parser *p)
{
    X *a = parse_sum(p);
    if (a && cond_tok(peek(p, 0)->k)) a = cond_after(p, a);
    while (a && peek(p, 0)->k == T_AND) {
        Tok *op = next(p);
        X *b = parse_sum(p);
        if (b && cond_tok(peek(p, 0)->k)) b = cond_after(p, b);
        if (!b) return NULL;
        X *x = xnew(p, X_AND, op->loc);
        x->u.bin.a = a;
        x->u.bin.b = b;
        a = x;
    }
    return a;
}

static X *parse_expr(Parser *p)
{
    X *a = parse_conj(p);
    while (a && peek(p, 0)->k == T_OR) {
        Tok *op = next(p);
        X *b = parse_conj(p);
        if (!b) return NULL;
        X *x = xnew(p, X_OR, op->loc);
        x->u.bin.a = a;
        x->u.bin.b = b;
        a = x;
    }
    return a;
}

static void parser_free(Parser *p)
{
    while (p->scopes.n) close_scope(p, p->scopes.v[p->scopes.n - 1]);
    VEC_FREE(p->vars);
    VEC_FREE(p->scopes);
    VEC_FREE(p->toks);
    VEC_FREE(p->comments);
    free(p->ilits.v);
    free(p->iparts.v);
}

/* The comment lines directly above line `line`, joined: a docstring. */
static const char *doc_above(Parser *p, int line)
{
    size_t end = 0;
    while (end < p->comments.n && p->comments.v[end].line < line) end++;
    size_t start = end;
    int want = line - 1;
    while (start > 0 && p->comments.v[start - 1].line == want) { start--; want--; }
    if (start == end) return NULL;
    Buf b = { 0 };
    for (size_t i = start; i < end; i++) {
        const char *t = p->comments.v[i].s;
        size_t n = p->comments.v[i].len;
        if (n && *t == ' ') { t++; n--; }
        while (n && (t[n - 1] == ' ' || t[n - 1] == '\r')) n--;
        if (i > start) qf_buf_putc(&b, '\n');
        qf_buf_printf(&b, "%.*s", (int) n, t);
    }
    char *r = qf_alloc(p->c->arena, b.len + 1);
    memcpy(r, b.s ? b.s : "", b.len + 1);
    qf_buf_free(&b);
    return r;
}

bool qf_parse(Front *c, const char *file_name, const char *src, DeclVec *out)
{
    Sym file = qf_intern(c, file_name, strlen(file_name));
    Parser p = { .c = c, .file = file };
    Sym pkg = NULL;
    size_t first = out->n;
    if (!lex(&p, src)) { parser_free(&p); return false; }
    /* `package name`, first: the file's declarations are in it */
    if (peek(&p, 0)->k == T_IDENT && peek(&p, 0)->len == 7 && memcmp(peek(&p, 0)->s, "package", 7) == 0 &&
        peek(&p, 1)->k == T_IDENT && peek(&p, 2)->k != T_COLON) {
        next(&p);
        Tok *name = next(&p);
        if (qualified(name)) perr(&p, name->loc, "a package name is one name");
        pkg = tok_sym(&p, name);
    }
    p.pkg = pkg;
    open_scope(&p, p.pos);                         /* the file's lets */
    while (peek(&p, 0)->k != T_EOF && !p.error) {
        Tok *lab = peek(&p, 0);
        if (lab->k == T_COMMA) { next(&p); continue; }
        if (let_at(&p, p.pos)) {
            if (!parse_let(&p)) break;
            continue;
        }
        /* another package's type, pkg.#Type, may be added to; anything else
         * qualified is declared in its own package's files */
        if ((lab->k == T_IDENT || lab->k == T_DEF) && qualified(lab) && !package_type(lab)) {
            perr(&p, lab->loc, "a declaration's name is its own: declare it in its package's files, unqualified");
            break;
        }
        Decl d = { 0 };
        d.pkg = pkg;
        if (lab->k == T_DEF) d.kind = L_DEF;
        else if (lab->k == T_STRING && lab->ninterp) { perr(&p, lab->loc, "a declaration's label can't be interpolated"); break; }
        else if (lab->k == T_STRING) d.kind = L_ATOM;
        else if (lab->k == T_IDENT) d.kind = L_INST;
        else { perr(&p, lab->loc, "expected a declaration"); break; }
        next(&p);
        d.label = tok_sym(&p, lab);
        d.loc = lab->loc;
        d.doc = doc_above(&p, lab->loc.line);
        if (peek(&p, 0)->k == T_ASSIGN) { perr(&p, peek(&p, 0)->loc, "expected ':' after declaration label ('=' only binds a let)"); break; }
        if (!expect(&p, T_COLON, "':' after declaration label")) break;
        d.value = parse_field_value(&p, &d.attrs, &d.nattrs);
        if (!d.value) break;
        VEC_PUSH(*out, d);
    }
    for (size_t i = first; i < out->n && !p.error; i++) {
        out->v[i].value = finish_expr(&p, out->v[i].value);
        out->v[i].comp = p.has_comp && x_has_comp(out->v[i].value);
    }
    bool ok = !p.error;
    if (!ok) out->n = first;   /* a file with an error adds no declarations */
    parser_free(&p);
    return ok;
}

X *qf_parse_expr(Front *c, const char *file_name, const char *src)
{
    Sym file = qf_intern(c, file_name, strlen(file_name));
    Parser p = { .c = c, .file = file };
    X *x = NULL;
    if (lex(&p, src)) {
        x = parse_expr(&p);
        if (x && peek(&p, 0)->k != T_EOF) {
            perr(&p, peek(&p, 0)->loc, "unexpected input after expression");
            x = NULL;
        }
        x = finish_expr(&p, x);
        c->parsed_comp = p.has_comp;
    }
    parser_free(&p);
    return p.error ? NULL : x;
}
