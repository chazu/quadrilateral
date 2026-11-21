# Quadrilateral: design review and plan

**Date:** 2026-11-21
**Status:** Working plan. No implementation exists.
**Inputs:** `quadrilateral-design.md`, and a check of the Bruggadung repository at `f425574` (`~/dev/objc/bruggadung`). In this document, `T/` means `bruggadung/triangle/` and `L/` means `bruggadung/lab/src/`.

This document records three things: what the check of Bruggadung found, the project owner's decisions of 2026-11-21, and a phased plan. The plan builds the fixed parts first and keeps the ambitious parts open.

## 1. Decisions

| # | Topic | Decision | Status |
|---|---|---|---|
| D1 | Relation to Triangle | Quadrilateral is a successor to Triangle. It starts as fresh code and borrows from Triangle where Triangle fits the design. Where Triangle limits Quadrilateral, or would cause work to be undone later, Quadrilateral rewrites or drops that part. | Decided |
| D2 | Implementation language | C, behind a C interface. The kernel interface is explicit and first-class. Triangle has no such interface. | Decided |
| D3 | Upstream | No tracking of upstream Triangle. If code is borrowed, it is copied once and then belongs to Quadrilateral. | Decided |
| D4 | Determinism | A run is reproducible from its inputs and its seed. Randomness is allowed, but only as an explicit part of the simulation, for example a phenomenon whose behavior is stochastic. Section 5.1 gives the mechanism. | Decided |
| D5 | Number representation | Authoritative state and transfers use fixed-point integers. The scale is part of the quantity type, so each domain can choose its range. Kernels can use floating point internally. Sections 5.2 and 5.3 give the reasons and the rules. | Decided |
| D6 | Scheduler | A new scheduler, built from Bruggadung's ideas (horizons, wake queues, conservative commitment). No port of the Odin code. | Decided (follows from D1 and D2) |
| D7 | Time unit | One int64 simulation clock with a tick of 1 µs by default. Section 4 gives the details. | Decided |

## 2. Findings from the code check

### 2.1 Most of the runtime is not in Triangle

Section 2 of the design document lists features that it attributes to Triangle. Several of them are in Bruggadung's Odin lab, not in Triangle:

| Feature | Location |
|---|---|
| Declarative effects | `L/act_declared.odin` (an effect resolves by unification) |
| Agent knowledge views | `L/know_knowledge_store.odin`, `L/act_view.odin` |
| Atomic commitment | `L/commit_committer.odin` (groups are applied, validated, kept or rolled back) |
| Variable detail | `L/domains_progressions.odin` (partly built; `DETAIL_REGIMES.md` says "proposed, built in part") |
| Scheduler | `L/sched_scheduler.odin` (271 lines), `L/sched_coupling.odin` |
| Descriptions built from entity state | `L/act_actions.odin:200-209` |

Triangle supplies only primitives for these: `tri_copy`, `tri_lgg`, `tri_generalize`, `tri_migrate`, and `tri_match`.

### 2.2 What Triangle is

Triangle is about 15,000 lines of C11.

- Unification is Tomabechi quasi-destructive unification (`T/src/fs.c:3`, `unify1` at `fs.c:426`).
- The type hierarchy is completed into a bounded complete partial order with generated `glb` types (`T/src/types.c:671-912`). Meets use bit-vector codes and a cache.
- The quick check (Malouf and Kiefer) is in `T/src/qc.c`.
- Definite clauses use LiLFeS-style depth-first resolution with depth and step limits (`T/src/clause.c`).
- Pattern matching has three values: `TRI_ENTAILED`, `TRI_UNKNOWN`, `TRI_INCOMPATIBLE` (`tri_match`, `T/src/load.c:3033`).
- The syntax is very close to CUE, with TDL-style `$tags` and `@attributes`.
- The test suite has 97 test functions and about 795 checks, including property and fuzz tests.

### 2.3 Arithmetic is better than the design document says

