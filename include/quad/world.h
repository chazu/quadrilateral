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
 * A phenomenon can have models (ADR 0010). A model's cells have paths under
 * the model, "roomB.fine.c0.energy", and one model is active. qw_port maps a
 * port of the phenomenon to a path prefix for each model, so that another
 * phenomenon can connect to "roomB.west" and reach the active model's cells.
 * qw_switch changes the active model with a map kernel.
 *
 * A phenomenon can be an instance (ADR 0013): it is declared dormant, it
 * starts and ends at a boundary, and its name is its key, for example
 * "combustion(building7)". An existence condition starts and ends it inside
 * qw_run, or qw_start and qw_end do, as inputs from outside.
 *
 * The scheduler is the first version of ADR 0006: every evolving kernel
 * advances to one shared boundary. A phenomenon runs at most two kernels:
 * its own, and its active model's. Each group of ADR 0009 section 9 has one
 * member.
 */

#include <stdbool.h>
#include <stddef.h>
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
    QW_ELIFE,           /* an instance in the wrong state, or existence conditions that do not settle */
    QW_ESNAPSHOT,       /* bytes that are not a snapshot, or a snapshot of a world with another structure */
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
/* An enumeration kind (ADR 0012): 1 to 64 atom names, in order. Atom i is
 * bit i of a set cell's mask. */
QwStatus qw_enum_kind(QWorld *w, const char *name, const char *const *atoms, unsigned n, QwKind *out);
int      qw_atom(const QWorld *w, QwKind kind, const char *name);   /* the atom's index, or -1 */
QwStatus qw_phenomenon(QWorld *w, const char *name, QwPhen *out);
QwStatus qw_find_phenomenon(const QWorld *w, const char *name, QwPhen *out);   /* QW_ENAME if there is none */
QwStatus qw_cell(QWorld *w, const char *path, QwKind kind, QInterval base, QnCell *out);
/* A set cell of an enumeration kind, with the mask of its possible atoms */
QwStatus qw_set_cell(QWorld *w, const char *path, QwKind kind, uint64_t base, QnCell *out);
/* An anonymous kind: a dimension and a scale, with no name (ADR 0001). A
 * value of an anonymous kind converts to every kind of its dimension. A
 * field with a dimension and no kind has one, and so does the value of an
 * inner expression of a law (ADR 0018). The same dimension and scale give the
 * same kind. An anonymous kind is not conserved. */
QwStatus qw_anonymous_kind(QWorld *w, const char *dim, int scale, QwKind *out);
/* A derived cell: a value that the laws give. It is a derived field, such
 * as the temperature of a room whose state is its energy (ADR 0019), or an
 * inner value of a law, such as the difference in g * (a - b), or a constant
 * of a law (ADR 0018). Its base holds at every time: the whole range, a
 * bound, or a point for a constant. A derived cell is not state: qw_hash
 * does not cover it, it takes no input, and a kernel cannot write it. */
QwStatus qw_derived_cell(QWorld *w, const char *path, QwKind kind, QInterval base, QnCell *out);
/* A second path for the cell `cell`, in the same phenomenon. A port that is a
 * view of the phenomenon's own state uses it: after
 * qw_alias(w, "roomA.west.energy", "roomA.energy"), a connection to
 * "roomA.west" reaches the room's cells. An alias is a name, not state:
 * qw_hash does not cover it, and qw_cell_path gives the cell's first path. */
QwStatus qw_alias(QWorld *w, const char *path, const char *cell);
/* The cell at a path or at an alias */
QwStatus qw_find(const QWorld *w, const char *path, QnCell *out);
const char *qw_cell_path(const QWorld *w, QnCell c);
QwStatus qw_connect(QWorld *w, QwPhen p, const char *port, const char *target);
QwStatus qw_attach(QWorld *w, QwPhen p, const QkKernel *k);
/* Gives a generated kernel to the world: qw_free calls free_kernel(k). The
 * world does not attach it: call qw_attach also. */
QwStatus qw_adopt_kernel(QWorld *w, QkKernel *k, void (*free_kernel)(QkKernel *));

/* --- models (ADR 0010) ----------------------------------------------------------- */

/* Declares a model. The first model of a phenomenon is active. A stock of an
 * inactive model must start at zero. */
