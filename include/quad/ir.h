#ifndef QUAD_IR_H
#define QUAD_IR_H

/* The IR: what the front end makes from Quadrilateral source (docs/adr/0011
 * section 3, docs/adr/0016).
 *
 * The IR is plain data. It describes a world before the world exists: the
 * kinds, the phenomena, the cells with their owners, the ports and the
 * connections between phenomena, the propagators that the laws become, the
 * kernels with the cell of each slot, and the dependency graph. qi_text gives
 * its text form, for a person or a test. qi_instantiate builds a world from
 * it through the C API of quad/world.h. Another backend would read the same
 * data.
 *
 * Everything in the IR has a defined order, which does not depend on the
 * order of the source files or of the declarations in them. An index in a
 * field is an index into the array of QIr that the comment names.
 *
 * Steps inside an epoch and steps at a boundary are separate from the start
 * (docs/adr/0008). A propagator is monotonic: it only narrows cells, inside an
 * epoch. A kernel is not: it advances time, and its writes and transfers take
 * effect at a boundary.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quad/kernel.h"
#include "quad/world.h"

#define QI_NONE UINT32_MAX

/* A quantity kind that a cell uses. An anonymous kind has a dimension and a
 * scale, and no name in source (ADR 0001, ADR 0018): a field with a dimension
 * and no kind has one, and so does the value of an inner expression of a law. */
typedef struct {
    const char *name;        /* as in source: %energy, shop.%coin. Anonymous: dim[K]@-6 */
    const char *dimension;   /* in the grammar of ADR 0009 section 10 */
    int8_t      scale;
    bool        conserved;
    uint8_t     rounding;    /* QRound */
    bool        anonymous;
} QiKind;

/* An enumeration that a set cell uses (ADR 0012): the atoms that the field
 * can have by its declaration, in the order of their names. */
typedef struct {
    const char  *name;       /* the type of the field: #Material, bool, "closed" | "open" */
    const char **atoms;
    uint32_t     natoms;
} QiEnum;

/* A phenomenon. With models (ADR 0021): the names of its models, in the
 * order of the names, and the model that is active at the start. A cell of a
 * model has a path below the model: roomB.fine.c0.energy. */
typedef struct {
    const char  *name;       /* the name of the instance: the first component of its cells' paths */
    const char  *type;       /* its type in source */
    const char **models;
    uint32_t     nmodels;    /* 0: the phenomenon has no models */
    uint32_t     start;      /* index into models, or QI_NONE */
} QiPhen;

typedef enum { QI_INTERVAL, QI_SET } QiDomain;

typedef struct {
    const char *path;        /* roomA.energy */
    uint32_t    phen;        /* its owner: index into phens */
    uint32_t    model;       /* the model that has the cell: index into the models of its owner, or QI_NONE */
    QiDomain    domain;
    uint32_t    kind;        /* index into kinds; for a set cell, into enums */
    QInterval   base;        /* an interval cell: quanta of its kind */
    uint64_t    mask;        /* a set cell: the possible atoms, atom i is bit i */
    bool        stock;       /* a cell of a conserved kind: only transfers change it. Its base is a point: the
                                author gives it, or the laws give it at the start (ADR 0019) */
    bool        derived;     /* the laws give its value in each epoch: not state. A derived field of a type
                                (ADR 0019), or an inner value of a law */
    bool        inner;       /* derived, with no field in source: an inner value or a constant of a law (ADR 0018) */
} QiCell;

/* A second path for a cell of the same phenomenon. A port that is a view of
 * the phenomenon's state gives these: roomA.west.energy is roomA.energy. */
typedef struct {
    const char *path;
    uint32_t    cell;        /* index into cells */
} QiAlias;

typedef enum {
    QI_PORT_VIEW,            /* each field is a cell of the phenomenon, by an alias */
    QI_PORT_CONNECTED,       /* the port is another phenomenon's port */
    QI_PORT_OPEN,            /* a field has no cell: nothing is connected */
    QI_PORT_MODEL,           /* the port of the active model: each model gives the port as a view (ADR 0021) */
} QiPortState;

typedef struct {
    uint32_t     phen;        /* index into phens */
    const char  *name;        /* the path of the port in the phenomenon: west */
    const char  *type;
    QiPortState  state;
    const char  *target;      /* connected: the path of the other port, roomA.west */
    uint32_t     target_phen; /* connected: index into phens */
    const char **targets;     /* by model: for each model of the phenomenon, the path of its port, roomB.fine.west */
} QiPort;