- Narrowing runs backward for `+`, `-`, and multiplication or division by a constant (`T/src/arith.c:292-335`). So `a + b = c` narrows `a` when `b` and `c` are known.
- A product or quotient of two variables does not narrow backward (`arith.c:295-296`). The distance, speed, and time example of design section 8.1 needs this.
- Values are int64 intervals in exact unit quanta, for example 0.1 mm for length (`T/src/units.c:16-29`). Intermediate results use `__int128`, and bounds round outward. There are no floats.
- Settling is not incremental. `settle_arith` (`T/src/load.c:1367`) rescans the whole structure on each round and stops after 16 rounds.

### 2.4 Obstacles to a persistent propagator network in Triangle

- Each node has scratch fields (`sx`, `cpy`) that every operation overwrites, also on nodes that the loaded content owns. Generation counters invalidate old scratch (`T/src/internal.h:426-436`). Persistent propagator links therefore cannot be stored on nodes.
- The content has one scratch arena, and the failure state is also stored in the content. Everything is single-threaded for each loaded content.
- The unification kernel calls back into the load pipeline (`settle`, `fs.c:922`).
- The only external hook is `tri_register_fn` (`T/src/load.c:2411`). Only `tri_derive` and `tri_construct` call it, never unification or matching. The builtins are a fixed static table.
- The API is text-shaped. Callers build descriptions as strings that Triangle parses again (Bruggadung `REVIEW.md` §3.11).
- Bruggadung `REVIEW.md:85-91` lists open memory-safety defects.

The core files have almost no global state: `fs.c`, `types.c`, `qc.c`, `util.c`, `units.c`, and `arith.c`. They are the best candidates to borrow.

### 2.5 Bruggadung's scheduler

The scheduler is event-driven, with one global int64 clock in 1 ms ticks. Each step goes to the earliest of these: the next input, activity completions, decision times, and each active domain's `t + safeStep` and `nextEvent` (`L/sched_scheduler.odin:127-158`). All domains advance together to that boundary. Commitment then runs, followed by perception and decision.

Design section 11.3 describes phenomena that advance asynchronously: the fire at minute 20 while a person acts at minute 12. Bruggadung cannot do that, so "a mechanism like Bruggadung's existing scheduler" overstates the match. The `Horizon{safeStep, nextEvent}` concept transfers directly. Asynchronous clocks for each phenomenon are new research.

### 2.6 Concept map

| Quadrilateral | Nearest Bruggadung concept | State in Bruggadung |
|---|---|---|
| Phenomenon | Domain (one family of stock kinds, world-wide), progression (what one thing does over time) | Domains built. Progressions partly built. |
| Model at a level of detail | Detail (full, reduced, abstract, frozen), regime (per kind), mode (per system) | Built for agents only. Regimes are design only, and they are resolved at load. |
| Abstraction and refinement | Fall and rise, summarise and settle, catch-up | Catch-up built for agents, and crude |
| Seeded reconstruction | Latent assembly: a part is revealed from the unit's seed and its role, and nothing contradicts revealed facts (`LATENT_ASSEMBLY.md:21-44`) | Built |
| Port, interaction | Participant, contribution (typed, with units, additive, tagged with its source), transit, proposal | Built. There are no declared typed ports. |
| Single-writer ownership, exact conservation | One owner per value; integer stocks | Built |
| Next event, safe horizon | `Horizon{safeStep, nextEvent}`, wake queues | Built |
| Validity contract | "Approximation limits" in a domain's declaration | Documentation only. Nothing invalidates a prediction. |
| EXPLAIN | Causal traces and the inspector ("why does this value have this value?") | Built for values, not for detail choices |

### 2.7 Lessons from Bruggadung