QwStatus qw_model(QWorld *w, QwPhen p, const char *model);
/* Propagators added between begin and end belong to the model, and run only
 * while it is active. */
QwStatus qw_model_begin(QWorld *w, QwPhen p, const char *model);
void     qw_model_end(QWorld *w);
/* A model's kernel. Its slot "x" binds to "<p>.<model>.x" if that cell
 * exists, and otherwise to "<p>.x". */
QwStatus qw_model_attach(QWorld *w, QwPhen p, const char *model, const QkKernel *k);
/* In `model`, the port "<p>.<port>" leads to the path prefix `target`. */
QwStatus qw_port(QWorld *w, QwPhen p, const char *port, const char *model, const char *target);
/* Switches to `model` at the current time. The map kernel's slots "from.x"
 * and "to.x" bind to the old and the new model. Every stock of the old model
 * must be exactly zero afterwards, or the switch fails and nothing changes. */
QwStatus qw_switch(QWorld *w, QwPhen p, const char *model, const QkKernel *map);
const char *qw_active_model(const QWorld *w, QwPhen p);   /* NULL without models */

/* A transition (ADR 0021 section 5): a name for the way from the model
 * `from` to the model `to`, with the map kernel of the switch. A phenomenon
 * has at most one transition for each pair of models, and a name once. The
 * kernel must live as long as the world. */
QwStatus qw_transition(QWorld *w, QwPhen p, const char *name, const char *from, const char *to, const QkKernel *map);
/* Switches to `model` with the map of the transition from the active model.
 * QW_ENAME if the phenomenon has no such transition. */
QwStatus qw_switch_to(QWorld *w, QwPhen p, const char *model);

/* --- instances (ADR 0013) ---------------------------------------------------------- */

/* The built-in enumeration kind "boolean": atom 0 is "false", atom 1 "true". */
QwKind qw_boolean(const QWorld *w);

/* Declares an instance: a phenomenon that starts and ends during a run. Its
 * key is its name. A name can contain letters, digits, '_', '(', ')' and ','.
 * A new instance is dormant. The call also creates the cell "<key>.alive", a
 * boolean that only the host writes; the feature name "alive" is reserved.
 *
 * The building functions above add the instance's cells, ports, models and
 * kernels, at any boundary: before the run or between calls to qw_run. A
 * stock of an instance must be declared as zero. While the instance is
 * dormant, its kernels do not run and do not exist. A phenomenon that is not
 * an instance cannot connect a port to an instance. */
QwStatus qw_instance(QWorld *w, const char *key, QwPhen *out);
/* Propagators added between begin and end belong to the instance, and run
 * only while it is alive. qw_model_begin and qw_model_end work inside. */
QwStatus qw_build_begin(QWorld *w, QwPhen p);
void     qw_build_end(QWorld *w);
/* The map kernels of the start and of the end; each can be NULL. A map's
 * slot "x" binds to "<key>.x", or through a port of the instance to a
 * participant. The start map can move stocks from the participants into the
 * instance. The end map must move every stock out: if a stock of the
 * instance is not exactly zero afterwards, the end fails and nothing
 * changes. */
QwStatus qw_instance_maps(QWorld *w, QwPhen p, const QkKernel *start, const QkKernel *end);
/* The existence condition: the path of a boolean cell. At each boundary of
 * qw_run, and when qw_run begins, the instance starts if the condition is
 * true and every participant is alive, and it ends if the condition is
 * false. An unknown condition changes nothing. When an instance ends, the
 * alive instances that connect to it end first. Starts and ends repeat until
 * nothing changes; an instance that would change twice at one boundary is
 * the error QW_ELIFE. */
QwStatus qw_exists_when(QWorld *w, QwPhen p, const char *condition);
/* Inputs from outside the simulation: start or end an instance that has no
 * condition, at the current time. A start fails while a participant is
 * dormant. An end fails while an alive instance connects to this one. At an
 * end, every cell of the instance that is not a stock returns to its
 * declared value. */
QwStatus qw_start(QWorld *w, QwPhen p);
QwStatus qw_end(QWorld *w, QwPhen p);
bool     qw_alive(const QWorld *w, QwPhen p);   /* true for a phenomenon that is not an instance */
uint64_t qw_starts(const QWorld *w);
uint64_t qw_ends(const QWorld *w);