typedef enum { QI_EQUAL, QI_SUM, QI_DIFFERENCE, QI_PRODUCT, QI_QUOTIENT, QI_LESS_EQUAL, QI_LESS } QiOp;

/* A propagator: cell[0] = cell[1] op cell[2]. For QI_EQUAL, cell[0] =
 * cell[1]. For QI_LESS_EQUAL and QI_LESS, cell[0] <= cell[1] or cell[0] <
 * cell[1]. It narrows every cell that it names, in every direction. One law
 * can give several propagators, with derived cells between them. */
/* A law of a model works only while the model is active (ADR 0021): `mphen`
 * and `model` name that model. A law that reads through a port of the active
 * model gives one propagator for each model of that phenomenon. */
typedef struct {
    uint32_t    phen;        /* the phenomenon that states the law */
    uint32_t    mphen;       /* the phenomenon whose model the propagator belongs to: index into phens, or QI_NONE */
    uint32_t    model;       /* index into the models of mphen, or QI_NONE */
    QiOp        op;
    uint32_t    cell[3];     /* indices into cells; cell[2] is QI_NONE where the propagator has two cells */
    const char *name;        /* the path of the law in source: roomA.storage */
    const char *law;         /* the text of the law */
    const char *file;
    int         line, col;
} QiProp;

/* The cell of a slot. A slot can go through a port of the active model of a
 * phenomenon (ADR 0021): the world then binds the slot again at each switch.
 * `cell` is the cell while the model of the start is active, and `mcells`
 * has the cell for each model. */
typedef struct {
    const char *slot;        /* the name of the slot in the kernel's contract */
    QkRole      role;
    uint32_t    cell;        /* index into cells */
    uint32_t    mphen;       /* the phenomenon whose model decides the cell: index into phens, or QI_NONE */
    uint32_t   *mcells;      /* for each model of mphen: index into cells. NULL if mphen is QI_NONE */
} QiBind;

/* A transfer of a kernel. An interaction with a rate in source (ADR 0020)
 * gives a transfer of the kernel native.rate: `rate` and `tolerance` are then
 * the cells that give its rate and the accuracy of its steps. A flow of a
 * reaction has the rate and the tolerance of its reaction, and its own
 * ratio: the flow moves the amount of the reaction times the ratio.
 *
 * A kernel use has each transfer of the contract of its kernel, also one that
 * source does not state. An interaction with no rate states one transfer of a
 * kernel in C (ADR 0021 section 6): `stated` is then the path of that
 * interaction. */
typedef struct {
    const char *name;
    uint32_t    from, to;    /* indices into cells, or QI_NONE */
    bool        conserved;
    uint32_t    rate, tolerance;   /* indices into cells; QI_NONE for a transfer of a kernel in C */
    const char *stated;      /* the interaction in source that states the transfer, wall.flow, or NULL */
    uint32_t    ratio;       /* a flow of a reaction (ADR 0021 section 7): the cell of its ratio. Else QI_NONE */
} QiTransfer;

/* A kernel of a phenomenon, or of one model of it, with the cell that each
 * slot binds to. The kernel of a model runs only while the model is active. */
typedef struct {
    uint32_t    phen;
    uint32_t    model;       /* index into the models of phen, or QI_NONE */
    const char *kernel;      /* the name of the evaluator: native.conduction */
    uint32_t    size;        /* a kernel of a room with n cells (quad/kernels.h) that the front end generates: n. Else 0 */
    QiBind     *binds;
    uint32_t    nbinds;
    QiTransfer *transfers;
    uint32_t    ntransfers;
} QiKernelUse;

/* The condition of a map (ADR 0022): the value of one cell of the phenomenon
 * is in a set. For an interval cell the set is `range`, in quanta of the
 * cell. For a set cell it is the atoms of `mask`. */
typedef struct {
    const char *path;        /* the cell, as a path in the phenomenon: visible, west.temperature. NULL: no condition */
    uint32_t    cell;        /* index into cells; for a path through the port of the active model, the cell at the start */
    QInterval   range;
    uint64_t    mask;        /* 0 for an interval cell */
    const char *text;        /* the condition in source: .visible == false */
} QiCondition;