- **Determinism.** Bruggadung dropped determinism as a goal (`CHANGELOG.md:56`, `ARCHITECTURE.md:145`). Quadrilateral needs it again for seeded refinement (D4).
- **Coupling.** Bruggadung puts tightly coupled processes in one domain, and domains exchange only additive contributions that lag by one step. Quadrilateral deliberately chooses atomic transfers that both endpoints agree on (design section 9.2).
- **Model selection.** Bruggadung resolves regimes at load, and they must not read changing state. Quadrilateral selects models while the simulation runs.
- **Where the cost is.** Minds use 88% of Bruggadung's runtime, and domains use 6% (`PERFORMANCE.md:77-79`). Physical level of detail may not save much. That is acceptable for research, but agents should become phenomena eventually.
- **Content cannot define mechanisms.** Bruggadung content names an engine mechanism but cannot define one (`REVIEW.md:51-58`). The pattern "value at a time, closed form, wake at a threshold" is written six times (`REVIEW.md:333-375`). That pattern is a candidate for a first-class construct, and the kernel interface plus the path to self-hosting addresses the general problem.
- **Missing operations.** Triangle has no exponentials, minimum, maximum, or loops, so the engine evaluates them (`DETAIL_REGIMES.md:142, 164`).

## 3. Response to the owner's notes in the design document

### 3.1 Non-monotonicity and CALM (design section 8.5)

Non-monotonicity is not a fundamental obstacle. The CALM theorem (Consistency As Logical Monotonicity) says that a program has a consistent implementation without coordination if and only if the program is monotonic. CALM therefore tells the design where coordination is necessary.

The design already separates two kinds of step:

- Inside one epoch, propagation only narrows domains. The result does not depend on the order of the propagators, so propagation can run in parallel without coordination.
- These steps are non-monotonic and need coordination: commitment of an evolution step, invalidation of a prediction, selection of the cheapest valid model, and a decision that turns an "unknown" match into "no match".

The non-monotonic steps are the synchronization boundaries. This gives a principled answer to open question 7: the runtime synchronizes only at non-monotonic steps. The IR should mark the monotonic and non-monotonic steps from the start. That is cheap now and hard to add later.

Prior art to add to design section 16:

| Work | Relevance |
|---|---|
| Dedalus (Alvaro et al., 2010), and Bloom | Datalog with explicit time. Rules inside one timestep are monotonic. Rules that go to the next timestep are not. This is almost exactly design section 8.5. |
| LVars (Kuper and Newton) | Lattice variables with threshold reads and a freeze operation. Triangle's "unknown" match is a threshold read. |
| Radul and Sussman, "The Art of the Propagator" | Propagator networks with cells that merge partial information, and premises for retraction. The closest prior art for design sections 8.3 and 14.2. |
| Chandy, Misra, and Bryant (conservative parallel discrete-event simulation) | Lookahead and null messages. Lookahead is the safe advancement horizon. Relevant to asynchronous clocks. |

### 3.2 Refinement as reification (design section 7.3)

Agreed. Refinement can be a domain-specific generator under a checked contract. The contract is the fixed part:

1. The output must unify with the constraints of the abstract state.
2. Conserved quantities must match exactly. With fixed-point state (D5), an exact comparison is possible.
3. The same state and the same seed must give the same output.

The plausibility of the generated detail is the open part, and it can improve later. Bruggadung's latent assembly is a working example of this kind of generator.

The guarantee that a coarse prediction encloses the detailed behavior (design section 7.4) is a separate and more ambitious problem. The plan does not depend on it.

## 4. Fixed and open parts

These decisions are expensive to change later, so the plan makes them early:

- **Kernel interface.** A C interface with opaque handles. A kernel declares what it reads, what it writes, and what it conserves. At run time it reports its next event, its safe horizon, and its invalidation dependencies. It also declares which directions it can evaluate, so that backward reasoning stays possible.
- **Numbers.** Fixed point for authoritative state, with the scale in the quantity type (D5, section 5.3).
- **Time.** An int64 tick of 1 µs, which gives about 292,000 years of range (D7). Make the tick a setting of each simulated world, so that a geological or astronomical world can choose a longer tick. Do not use a different tick for each phenomenon, because synchronization needs one clock. A phenomenon that needs finer steps than the tick takes substeps inside its kernel. An event time is an interval from the start, even if the first scheduler reads only the lower bound.
- **Determinism and randomness.** Seeded random streams that do not depend on execution order (section 5.1).
- **State versions.** Each propagation context has an epoch, an immutable version. Epochs keep these options open: rollback, debugging by evaluation time, backward reasoning.
- **Cells outside the feature-structure graph.** Feature structures give the schema and the initial domains. Cells hold the changing state. This follows from section 2.4, and it agrees with design section 10.5.
- **Monotonic and non-monotonic steps marked in the IR** (section 3.1).

