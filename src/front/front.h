#ifndef QUAD_FRONT_H
#define QUAD_FRONT_H

/* The language front end (docs/adr/0011, 0014, 0015). This header is
 * internal: the product of the front end is the IR, not the syntax tree.
 *
 * Stage 1 is the parser (parse.c, print.c): Quadrilateral source to
 * declarations with syntax trees.
 *
 * Stage 2 is the resolver: kinds and units (kinds.c), the type hierarchy
 * (types.c), feature structures and unification (fs.c), the quick check
 * (qc.c), and the load that unifies each declaration (resolve.c).
 *
 * Stage 3 is the elaborator (elab.c): the result of a load to the IR of
 * quad/ir.h (ADR 0016). ir.c has the text form of the IR, and instantiate.c
 * builds a world from it.
 *
 * The code comes from Triangle (bruggadung f425574), under ADR 0003. Each
 * file says what it changes.
 *
 * The front end stops the process if memory runs out (qf_oom). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "quad/ir.h"
#include "quad/quantity.h"

typedef const char *Sym;   /* interned in a Front; compare by pointer */

/* --- utilities ------------------------------------------------------------------- */

void  qf_oom(void);
void *qf_xmalloc(size_t n);                 /* these three stop the process if memory runs out */
void *qf_xcalloc(size_t n, size_t size);
void *qf_xrealloc(void *p, size_t n);

typedef struct QfArena QfArena;
QfArena *qf_arena_new(void);
void     qf_arena_free(QfArena *a);
void    *qf_alloc(QfArena *a, size_t n);   /* zeroed; freed with the arena */
void    *qf_alloc_raw(QfArena *a, size_t n);   /* not zeroed */
typedef struct { void *chunk; size_t used; } QfMark;
QfMark   qf_arena_mark(QfArena *a);
void     qf_arena_release(QfArena *a, QfMark m);   /* frees what was allocated after the mark */

typedef struct { char *s; size_t len, cap; } Buf;
void qf_buf_putc(Buf *b, char c);
void qf_buf_puts(Buf *b, const char *s);
void qf_buf_putn(Buf *b, const char *s, size_t n);   /* bytes, kept terminated */
void qf_buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void qf_buf_free(Buf *b);

typedef struct { Sym *slots; size_t cap, count; } SymTable;

/* Open-addressing map from uint64 keys to uint64 values. Lookup only: it is
 * never iterated, so it cannot affect determinism. */
typedef struct { uint64_t *keys, *vals; uint8_t *used; size_t cap, count; } U64Map;
bool u64map_get(const U64Map *m, uint64_t key, uint64_t *val);
void u64map_put(U64Map *m, uint64_t key, uint64_t val);
void u64map_clear(U64Map *m);
void u64map_free(U64Map *m);

uint64_t qf_hash64(uint64_t x);
uint64_t qf_fnv1a(uint64_t h, const void *data, size_t n);

void *qf_grow(void *v, size_t *cap, size_t elem);

#define VEC(T) struct { T *v; size_t n, cap; }
#define VEC_PUSH(vec, x) do { \
    if ((vec).n == (vec).cap) (vec).v = qf_grow((vec).v, &(vec).cap, sizeof *(vec).v); \
    (vec).v[(vec).n++] = (x); \
} while (0)
#define VEC_FREE(vec) do { free((vec).v); (vec).v = NULL; (vec).n = (vec).cap = 0; } while (0)

typedef VEC(uint32_t) U32Vec;
typedef VEC(U32Vec) PathVec;
typedef VEC(Sym) SymVec;

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

/* A number literal: mant × 10^-frac, in `unit`. An exponent changes frac:
 * 1.5e-3 has frac 4, and 1e5 has frac -5. The unit is the text after the
 * number, for example "mm" or "J/K", or NULL. The parser does not know
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
        Sym name;                                         /* NAME, ATOM, TAG. A NAME that starts with '#' is a type,
                                                             and one with '%' is a kind (also after a package:
                                                             pkg.%kind). A TAG that starts with '.' is a reference. */
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

enum { L_DEF, L_ATOM, L_INST, L_KIND };   /* #Type: ..., "atom": ..., instance: ..., %kind: ... */

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

/* --- kinds, dimensions and units (ADR 0015) ---------------------------------------------- */

