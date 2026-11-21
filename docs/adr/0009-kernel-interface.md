---
status: accepted
---

# Kernels use a small C interface of two function tables, and conserved quantities change only through transfers

A kernel is external code that implements part of the behavior of a phenomenon. The design calls the kernel interface the most important architectural decision (design section 12.2), because every kernel depends on it and other hosts (BEAM, Go, Quadrilateral itself) must be able to implement it. We decided on an interface in the style of FMI 3.0: the kernel gives a table of functions and a static contract, and the host gives a table of services. Neither side sees the other's data structures. We also decided that conserved quantities are stocks that change only through transfers, so that conservation is exact by construction.

## Decision

### 1. Three kinds of kernel

One kernel can be one or more of these kinds. Flags in the contract name the kinds.

| Kind | Entry points | Purpose |
|---|---|---|
| Evolution | `predict`, `advance` | Advances a phenomenon through time (design section 10.5) |
| Map | `map` | Moves state from one model to another: abstraction and refinement (design section 7.3) |
| Narrowing | `narrow` | A propagator over the kernel's slots, run inside propagation (design section 8.3) |

### 2. Slots

A kernel declares its slots in its contract. A slot has a name, a role, a dimension in text form, a scale, and an optional static valid range. The host binds each slot to a cell by name: the name is a feature path relative to the phenomenon, for example `hot.temperature`. The kernel refers to a slot by its position in its own declaration, never by a cell index.

| Role | The kernel can | Example |
|---|---|---|
| `QK_IN` | read | a port's temperature, a parameter such as conductance |
| `QK_OUT` | read, write | the temperature that a room publishes through its port |
| `QK_STATE` | read, write | a state value that is not conserved |
| `QK_STOCK` | read, transfer from or to | the room's internal energy |
| `QK_LINK` | transfer from or to | another phenomenon's stock, reached through a port |

The host converts each `IN`, `OUT`, and `STATE` value between the cell's scale and the slot's declared scale (ADR 0001). A `STOCK` or `LINK` slot must have exactly the cell's quantity type, because a conversion could lose a conserved quantum. The compiler rejects a mismatch.

A slot's static valid range states where the kernel is valid (design section 12.2). If an input is outside its valid range, the host does not call the kernel. That model is then not applicable, and model selection must choose another one.

### 3. Transfers and conservation

- A kernel declares each transfer in its contract: a name, a `from` slot, and a `to` slot. Each endpoint is a `STOCK` slot, a `LINK` slot, or `QK_NONE`.
- At run time, the kernel proposes an amount for a transfer. At commitment, the host subtracts the amount from `from` and adds it to `to`, atomically, in exact integers.
- A kernel cannot write a stock. A stock changes only through transfers.
- An endpoint of `QK_NONE` creates or destroys a quantity. The contract must declare such a transfer as not conserved.

The result is that conservation is exact by construction. The host needs no conservation ledger and no check after the kernel runs. This answers open question 9 of the design: neither runtime checks nor trust are necessary for stocks. The only claim the host must trust is the amount of each transfer, and that amount does not affect conservation.

The ownership rule of design section 9.1 also follows: a phenomenon proposes transfers, and only the host changes another phenomenon's stock.

### 4. Evolution

- `predict(now)` reports the safe horizon (the latest time to which `advance` is safe) and the next event as an interval of ticks (ADR 0005).
- Each read during `predict` is an assumption that the input stays inside the interval that the kernel read. The kernel can declare a wider tolerance with `assume(slot, range)`, for example "oxygen within 5%". At each boundary, the host compares the current inputs with the assumptions. If an input is outside its assumption, the prediction is invalid, and the host calls `predict` again. These assumptions are the invalidation dependencies and the validity contract of ADR 0006.
- `advance(to)` advances from the kernel's current time to `to`, which is never later than the horizon. The kernel reads its inputs as committed at the start of the step, writes `OUT` and `STATE` slots, and proposes transfers. Writes and transfers are staged, and the host commits them at the boundary. A kernel never sees another kernel's writes from the same step, so the order in which the host calls kernels does not matter.