These parts stay open, and nothing in the early phases closes them: asynchronous clocks for each phenomenon, model selection by budget, EXPLAIN, trajectory constraints, backward reasoning, kernels written in Quadrilateral, a BEAM backend, FMI interoperability, Differential Dataflow, and minds as phenomena.

## 5. Notes on decisions D4 and D5

### 5.1 Explicit randomness

Randomness inside the simulation fits D4. A stochastic phenomenon is part of the simulated world, and a run stays reproducible if its random numbers come from the seed.

One property is important. A random draw must not depend on the order in which the scheduler runs phenomena. If all phenomena share one random stream, a change in scheduling order changes every later draw. Concurrency would then break reproducibility.

Use a counter-based generator (for example Philox, or SplitMix64 applied to a hash). Each draw is a pure function of (run seed, phenomenon identity, stream name, event index). Then:

- A draw is reproducible without a shared generator state.
- Parallel execution gives the same results as sequential execution.
- Refinement can use the same mechanism. Its seed is a function of the abstract state's identity and epoch.
- The language can mark a phenomenon as stochastic, and EXPLAIN can report which draws affected an outcome.

### 5.2 Fixed point or floating point

Floating-point error alone is often small. The problem is that floating point breaks three properties that Quadrilateral depends on:

1. **Exact conservation.** Floating-point addition is not associative. The sum of a set of transfers depends on the order of the additions. A conservation ledger cannot then check for exact equality, and small errors accumulate over long runs.
2. **Reproducibility.** Different compilers, optimization flags, fused multiply-add instructions, and platforms can give different results. A different order of execution also gives different results.
3. **Sound intervals.** An interval must round its bounds outward. In floating point that needs directed rounding modes, which are slow and not portable. In fixed point it is simple, and Triangle already does it.

The proposed rule:

- Authoritative state, transfers, and interval bounds are fixed-point integers. Intermediate results use `__int128`.
- A kernel can compute internally in double precision. For example, SUNDIALS requires doubles. The kernel converts its result to fixed point when it commits.
- A transfer is converted once to a quantity `q`. The source loses `q` and the sink gains `q`, so conservation is exact for any internal kernel error.

### 5.3 Scales for arbitrary domains

Triangle has one fixed scale for each dimension, for example 0.1 mm for length. One scale cannot serve arbitrary domains. An int64 has about 18 significant decimal digits. A room in millijoules and a star in millijoules do not both fit, and neither do a cell in nanometres and a continent in nanometres.

The recommendation is to put the scale in the quantity type, not in the dimension:

1. A quantity type has three parts: a dimension (exponents of the SI base dimensions), a decimal scale exponent, and a storage width. Example: "energy, 10^-3 J, int64" or "energy, 10^30 J, int64".
2. Each dimension has a default scale, taken from Triangle's `units.c`. An author declares a different scale only when a domain needs it. Most content never declares a scale.
3. The storage width is int64 by default. An author can declare int128 for a quantity type that needs more than 18 digits of range and precision together (about 38 digits).
4. A conversion to a finer scale is exact. A conversion to a coarser scale rounds. Interval bounds round outward. A point value uses a declared rounding rule, with round-half-even as the default.
5. Both ports of a conserved interaction use the same quantity type. The compiler rejects a mismatch. To couple two scales, the author inserts an explicit converter. The converter keeps the rounding remainder in its own ledger, so nothing is lost.
6. An overflow is an error. It never wraps, and it never saturates silently. Bruggadung learned this lesson: its fixed caps that failed silently became errors (commit `f405c3a`).
7. Units with an offset, such as °C, exist only at input and output. Storage uses an absolute scale, such as kelvin.
8. Probabilities and other dimensionless ratios use the same mechanism, for example a scale of 10^-9.

