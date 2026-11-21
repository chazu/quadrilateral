#ifndef QUAD_WORLD_H
#define QUAD_WORLD_H

/* A world: the host of ADR 0009 and the scheduler of ADR 0006.
 *
 * A world has quantity kinds, phenomena, cells, and kernels. A cell has a
 * path such as "roomA.energy", and the first component of the path names the
 * phenomenon that owns the cell. A cell of a conserved kind is a stock: only
 * transfers change it, and its value is always exact.
 *
 * A kernel's slot "x.y" binds to the cell "<phenomenon>.x.y", unless a port
 * connection redirects it: after qw_connect(w, wall, "hot", "roomA"), the slot
 * "hot.temperature" binds to "roomA.temperature".
 *
 * This is the C-level host. Quadrilateral source will describe the same
 * things, and error messages already name cells by path (ADR 0007).
 *
 * The scheduler is the first version of ADR 0006: every evolving phenomenon
 * advances to one shared boundary. Each phenomenon has at most one kernel, so
 * each group of ADR 0009 section 9 has one member.
 */

#include <stdbool.h>
#include <stdint.h>

#include "quad/kernel.h"
#include "quad/network.h"

typedef struct QWorld QWorld;
typedef uint32_t QwKind;
typedef uint32_t QwPhen;

typedef enum {
    QW_OK = 0,
    QW_ENAME,           /* an unknown or malformed name */
    QW_EDUP,            /* a name that already exists */
    QW_ETYPE,           /* quantity types or dimensions do not fit */
    QW_EOWNER,          /* a write to a cell that another phenomenon owns */
    QW_EKERNEL,         /* a bad contract, or a kernel broke a rule of the interface */
    QW_EINAPPLICABLE,   /* an input is outside a kernel's valid range */
    QW_ECONTRADICTION,  /* propagation found an empty domain */
    QW_ESTOCK,          /* a stock changed other than by a transfer */
    QW_ETIME,           /* a time before now, or past the end of the clock */
    QW_ENOMEM,
} QwStatus;

const char *qw_status_str(QwStatus s);
const char *qw_error(const QWorld *w);   /* the message of the last error */

QWorld *qw_new(uint64_t seed, int8_t tick_scale);
void    qw_free(QWorld *w);
QNet   *qw_net(QWorld *w);

/* --- building ---------------------------------------------------------------- */

QwStatus qw_base_dim(QWorld *w, const char *name);
QwStatus qw_kind(QWorld *w, const char *name, const char *dim, int scale, bool conserved, QwKind *out);
QwStatus qw_kind_type(const QWorld *w, QwKind kind, QType *out);
QwStatus qw_phenomenon(QWorld *w, const char *name, QwPhen *out);
QwStatus qw_cell(QWorld *w, const char *path, QwKind kind, QInterval base, QnCell *out);
QwStatus qw_find(const QWorld *w, const char *path, QnCell *out);
const char *qw_cell_path(const QWorld *w, QnCell c);
QwStatus qw_connect(QWorld *w, QwPhen p, const char *port, const char *target);
QwStatus qw_attach(QWorld *w, QwPhen p, const QkKernel *k);

/* --- running ------------------------------------------------------------------- */

/* Advances the world to `until`, boundary by boundary. */
QwStatus qw_run(QWorld *w, int64_t until);

/* An input from outside the simulation: replaces a value at the current time,
 * in a new epoch. A stock cannot take an input. */
QwStatus qw_input(QWorld *w, const char *path, QInterval v);

int64_t   qw_now(const QWorld *w);
uint64_t  qw_boundaries(const QWorld *w);
QInterval qw_get(const QWorld *w, const char *path);     /* the top interval if unknown */
uint64_t  qw_predicts(const QWorld *w, QwPhen p);        /* predict calls so far */

/* A hash of all authoritative state: the clock, the epoch, and every cell's
 * base. Two runs with the same inputs must give the same hash (ADR 0004). */
uint64_t qw_hash(const QWorld *w);

#endif