### 5. Map: abstraction and refinement

A map kernel moves state from a source model to a target model. It reads the source's slots, writes the target's `STATE` and `OUT` slots, and transfers from the source's stocks to the target's stocks. After `map`, every source stock must be exactly zero, or the host rejects the map. So a model switch conserves every stock exactly, by construction. A map kernel gets a random cursor whose phenomenon is the abstract state's identity and whose event is the epoch (ADR 0002), so refinement is reproducible.

### 6. Narrowing

`narrow` runs inside propagation. It reads slots and narrows them with `tell`. During `narrow`, the host refuses `write`, `transfer`, and `assume`, because those are not monotonic (ADR 0008).

### 7. Private state and determinism

- A kernel should keep its authoritative state in slots. A kernel with the flag `QK_STATELESS` keeps nothing else, so the host can destroy and create it again at any time.
- A kernel with private state can give `save` and `restore` functions. Without them, the host cannot restore that kernel to an earlier epoch.
- A kernel with the flag `QK_PORTABLE` uses integer arithmetic only, and its results are the same on every platform. Without the flag, its results are reproducible only for the same build on the same platform (ADR 0001 section 7).
- Random draws come only from `host->random`, which gives a cursor for (run seed, phenomenon, stream name, event index) as ADR 0002 defines.

### 8. Versions

Each table starts with an interface version and its own size in bytes. A host accepts a table with the same major version and a size at least as large as the fields it uses. Later versions can add fields at the end. The contract also states the `QR_VERSION` that the kernel expects.

### 9. Explicit coupling now, implicit coupling later

`advance` reads its inputs as committed at the start of the step. The coupling between two kernels is therefore explicit, and it has first-order error in the step length. Tightly coupled physics needs one kernel or short horizons. This is the main weakness of the decision.

To keep implicit coupling possible, the interface follows FMI's two modes:

- **Co-simulation (now).** Each kernel advances itself with `advance`.
- **Model exchange (later).** A kernel with the reserved flag `QK_RATES` reports the rates of change of its slots and does not advance itself. The host groups tightly coupled rate kernels and solves each group as one system, for example with SUNDIALS.

So that model exchange can be added without a redesign, the scheduler never assumes one kernel for each phenomenon, and it advances groups of kernels, where a group of one is the common case.

### 10. One text form for dimensions

The text form of a dimension in a kernel contract and the unit symbols in Quadrilateral source use one grammar and one parser. For example, `kg m^2 s^-2` means the same in both places.

## C sketch

This sketch shows the intended shape. It is not a final interface.