/* A dimension that the content uses. It has one generated type, below
 * `number`, and the kinds of the dimension are below that type. */
typedef struct {
    QDim     dim;
    Sym      name;       /* its type's name: dim[kg m^2 s^-2], dim[1] */
    uint32_t type;
} DimInfo;

/* A kind: %energy: { dimension: "J", scale: -3, conserved: true }. */
typedef struct {
    Sym      name;       /* with its prefix, and its package if it has one: %energy, money.%coin */
    uint32_t dimix;      /* index into Front.dims */
    int8_t   scale;      /* one quantum is 10^scale coherent units */
    bool     conserved;
    uint8_t  rounding;   /* QRound */
    Sym      base;       /* the unit symbol of the base dimension that it declares, or NULL */
    Loc      loc;
    bool     builtin;    /* built in, and no declaration changed it */
    bool     fixed;      /* a built-in kind that a declaration cannot change: %time, %count, %ratio */
    uint32_t type;
} KindInfo;

/* A unit symbol: one unit is mult × 10^exp10 coherent units of its dimension. */
typedef struct { QDim dim; int64_t mult; int exp10; } QfUnit;

/* An exact decimal interval in coherent units: lo × 10^exp to hi × 10^exp.
 * Q_NINF and Q_INF are the infinities. An open bound excludes its value. */
typedef struct { int64_t lo, hi; int exp; bool lo_open, hi_open; } QfDec;

/* --- types ---------------------------------------------------------------------------------- */

#define QF_BOTTOM UINT32_MAX     /* no type, no feature: the failure of a meet or a lookup */

/* TK_NUMBER: `number`. TK_DIMT: a dimension type. TK_KIND: a kind.
 * TK_DISJ: an anonymous disjunction of structures; TK_ALT: one of its
 * alternatives (or of a named type defined as a disjunction). */
typedef enum { TK_TOP, TK_NUMBER, TK_DIMT, TK_KIND, TK_TEXT, TK_CONSTRAINT, TK_ATOMROOT, TK_DEF, TK_ATOM,
               TK_ANON, TK_GLB, TK_DISJ, TK_ALT } TypeKind;

/* A disjunction of structures in a type body. Types are referred to by name,
 * since IDs change when the hierarchy is renumbered. */
typedef struct {
    X     *orx;                 /* the disjunction */
    Sym    owner;               /* named type defined by it, or NULL */
    Sym    dname;               /* the disjunction's type */
    XPtrVec altx;               /* alternatives, in written order, defaults unwrapped */
    SymVec alts;                /* their types, in written order */
    int    def;                 /* the default alternative, or -1 */
    Sym    collapse;            /* one alternative subsumes the others: just that type */
    Loc    loc;
} DisjInfo;

typedef struct { Sym name; U32Vec path; } TagPath;

/* A constraint record (ADR 0015 section 5): the value at `path` is `expr`,
 * or, for a comparison, `expr` holds. The references of the expression are
 * at the given paths. All paths are relative to the structure that declares
 * the record. Nothing in the front end evaluates it. */
typedef struct {
    U32Vec path;
    X     *expr;
    VEC(TagPath) tags;
} ArithRec;
typedef VEC(ArithRec) ArithVec;

/* A reference record: the structure at `path` is a copy of the instance
 * `inst`, or of its part at `sub`. `hot: roomA.west` gives (hot, roomA, west).
 * A copy has no link to its source, so stage 3 reads these records to find
 * the connections between phenomena (ADR 0016). */
typedef struct { U32Vec path; Sym inst; U32Vec sub; } InstRef;
typedef VEC(InstRef) InstRefVec;

typedef struct QfNode QfNode;

