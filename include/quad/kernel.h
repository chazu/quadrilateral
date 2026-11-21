#ifndef QUAD_KERNEL_H
#define QUAD_KERNEL_H

/* The kernel interface (docs/adr/0009).
 *
 * A kernel gives a static contract and a table of functions (QkKernel). The
 * host gives a table of services (QkHost). Neither side sees the other's data
 * structures: QkInstance belongs to the kernel and QkCall to the host.
 * QInterval and QRCursor are the only Quadrilateral types in the interface,
 * and ADR 0001 and ADR 0002 fix their layouts.
 *
 * A kernel refers to a slot by its index in its own `slots` array and to a
 * transfer by its index in `transfers`. Values cross the interface in the
 * slot's declared scale. Time crosses it in ticks of the world's clock.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quad/quantity.h"
#include "quad/random.h"

#define QK_ABI_VERSION 1
#define QK_NONE        UINT32_MAX
#define QK_NO_TIME     INT64_MAX

typedef struct QkInstance QkInstance;
typedef struct QkCall     QkCall;

typedef enum {
    QK_IN,      /* read */
    QK_OUT,     /* read and write; published through a port */
    QK_STATE,   /* read and write; not conserved */
    QK_STOCK,   /* read; changed only by transfers */
    QK_LINK,    /* another phenomenon's stock; a transfer endpoint only */
} QkRole;

typedef struct {
    const char *name;        /* feature path relative to the phenomenon */
    QkRole      role;
    const char *dimension;   /* q_parse_dim grammar */
    int8_t      scale;
    QInterval   valid;       /* in the slot's scale; {Q_NINF, Q_INF} for no limit */
} QkSlot;

typedef struct {
    const char *name;
    uint32_t    from, to;    /* slot indices or QK_NONE */
    bool        conserved;
} QkTransfer;

enum {
    QK_EVOLVE    = 1,
    QK_MAP       = 2,
    QK_NARROW    = 4,
    QK_RATES     = 8,    /* reserved: model exchange */
    QK_STATELESS = 16,
    QK_PORTABLE  = 32,
};

typedef enum { QK_OK = 0, QK_INAPPLICABLE, QK_ERROR } QkResult;

/* Absolute times in ticks. QK_NO_TIME means no limit or no event. */
typedef struct { int64_t horizon, event_lo, event_hi; } QkPrediction;

typedef struct QkHost {
    uint32_t  abi_version, size;
    QInterval (*read)(QkCall *call, uint32_t slot);
    int       (*write)(QkCall *call, uint32_t slot, int64_t value);        /* OUT, STATE; advance, map */
    int       (*transfer)(QkCall *call, uint32_t transfer, int64_t amount); /* advance, map */
    int       (*tell)(QkCall *call, uint32_t slot, QInterval range);      /* narrow */
    int       (*assume)(QkCall *call, uint32_t slot, QInterval range);    /* predict */
    QRCursor  (*random)(QkCall *call, const char *stream);
    int64_t   (*now)(QkCall *call);
    int8_t    (*tick_scale)(QkCall *call);
    void      (*log)(QkCall *call, const char *message);
} QkHost;

typedef struct QkKernel {
    uint32_t          abi_version, size, qr_version;
    const char       *name;
    uint32_t          flags;
    const QkSlot     *slots;
    uint32_t          nslots;
    const QkTransfer *transfers;
    uint32_t          ntransfers;

    QkResult (*create)(const QkHost *host, QkCall *call, QkInstance **out);
    void     (*destroy)(QkInstance *inst);
    QkResult (*predict)(QkInstance *inst, QkCall *call, QkPrediction *out);
    QkResult (*advance)(QkInstance *inst, QkCall *call, int64_t to);
    QkResult (*map)(QkInstance *inst, QkCall *call);
    void     (*narrow)(QkInstance *inst, QkCall *call);
    size_t   (*save)(QkInstance *inst, void *buf, size_t len);
    QkResult (*restore)(QkInstance *inst, const void *buf, size_t len);
} QkKernel;

/* Status codes of the host functions that return int */
enum { QK_HOST_OK = 0, QK_HOST_EMODE, QK_HOST_EROLE, QK_HOST_EINDEX, QK_HOST_EVALUE };

#endif
