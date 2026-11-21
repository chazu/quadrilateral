---
status: proposed
---

# Propagation is monotonic inside an epoch, and every non-monotonic step happens at an epoch boundary

Propagation only narrows domains, and simulation changes values over time (design section 8.5). The CALM theorem says that a program has a consistent implementation without coordination if and only if it is monotonic. So the monotonic steps can run in any order and in parallel, and the non-monotonic steps need coordination (`quadrilateral-review-fable.md` section 3.1). We decided to separate the two kinds of step by epochs: inside an epoch, values only narrow; every step that removes information or replaces a value happens at an epoch boundary, in a defined order.

## Decision

- An epoch is an immutable version of the authoritative state. The scheduler opens a new epoch at each boundary (ADR 0006). The epoch number is the evaluation time (ADR 0005).
- Inside an epoch, every propagator is monotonic and contracting: it can only narrow a cell, and a narrower input never gives a wider output. Interval functions with outward rounding meet this rule (ADR 0001). Under this rule, propagation to a fixed point gives the same result in every order (Apt, "The essence of constraint propagation", 1999). An empty domain is a contradiction, and it is a result like any other.
- These steps are non-monotonic, and they happen only at an epoch boundary:
  - Commitment of an evolution step: a value is replaced.
  - Invalidation of a prediction.
  - Selection of a model, for example the cheapest valid model.
  - Turning an "unknown" match into "no match", because that decision depends on the absence of information.
  - Retraction of an assumption.
- The scheduler runs the non-monotonic steps sequentially, in a defined order.
- The IR marks each operation as monotonic or non-monotonic. The compiler rejects a non-monotonic operation inside propagation.
- A new epoch starts from the committed state and the schema constraints. Propagation then runs again. It can be incremental: only the propagators whose input cells changed run again.
- A world chooses how many old epochs it keeps: none, the last N, or all. Old epochs support debugging by evaluation time and, later, backward reasoning.

## Consequences

- Interval propagation can converge slowly. For example, `x = y + 1` and `y = x - 2` contradict each other, but on wide domains propagation narrows them by one quantum at a time before it finds the empty domain. Propagation therefore has a budget. A budget that stops before the fixed point gives a sound but wider result. That result depends on the agenda order, so the agenda order is defined (ADR 0004).
- Parallel propagation inside an epoch needs no coordination beyond the meet of each cell's updates, because the meet is commutative and associative.
- Related work: Dedalus and Bloom (deductive rules inside a timestep, inductive rules between timesteps), LVars (threshold reads and freezing), and Radul and Sussman's propagator networks.
