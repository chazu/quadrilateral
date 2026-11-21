---
status: accepted
---

# A run is deterministic

Seeded refinement, replay, debugging by evaluation time, and comparison of two runs all need the same inputs to give the same results. Bruggadung stopped treating determinism as a goal (`CHANGELOG.md:56` there), and its state hashes cover less than they seem to (its `REVIEW.md` section 4.5). We decided that determinism is a firm requirement (decision D4), and that randomness is allowed only as an explicit, seeded part of the simulation (ADR 0002).

## Decision

1. The same inputs, seed, `QR_VERSION`, and build give the same authoritative state, bit for bit. Authoritative state is fixed point (ADR 0001), so the result is also the same across platforms. Kernel-private floating-point state is exempt, as ADR 0001 section 7 states.
2. Random draws come only from the generator of ADR 0002.
3. Simulation logic never reads wall-clock time, memory addresses, thread timing, or any other value outside the run's inputs.
4. No iteration order depends on a hash table's bucket order or on memory addresses. Iteration follows identifiers or another defined order.
5. Events at the same time run in the order (time, phenomenon identity, event index).
6. The propagation agenda has a defined order.
7. When parallel execution comes, it must give the same results as sequential execution. Reductions run in a fixed order.
8. A replay test hashes all authoritative state at each boundary, and two runs must give the same hashes. All state means all of it: Bruggadung's hashes omitted the knowledge and motivation stores.

## Consequences

- Phenomenon identities must be stable for the same inputs. The runtime assigns them in a defined order, not from allocation addresses.
- A budget that stops propagation early gives a result that depends on the agenda order. Rule 6 makes that result reproducible (ADR 0008).