```c
#define QK_ABI_VERSION 1
#define QK_NONE        UINT32_MAX

typedef struct QkInstance QkInstance;   /* the kernel's; opaque to the host */
typedef struct QkCall     QkCall;       /* the host's context for one call; opaque to the kernel */

typedef enum { QK_IN, QK_OUT, QK_STATE, QK_STOCK, QK_LINK } QkRole;

typedef struct {
    const char *name;       /* feature path relative to the phenomenon */
    QkRole      role;
    const char *dimension;  /* text, for example "kg m^2 s^-2" */
    int8_t      scale;
    QInterval   valid;      /* in the slot's scale; qi_top() for no limit */
} QkSlot;

typedef struct {
    const char *name;
    uint32_t    from, to;   /* slot index or QK_NONE */
    bool        conserved;
} QkTransfer;

enum {
    QK_EVOLVE = 1, QK_MAP = 2, QK_NARROW = 4,     /* kinds */
    QK_RATES = 8,                                  /* reserved: model exchange (section 9) */
    QK_STATELESS = 16, QK_PORTABLE = 32,
};

typedef enum { QK_OK = 0, QK_INAPPLICABLE, QK_ERROR } QkResult;

typedef struct { int64_t horizon, event_lo, event_hi; } QkPrediction;   /* ticks; INT64_MAX if none */

typedef struct QkHost {
    uint32_t  abi_version, size;
    QInterval (*read)(QkCall *call, uint32_t slot);
    int       (*write)(QkCall *call, uint32_t slot, int64_t value);        /* OUT, STATE */
    int       (*transfer)(QkCall *call, uint32_t transfer, int64_t amount);
    int       (*tell)(QkCall *call, uint32_t slot, QInterval range);      /* narrow only */
    void      (*assume)(QkCall *call, uint32_t slot, QInterval range);    /* predict only */
    QRCursor  (*random)(QkCall *call, const char *stream);
    int64_t   (*now)(QkCall *call);
    int8_t    (*tick_scale)(QkCall *call);   /* the world's tick is 10^tick_scale s */
    void      (*log)(QkCall *call, const char *message);
} QkHost;

typedef struct QkKernel {
    uint32_t          abi_version, size, qr_version;
    const char       *name;          /* for example "native.conduction" */
    uint32_t          flags;
    const QkSlot     *slots;
    uint32_t          nslots;
    const QkTransfer *transfers;
    uint32_t          ntransfers;

    QkResult (*create)(const struct QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out);
    void     (*destroy)(QkInstance *inst);
    QkResult (*predict)(QkInstance *inst, QkCall *call, QkPrediction *out);
    QkResult (*advance)(QkInstance *inst, QkCall *call, int64_t to);
    QkResult (*map)(QkInstance *inst, QkCall *call);
    void     (*narrow)(QkInstance *inst, QkCall *call);
    size_t   (*save)(QkInstance *inst, void *buf, size_t len);              /* optional */
    QkResult (*restore)(QkInstance *inst, const void *buf, size_t len);     /* optional */
} QkKernel;
```

`QInterval` and `QRCursor` are public value types whose layouts ADR 0001 and ADR 0002 fix. They are the only Quadrilateral types in the interface.

## Example: the conductive wall

The wall of design section 9.2 has five slots and one transfer:

| Slot | Role |
|---|---|
| `hot.temperature`, `cold.temperature` | `IN` |
| `conductance` | `IN` |
| `hot.energy`, `cold.energy` | `LINK` (the rooms' energy stocks) |

The transfer `heat` goes from `hot.energy` to `cold.energy` and is conserved. `predict` reads the two temperatures and returns a horizon from its error tolerance. `advance(to)` proposes the amount `conductance × (T_h − T_c) × (to − now)`. Each room is a phenomenon that owns its energy stock and publishes its temperature as an `OUT` slot.

## Considered options

- **Shared-variable coupling, as in Bruggadung's additive contributions.** Design section 9.1 rejects this option.
- **A conservation ledger that checks each kernel after it runs.** This option would allow kernels to write stocks directly, and the host would compare the totals afterwards. Structural conservation is simpler and cannot fail, and the cost is that every change of a stock must be expressed as a transfer. We rejected the ledger.
- **Kernels that link against the runtime's symbols.** This option is simpler for C, but a BEAM or Go host could not give the same functions. Function tables work for any host. We rejected linking.
- **FMI 3.0 itself.** FMI is larger and has no typed ports or conservation. Design section 15 recommends a smaller, more strongly typed interface first and FMU interoperability later. An FMU can later be wrapped as a kernel.

## Consequences

- Every conserved quantity in a world must be a stock, and every process that changes it must be a transfer. For example, combustion transfers energy from a chemical-energy stock to a heat stock. It does not create heat.
- The host checks each input against its static valid range and its assumptions at each boundary. That check is cheap, because it is an interval comparison.
- A kernel's writes are staged, so the host can call kernels in any order and, later, in parallel.
- The language needs a construct for a reaction: a set of flows with fixed ratios and one rate. Without it, a process such as combustion would be several transfer declarations that the author keeps consistent by hand. The compiler lowers a reaction to one transfer for each flow. Each transfer is exact, so a ratio that rounds to whole quanta does not break conservation.
- Authors decide once for each quantity kind whether it is conserved. A value of a non-conserved kind, such as hunger, is ordinary state that a law writes directly. Only conserved kinds need flows.
- An open boundary is explicit: a flow to or from `none`. This documents where a model is open, and EXPLAIN can report it.