/* --- running ------------------------------------------------------------------- */

/* Advances the world to `until`, boundary by boundary. Existence conditions
 * are checked when the call begins and after each boundary. */
QwStatus qw_run(QWorld *w, int64_t until);

/* An input from outside the simulation: replaces a value at the current time,
 * in a new epoch. A stock cannot take an input. */
QwStatus qw_input(QWorld *w, const char *path, QInterval v);
QwStatus qw_input_set(QWorld *w, const char *path, uint64_t mask);

int64_t   qw_now(const QWorld *w);
uint64_t  qw_boundaries(const QWorld *w);
QInterval qw_get(const QWorld *w, const char *path);     /* through ports; the top interval if unknown */
uint64_t  qw_get_set(const QWorld *w, const char *path); /* through ports; 0 if not a set cell */
uint64_t  qw_predicts(const QWorld *w, QwPhen p);        /* predict calls so far */
uint64_t  qw_advances(const QWorld *w);                  /* advance calls so far */
uint64_t  qw_switches(const QWorld *w);
int64_t   qw_total(const QWorld *w, QwKind kind);        /* the sum of every cell of a kind */

/* A hash of all authoritative state: the clock, the epoch, each active
 * model, and every cell's base with its path. It does not depend on the
 * order in which phenomena, models and cells were declared (ADR 0013). Two runs with
 * the same inputs must give the same hash (ADR 0004). */
uint64_t qw_hash(const QWorld *w);

/* --- snapshot and restore (ADR 0004 rule 8) ---------------------------------------- */

/* A snapshot is the state of a world at a boundary, as bytes: the clock, the
 * epoch, the base of each cell, the active model of each phenomenon, the
 * instances that are alive, and for each kernel its prediction, its
 * assumptions, its event count and its private state. A prediction decides
 * the next boundary, so it is state.
 *
 * qw_snapshot gives the bytes, and the caller frees them. A kernel that is
 * not stateless must give `save`, or the snapshot fails.
 *
 * qw_restore puts a snapshot back. The world must have the structure of the
 * world of the snapshot: the same phenomena, cells, models and kernels,
 * built in the same order. It can be the same world later in its run, or a
 * new world from the same build code or the same IR. After a restore, a run
 * gives the same state hash at each time as the run of the first world. The
 * change log is empty after a restore. If a restore fails, the world can be
 * in a state between the two: restore it again, or free it. */
QwStatus qw_snapshot(QWorld *w, void **bytes, size_t *len);
QwStatus qw_restore(QWorld *w, const void *bytes, size_t len);

/* --- the change log (ADR 0011 section 6) ------------------------------------------ */

/* The world commits authoritative changes at a boundary of qw_run, at a
 * successful qw_switch, at qw_input, and at the start or the end of an
 * instance. When logging is on, each commit
 * records one entry for each cell whose base changed.
 *
 * `epoch` is the epoch in which the change takes effect: the epoch number
 * after the commit. `before` and `after` are copies of the cell's base. The
 * log does not interpret them, because a cell can have another domain in the
 * same storage. For a stock, `amount` is the net transfer that the commit
 * applied, so after = before + amount. For a replaced value, `amount` is 0.
 *
 * Entries are in order of epoch, then of cell index. A failed step or a
 * failed switch records nothing. A commit records its entries even if
 * propagation in the new epoch then finds a contradiction, because the bases
 * changed. A write of the value that the cell already has records nothing. */
typedef enum { QW_CHANGE_VALUE, QW_CHANGE_STOCK } QwChangeKind;

typedef struct {
    uint64_t     epoch;
    QnCell       cell;
    QwChangeKind kind;
    QInterval    before, after;
    int64_t      amount;
} QwChange;

/* Turns logging on or off. Logging is off by default. Turning it off keeps
 * the entries that the log already has. */
void qw_log_changes(QWorld *w, bool on);
/* The entries of the log, and their number in *n. The pointer is valid until
 * the next call that commits changes or discards entries. */
const QwChange *qw_changes(const QWorld *w, size_t *n);
/* Drops every entry whose epoch is before `before_epoch`. */
void qw_changes_discard(QWorld *w, uint64_t before_epoch);

#endif
