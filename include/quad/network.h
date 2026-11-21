#ifndef QUAD_NETWORK_H
#define QUAD_NETWORK_H

/* The propagator network (design section 8.3, docs/adr/0007 and 0008).
 *
 * A network has cells and propagators. A cell has a quantity type and two
 * interval values: its base, which is the committed state plus the schema
 * constraints, and its current value, which propagation narrows inside the
 * epoch. The real value of a quantity is always inside the cell's current
 * interval.
 *
 * Inside an epoch, everything is monotonic. qn_tell only narrows a cell, and a
 * propagator writes only through qn_tell. Propagation to a fixed point
 * therefore gives the same result in every order. The only non-monotonic
 * operations are qn_stage and qn_begin_epoch, and they are refused during
 * propagation.
 *
 * The agenda is first in, first out, and it starts in the order in which the
 * propagators were added, so a run that stops at its budget is reproducible.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quad/quantity.h"

typedef struct QNet QNet;
typedef uint32_t QnCell;

typedef enum {
    QN_OK = 0,
    QN_CONTRADICTION,   /* a cell's domain became empty */
    QN_BUDGET,          /* propagation stopped at its budget; qn_propagate continues it */
    QN_ETYPE,           /* the cells' quantity types do not fit the propagator */
    QN_ECELL,           /* no such cell */
    QN_EVALUE,          /* an invalid interval */
    QN_ESTAGED,         /* the cell already has a staged value for this boundary */
    QN_EPROPAGATING,    /* a non-monotonic call during propagation */
    QN_ENOMEM,
} QnStatus;

const char *qn_status_str(QnStatus s);

QNet *qn_new(void);
void  qn_free(QNet *net);

/* --- building ------------------------------------------------------------ */

QnStatus qn_cell(QNet *net, const QType *type, QInterval base, QnCell *out);

/* Built-in propagators. Each one narrows every cell it names, in every
 * direction. `why`, if not NULL, gets the type error behind QN_ETYPE. */
QnStatus qn_sum(QNet *net, QnCell z, QnCell x, QnCell y, QStatus *why);         /* z = x + y */
QnStatus qn_difference(QNet *net, QnCell z, QnCell x, QnCell y, QStatus *why);  /* z = x - y */
QnStatus qn_product(QNet *net, QnCell z, QnCell x, QnCell y, QStatus *why);     /* z = x * y */
QnStatus qn_equal(QNet *net, QnCell a, QnCell b, QStatus *why);                 /* a = b */

/* z = x / y, as x = z * y. Where y can be 0, this is weaker than division:
 * it also allows y = 0 with x = 0 and any z. */
QnStatus qn_quotient(QNet *net, QnCell z, QnCell x, QnCell y, QStatus *why);

/* A custom propagator. It reads with qn_get and writes only with qn_tell, so
 * it can only narrow. It runs again whenever one of `cells` narrows. */
typedef void (*QnFn)(QNet *net, void *ctx, const QnCell *cells, size_t n);
QnStatus qn_custom(QNet *net, const QnCell *cells, size_t n, QnFn fn, void *ctx);

/* --- inside an epoch (monotonic) ------------------------------------------ */

size_t       qn_ncells(const QNet *net);
QInterval    qn_get(const QNet *net, QnCell c);
QInterval    qn_base(const QNet *net, QnCell c);
const QType *qn_type(const QNet *net, QnCell c);

/* Narrows c to c ∩ v. QN_CONTRADICTION if the result is empty; then the cell
 * keeps its value, the network is contradicted until the next epoch, and
 * every later tell returns QN_CONTRADICTION. */
QnStatus qn_tell(QNet *net, QnCell c, QInterval v);

/* Runs the agenda until it is empty (QN_OK), a cell is empty
 * (QN_CONTRADICTION), or the budget is used (QN_BUDGET). */
QnStatus qn_propagate(QNet *net);

/* The number of propagator runs for each qn_propagate call. 0 means no limit,
 * the default. */
void     qn_set_budget(QNet *net, uint64_t runs);
uint64_t qn_runs(const QNet *net);   /* all propagator runs so far */
bool     qn_contradicted(const QNet *net, QnCell *where);

/* --- epoch boundaries (non-monotonic) ------------------------------------- */

uint64_t qn_epoch(const QNet *net);

/* Replaces the cell's base at the next boundary. A cell has one writer, so a
 * second stage of the same cell before the boundary is QN_ESTAGED. */
QnStatus qn_stage(QNet *net, QnCell c, QInterval v);

/* Applies the staged values, increments the epoch, sets every cell's current
 * value to its base, and puts every propagator on the agenda in order. The
 * caller then runs qn_propagate. */
QnStatus qn_begin_epoch(QNet *net);

#endif
