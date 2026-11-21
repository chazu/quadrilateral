---
status: accepted
---

# Quantity types carry a decimal scale, and values are fixed-point intervals

Quadrilateral must simulate arbitrary domains, from a room to a star, with exact conservation and reproducible runs (decisions D4 and D5 in `quadrilateral-review-fable.md`). Triangle has one fixed quantum for each dimension, for example 0.1 mm for length, so it cannot serve a domain far from the one it was built for. We decided that each quantity type carries its own power-of-ten scale. A value is an interval of int64 quanta of that scale. The compiler resolves all types when it builds the execution graph, so the run-time arithmetic is plain integer interval arithmetic.

## Decision

### 1. A quantity type has four parts

| Part | Meaning |
|---|---|
| Kind | A name that separates quantity types that have the same dimension but must not mix. Example: energy and torque are both kg·m²/s². |
| Dimension | Exponents over the base dimensions. |
| Scale | An integer `e`. One quantum is 10^e of the coherent unit of the dimension. Example: energy with `e = -3` counts in millijoules. |
| Width | 64. A width of 128 is reserved and is not implemented until a domain needs it (see Consequences). |

**Base dimensions.** The seven SI base dimensions are always present: length, mass, time, electric current, temperature, amount of substance, and luminous intensity. A world can declare more base dimensions, for example currency, information, or angle. The limit is 32 base dimensions in each world, so a world can declare 25. The dimension of a quantity type is an array of 32 signed 8-bit exponents, one for each base dimension of the world. Most exponents are zero.

A base dimension is necessary only when products and quotients must track it, for example a price in currency per kilogram. To keep two quantities apart that must not be added, such as a count of dwarves and a count of apples, use two kinds, not two base dimensions.

**Kinds.**
- Addition, subtraction, comparison, and unification need two operands of the same kind.
- A product or a quotient gives an anonymous intermediate kind that has only a dimension.
- An assignment of an intermediate value to a field converts it to the field's kind if the dimensions are equal.
- Triangle chose torque silently for a product that could be torque or energy. This rule removes that guess, because the field that receives the product names the kind.

**Default scales.** Most content never declares a scale. A kind without a declared scale uses the default for its dimension:

| Dimension | Default quantum | Source |
|---|---|---|
| Length | 10^-4 m (0.1 mm) | Triangle |
| Mass | 10^-3 kg (1 g) | Triangle |
| Time | The world's tick (D7: 10^-6 s by default) | D7 |
| Temperature | 10^-2 K | Triangle |
| Amount of substance | 10^-3 mol | Triangle |
| Electric current | 10^-3 A | New |
| Energy | 10^-3 J | New. Triangle used 100 J, which is too coarse for the two-room experiment. |
| Power | 10^-3 W | New |
| Dimensionless ratio, probability | 10^-9 | New |
| Count (a built-in dimensionless kind) | 10^0 | New. A count and a ratio have the same dimension, so the kind selects this default. |
| Any other dimension | 10^-3 of the coherent unit | New |

A world's tick must be a power of ten of seconds, so that time follows the same rule as every other dimension.

### 2. A value is a closed interval of quanta

- A value is a pair `lo`, `hi` of int64 quanta, with `lo <= hi`. A point value has `lo == hi`.
- `INT64_MIN` means negative infinity, and `INT64_MAX` means positive infinity, as in Triangle. A finite value is strictly between the two.
- An interval with an infinite bound is a constraint that waits for more information. Triangle calls this LIFE-style suspension.
- The type is not stored in the value. A cell or a port has its type in a static table that the compiler builds.

### 3. Arithmetic

- All operations are interval operations, and the bounds round outward. A result always contains every exact answer.
- Intermediate results use `__int128`.
- A sum or a difference first converts both operands to the finer of their two scales. A product adds the two scales. A quotient subtracts the second scale from the first. The compiler then converts the result to the scale of its destination.
- Narrowing also runs backward, for sums, differences, products, and quotients, including a product or quotient of two variables. Phase 2 implements the backward direction.
- The compiler precomputes each conversion factor (a power of ten) when it builds the execution graph. The run-time arithmetic does no type work. Triangle carried exponents and a rational quantum in each run-time value. That costs space and time on every operation.

### 4. Conversion and rounding

- A conversion to a finer scale is exact. If the result does not fit, see section 5.
- A conversion to a coarser scale rounds. Interval bounds round outward. A point value uses round-half-even by default. A field can declare another rule: floor, ceiling, or toward zero.
- A literal that is not a whole number of quanta is an error, as in Triangle. Example: `0.15 mm` is an error for a length kind with a quantum of 0.1 mm.
- Display units, such as km, h, and °C, are a table used only to parse and format values. A display unit must be a whole number of quanta of the kind that uses it, or the compiler rejects it for that kind. Example: 1 min is 60,000,000 quanta at 1 µs, and 1 inch is 254 quanta at 0.1 mm.
- An offset, as in °C, exists only for input and output. Storage always uses an absolute scale, such as kelvin.

