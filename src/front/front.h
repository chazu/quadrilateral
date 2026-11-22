#ifndef QUAD_FRONT_H
#define QUAD_FRONT_H

/* The language front end (docs/adr/0011, docs/adr/0014). This header is
 * internal: the product of the front end is the IR, not the syntax tree.
 *
 * Stage 1 is the parser: Quadrilateral source to declarations with syntax
 * trees. The arena, the symbol table, the buffers and the syntax tree come
 * from Triangle's internal.h and util.c (bruggadung f425574), under ADR 0003.
 *
 * The front end stops the process if memory runs out (qf_oom). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef const char *Sym;   /* interned in a Front; compare by pointer */

/* --- utilities ------------------------------------------------------------------- */

void qf_oom(void);

typedef struct QfArena QfArena;
QfArena *qf_arena_new(void);
void     qf_arena_free(QfArena *a);
void    *qf_alloc(QfArena *a, size_t n);   /* zeroed; freed with the arena */

typedef struct { char *s; size_t len, cap; } Buf;
void qf_buf_putc(Buf *b, char c);
void qf_buf_puts(Buf *b, const char *s);
void qf_buf_putn(Buf *b, const char *s, size_t n);   /* bytes, kept terminated */
void qf_buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void qf_buf_free(Buf *b);

typedef struct { Sym *slots; size_t cap, count; } SymTable;

void *qf_grow(void *v, size_t *cap, size_t elem);

#define VEC(T) struct { T *v; size_t n, cap; }
#define VEC_PUSH(vec, x) do { \
    if ((vec).n == (vec).cap) (vec).v = qf_grow((vec).v, &(vec).cap, sizeof *(vec).v); \
    (vec).v[(vec).n++] = (x); \
} while (0)
#define VEC_FREE(vec) do { free((vec).v); (vec).v = NULL; (vec).n = (vec).cap = 0; } while (0)

/* --- the syntax tree --------------------------------------------------------------- */

typedef struct { Sym file; int line, col; } Loc;

/* An attribute, @name or @name(args). The parser keeps the arguments as
 * text and knows no attribute names: a later stage reads them. */
typedef struct { Sym name; char *args; } Attr;

typedef enum { X_TOP, X_NAME, X_ATOM, X_NUM, X_BOUND, X_TAG, X_STRUCT, X_LIST,
               X_AND, X_OR, X_DEFAULT, X_ARITH, X_REGEX, X_INTERP, X_LET,
               X_COMP, X_VAR, X_CMP, X_LOGIC, X_NOT, X_CALL } XKind;

typedef struct X X;
typedef VEC(X *) XPtrVec;

/* A number literal: mant × 10^-frac, in `unit`. The unit is the text after
 * the digits, for example "mm" or "J/K", or NULL. The parser does not know
 * units. The kind of the field converts the literal later, and exactly
 * (ADR 0001, ADR 0014). */
typedef struct { int64_t mant; int frac; Sym unit; } XNum;

typedef struct {
    Sym   label;
    X    *value;
    Attr *attrs;
    int   nattrs;
    Loc   loc;
    X    *dlabel;        /* a computed label, (expr) or "\(k)_x"; then label is NULL */
} XField;

/* Comprehensions (for, if). A later stage expands them. */
typedef struct CompVar { Sym name; Loc loc; } CompVar;
enum { C_FOR, C_IF };
typedef struct { int kind; CompVar *key, *val; X *expr; Loc loc; } CompClause;

enum { B_GE, B_LE, B_GT, B_LT };

