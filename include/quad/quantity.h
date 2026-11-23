#ifndef QUAD_QUANTITY_H
#define QUAD_QUANTITY_H

/* Quantity types and fixed-point interval arithmetic (docs/adr/0001).
 *
 * A quantity type has a kind, a dimension, a decimal scale, and a width. A
 * value is a closed interval of int64 quanta, where one quantum is 10^scale
 * coherent units. The value does not carry its type: the compiler resolves
 * types when it builds the execution graph, and it gives each run-time
 * operation a conversion plan (QConv). The run-time functions (qi_*, qp_*)
 * therefore do no type work.
 *
 * Two families of run-time functions exist:
 *   qi_*  interval operations. Bounds round outward, so a result contains
 *         every exact answer. A bound that overflows widens. They never fail.
 *   qp_*  point operations, for authoritative state and transfers. They
 *         round with the plan's rounding rule, and an overflow is an error.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- status ---------------------------------------------------------------- */

typedef enum {
    Q_OK = 0,
    Q_EOVERFLOW,   /* a point value does not fit */
    Q_EINFINITE,   /* a point operation got an infinite value */
    Q_EDIVZERO,    /* a point division by zero */
    Q_EINEXACT,    /* a literal is not a whole number of quanta */
    Q_ESYNTAX,     /* a literal is malformed */
    Q_EDIM,        /* dimensions do not match, or an exponent overflows */
    Q_ESCALE,      /* a scale is out of range */
    Q_EKIND,       /* kinds do not match */
    Q_EWIDTH,      /* width 128 is reserved and not implemented */
} QStatus;

const char *q_status_str(QStatus s);

/* --- values ---------------------------------------------------------------- */

/* INT64_MIN and INT64_MAX are the infinities. A finite value is in
 * [Q_FMIN, Q_FMAX], which is symmetric, so negation never overflows.
 * INT64_MIN + 1 is not a valid value. */
#define Q_NINF INT64_MIN
#define Q_INF  INT64_MAX
#define Q_FMAX (INT64_MAX - 1)
#define Q_FMIN (-Q_FMAX)

typedef struct { int64_t lo, hi; } QInterval;

typedef enum {
    Q_ROUND_HALF_EVEN = 0,
    Q_ROUND_FLOOR,
    Q_ROUND_CEIL,
    Q_ROUND_ZERO,
} QRound;

/* A conversion plan: the exact result is multiplied by 10^k and then rounded
 * to an integer number of quanta. Interval operations ignore `rounding` and
 * round outward. */
typedef struct { int16_t k; uint8_t rounding; } QConv;

/* --- dimensions and types (compile time) ------------------------------------ */

#define Q_MAXBASE 32

/* The SI base dimensions have fixed indices. A world declares more base
 * dimensions from Q_NSI upward. */
enum {
    Q_BASE_LENGTH, Q_BASE_MASS, Q_BASE_TIME, Q_BASE_CURRENT,
    Q_BASE_TEMPERATURE, Q_BASE_AMOUNT, Q_BASE_LUMINOUS,
    Q_NSI
};

typedef struct { int8_t exp[Q_MAXBASE]; } QDim;

typedef struct {
    uint32_t kind;      /* index into the world's kind table; 0 = anonymous intermediate */
    QDim     dim;
    int8_t   scale;     /* one quantum = 10^scale coherent units */
    uint8_t  width;     /* 64; 128 reserved */
    uint8_t  rounding;  /* QRound for point results stored in this type */
} QType;

QDim    qdim_none(void);
QDim    qdim_base(int base);
bool    qdim_eq(const QDim *a, const QDim *b);
bool    qdim_is_none(const QDim *d);
QStatus qdim_mul(const QDim *a, const QDim *b, QDim *out);
QStatus qdim_div(const QDim *a, const QDim *b, QDim *out);

/* The default scale for a dimension (ADR 0001 section 1). `tick_scale` is
 * the scale of the world's tick. A dimensionless kind that is a count uses
 * scale 0, but only the kind table knows that, so this function returns the
 * ratio default (-9) for every dimensionless value. */