Some domains do not fit fixed point at all. Examples are chaotic systems and quantities that cover many orders of magnitude inside one phenomenon. A kernel can keep such values as kernel-private floating-point state. Kernel-private state is not authoritative, and other phenomena cannot read it. The kernel publishes only fixed-point values through its ports. For such a kernel, reproducibility holds only for the same build on the same platform, and the kernel declares this limitation in its contract.

This design keeps exact conservation and reproducibility for all shared state, and it supports arbitrary domains. Its cost is that the compiler must check the quantity type at each port and insert or reject conversions.

## 6. Phased plan

The propagator engine, the kernel interface, and the scheduler do not depend on the language front end. C tests can drive them directly. The front end (parser, types, unifier) can therefore come in parallel or later. Its absence does not block the semantic core.

### Phase 1: foundations

1. Create the repository layout, the build, and the test harness.
2. Write short ADRs (architecture decision records) for D1 to D7, the epoch model, and cells outside the feature-structure graph.
3. Implement the fixed-point quantity-type and interval library of section 5.3. Borrow from `T/src/units.c` and `T/src/arith.c`.
4. Implement the counter-based random number generator.

### Phase 2: propagator engine

1. Implement cells, an agenda, and propagators that declare their dependencies (design section 8.3).
2. Implement epochs and domain-version tracking.
3. Implement backward narrowing for the product and the quotient of two variables.
4. Test with the distance, speed, and time example (design section 8.1) and the 70 to 130 W wall example (design section 9.4).

### Phase 3: kernel interface and phenomenon schemas

1. Define the C kernel interface, version 0.
2. Define phenomenon, port, and interaction descriptions. At first, C code or a small data format describes them. When the front end exists, Quadrilateral source describes them.
3. Compile a description into an execution graph of ownership, dependencies, and transfers.

### Phase 4: scheduler and transfers

1. Implement a global-boundary event scheduler in Bruggadung's style, with safe horizons and next events.
2. Implement atomic transfers and a conservation ledger.
3. Run the two rooms and the wall (design section 9.2). Two lumped rooms have an exact exponential solution, so the test has a ground-truth answer without SUNDIALS.

### Phase 5: switching the level of detail

1. Switch one room between an aggregate model and an N-cell model.
2. Implement seeded reification under the contract of section 3.2.
3. Check conservation across each switch, and measure prediction error and cost.
4. Add a door event that changes the conductance. This tests validity contracts and invalidation, and it is a small form of open question 6.

### Language front end (in parallel with phases 2 to 5)

1. Write a parser for the CUE-like syntax. Borrow from `T/src/parse.c` where it fits.
2. Implement the type hierarchy with glb completion and the unifier. Borrow from `T/src/types.c` and `T/src/fs.c`, but give each operation its own context so that operations do not share a scratch arena.
3. Use a handle-based API, not a text-shaped API.
4. Decide later whether definite clauses (`T/src/clause.c`) are necessary.

### After phase 5

The research branches in section 4 are independent of each other. The results of phases 4 and 5 can decide their order.

## 7. Open items

No decisions are open.

Section 2 of `quadrilateral-design.md` now includes the corrections from section 2 of this document.

## 8. Points from the source conversation that the ADRs do not cover yet

A second reading of `Brigadoon Triangle Language (2026-11-20).md`, on 2026-11-21, after phase 5, found these points. Line numbers refer to a copy of the transcript without the filler turns, so the quotes are the reference.

### 8.1 Points that shape the language front end (ADR 0011 takes them in)