struct X {
    XKind k;
    Loc   loc;
    union {
        Sym name;                                         /* NAME, ATOM, TAG. A TAG that starts with '.' is a reference. */
        XNum num;
        struct { int op; XNum n; X *of; } bound;          /* of: a named value in place of the number */
        struct { XField *fields; int n; Attr *attrs; int nattrs; bool refs; Sym pkg; } st;   /* refs: made for references;
                                                                                             pkg: its file's package */
        struct { X **items; int n; bool open; X *elem; } list;   /* open: [...]; elem: [...X] */
        struct { X *a, *b; } bin;                         /* AND, OR */
        X *sub;                                           /* DEFAULT, NOT */
        struct { X *a, *b; int op; } ar;                  /* ARITH: '+' '-' '*' '/'. CMP: 'e' 'n' '<' 'l' '>' 'g'.
                                                             LOGIC: '&' '|'. */
        struct { Sym pat; bool neg; } rx;                 /* REGEX: =~ or !~ a pattern, as a canonical string */
        struct { int n; const char **lits; X **parts; } in;  /* INTERP: lits[0] part[0] lits[1] ... lits[n] */
        struct LetBind *let;                              /* LET: a use of a let's name, until the parser replaces it */
        struct { CompClause *clauses; int n; X *body; bool list; } comp;   /* COMP: in a list, body is one item */
        struct { CompVar *var; Sym path; } var;           /* VAR: a comprehension's variable, then .a[0] or NULL */
        struct { Sym name; X **args; int n; } call;       /* CALL: a function; a later stage checks its name */
    } u;
};

/* Instead of a field, a comprehension whose fields go here: no label, and the
 * comprehension for a value. */
static inline X *field_comp(const XField *f) { return !f->label && !f->dlabel && f->value && f->value->k == X_COMP ? f->value : NULL; }

enum { L_DEF, L_ATOM, L_INST };   /* #Type: ..., "atom": ..., instance: ... */

typedef struct {
    Sym   label;
    int   kind;
    X    *value;
    Attr *attrs;
    int   nattrs;
    Loc   loc;
    const char *doc;     /* the comment lines directly above, or NULL */
    Sym   pkg;           /* the package that its file declares, or NULL */
    bool  comp;          /* has comprehensions, computed labels, conditions or calls */
} Decl;

typedef VEC(Decl) DeclVec;

/* A name that a let or a comprehension binds. A later stage checks these
 * names against the declarations. */
typedef struct { Sym name; Sym pkg; Loc loc; bool var; } LetName;

typedef struct { bool error; char *msg; Loc loc; } Diag;   /* msg starts with "file:line:col: " */

/* --- the front end -------------------------------------------------------------------- */

typedef struct Front {
    QfArena  *arena;        /* syntax trees and their strings */
    SymTable  syms;
    VEC(Diag) diags;
    size_t    nerrors;
    VEC(LetName) lets;
    bool      parsed_comp;  /* the last qf_parse_expr had comprehensions, conditions or calls */
} Front;

Front *qf_new(void);
void   qf_free(Front *f);

Sym  qf_intern(Front *f, const char *s, size_t len);
/* A string's canonical spelling, with its quotes: what an atom's name is. */
Sym  qf_string_canonical(Front *f, const char *bytes, size_t n);
void qf_string_escape_into(Buf *b, const char *bytes, size_t n);
void qf_diag(Front *f, bool error, Loc loc, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* Parses a source file and appends its declarations to `out`. The parser
 * stops at the first syntax error in the file, records a diagnostic with the
 * file, the line and the column, and returns false. The trees live as long
 * as the Front.
 *
 * The parser replaces each use of a let with the expression that the let
 * binds. For each reference .a.b, the declaration gains the conjunct
 * { a: { b: .a.b } }, which puts the reference's tag at its path. */
bool qf_parse(Front *f, const char *file, const char *src, DeclVec *out);
/* Parses one expression, or returns NULL with a diagnostic. */
X   *qf_parse_expr(Front *f, const char *file, const char *src);

/* The text form of a tree: Quadrilateral source that parses to the same
 * tree. It leaves out the conjuncts that the parser made for references. */
void qf_print(Buf *b, const X *x);
/* The declarations of one file, with their docstrings and the package line. */
void qf_print_decls(Buf *b, const Decl *d, size_t n);

#endif
