---
status: accepted
---

# Random numbers come from a counter-based generator, Philox4x64-10

A run must be reproducible from its inputs and its seed, and randomness is allowed only as an explicit part of the simulation (decision D4 in `quadrilateral-review-fable.md`). A generator with shared state would make each draw depend on the order in which the scheduler runs phenomena, so concurrency or a change in scheduling would change the results. We decided that each draw is a pure function of (run seed, stream name, phenomenon, event index, draw index), computed by Philox4x64-10 (Salmon et al., 2011, the Random123 library). The implementation is `include/quad/random.h` and `src/random.c`.

## Decision

- The Philox key is (run seed, FNV-1a hash of the stream name). The counter is (event index, block index, phenomenon identity, 0). Each block gives four 64-bit draws. The fourth counter word is reserved and is 0.
- The event index is the phenomenon's own count of its events. The runtime owns this count, and the scheduling order does not change it.
- Stream names are hashed, not numbered. A new stream therefore does not change the draws of existing streams.
- Integer functions serve authoritative state: a uniform integer below a bound without bias (Lemire's method), a uniform integer in a range, and an exact chance `p × 10^scale` in fixed point. `qr_chance` always uses a draw, so a change in a probability does not shift the later draws of the event.
- `qr_double` exists for kernel-private floating-point state only.
- Refinement uses the same mechanism. Its phenomenon identity is the identity of the abstract state, and its event index is the epoch.
- `QR_VERSION` names the mapping from inputs to draws. A change to the generator or to the counter layout changes every draw, so it increments the version. A saved run records the version.

## Considered options

- **A sequential generator for each phenomenon (xoshiro256, PCG).** Each phenomenon would own generator state that advances with each draw. This option is fast, but the state must be saved and restored with the phenomenon. A refinement that is computed again would need the state as of that time. A counter-based generator needs no state. We rejected the sequential option.
- **A hash of the inputs (for example SplitMix64 of a combined value).** This option is simpler. It has no published statistical validation for structured inputs. Philox passes the BigCrush test suite and has published known-answer vectors. We rejected the hash.
- **Philox4x32-10.** This variant has a 64-bit key, too small to hold the run seed and the stream name separately. We chose the 4x64 variant, which uses the compiler's 128-bit multiply.

## Consequences

- Two stream names with the same 64-bit hash would give correlated streams. The compiler can compare the hashes of the declared stream names and report a collision.
- Philox has a fixed cost of 10 rounds for each block of four draws. That is slower than xoshiro, but it is not likely to matter, because random draws are not the bulk of the simulation work.
- The number of draws that one call to `qr_below` uses can vary, because of rejection. Draws inside one event are therefore reproducible only if the sequence of calls is the same.