typedef struct {
    Sym      name;
    TypeKind kind;
    uint32_t kindix;                    /* TK_KIND: index into Front.kinds */
    uint32_t dimix;                     /* TK_KIND, TK_DIMT: index into Front.dims */
    U32Vec parents;                     /* primary first */
    U32Vec children;
    uint64_t *code;                     /* descendants, including self; NULL for leaves */
    uint32_t  wlo, whi;                 /* the code's non-zero words lie in [wlo, whi) */
    int32_t   bit;                      /* position in codes, or -1 for leaves */
    U32Vec feats;                       /* appropriate features, ascending */
    VEC(Decl *) decls;                  /* declarations naming this type */
    SymVec    members;                  /* ANON / GLB: member names, for naming */
    int       xstate;                   /* 0 none, 1 in progress, 2 done */
    QfNode   *expansion;
    ArithVec  ariths;                   /* constraint records, own and inherited */
    InstRefVec refs;                    /* reference records, own and inherited */
    Sym       home;                     /* the package it is declared in, or NULL (the root) */
    bool      isdisj;                   /* its subtypes are alternatives (TK_ALT) */
    VEC(Attr *) field_attrs;            /* parallel to own introduced features */
    U32Vec field_feats;
} TypeInfo;

/* A feature is a field name of a package: two packages may each have one of
 * a label. Features are numbered by label, then package (the root first), so
 * packages don't reorder them.
 *
 * Unrelated types can each have a field with the same name (ADR 0017). So a
 * feature has a list of the types that introduce it: each type that declares
 * the field and has no supertype that declares it. A type has the feature if
 * it is below one of them. */
typedef struct {
    Sym      name;       /* as it prints: the label, unless that is ambiguous */
    Sym      label;      /* as written: larder */
    Sym      pkg;        /* its package, or NULL (the root) */
    Sym      full;       /* pkg.label, or the label */
    U32Vec   intros;     /* the types that introduce it, ascending */
} FeatInfo;

/* --- nodes, stores and operations (fs.c) ---------------------------------------------------- */

typedef struct { uint32_t feat; QfNode *val; } Arc;

/* A store is a home of nodes: an arena, and a number for each node in it.
 * The content is one store. A caller makes a store for the structures that
 * an operation gives to it. */
typedef struct QfStore { QfArena *arena; uint32_t n; } QfStore;

/* NF_VALUE: the node holds a value, not arcs: a number, a text or a constraint.
 * NF_LAZY: the constraint of the node's type is not applied yet, because the
 * type was being expanded when the node was made (a recursive constraint). It
 * is applied when unification adds structure to the node.
 * NF_LO_OPEN, NF_HI_OPEN: a decimal bound that excludes its value. */
enum { NF_SCRATCH = 1, NF_VALUE = 2, NF_LAZY = 4, NF_LO_OPEN = 8, NF_HI_OPEN = 16 };

/* What a value node holds, by the kind of its type (ADR 0015 sections 2, 3):
 *   a kind             lo..hi in quanta of the kind
 *   a dimension type,  lo × 10^exp .. hi × 10^exp in coherent units, exactly;
 *   or `number`        the open flags say if a bound excludes its value
 *   `text`             lo == hi: a canonical string (a Sym, as an integer);
 *                      Q_NINF..Q_INF: any text
 *   `constraint`       lo == hi: the comparison (an X *, as an integer)
 * A node holds no scratch: an operation keeps that in its context, by the
 * node's store and id (ADR 0015 section 7). */
struct QfNode {
    uint32_t type;
    uint16_t narcs;
    uint16_t flags;
    union {
        Arc *arcs;                    /* sorted by feature id; nodes without a value */
        struct { int64_t lo, hi; };   /* value nodes */
    };
    uint32_t id;                      /* its number in its store */
    int32_t  exp;                     /* a decimal value's power of ten */
    QfStore *store;
};

/* A node's value with the fields that a decimal needs. */
typedef struct { int64_t lo, hi; int32_t exp; uint8_t open; } QfVal;   /* open: NF_LO_OPEN | NF_HI_OPEN */

/* Per-operation scratch of one node, valid only while its generation is current. */
typedef struct {
    QfNode  *fwd;                /* unification forward; subsumption a→b */
    QfNode  *bwd;                /* subsumption b→a */
    Arc     *carcs;              /* comp-arcs added during unification */
    uint32_t ncarcs, ccap;
    QfVal    val;                /* narrowed value */
    bool     hasiv;
    bool     forced;             /* a lazy node whose constraint this operation applied */
} Scratch;

typedef struct { uint64_t sxgen, cpygen; Scratch *sx; QfNode *cpy; } Slot;
typedef struct { const QfStore *store; Slot **pages; size_t npages; } SlotTable;

