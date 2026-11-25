---
status: accepted
---

# A phenomenon has several models, one model is active, and a map kernel moves the state when the model changes

Variable level of detail is the reason phenomena are the fundamental abstraction (design section 7). A phenomenon needs several models at different levels of detail, behind a stable interface (design section 7.5). The change from one model to another must conserve every stock exactly and must be reproducible (design section 14.4). We decided that each model of a phenomenon has its own cells, propagators, and kernel. Exactly one model is active. Ports resolve to the active model's cells. A switch runs a map kernel (ADR 0009 section 5) at a boundary. In the first version, an explicit call starts a switch. Automatic model selection comes later and uses the same switch.

## Decision

### 1. Models

- A phenomenon declares its models by name, for example `agg` and `fine`. The first model that a phenomenon declares is active at the start.
- A model's cells have paths under the model: `roomA.fine.c0.energy`. Cells directly under the phenomenon, such as `roomA.capacity`, belong to every model.
- A model has an optional kernel. The kernel of an inactive model does not run.
- A model's propagators form a group in the network. The network does not run the propagators of a disabled group, and a disabled group costs nothing during propagation. The network enables and disables groups only between epochs, because enabling a group is not monotonic (ADR 0008).
- Every stock of an inactive model is exactly zero. So the total of each conserved quantity is always in the active models' stocks.

### 2. Ports that do not depend on the model

- A phenomenon declares a port once and maps it for each model. For example, the port `west` of `roomA` maps to `roomA.agg` in the aggregate model and to `roomA.fine.c0` in the detailed model.
- Another phenomenon connects to the port, not to a model: the wall connects `hot` to `roomA.west`. Its slot `hot.temperature` then resolves to `roomA.agg.temperature` or to `roomA.fine.c0.temperature`, depending on which model of `roomA` is active.
- After a switch, the host binds every kernel's slots again and invalidates every prediction that read through the changed port. This is the stable interface of design section 7.5: the wall does not know which model is behind the port.

### 3. The switch

A switch from model A to model B happens at the current time, at a boundary:

1. The host binds the map kernel. Its slots `from.x` resolve to model A's cells and its slots `to.x` resolve to model B's cells. Its other slots resolve under the phenomenon.
2. The host calls `map`. The map kernel reads model A, writes model B's `STATE` and `OUT` slots, and transfers model A's stocks into model B's stocks.
3. The host checks that every stock of model A is exactly zero. If one is not, the host discards the map's writes and transfers, and the switch fails.
4. The host commits the writes and transfers, makes B active, enables B's group and disables A's group, binds the kernels again, and opens a new epoch.

The map's random cursor uses the phenomenon's identity, the stream name, and the epoch number as the event index (ADR 0002). The same state and seed therefore give the same detail.

### 4. What starts a switch

- First version: an explicit call, `qw_switch(world, phenomenon, model, map)`. A test or a game script decides when to switch.
- ADR 0021 adds a condition for a transition. The world switches at a boundary while the condition is true, and the application can replace the condition. ADR 0022 has the details.
- Later: model selection by observability, causal relevance, uncertainty, and budget (design section 7.1). It is a non-monotonic step at an epoch boundary (ADR 0008), and it uses the same switch.
- Later: a kernel that returns `QK_INAPPLICABLE`, or an input outside a valid range, can request a switch instead of stopping the run.

## Considered options

- **One set of cells for all models, with a different interpretation for each model.** Models do not have the same state: an aggregate room has one energy stock, and a detailed room has N. We rejected this option.
- **Destroy and create cells at each switch.** This option would change cell indices during the run, and it would make old epochs hard to keep. Dormant cells cost only memory, so we keep them. We rejected destruction.
- **Automatic selection from the start.** The success criteria of design section 14.4 (bounded error, exact conservation across switches, reproducible reconstruction, lower cost) can be measured with explicit switches. Automatic selection is a separate research question. We deferred it.

## Consequences

- Each model of a phenomenon pays memory for its dormant cells, but no computation.
- A map kernel is specific to a pair of models. A world with K models for a phenomenon may need up to K × (K − 1) map kernels. In practice, models form a chain of levels, and maps go between neighbors.
- Refinement can invent detail, for example a temperature gradient, but only within the rules of ADR 0009: stocks move exactly, and the result is reproducible from the seed. The quality of the invented detail is open (`quadrilateral-review-fable.md` section 3.2).