| Point | Source | State |
|---|---|---|
| The type system as the dependency system: the compiler knows which facts depend on which, and builds the incremental execution graph from that | "whether Triangle's type system can also serve as its simulation dependency system" | Not addressed. ADR 0011 makes the elaborator compute the dependency graph and record it in the IR. |
| An explicit distinction between pure, monotonic deductions and state-changing processes | "an explicit distinction between pure, monotonic deductions and state-changing processes" | ADR 0008 for propagation. ADR 0011 extends it to rules and relations. |
| A Datalog-like relational layer (reachability, habitability, task eligibility) | Capability 2 of the first proposal | Not built. ADR 0011 constrains its semantics so that incremental evaluation stays valid. |
| CHR (Constraint Handling Rules), rated "very high" | The technology table | Not addressed. CHR propagation rules are monotonic, but simplification rules remove constraints, so they are not monotonic. ADR 0011 allows only the propagation form inside an epoch. |
| Applicability conditions of a model as constraints. A model declares its assumptions, the information it preserves, its cost, and its validity | "those applicability conditions could themselves be expressed as constraints" | ADR 0010 has models but no declarations of this kind. ADR 0011 gives model descriptions these fields, even before automatic selection uses them. |
| Laws attached to a phenomenon (`#ConservationOfMass`, …) | The `#Combustion` sketch | Conservation is a property of a kind (ADR 0009). ADR 0011 lowers laws to conserved kinds, propagators, and invariants. |
| Interactions as typed feature structures, which can be unified and inspected | "make interactions themselves typed feature structures" | The C host has port connections and transfers, but no interaction object. ADR 0011 makes interactions source-level structures that lower to both. |
| A shared-variable view for convenience, compiled down to interactions | "offer a shared-variable view for convenience, but compile it down to interactions" | Not addressed. ADR 0011 lists it as optional syntax. |
| Phenomena that start and end during a run (a fire starts) | The fire examples throughout | Not addressed: the world is built before the run. ADR 0011 requires instantiation rules, and the runtime must create and end phenomena at boundaries with stable identities. |
| The workshop experiment: workers, tools, materials, recipes, needs. Measure how naturally the language expresses it, how much work is incremental, and whether the hot paths compile | "My first experiment would be to take a small simulation—perhaps a workshop" | Triangle has `examples/workshop`. ADR 0011 uses it as an acceptance test for the front end. |

### 8.2 Points for the runtime that stay open

| Point | Source | State |
|---|---|---|
| Probabilistic values: distributions over quantities and over future events ("for a negotiation, a distribution over possible future events") | Capability 4, and the time discussion | Intervals and seeded draws exist. Distributions as values do not. An event prediction is an interval, with no probability. |
| A coarse model's port values carry an error bound ("expose a bounded boundary-temperature estimate"), and a bound that is too wide requests refinement | The wall example, section 5 | The aggregate room publishes its mean temperature as if it were the boundary temperature. Intervals could carry the bound, but no model widens them yet. This is also the "uncertainty" criterion of model selection. |
| Evaluator directions: "the language could distinguish the mathematical relation from the directions in which an implementation can efficiently evaluate it" | The time discussion | Section 4 of this document planned this, but ADR 0009 has no flag for it. Narrowing kernels are relations in every direction, and evolution kernels are forward only. A reserved flag for backward evaluation would keep the option visible. |
| Temporal constraints between events (Simple Temporal Networks: "A before B, B within five minutes of A") | The time discussion | Event times are intervals (ADR 0005), but events are not cells, so no propagator relates them. |
| Trajectory constraints ("temperature never exceeds this threshold during the interval") | The time discussion | Open question 4 of the design. An evolution kernel could publish the range of a value over its step as an interval in an `OUT` slot, which would let a propagator check such a constraint. |
| Agents as actors with beliefs, goals, and mailboxes, whose decisions are proposals; the separation of subjective knowledge from the authoritative simulation | The concurrency discussion | Not addressed. Runtime unification (ADR 0007) is the planned basis for exchanging partial descriptions. |
| Spatial state, spatial queries, and sparse spatial updates | The first architecture | Not addressed. A Dwarf Fortress-like world needs a spatial index. |
| Event history in the persistent world | The first architecture | Epochs can be kept (ADR 0008), but nothing keeps them yet, and there is no history of events. |
| FMI capability negotiation | The FMI recommendation | Flags and table sizes cover part of it. |