/* Where and why a unification failed. The texts are for a person. */
typedef struct {
    char path[256];
    char left[160];
    char right[160];
    char why[160];               /* a reason beyond "these two do not meet", or empty */
} QfFailure;

/* The context of operations (ADR 0015 section 7). It owns everything that
 * an operation writes: the scratch arena, the scratch of each node, the path
 * stack, the failure record, and the cache of meets. Two contexts can work
 * on one content at the same time. One context runs one operation at a time. */
typedef struct QfOp {
    struct Front *f;
    QfStore   scratch;
    SlotTable tscratch, tcontent;
    VEC(SlotTable) tother;
    struct { uint64_t key; uint32_t val; } meetcache[1024];

    /* generations: a node's scratch and copy count only while their mark
     * equals the current generation, so no mark is ever cleared */
    uint64_t gen, genctr, cgenctr;
    int opdepth;                 /* nesting of operations; 1 = top level */

    /* failure recording. A list is a chain of rest arcs, so a path is as
     * long as the list it goes into: the path stack has no fixed size. */
    uint32_t *pathstack;
    size_t pathcap;
    int depth;
    bool failed;
    QfFailure fail_;             /* formatted on demand: read it through fail_info() */
    bool failfmt;                /* fail_ is up to date */
    uint32_t *failpath;          /* the failing path, as feature IDs */
    size_t failcap;
    int faildepth;
    struct { const char *why; uint32_t ltype, rtype; QfVal l, r; bool hasr; } failraw;
    bool cyclic;                 /* set when sharing meets a cycle during copy_out */
} QfOp;

/* An instance: the unification of its declarations and of the types that
 * they name. Stage 2 gives it one structure (ADR 0015 section 6). */
typedef struct {
    Sym       name;
    VEC(Decl *) decls;
    int       pstate;            /* 0 none, 1 in progress, 2 done, 3 failed */
    QfNode   *node;              /* its structure */
    VEC(TagPath) tags;           /* where each tag and reference stands */
    ArithVec  ariths;            /* its constraint records */
    InstRefVec refs;             /* its reference records */
} InstInfo;

/* --- the front end -------------------------------------------------------------------- */

typedef struct Front {
    QfArena  *arena;        /* syntax trees and their strings */
    SymTable  syms;
    VEC(Diag) diags;
    size_t    nerrors;
    VEC(LetName) lets;
    bool      parsed_comp;  /* the last qf_parse_expr had comprehensions, conditions or calls */

    /* stage 2: the result of qf_resolve */
    int8_t    tick_scale;   /* the scale of %time: the world's tick */
    DeclVec   decls;        /* every declaration of the content */
    VEC(KindInfo) kinds;
    U64Map    kindidx;      /* Sym → index + 1 */
    VEC(DimInfo) dims;
    SymVec    bases;        /* the unit symbols of declared base dimensions, sorted: base Q_NSI + i */

    VEC(TypeInfo) types;
    U64Map typeidx;              /* Sym → type id + 1 */
    size_t codewords, nbits;
    uint32_t *bittype;           /* bit → type */
    size_t nglb;
    U64Map codeidx;              /* hash(code) → type id + 1 (first with that hash) */
    U64Map meetmemo;             /* load only: an operation after load has its own cache */
    struct { uint64_t key; uint32_t val; } meetcache[4096];

    VEC(FeatInfo) feats;
    U64Map featidx;              /* Sym → feature id + 1: by full name, and by name */
    U64Map labelidx;             /* label Sym → the first feature with it + 1 */
    U64Map plainidx;             /* a packaged name, plain → its one qualified name; or 1
                                    where two packages declare it */

    VEC(InstInfo) insts;
    U64Map instidx;              /* Sym → instance + 1 */

    XPtrVec listelems;           /* element expressions of typed lists [...X] */
    VEC(char *) buildlog;        /* generated types, for the developer */
    VEC(DisjInfo) disjs;
    U64Map disjidx;              /* X * of a disjunction → index + 1 */

    uint32_t top, atomroot, tnumber, ttext, tconstraint, tlist, tcons, tnull, tbool;
    uint32_t ffirst, frest;

    QfStore   store;             /* the content: type expansions and instance structures */
    QfOp     *op;                /* the context of the load */
    bool      loaded;            /* the load is over: the content does not change */
    uint64_t  hash;              /* the content hash (ADR 0015 section 9) */
} Front;

