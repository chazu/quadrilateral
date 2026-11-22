---
status: accepted
---

# A phenomenon instance starts and ends at a boundary, and an existence condition or an outside input decides when

A fire starts during a run, and it ends when its fuel is gone. Until now, a world was built before the run, and every phenomenon lived for the whole run (`quadrilateral-review-fable.md` section 8.1). ADR 0011 section 7 requires instantiation rules: source declares when a phenomenon exists. The design sets three constraints on the runtime. Authors describe causality and the runtime manages coordination (design section 11.5), so an author does not write the call that starts a fire. The runtime must detect an interaction before it advances past the interaction (design section 11.3), so a start cannot wait until a call to `qw_run` returns. A start or an end is not monotonic, so it happens at a boundary, in a defined order (ADR 0008). We decided that an instance is declared with a stable key, that it is alive or dormant, that a map kernel moves its stocks at a start and at an end, and that the world checks existence conditions in a new step of each boundary. An explicit call also starts or ends an instance, as an input from outside the simulation.

## Decision

### 1. Instances and keys

- An instance is a phenomenon that can start and end during a run. An instance is alive or dormant. A phenomenon from `qw_phenomenon` is alive for the whole run, as before.
- The key of an instance is its name. The front end makes the key from the rule and the participants, in the order of the rule's roles, for example `combustion(building7)`. The runtime does not interpret the key. Names can now contain `(`, `)`, and `,`.
- The participants of an instance are the phenomena that its ports connect to.
- Everything that must be stable uses the key, and never a cell index, a phenomenon index, or the order of creation:
  - The phenomenon identity of a random stream (ADR 0002) is a hash of the key. Today it is the phenomenon index.
  - The state hash covers each cell with its path, and it does not depend on the order of the cells.
  - The order of the lifecycle steps at one boundary (section 5).

### 2. Declaration

- The world declares an instance at any boundary: before the run, or during it. A new instance is dormant.
- The ordinary building functions add the instance's cells, propagators, ports, models, and kernels. They now work at any boundary. Cells are only added. No cell is removed or gets a new index.
- A stock of an instance is declared as zero. An instance gets its stocks only through transfers.
- While an instance is dormant, every one of its stocks is exactly zero, its propagator group is disabled, and its kernels do not run. A dormant instance costs memory, but no computation.
- A phenomenon that is not an instance cannot connect a port to an instance, because it would read dormant cells. It reads the instance's effects through its own stocks, or through cells such as `alive`.
- The world records the state of an instance in a boolean cell, `<key>.alive`, that only the host writes. So the change log, the state hash, and later snapshots and EXPLAIN see a start or an end as an ordinary change of a cell (ADR 0012). Rules and propagators can read the cell. The feature name `alive` is reserved.
- The world has a built-in enumeration kind `boolean`, with the atoms `false` and `true` (ADR 0012 section 1).

### 3. The start

A start happens at the current time, at a boundary:

1. The host creates the instance's kernel instances. It binds the start map kernel, if the instance has one. The map's slots resolve under the instance, and through the instance's ports to its participants.
2. The host calls `map`. The map writes the instance's `STATE` and `OUT` slots, and it can transfer stocks from the participants into the instance. Its random cursor uses the key and the epoch number (ADR 0010 section 3).
3. The host commits the writes and the transfers, writes `alive`, enables the instance's propagator group, and opens a new epoch.

If a step fails, nothing changes. A start fails while a participant is dormant.

### 4. The end

1. The host calls the end map kernel. It transfers every stock of the instance out to the participants. A transfer to `QK_NONE` is possible only for a kind that is not conserved (ADR 0009 section 3).
2. The host checks that every stock of the instance is exactly zero. If one is not, the end fails, and nothing changes. This is the check of a model switch (ADR 0010 section 3).
3. The host commits, writes `alive`, sets every other cell of the instance to its declared initial value, disables the group, destroys the kernel instances, and opens a new epoch.

- A dormant instance holds no state. A later start of the same key is therefore an ordinary start, and its result does not depend on the earlier life.
- The event count of each kernel continues across lives, so a second life does not repeat the random draws of the first.
- The instances that depend on an instance end first. An explicit end fails while an alive instance connects to the instance through a port. In the lifecycle step, the world ends those instances in the same round (section 5).