### 5. Overflow

- If an interval bound overflows, the interval widens, and it still contains every exact answer. A lower bound below the finite range becomes negative infinity. A lower bound above the finite range becomes the largest finite value, because negative infinity would also be sound but needlessly wide. Upper bounds are the mirror image.
- The finite range is symmetric, from `-(INT64_MAX - 1)` to `INT64_MAX - 1`, so negation never overflows. `INT64_MIN + 1` is not a valid value.
- If a point value overflows, it is an error. Point values are authoritative state and transfers, so a silent wrap or saturation would corrupt the world. Bruggadung learned this lesson: its fixed caps that failed silently became errors (`f405c3a`).

### 6. Conserved interactions

Both ports of a conserved interaction use the same quantity type, and the compiler rejects a mismatch. To couple two scales, the author inserts an explicit converter phenomenon. The converter keeps the rounding remainder in its own ledger, so the total of the conserved quantity stays exact.

### 7. Kernel-private floating-point state

A kernel can keep private state in floating point, for example a chaotic system or a value that covers many orders of magnitude. Private state is not authoritative, and no other phenomenon can read it. The kernel publishes only quantity values through its ports. Its contract states that it is reproducible only for the same build on the same platform.

## C sketch

This sketch shows the intended shape. It is not a final interface.

```c
#define Q_MAXBASE 32

typedef struct { int8_t exp[Q_MAXBASE]; } QDim;      /* exponents over base dimensions */

typedef struct {
    uint32_t kind;      /* index into the world's kind table; 0 = anonymous intermediate */
    QDim     dim;
    int8_t   scale;     /* one quantum = 10^scale coherent units */
    uint8_t  width;     /* 64; 128 reserved */
    uint8_t  rounding;  /* Q_ROUND_HALF_EVEN, Q_ROUND_FLOOR, Q_ROUND_CEIL, Q_ROUND_ZERO */
} QType;

typedef struct { int64_t lo, hi; } QInterval;         /* quanta; INT64_MIN/MAX = -inf/+inf */

#define Q_NINF INT64_MIN
#define Q_INF  INT64_MAX

/* Compile time: result types and conversion plans. */
int  qtype_mul(const QType *a, const QType *b, QType *out);
int  qtype_conv_plan(const QType *from, const QType *to, QConv *plan);

/* Run time: no type work. */
QInterval qi_add(QInterval a, QInterval b);           /* same scale */
QInterval qi_mul(QInterval a, QInterval b);
QInterval qi_conv(QInterval a, const QConv *plan);    /* outward */
int       qp_conv(int64_t v, const QConv *plan, int64_t *out);  /* point value; error on overflow */
```

## Considered options

- **One scale for each dimension (Triangle).** This option is simple, but it cannot serve domains far apart in magnitude. We rejected it.
- **A rational quantum `num/den` for each value (Triangle's `Quantity`).** This option is more general. We rejected it for two reasons. Decimal scales combine by addition of exponents, and that addition never overflows. A rational quantum needs a greatest-common-divisor reduction and can overflow. Units that are not decimal, such as the minute and the inch, are exact multiples of a decimal quantum, so they work as display units.
- **Binary scales (powers of two).** A conversion would be a fast shift. We rejected this option because decimal values such as 0.1 would not be exact.
- **Floating point or decimal floating point.** We rejected this option. It breaks exact conservation and reproducibility (review section 5.2).
- **int128 for every value.** This option doubles memory and slows the common case. We rejected it as the default and reserved it as a width.
- **Arbitrary-precision integers.** This option is slow and needs allocation in the run-time arithmetic. We rejected it.

## Consequences

- The compiler must check the quantity type at each port and at each assignment, and it must insert or reject conversions.
- Width 128 needs 256-bit intermediate results for products. The project defers this until a domain needs width 128. The design keeps the width field so that the decision stays open.
- An angle in degrees is not an exact multiple of a power-of-ten quantum of radians. A world that needs exact degrees can declare angle as a base dimension with the degree or the turn as its coherent unit.
- A conversion between two scales that differ by more than 38 decades either gives zero or overflows. The compiler can report such a conversion when it builds the graph.
- A world can have at most 32 base dimensions. The limit costs almost nothing, because a dimension exists only at compile time. Run-time values do not carry it.
- The layout of `QDim` must not appear in the kernel interface. A kernel refers to a quantity type by an identifier and asks the runtime for the scale. With this rule, the limit can change later and no kernel breaks.