int8_t qdim_default_scale(const QDim *d, int8_t tick_scale);

/* A width-64 type with round-half-even. */
QType qtype_make(uint32_t kind, QDim dim, int8_t scale);

/* Result types: a product or a quotient is anonymous, with the exact scale.
 * A sum needs equal dimensions and compatible kinds, and it has the finer
 * scale. Two kinds are compatible if they are equal or one is anonymous. */
QStatus qtype_mul(const QType *a, const QType *b, QType *out);
QStatus qtype_div(const QType *a, const QType *b, QType *out);
QStatus qtype_sum(const QType *a, const QType *b, QType *out);

/* Plans. A conversion needs equal dimensions, and the source kind must equal
 * the destination kind or be anonymous. A product or quotient plan writes
 * directly into the destination type. */
QStatus qtype_conv_plan(const QType *from, const QType *to, QConv *out);
QStatus qtype_mul_plan(const QType *a, const QType *b, const QType *dest, QConv *out);
QStatus qtype_div_plan(const QType *a, const QType *b, const QType *dest, QConv *out);

/* --- interval operations (run time) ----------------------------------------- */

QInterval qi_point(int64_t v);
QInterval qi_top(void);
bool      qi_valid(QInterval a);
bool      qi_is_point(QInterval a);
bool      qi_contains(QInterval a, int64_t v);
bool      qi_subset(QInterval a, QInterval b);           /* a within b */
bool      qi_meet(QInterval a, QInterval b, QInterval *out);  /* false if empty */

QInterval qi_neg(QInterval a);
QInterval qi_add(QInterval a, QInterval b);              /* operands in one scale */
QInterval qi_sub(QInterval a, QInterval b);              /* operands in one scale */
QInterval qi_conv(QInterval a, const QConv *c);
QInterval qi_mul(QInterval a, QInterval b, const QConv *c);
QInterval qi_div(QInterval a, QInterval b, const QConv *c);  /* b containing 0 gives top */

/* --- point operations (run time) -------------------------------------------- */

QStatus qp_add(int64_t a, int64_t b, int64_t *out);
QStatus qp_sub(int64_t a, int64_t b, int64_t *out);
QStatus qp_conv(int64_t a, const QConv *c, int64_t *out);
QStatus qp_mul(int64_t a, int64_t b, const QConv *c, int64_t *out);
QStatus qp_div(int64_t a, int64_t b, const QConv *c, int64_t *out);

/* --- dimension text ------------------------------------------------------------ */

/* Parse a dimension (ADR 0009 section 10). The grammar:
 *
 *   dim   := ""  |  "1"  |  terms [ "/" terms ]
 *   terms := term { (" " | "*") term }
 *   term  := symbol [ "^" ["-"] digits ]
 *
 * The symbols are the SI base units (m kg s A K mol cd), the derived units
 * N, J, W, Pa, Hz, C, V, ohm, F, H, S, T and Wb, and the names of the
 * world's declared base dimensions:
 * names[i] is base dimension Q_NSI + i. Every term after "/" is negated.
 * Example: "kg m^2 s^-2" and "J" and "W s" are the same dimension. */
QStatus q_parse_dim(const char *s, size_t len, const char *const *names, size_t nnames, QDim *out);

/* --- decimal text ----------------------------------------------------------- */

/* Parse [+-]digits[.digits][e[+-]digits] in coherent units into quanta of
 * `scale`. A value that is not a whole number of quanta is Q_EINEXACT. */
QStatus q_parse_decimal(const char *s, size_t len, int scale, int64_t *out);

/* Format quanta of `scale` as a decimal in coherent units, with no trailing
 * zeros after the point. Infinities are "inf" and "-inf". Returns the length
 * of the full text, as snprintf does. */
size_t q_format_decimal(int64_t q, int scale, char *buf, size_t len);

#endif
