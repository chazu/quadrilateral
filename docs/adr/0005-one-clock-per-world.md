---
status: accepted
---

# Each world has one int64 clock, and event times are intervals

Phenomena work at very different time scales: a fire at milliseconds, a negotiation over days. Synchronization needs one shared measure of time, and determinism needs time in integers. We decided that each world has one simulation clock: an int64 count of ticks, with a tick of 10^-6 s by default (decision D7).

## Decision

- The tick is a setting of the world. It must be a power of ten of seconds, so that time is a quantity type like any other (ADR 0001). A geological or astronomical world can choose a longer tick.
- At 1 µs, an int64 clock covers about 292,000 years. An advance past the end of the clock is an error.
- There is no separate tick for each phenomenon. A phenomenon that needs steps shorter than one tick takes them inside its kernel and reports the result at tick boundaries.
- An event time is an interval of ticks, [earliest, latest]. The first scheduler uses only the earliest time. The interval exists from the start so that temporal propagation (design section 10.2) needs no change to the representation.
- The epoch number (ADR 0008) is the evaluation time of design section 10.3. Valid time and event time are clock values.

## Considered options

- **A clock for each phenomenon at its own resolution.** Each synchronization would need a conversion, and a comparison between two phenomena would need a common unit anyway. We rejected it.
- **Rational or floating-point time.** Both make comparison and reproducibility harder, and floating point gives order-dependent rounding. We rejected them.