/* A map of a phenomenon (ADR 0021 section 5): the way from one model to
 * another. The kernel is a map kernel (ADR 0009 section 5). Its slot "from.x"
 * binds to a cell of the model `from`, "to.x" to a cell of the model `to`,
 * and another slot to a cell of the phenomenon. The map is a transition of
 * the world, with the name of its field. */
typedef struct {
    uint32_t    phen;
    const char *name;        /* the name of the field: refine */
    uint32_t    from, to;    /* indices into the models of phen */
    const char *kernel;
    uint32_t    size;        /* as in QiKernelUse */
    QiBind     *binds;
    uint32_t    nbinds;
    QiCondition when;        /* the world switches with the map while this is true (ADR 0022) */
} QiMap;

/* One edge of the dependency graph: a propagator or a kernel uses a cell. */
typedef enum {
    QI_NARROWS,              /* a propagator reads the cell and can narrow it */
    QI_READS,                /* a kernel reads the cell */
    QI_WRITES,               /* a kernel writes the cell at a boundary */
    QI_TRANSFERS,            /* a kernel moves a stock out of or into the cell */
} QiUse;

typedef struct {
    uint32_t cell;           /* index into cells */
    bool     kernel;         /* `node` is an index into kernels; otherwise into props */
    uint32_t node;
    QiUse    use;
} QiDep;

typedef struct QIr {
    int8_t        tick_scale;    /* the scale of the world's tick: the scale of %time */
    const char  **bases;         /* the declared base dimensions: base Q_NSI + i */
    uint32_t      nbases;
    QiKind       *kinds;
    uint32_t      nkinds;
    QiEnum       *enums;
    uint32_t      nenums;
    QiPhen       *phens;
    uint32_t      nphens;
    QiCell       *cells;
    uint32_t      ncells;
    QiAlias      *aliases;
    uint32_t      naliases;
    QiPort       *ports;
    uint32_t      nports;
    QiProp       *props;
    uint32_t      nprops;
    QiKernelUse  *kernels;
    uint32_t      nkernels;
    QiMap        *maps;          /* in the order of phenomenon, then name */
    uint32_t      nmaps;
    QiDep        *deps;          /* in the order of cell, then propagators, then kernels */
    uint32_t      ndeps;
    void         *mem;           /* owns the strings and the arrays */
} QIr;

/* The kernels that source can name with `evaluator`. The native kernels of
 * quad/kernels.h are always known. A kernel of a room with n cells has a
 * contract for each n: the front end counts the cells c0, c1, ... of the
 * model and generates the kernel, and the world frees it. A kernel that the
 * options give has precedence over a native kernel with the same name. */
typedef struct {
    const QkKernel *const *kernels;
    size_t                 nkernels;
} QiOptions;

/* Compiles a content to an IR. names[i] is the file name of sources[i], for
 * diagnostics. `opt` can be NULL. The result is NULL if the content has an
 * error. `diagnostics`, if not NULL, gets the diagnostics as text, one on
 * each line, sorted by location; the caller frees it. Each instance of a
 * type below #Phenomenon becomes a phenomenon. */
QIr  *qi_compile(const char *const *names, const char *const *sources, size_t n, const QiOptions *opt,
                 char **diagnostics);
void  qi_free(QIr *ir);

/* The text form. The caller frees it. Two contents with the same meaning give
 * the same text. */
char *qi_text(const QIr *ir);

/* For a person: a range in quanta of `scale` as text, and a common unit for
 * a dimension of the IR. The text: 303.15, ~303.15 (between two steps),
 * >= 0, <= 5, 2.5 .. 3, unknown. The unit: J for "m^2 kg s^-2", or the
 * dimension itself if it has no common unit. */
void        qi_format_interval(QInterval v, int scale, char *out, size_t len);
const char *qi_unit(const char *dimension);

/* Builds a world from the IR. `opt` gives the same kernels as at qi_compile.
 * On an error, *out is NULL and `error`, if not NULL, gets a message that the
 * caller frees. */
QwStatus qi_instantiate(const QIr *ir, uint64_t seed, const QiOptions *opt, QWorld **out, char **error);

/* Switches the phenomenon `phen` of a world from the IR to `model`, at the
 * current time (ADR 0021 section 5). It finds the map from the active model
 * to `model` and switches with the kernel of that map. If the phenomenon has
 * no such map, the result is QW_ENAME, and qw_error gives a message. */
QwStatus qi_switch(const QIr *ir, QWorld *w, const char *phen, const char *model);

#endif
