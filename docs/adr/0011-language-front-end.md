---
status: accepted
---

# The front end compiles Quadrilateral source to an IR that builds a world, and every rule is monotonic inside an epoch

Phases 1 to 5 built the semantic core as a C API: quantity types, propagators, epochs, kernels, models, and the scheduler. Quadrilateral source must describe the same things: kinds, phenomena, ports, interactions, laws, models, and relations. The front end also decides whether the relational layer of the design (design section 17.1) can later be evaluated incrementally, in the style of Differential Dataflow (design section 15). A rule that depends on evaluation order, or that reads "the previous value" implicitly, would make incremental evaluation invalid, and that would be expensive to undo. We decided on a CUE-like language borrowed from Triangle, a pipeline that produces an inspectable IR, and semantic rules that keep every deduction monotonic inside an epoch.

## Decision

### 1. The language

- The syntax and the core semantics come from Triangle, which follows CUE: types (`#Name`), unification (`&`), disjunction (`|`), defaults (`*`), bounds, tags, references, comprehensions, packages, attributes, and docstrings. Source files have the extension `.quad`.
- References share in both directions, as in Triangle: `size: .instrument.size` makes one node. This is relational semantics, and it matches propagation.
- The result of loading does not depend on the order of files or declarations. Triangle guarantees this, and Quadrilateral keeps it (ADR 0004).
- A number with a unit converts exactly to the quantity type of its field (ADR 0001). A literal that is not a whole number of quanta is an error. Kinds are declared in source, with the dimension grammar of ADR 0009 section 10. ADR 0014 decides the syntax of units in literals: `1000J/K`.
- Not borrowed at first: Triangle's TDL front end, and component generation for Odin.

### 2. The prelude

A prelude defines the types that the elaborator recognizes: `#Phenomenon`, `#Port`, `#Interaction`, `#Model`, `#Reaction`, and `#Rule`. A kind is not a prelude type: a declaration with the prefix `%` declares it (ADR 0015). They are ordinary types, so an author specializes them by unification, for example `#WoodCombustion: #Combustion & {…}`. This keeps the decision that a process is a typed feature structure.

An illustrative sketch of the wall of design section 9.2. The syntax is not final.

```cue
%energy:      { dimension: "J",   scale: -3, conserved: true }
%temperature: { dimension: "K",   scale: -6 }
%capacity:    { dimension: "J/K", scale: -3 }

// A type introduces each feature once (ADR 0015), so one supertype has the
// three fields that the port and the room share.
#Thermal: {
    temperature: %temperature   // an observation: intensive, read only
    capacity:    %capacity
    energy:      %energy        // a stock: reached only through transfers
}

#ThermalPort: #Port & #Thermal

#LumpedRoom: #Phenomenon & #Thermal & {
    laws: [ .energy == .capacity * .temperature ]
    west: #ThermalPort & { temperature: .temperature, capacity: .capacity, energy: .energy }
}

#ConductiveWall: #Phenomenon & {
    hot:  #ThermalPort
    cold: #ThermalPort
    conductance: %thermal_conductance
    interaction: #EnergyTransfer & { source: .hot.energy, sink: .cold.energy }
    evaluator: "native.conduction"
}
```

### 3. The pipeline

1. **Parse** to a syntax tree.
2. **Resolve**: build and complete the type hierarchy, then unify each declaration. The code for this comes from Triangle's `types.c`, `fs.c`, and `qc.c`, under ADR 0003. Each unification has its own context, not one scratch arena for each loaded content (ADR 0007).
3. **Elaborate**: recognize the prelude's types and produce the IR. This step computes the cells, propagators, ownership, ports, transfers, models, and kernel bindings. It also computes the dependency graph: which cells each propagator, rule, and kernel reads and writes. This is the "type system as the dependency system" of design section 17.3.
4. **Instantiate**: build a world from the IR through the C API of phases 3 to 5.

The IR is plain data with a text form, so a person or a test can inspect it. It is the "typed simulation IR" of design section 13.2. Another backend, for example a BEAM backend, would read the same IR.

### 4. Laws and interactions

