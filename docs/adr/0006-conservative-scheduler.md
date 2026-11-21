---
status: accepted
---

# The scheduler is conservative, and its first version advances all phenomena to one boundary

The design wants phenomena that advance asynchronously (design section 11.3). That needs a conservative parallel discrete-event algorithm, with lookahead in the style of Chandy, Misra, and Bryant, and it is research. Bruggadung's scheduler is simpler: all domains advance together to the earliest boundary that any horizon or input sets (`quadrilateral-review-fable.md` section 2.5). We decided that the first scheduler is new code with Bruggadung's simpler shape, that it never speculates or rolls back, and that the kernel interface is designed so that asynchronous advancement can replace it later (decision D6).

## Decision

- The scheduler never commits a future that a later event could invalidate. There is no speculation and no rollback.
- The first version chooses the next boundary as the earliest of these: every active phenomenon's current time plus its safe step, the earliest time of every predicted event, and every pending input.
- At a boundary, the scheduler does these steps in order:
  1. It advances each active phenomenon to the boundary.
  2. It commits all transfers atomically.
  3. It opens a new epoch (ADR 0008).
  4. It runs propagation.
  5. It checks validity contracts and invalidates the predictions whose assumptions no longer hold.
- An evaluator reports three things: its safe horizon, its next event as an interval (ADR 0005), and its invalidation dependencies.
- An evaluator reads other phenomena only through its ports. It never reads global state. This rule keeps asynchronous advancement possible: if a phenomenon's advance is a function of its port inputs over an interval, the scheduler can later advance phenomena to different times.

## Consequences

- With one boundary for all, a phenomenon with a short horizon makes every active phenomenon stop often. Bruggadung accepted this cost. Detail switching (phase 5) and asynchronous advancement are the ways to reduce it.
- Asynchronous advancement needs no change to the evaluator interface. It needs only a different scheduler.