### 5. What starts and ends an instance

**An existence condition.** An instance can name a boolean cell as its condition. Propagation computes the condition, so it is monotonic inside an epoch. For example, a reification propagator (ADR 0012) makes the condition true when `building7.temperature` is at or above the ignition temperature.

The boundary sequence of ADR 0006 gets a sixth step, the lifecycle step, after propagation and the validity checks. It runs inside `qw_run`, after each boundary and also when the call begins:

- For each instance that has a condition: if the condition is true, the instance is dormant, and every participant is alive, the instance starts. If the condition is false and the instance is alive, the instance ends. If the condition is unknown, nothing changes. That last rule is a decision from the absence of information, which ADR 0008 allows only at a boundary.
- When an instance ends, every alive instance that connects to it ends in the same round, with or without a condition of its own.
- All decisions of one round read the same fixed point. The world then applies the ends, each instance before its participants, with ties in key order. Then it applies the starts in key order. An instance never starts in the round in which a participant starts or ends, so it starts one round after its participants.
- Each start and each end opens its own epoch.
- A change can make another condition true or false, so the world repeats the round until no instance changes. An instance changes at most once at one boundary. A second change of the same instance is an error that names the instance. This is the run-time form of a cycle through negation (ADR 0011 section 6).

**An outside input.** `qw_start` and `qw_end` start and end an instance at the current time, between calls to `qw_run`. They are inputs from outside the simulation, like `qw_input`: a player lights a match, or a test controls an instance. They apply only to an instance without a condition.

**The front end.** An instantiation rule lowers to declared instances and condition cells. The "for each" part of a rule is the work of the relation engine (quad-ijx.5 and quad-ijx.7), which will declare an instance when a new candidate appears. For that, the lifecycle step reserves a hook. This ADR does not build the hook.

### 6. Timing

- The world sees a condition only at a boundary. A start can therefore be late by up to one step, unless a kernel predicts the crossing as an event. Exact crossing times are the work of quad-h21.1, which makes predicted event windows cells.
- After a lifecycle change, the host checks the assumptions of every prediction, as after any boundary.

## Considered options

- **Explicit calls only, as ADR 0010 did for model switches.** A fire could then not start inside a call to `qw_run`. The caller would have to stop the run often and test the conditions, which is the coordination that design section 11.5 gives to the runtime. The front end would later need the lifecycle step in any case, after content depends on its absence. We kept explicit calls as outside inputs only.
- **Existence as one more model, with an "absent" model and a switch.** The mechanism is the same: a map kernel, the zero-stock check, and propagator groups. But a phenomenon declares its models before the run, and an instance can be declared during the run, for participants that did not exist before. We reuse the mechanism, not the model list.
- **Destroy the cells of an instance that ends.** Indices would change during the run, and old epochs would be hard to keep (ADR 0010). We rejected it.
- **Identity from the phenomenon index.** The index depends on the order of creation, which ADR 0004 and ADR 0011 section 6 forbid for identities. We rejected it.
- **Separate start and end conditions.** One condition is enough for the first version. A rule that needs a latch can read `alive` in its condition, because `alive` does not change inside an epoch. That needs boolean propagators on set cells, which do not exist yet.

## Consequences

- The random identity changes from the phenomenon index to a hash of the key, so the draws of existing worlds change once. No saved runs exist yet. The world rejects a key whose hash is equal to the hash of an existing key.
- The value of `qw_hash` changes for existing worlds, because it now covers paths. The tests compare two runs, and they contain no fixed hash values.
- A snapshot (quad-5xe.1) must hold the declared instances and each kernel's event count, as well as the cells.
- An instance that is declared for each candidate costs its cells while it is dormant. The reserved hook lets the relation engine declare instances later, when a candidate appears.
- Automatic model selection (quad-ft5.3) is a decision of the same kind, and it belongs at the same place in the boundary sequence.
- Tests for this ADR: a condition starts and ends an instance inside one call to `qw_run`; every conserved total is exactly constant across the start and the end; two runs give the same hash; two worlds that declare and start the same instances in a different order give the same hash; a second life of an instance gives a valid run.