Front *qf_new(void);
void   qf_free(Front *f);

Sym  qf_intern(Front *f, const char *s, size_t len);
/* A string's canonical spelling, with its quotes: what an atom's name is. */
Sym  qf_string_canonical(Front *f, const char *bytes, size_t n);
void qf_string_escape_into(Buf *b, const char *bytes, size_t n);
void qf_diag(Front *f, bool error, Loc loc, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
Sym  qf_sym(Front *f, const char *s);                 /* interns a terminated string */
void qf_string_decode(const char *quoted, Buf *out);  /* the bytes that a canonical string denotes */

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

/* --- syntax tree helpers (xtree.c) ------------------------------------------------------- */

char *x_text(const X *x);                     /* the text form, malloc'd */
X    *x_new(Front *c, XKind k, Loc loc);
X    *x_name(Front *c, Sym name, Loc loc);
X    *x_and(Front *c, X *a, X *b);
void  x_flatten(X *x, XKind k, XPtrVec *out);
bool  x_numeric(const X *x);                  /* a number, a bound, a kind, or a conjunction of those */
bool  x_numeric_or(X *x);                     /* a disjunction of numeric expressions */
bool  x_text_or(X *x);                        /* a disjunction of text: `text` among strings */

/* --- kinds and units (kinds.c) --------------------------------------------------------------- */

/* Reads the kind declarations and makes the kinds and their dimensions.
 * It runs before the type hierarchy is built. */
bool     qf_kinds_build(Front *c);
uint32_t qf_kind_lookup(const Front *c, Sym name);      /* index into kinds, or QF_BOTTOM */
/* The index of a dimension, made on first use. Only before the hierarchy is complete. */
uint32_t qf_dim_index(Front *c, const QDim *d);
uint32_t qf_dim_find(const Front *c, const QDim *d);    /* QF_BOTTOM if the content does not use it */
uint32_t qf_dim_type(Front *c, uint32_t dimix);         /* its type, made on first use */
/* Makes the dimension type that a literal's unit or an arithmetic of kinds
 * needs. Errors are reported later, when the expression is built. */
void     qf_dim_scan(Front *c, const X *x);
/* The unit of a literal (ADR 0014 section 2, ADR 0015 section 4). False with a message. */
bool     qf_unit_parse(const Front *c, const char *text, QfUnit *out, char *err, size_t errlen);
bool     qf_unit_symbol(const Front *c, const char *name);   /* true if the name is a unit symbol */
bool     qf_builtin_kind(const char *name);                  /* %length, %time, ...: a kind of every content */
/* A literal as an exact decimal with its dimension. False with a message. */
bool     qf_literal(const Front *c, const XNum *n, QDim *dim, int64_t *value, int *exp, char *err, size_t errlen);
/* The dimension of a type-level expression: a kind, or a product or quotient of kinds. */
bool     qf_x_dim(const Front *c, const X *x, QDim *out);
void     qf_dim_format(const Front *c, Buf *b, const QDim *d);   /* kg m^2 s^-2, or 1 */

/* Decimal intervals. */
QfDec    qf_dec_norm(QfDec d);                              /* no common factor of ten in its finite bounds */
bool     qf_dec_meet(QfDec a, QfDec b, QfDec *out, bool *range);   /* false if empty; *range: out of range */
/* Converts to quanta of `scale`. A point value must be a whole number of
 * quanta. A bound moves inward to the next quantum. NULL, or the reason. */
const char *qf_dec_to_quanta(QfDec d, int scale, int64_t *lo, int64_t *hi);
void     qf_dec_format(Buf *b, int64_t v, int exp);        /* a decimal number, exactly */

/* --- types (types.c) ------------------------------------------------------------------------ */

bool     types_build(Front *c);
uint32_t type_lookup(const Front *c, Sym name);         /* QF_BOTTOM if none */
uint32_t feat_lookup(const Front *c, Sym name);         /* QF_BOTTOM if none */
/* The feature a label written in content names: a qualified label's; else,
 * of those with the label, the one appropriate to ctx (the type it is written
 * against, or QF_BOTTOM), the package's own, the root's, or the only one.
 * QF_BOTTOM if none; *ambiguous is set if that leaves several. */
uint32_t feat_resolve(const Front *c, Sym label, uint32_t ctx, Sym pkg, bool *ambiguous);
bool     label_names(const Front *c, Sym label, uint32_t f);
Sym      type_pkg(const Front *c, uint32_t t);
/* Names in order: the plain name first, then its package (the root first), so
 * moving declarations into packages doesn't reorder them. */
int      name_cmp(const char *a, const char *b);
bool     is_subtype(const Front *c, uint32_t a, uint32_t b);
uint32_t meet(Front *c, uint32_t a, uint32_t b);        /* during load: it fills the content's cache */
uint32_t meet_pure(const Front *c, uint32_t a, uint32_t b);   /* no cache: for an operation's own cache */
void     meet_cache_clear(Front *c);
bool     approp(const Front *c, uint32_t t, uint32_t f);
bool     introduces(const Front *c, uint32_t t, uint32_t f);     /* t is one of the types that introduce f */
bool     is_value(const Front *c, uint32_t t);          /* a number, a text or a constraint: a node with a value */
bool     is_leaf(const Front *c, uint32_t t);           /* no subtypes, no features */
Sym      anon_name(Front *c, X *orx);
Sym      list_type_name(Front *c, const X *elem, bool cons);

/* --- feature structures (fs.c) --------------------------------------------------------------- */

QfOp    *qf_op_new(Front *f);
void     qf_op_free(QfOp *o);
QfStore *qf_store_new(void);
void     qf_store_free(QfStore *s);

/* Unifies two structures. The result is in `out`, and it can share unchanged
 * nodes with the operands, so the stores of the operands must live as long
 * as the result. NULL if they do not unify: then *failure, if given, says
 * where and why. The two operands must not share nodes at different places. */
const QfNode *qf_unify(QfOp *o, const QfNode *a, const QfNode *b, QfStore *out, QfFailure *failure);
/* *forward: a subsumes b (a is at least as general). *backward: b subsumes a. */
void          qf_subsumes(QfOp *o, const QfNode *a, const QfNode *b, bool *forward, bool *backward);
/* A copy that shares nothing with its source. */
const QfNode *qf_copy(QfOp *o, const QfNode *n, QfStore *out);
/* The most specific structure that subsumes both. */
const QfNode *qf_generalize(QfOp *o, const QfNode *a, const QfNode *b, QfStore *out);
/* The node at a path of features, or NULL. */
const QfNode *qf_at(QfOp *o, const QfNode *n, const uint32_t *path, size_t len);
/* The text form of a structure, for tests and tools. */
void          qf_node_print(const Front *f, Buf *b, const QfNode *n);

/* A structure from handles, with no text (ADR 0015 section 8). */
QfNode       *qf_build_struct(QfOp *o, QfStore *s, uint32_t type, const uint32_t *feats, QfNode *const *vals, size_t n);
QfNode       *qf_build_quantity(QfOp *o, QfStore *s, uint32_t kind_type, QInterval v);
/* qf_build_text interns its string in the front end. So only one thread at a
 * time can call it. Every other call of a context is safe next to other contexts. */
QfNode       *qf_build_text(QfOp *o, QfStore *s, const char *bytes);
const QfNode *qf_type_node(const Front *f, uint32_t type);

/* The quick check (qc.c): a vector of the types at a few paths, compared by
 * meet before a unification. It never rejects a unification that succeeds. */
typedef struct QfQuickCheck QfQuickCheck;
QfQuickCheck *qf_qc_new(const Front *f, const uint32_t *const *paths, const size_t *lens, size_t n);
void          qf_qc_free(QfQuickCheck *qc);
size_t        qf_qc_size(const QfQuickCheck *qc);              /* the paths, and the root */
void          qf_qc_vector(const QfQuickCheck *qc, const QfNode *node, uint32_t *out);
bool          qf_qc_compatible(const QfQuickCheck *qc, const uint32_t *a, const uint32_t *b);

/* Internal to the engine. */
uint64_t op_begin(QfOp *c);
void     op_end(QfOp *c, uint64_t saved);
uint32_t op_meet(QfOp *c, uint32_t a, uint32_t b);
void     path_push(QfOp *c, uint32_t f);
void     path_set(QfOp *c, const uint32_t *path, size_t n);
QfNode  *node_new(QfOp *c, QfStore *a, uint32_t type);
QfNode  *leaf_new(QfOp *c, QfStore *a, uint32_t type, int64_t lo, int64_t hi);
Scratch *sx_get(QfOp *c, const QfNode *n);
Scratch *sx_make(QfOp *c, const QfNode *n);
QfNode  *carc_find(QfOp *c, QfNode *n, uint32_t f);
void     carc_add(QfOp *c, QfNode *n, uint32_t f, QfNode *v);
QfNode  *deref(QfOp *c, QfNode *n);
QfVal    node_val(QfOp *c, const QfNode *n);
bool     unify1(QfOp *c, QfNode *a, QfNode *b);
QfFailure *fail_info(QfOp *c);                           /* the last failure, formatted */
QfNode  *copy_raw(QfOp *c, QfStore *a, QfNode *n);
QfNode  *copy_out(QfOp *c, QfStore *a, QfNode *n, bool *closed_ok);
QfNode  *last_copy(QfOp *c, QfNode *n);                  /* the copy that the last copy_out made of n */
void     copy_out_many(QfOp *c, QfStore *a, QfNode **roots, size_t n);
QfNode  *node_arc(const QfNode *n, uint32_t f);          /* final structures only */
void     path_format(const Front *f, Buf *b, const uint32_t *path, size_t n);
QfNode  *node_at(QfNode *n, const uint32_t *path, size_t len);
bool     ext_equal(const Front *f, const QfNode *a, const QfNode *b);
bool     is_lazy(QfOp *c, const QfNode *n);
QfNode  *materialize(QfOp *c, QfNode *n);
QfNode  *fs_generalise(QfOp *c, QfStore *out, QfNode *a, QfNode *b);

/* --- the load (resolve.c) ------------------------------------------------------------------- */

/* Loads a content: parses each source, then resolves all of them together
 * (ADR 0015). names[i] is the file name of sources[i], for diagnostics. A
 * front end loads one content. False if there is an error: the diagnostics
 * are in f->diags, sorted by location.
 *
 * The result does not depend on the order of the sources or on the order of
 * the declarations in a source. */
bool     qf_load(Front *f, const char *const *names, const char *const *sources, size_t n);
/* The result of a load as text: the kinds, the types with their expansions,
 * the features, the instances, and the constraint records. The content hash
 * (f->hash) is the hash of this text. */
void     qf_dump(const Front *f, Buf *b);
uint32_t qf_instance(Front *f, const char *name);        /* index into f->insts, or QF_BOTTOM */

/* Every constraint record of a structure: `own` (the records of an instance,
 * or NULL), then the records of the type of each node, with the path of the
 * node as their prefix. Stage 3 makes propagators from them. */
typedef struct { const ArithRec *rec; U32Vec prefix; } QfRecUse;
typedef VEC(QfRecUse) QfRecUses;
void     qf_records(const Front *f, const QfNode *root, const ArithVec *own, QfRecUses *out);
void     qf_records_free(QfRecUses *v);

/* Every reference record of a structure, with paths from its root: `own`
 * (the records of an instance, or NULL), then the records of the type of
 * each node. Free the result with qf_refs_free. */
void     qf_refs(const Front *f, const QfNode *root, const InstRefVec *own, InstRefVec *out);
void     qf_refs_free(InstRefVec *v);

void     qf_sort_diags(Front *f);                        /* by location, then by message */

/* --- the elaborator (elab.c, ir.c) ------------------------------------------------------------ */

/* Makes the IR of a loaded content (ADR 0016). NULL if the content has an
 * error: the diagnostics are in f->diags. The IR owns its memory, so it
 * lives after the Front. */
QIr     *qf_elaborate(Front *f, const QiOptions *opt);
/* The kernel that source names, from `opt` or from the native kernels. NULL if none. */
const QkKernel *qi_find_kernel(const QiOptions *opt, const char *name);

QfNode  *type_expansion(QfOp *c, uint32_t t);            /* NULL while t is being expanded */
QfNode  *type_instance(QfOp *c, uint32_t t, QfStore *a); /* a copy, or a lazy node */

#endif