- A conservation law is a conserved kind (ADR 0009).
- An equation or an inequality becomes propagators: sum, product, equality, or a custom propagator.
- An invariant is a constraint. If propagation finds it empty, that is a contradiction, and the error names the source location and the feature path (ADR 0007).
- An interaction is a typed feature structure with a source, a sink, a kind, and a conserved flag. It lowers to port connections and to the transfer declarations of a kernel. An interaction without state is a relation. An interaction with state, such as a wall with thermal mass, is a phenomenon (design section 9.5).
- A reaction (ADR 0009 consequences) lowers to one transfer for each flow, with fixed ratios.
- A shared-variable view, compiled to interactions, is optional syntax for later.

### 5. Models

A model declaration has fields for its applicability conditions (constraints), the information it preserves, and its cost. Selection is still explicit (ADR 0010), but the fields exist from the start, so automatic selection can read them later. Map kernels are named in source like evaluators.

### 6. Rules and relations stay monotonic inside an epoch

These constraints keep incremental and differential evaluation valid.

1. Inside an epoch, a rule can only add facts or narrow values. Unification, propagators, Datalog rules (bottom-up evaluation, set semantics), and the propagation form of CHR rules all have this property.
2. Negation, aggregation (count, sum, minimum, maximum over a relation), a decision of "no match" on an unknown match, retraction, and CHR simplification rules are not monotonic. They are stratified, and they run only at epoch boundaries, after the monotonic fixed point, in a defined order (ADR 0008). The compiler rejects a cycle through negation or aggregation.
3. No result depends on the order of rules, clauses, or evaluation. Triangle's definite clauses use depth-first search in clause order with a depth limit, so they are a search facility, not a relation definition. Quadrilateral keeps them as an explicit query, for example for planning by agents, and runs them only at boundaries.
4. No rule reads a previous value implicitly. A law that relates two states names both states explicitly. Only evolution can use such a law: kernels, and transitions at boundaries.
5. A fact has a stable identity: a key made from paths and participant identities, never from memory addresses or creation order.
6. The runtime keeps a change log for each boundary: (epoch, cell, old value, new value) for a replaced value, and (epoch, cell, amount) for a stock. The relational layer maintains its results from this log, and a later differential engine would read the same log.

### 7. Phenomena that start and end

Source declares instantiation rules: when a phenomenon exists. For example, a combustion exists for each combustible whose temperature is above its ignition temperature.

- Creating or ending an instance is not monotonic, so it happens only at a boundary.
- The identity of an instance is a key made from the rule and its participants, for example `combustion(building7)`. It never depends on creation order.
- Cells are only ever added, so indices stay stable. An instance that ends must first transfer its stocks out, as a model switch does. Its cells then become dormant.

The runtime cannot do this yet. It is new work, and it is the first runtime work that the front end needs.

## Acceptance tests

1. The two-room world of phase 4, written in source, gives the same state hash at 1000 s as the world that the C tests build.
2. The experiment of phase 5, written in source, gives the same results.
3. Triangle's `examples/workshop`, ported, measures the three things of design section 17.5: how naturally the language expresses it, how much work is incremental, and which hot paths need native code.

## Considered options

- **Only a C API, with no source language.** Then content would be C code, and none of the language's reasoning (unification, dependency analysis, EXPLAIN by path) would be possible. We rejected it.
- **Copy Triangle's loader as a whole.** `load.c` mixes the load pipeline with runtime functions and a text-shaped API (`quadrilateral-review-fable.md` section 2.4). We copy the parts that fit, under ADR 0003.
- **Elaborate directly into a world, with no IR.** That is simpler, but a second backend, inspection, and the dependency graph all need the description as data. We rejected it.
- **Definite clauses as the relational layer.** Their results depend on clause order, a search can fail to end, and they cannot be maintained incrementally. We rejected them for relations and kept them as queries.
- **A new syntax, unlike CUE.** It would lose continuity with Triangle and its documentation. We rejected it.

## Consequences

- The runtime needs a change log, phenomena that start and end, and a relation engine.
- Cells need domains other than quantity intervals, for example atoms and finite sets (`door: "open" | "closed"`). This is the CLP(X) pattern of design section 5: unification coordinates several constraint domains. The network has only intervals today.
- The front end can grow in stages:
  1. Parser.
  2. Type hierarchy and unifier.
  3. Elaborator and IR for the two-room world.
  4. Models, ports, interactions, and reactions.
  5. Relations and the change log.
  6. Runtime unification for agents.
