# Quadrilateral design document

**Date:** 2026-11-20
**Status:** Exploratory. No implementation exists.
**Source:** `Brigadoon Triangle Language (2026-11-20).md`, a voice conversation with ChatGPT.

This document extracts the design content of the source conversation. Each design point has one of three statuses:

- **Decided**: the project owner stated the choice.
- **Proposed**: ChatGPT recommended the choice, and the project owner did not object.
- **Open**: the conversation raised the question and did not answer it.

The source transcript writes "Brigadoon" for the project name. The repository is `chazu/bruggadung`, so this document uses "Bruggadung".

## 1. Purpose

Quadrilateral is an experimental fork of Triangle. Triangle is the typed feature-structure language in the Bruggadung project. A typed feature structure is a record of named features with typed values, which can be combined with another record by unification.

Triangle largely imitates the syntax of CUE, and to a large extent its semantics. The transcript writes "QLang". This document reads that as CUE, because ChatGPT calls Triangle "CUE-like". Triangle is an authoring language for simulation content. The first application is a roguelike or management game in the style of Dwarf Fortress.

Quadrilateral has a larger scope than Triangle:

- Quadrilateral is the front end for a largely self-contained model of deep simulation (Decided).
- The goal is maximum expressive flexibility together with performant deep-simulation code (Decided).
- Quadrilateral describes the laws, constraints, and processes of a simulated world, and also its content (Proposed).
- The language is declarative and concurrent. Typed feature structures are the foundation of the whole computational model (Proposed).

A potential guiding principle is that the authoring language can be uniform while the execution model is not uniform. The author describes what happens and what must stay true. The compiler and the runtime select how to execute it. While it may be the case that in the future quadrilateral aims to _fully_ describe the simulation with no recourse to external kernels or runtimes, the most likely direction of its development is one where we rely heavily on compute 'kernels' to do the job of heavy computation while maintaining an interface to the quadrilateral language itself.

## 2. Background: what Triangle already has

ChatGPT reported these facts after it read the repository. This document did not verify them against the code.

- A C11 implementation with Tomabechi-style feature-structure unification.
- A completed type hierarchy and a quick-check optimization.
- Definite clauses.
- Interval arithmetic with physical units. The documented arithmetic propagates constraints mostly forward.
- Three-valued pattern matching (match, no match, unknown).
- An Odin integration layer and generated component structures.
- Full constraint resolution when content loads, and a restricted, conjunctive runtime. ChatGPT judged that this separation can make a highly expressive simulation language practical.
- Declarative effects, agent-specific knowledge views, atomic change commitment, and variable simulation detail.
- A scheduler. The source does not describe it.
- A lab that constructs descriptions from entity state and matches operations against them.

Bruggadung separates the subjective knowledge of an agent from the authoritative physical simulation. Quadrilateral keeps that separation.

ChatGPT named three Triangle semantics that optimization work must preserve: unknown matches, shared feature-structure nodes, and atomic commitment. ChatGPT stated this constraint for the Triangle work in section 17.4, before the fork decision. It is a reasonable constraint for Quadrilateral too, but the source does not say so.

ChatGPT also said that Triangle's typed feature-structure engine is one of the hardest pieces, and that it already exists.

## 3. Summary of decisions

| Topic | Choice | Status |
|---|---|---|
| Project form | Fork of Triangle, named Quadrilateral | Decided |
| Scope | Front end for a self-contained deep-simulation model | Decided |
| Fundamental simulation abstraction | Phenomenon, selected because it supports dynamic level of detail | Decided, as a stated preference ("I tend to think") |
| Process representation | A Quadrilateral process is a typed feature structure, subject to unification | Decided, as a stated preference ("I like the concept") |
| Simulation kernels | Implemented outside Quadrilateral for the first pass | Decided |
| Shared state between phenomena | Typed interactions, not shared variables | Decided |
| Synchronization boundaries | Hidden by default, with exposure as an advanced capability. The project owner said only that explicit exposure seems complicated and could be an ambition | Proposed |
| Phenomenon semantics | A relation over world states and time | Proposed |
| Fundamental computation abstraction | Propagator | Proposed |
| Time | Continuous in semantics, discrete and event-driven in execution | Proposed |
| Synchronization method | Conservative, with safe advancement horizons. No rollback | Proposed |
| Language self-hosting | Semantically self-contained, not computationally self-hosting | Proposed |
| First runtime | Triangle's C core, single-threaded | Proposed |
| BEAM | Experimental backend, not foundational immediately | Proposed |
| Target runtime (BEAM, Go, or C) | Not selected | Open |

## 4. Core concepts

Quadrilateral has a small set of first-class concepts.

| Concept | Definition |
|---|---|
| Phenomenon | A typed description of something that happens, with the laws that govern its evolution. A phenomenon owns state and can retain it. |
| Model | A typed feature structure that describes one way to simulate a phenomenon: its assumptions, the information it preserves, its cost, and when it is valid. |
| Evaluator | An implementation that computes the behavior of a phenomenon. |
| Kernel | An implementation of a propagator or of a temporal evolution operator. A kernel is external native code in the first pass. Later, a kernel can be written in Quadrilateral. |
| Port | A typed boundary that a phenomenon exposes. |
| Interaction | A typed feature structure that relates ports and states what can pass between them. |
| Propagator | A computation that watches some constraints and narrows other constraints when it learns something. |

The source uses "model" in the level-of-detail discussion and "evaluator" in the later discussion. The source does not state the exact relation between the two terms. This document reads a model as the description and an evaluator as the code that executes it.

ChatGPT's explicit recommendation, after the wall example, was four first-class concepts: phenomena, ports, interactions, and evaluators. Interactions are typed feature structures, so they can be unified, specialized, and inspected.

ChatGPT also stated that a phenomenon is not an entity, an actor, or a process. A phenomenon is more than a process.

ChatGPT first called the phenomenon the fundamental computational object. Later it made the propagator the computational abstraction and the phenomenon the simulation abstraction (section 12.5).

The aim is a relatively small conceptual core with considerable expressive power.

Phenomena describe processes. Interactions describe the coupling between processes. The runtime assembles these descriptions into an executable network.

## 5. Semantic layers

Quadrilateral has three connected layers (Proposed).

1. The structural constraint layer is Triangle's typed feature structures and unification.
2. The relational constraint layer adds equations, logical relations, and propagation across many variables.
3. The temporal layer lets phenomena constrain how state evolves, including what stays invariant across changes.

The runtime selects a solving and execution strategy for each problem:

- A small system of linear constraints can use a specialized arithmetic solver.
- A recursive relation can use a logic-programming engine.
- A large heat-transfer problem can compile into an optimized numerical kernel.

Tomabechi unification does not solve every mathematical problem. Feature-structure unification coordinates specialized constraint domains. The literature calls this pattern constraint logic programming, or CLP(X), where X is a constraint domain.

ChatGPT's hypothesis is that ordinary procedural code is an optimization or a specialized solver, and is not the semantic core. Open question 3 covers this hypothesis.

ChatGPT proposed that the language distinguish pure, monotonic deductions from processes that change state. This distinction permits caching, incremental evaluation, parallelism, and static dependency analysis. ChatGPT made this proposal for Triangle, before the fork decision. The Quadrilateral discussion did not restate it.

## 6. Phenomena

### 6.1 Identity independent of algorithm

A phenomenon has an identity and semantics that do not depend on the algorithm that currently simulates it.

A fire in a wooden building shows the idea. The four rows are four representations of one phenomenon, and not necessarily four phenomena.

| Representation | Method | State kept | Illustrative relative cost |
|---|---|---|---|
| Detailed | Spatial combustion and heat transport | Local temperature, oxygen, material changes | 95 |
| Regional | Coupled thermal compartments | Room-level temperature and fuel stocks | 42 |
| Aggregate | Lumped combustion dynamics | Total fuel, heat output, spread potential | 12 |
| Event-based | Predicted transitions and wake conditions | Estimated outcome, uncertainty, next event | 2 |

The costs are illustrative. They are not measured benchmarks.

The source prose gives more detail for each level:

- High fidelity simulates heat transfer between materials, oxygen consumption, combustion chemistry, smoke movement, and structural degradation.
- Medium fidelity treats the building as a few thermal regions. Each region has a temperature, a fuel quantity, and a combustion rate.
- Low fidelity treats the whole fire as one process with an estimated rate of fuel consumption and a probability of spreading.
- The lowest fidelity is a scheduled event. An example of the event-based form: "this building burns down in approximately twenty minutes unless something intervenes."

### 6.2 Phenomena as typed feature structures

The syntax below is illustrative. Current Triangle does not support it.

```cue
#Phenomenon: {
    participants: _
    state:        _
    laws:         _
    observables:  _
    regimes:      _
}

#Combustion: #Phenomenon & {
    participants: {
        fuel:     #Combustible
        oxidizer: #Oxidizer
        region:   #SpatialRegion
    }

    state: {
        fuelRemaining: mass
        temperature:   temperature
        burnRate:      mass / time
    }

    laws: [
        #ConservationOfMass,
        #ConservationOfEnergy,
        #CombustionKinetics,
    ]
}
```

Unification specializes a phenomenon. For example, unification specializes combustion into wood combustion. This is more than inheritance. The result can carry additional constraints, applicable laws, and information about which simulation algorithms are valid.

Applicability conditions are constraints. For example, a combustion model can apply only when oxygen is plentiful and the material is approximately homogeneous.

### 6.3 Phenomena as relations over states and time

A phenomenon is a relation over world states and time (Proposed). The relation says: given this world state at time T, these future states are valid. A phenomenon is not a small program with mutable state that the engine steps on each tick. This view brings Quadrilateral closer to constraint programming, relational semantics, and declarative physics than to a traditional game loop.

Consequences:

- Execution is one way to explore or sample the relation. Execution does not define the phenomenon.
- Time is explicit, and the language can reason about it.
- The compiler can ask what must be true for a coarse model to refine into a detailed model without contradiction.
- Two phenomena can be compared by their constraints, without running them.
- A relation can be reasoned about backward as well as forward. Example: "What conditions were sufficient for this building to collapse before noon?"
- Backward reasoning supports planning, diagnosis, counterfactual reasoning, and agent inference in the same language.

Not every evaluator supports backward reasoning. A numerical kernel can support forward evolution only. The language distinguishes the mathematical relation from the directions in which an implementation can evaluate it efficiently.

ChatGPT suspects that Quadrilateral should have trajectory semantics, with state-to-state transitions as the primary executable approximation. This point is Open (open question 4). A trajectory constraint covers a whole interval, for example "temperature never exceeds this threshold during the interval". Trajectory constraints are more expressive and much harder to implement efficiently.

### 6.4 Composition

One object can participate in several phenomena at the same time. A burning wooden building participates in:

- Combustion, which consumes fuel.
- Heat transfer, which changes temperatures.
- Structural mechanics, which responds to material degradation.
- Smoke transport, which affects visibility and breathing.
- Human behavior, which responds to perceived danger.

These phenomena share participants and state, and they operate at different timescales. They couple through typed interfaces and do not manipulate each other directly. Example: combustion produces heat, heat transfer consumes that contribution, and structural degradation responds to temperature histories.

The runtime can analyze a composition:

- If two phenomena operate on disjoint state, they can execute independently.
- If they share conserved quantities, they need a coordinated transfer mechanism.
- If one depends on the output of another, the runtime derives a scheduling dependency.
- If the coupling is weak enough, the runtime may run them at different fidelity levels.

The scheduler therefore schedules phenomena and their interactions. It does not schedule entities or ticks.

## 7. Variable level of detail

Dynamic level of detail is the reason phenomena are the fundamental abstraction (Decided). The simulation has a locus of focus, and it must scale detail dynamically to use resources efficiently.

### 7.1 Model selection

Level of detail is a model-selection problem. The design does not hardcode a fixed number of fidelity levels. Each phenomenon has a collection of models.

Each model declares:

- Its assumptions.
- The information it preserves.
- Its computational cost.
- The circumstances in which it is valid.

The runtime matches a phenomenon against the available models. It rejects incompatible models and selects the cheapest valid model that satisfies the current requirements.

The requirements depend on more than distance from the player:

- Observability: what must be visible or measurable now.
- Causal relevance: which phenomena can affect something important.
- Uncertainty: how far the approximate model has diverged from a trustworthy prediction.
- Interactions: whether another phenomenon introduces conditions the simple model cannot handle.
- Computational budget: how much work the runtime can afford.

Example: an unattended fire runs at low fidelity. It becomes detailed when a character enters the building or when its prediction becomes unreliable.

### 7.2 Model composition

Models are typed feature structures, so the runtime can also compose them. A detailed combustion model can be paired with an aggregate smoke model if their coupling contracts are compatible. ChatGPT called the result a constraint-based simulation compiler.

### 7.3 Abstraction and refinement

Each simulation model has two operations.

- Abstraction converts a detailed state into a cheaper representation.
- Refinement converts an abstract state into a more detailed representation that is consistent with everything currently known.

Abstraction is fairly simple. Refinement is much harder, because information that was not retained cannot be recovered. Example: an abstract fire consumed twenty kilograms of wood. The detailed state needs a plausible distribution of charred material, hot spots, smoke, and structural damage.

Refinement is constrained reconstruction. It is not reversal.

1. The abstract model supplies constraints.
2. The detailed model supplies more constraints.
3. Unification determines whether the two sets are compatible.
4. A reconstruction procedure fills the unconstrained details.
5. A deterministic seed makes the choices reproducible.

Unification establishes compatibility only. It does not recover lost history, and it does not guarantee a physically plausible reconstruction. Quadrilateral therefore needs explicit reconstruction policies, conservation checks, and possibly retained latent state. This distinction is fundamental to the language (Proposed).

### 7.4 Uncertainty from abstraction

A coarse model can produce constraints on a future state, not one exact state. A detailed model refines those constraints when the simulation needs more fidelity.

Example: an abstract furnace model predicts a temperature between 600 and 650 degrees after ten minutes. A detailed model narrows the range with information about airflow and fuel distribution.

The prediction of the coarse model must enclose the behavior of the detailed model. This is a numerical-analysis and model-validation problem. Unification does not guarantee it.

### 7.5 Fidelity behind a stable interface

Two phenomena at different fidelity can expose the same interaction contract. Section 9.4 gives the two-room example. Fidelity is an implementation choice behind a stable semantic interface.

## 8. Constraint propagation

### 8.1 Definitions

Three activities are distinct: constraint representation, constraint propagation, and constraint solving. Triangle already does much of the first two.

Constraint propagation uses what is known about some variables to narrow what is possible for other variables.

Example with distance = speed × time:

1. Distance is 100 to 200 m, and speed is 10 to 20 m/s. Time is unknown. Propagation infers a time of 5 to 20 s.
2. Distance becomes exactly 150 m. Time narrows to 7.5 to 15 s.
3. Time becomes exactly 10 s. Speed becomes exactly 15 m/s.

The author never states the direction of evaluation. The author describes the relation, and the propagator narrows each variable that it can.

Propagation is a fixed-point computation. One propagator narrows a variable, and that change activates other propagators. The process stops when no propagator can narrow anything more. A fixed point is not always a unique solution. Example: X + Y = 10 with both between 0 and 10 leaves many possibilities. Selection of one solution needs search, branching, or a stronger solver.

### 8.2 Fit with Triangle

Unification is already a form of propagation. If one structure says 100 to 500 degrees and another says 300 to 700 degrees, unification produces 300 to 500 degrees. In a second example, a furnace description is unified with a description that says 500 to 700 degrees, and the result is the narrower constraint. Information becomes more specific as constraints combine.

Quadrilateral adds bidirectional or multidirectional propagators. Example: heat, fuel consumption, and energy density are related. Any two of them constrain the third, and ranges on all three narrow together.

### 8.3 Propagator protocol

The propagation machinery does not need to understand the implementation of each constraint. It exposes a general protocol (Proposed):

1. A propagator declares the variables it depends on.
2. The propagator examines their current domains.
3. The propagator returns narrower domains.
4. The runtime schedules a propagator when relevant information changes.

This protocol is close to the propagation engines in constraint programming systems.

### 8.4 Propagators inside phenomena

Each phenomenon is a typed feature structure that contains its participants, its state constraints, and a set of propagators (Proposed). Then:

- Unification is the structural substrate.
- Propagators supply computational behavior.
- A scheduler runs propagators until the system is stable, or until simulation time must advance.

The research bet is that constraint propagation can be the fundamental computational mechanism of Quadrilateral. ChatGPT rated this question as much more promising than the choice between BEAM and Go as a compilation target.

### 8.5 Monotonic propagation and change

Propagation is monotonic: the possible values of a variable get narrower and never wider. Classic concurrent constraint programming has the same property. Simulation is not monotonic. Temperature changes, objects move, and fuel disappears.

The design separates information refinement from temporal evolution:

- Inside one simulation state, propagators narrow possibilities.
- Across time, phenomena relate one state to another state, which can have different values.

A temperature that changes from 300 to 400 degrees does not violate monotonic propagation. Temperature at time one and temperature at time two are two different variables. The change is a relation between two states and is not a contradiction inside one state.

When simulated state changes, the runtime must invalidate or replace the old constraints. It cannot continue to narrow the domains of the previous instant. Each propagation context has an explicit validity epoch or an immutable state version (Proposed).

### 8.6 Propagation and evolution

Two operations stay separate:

- Propagation refines knowledge about which values or transfers are possible.
- Evolution computes or commits a change over simulated time.

A refined constraint does not force a numerical simulation to run. This separation keeps new information distinct from physical change.

## 9. Interactions and state ownership

### 9.1 Typed interactions

Phenomena communicate through typed interactions (Decided). The reason is that typed interactions keep the mental model scalable.

- Each phenomenon owns its state.
- Each phenomenon exposes typed ports, such as heat flux, force, or treaty terms.
- The runtime links ports with explicit contracts.
- Reasoning stays local, and the scheduler builds a dependency graph from the contracts.

The question was: if two phenomena both influence the temperature of one object, do both contribute constraints to a shared variable, or does one own the variable while the other communicates through a typed interaction? This choice determines whether Quadrilateral behaves more like a concurrent constraint language, an actor system, or a hybrid of the two.

The proposed shape of an interaction contract is: "this port provides energy flow within these bounds over this interval". In the wall example, the wall needs the temperature on each side and the material properties. The wall promises a bounded heat transfer over the next interval.

Shared variables look simpler, but they quickly become global coupling. Quadrilateral can offer a shared-variable view for convenience. The compiler lowers that view to interactions.

The ownership rule: a phenomenon can propose a transfer, but it cannot directly mutate the state of another phenomenon.

### 9.2 Worked example: heat flow through a wall

Two rooms are separated by a wall. The hot room is at 30 °C and the cold room is at 20 °C. There are three phenomena and two interactions.

- Each room is a thermal-storage phenomenon that owns its internal energy.
- The wall is a heat-transfer phenomenon. It relates the two boundary temperatures to the heat flow.
- Neither room knows how the other room works. The rooms agree only on the meaning of the interaction.

For a steady-state wall, Fourier's law gives the transfer power:

```
P = (k * A / L) * (T_h - T_c)
```

P is power in watts, k is thermal conductivity, A is wall area, L is wall thickness, and T_h and T_c are the boundary temperatures.

| Quantity | Value |
|---|---|
| Effective thermal conductance | 10 W/K |
| Temperature difference | 10 K |
| Transfer power | 100 W |
| Duration | 60 s |
| Energy transferred | 6,000 J |

The table assumes constant boundary temperatures and conductance during the interval. It is an approximation and is not exact for rooms that cool freely.

The wall proposes a transfer of 6,000 J over the interval, and the two endpoints must agree on it. The interaction is a typed, conservative transaction, not a message that says "add 6,000 joules":

- The hot room loses 6,000 J.
- The cold room gains 6,000 J.
- The runtime applies the transfer atomically.
- Both sides agree on the same time interval.

### 9.3 Interaction syntax sketch

The syntax is speculative and borrows the feature-structure style of Triangle.

```text
#EnergyTransfer: #Interaction & {
    source: #ThermalPort
    sink:   #ThermalPort

    interval: #TimeInterval
    quantity: energy

    conserved: true
}

#ConductiveWall: #Phenomenon & {
    ports: {
        hot:  #ThermalPort
        cold: #ThermalPort
    }

    conductance: thermal_conductance

    interaction: #EnergyTransfer & {
        source: .ports.hot
        sink:   .ports.cold
    }

    evaluator: "native.conduction"
}
```

The native conduction evaluator calculates the energy transfer. Quadrilateral understands the types, participants, ownership, and conservation contract.

Port types distinguish temperature observations from energy transfers. Temperature is an intensive quantity, and energy is an extensive quantity. Transferred energy is conserved. Temperature is not conserved.

### 9.4 Propagation and fidelity in the example

Propagation: the hot room reports 28 to 32 °C, and the cold room reports 19 to 21 °C. With a conductance of exactly 10 W/K, the wall's propagator derives a transfer rate of 70 to 130 W. If a later observation narrows the hot room's temperature, the predicted transfer narrows too. The wall does not run its numerical simulation for this.

Fidelity: a room far from the player uses one aggregate temperature and heat capacity. A nearby room uses a detailed three-dimensional temperature field. Both rooms expose the same thermal interaction contract.

- The detailed room supplies a boundary temperature, which can vary across the wall.
- The aggregate room supplies an approximate boundary temperature with an error bound.
- The wall evaluator selects a compatible coupling strategy.
- If the aggregate approximation is not accurate enough, the scheduler requests refinement.

An aggregate room temperature is not always equal to the boundary temperature. The coarse model must justify that approximation or expose a bounded estimate of the boundary temperature.

### 9.5 Interactions and phenomena

Interactions describe exchange. Phenomena describe things that evolve and retain state.

For an instantaneous energy transfer, a relation between ports is sufficient. A wall with thermal mass stores energy and introduces delay. Such a wall must be a stateful phenomenon.

## 10. Time

### 10.1 Continuous semantics, event-driven execution

Time is continuous in the semantics, and execution is discrete and event-driven (Proposed). Time is a first-class quantity over which phenomena express constraints. The simulation does not advance through a universal sequence of ticks. The choice between discrete steps and constraints over intervals affects everything from refinement to scheduling.

The design combines ideas from discrete-event simulation, hybrid dynamical systems, and temporal constraint programming.

ChatGPT named its preference a hybrid temporal constraint model with event-driven execution and adaptive numerical integration. It said this gives more flexibility than fixed ticks or purely interval-based time. Adaptive numerical integration handles the continuous parts, and temporal constraint solving handles scheduling. Their interfaces are unified, so a phenomenon does not depend on which mechanism is in use.

A global tick forces all phenomena into one resolution. A fire can need millisecond integration. A walking person needs updates each fraction of a second. A treaty negotiation proceeds over days.

Each phenomenon declares a prediction: given its state at time T, it can predict its evolution until T plus a horizon, subject to assumptions.

| Phenomenon | Form of the prediction |
|---|---|
| Fire | A numerical trajectory |
| Walking | A predicted arrival time |
| Negotiation | A distribution over possible future events |

The scheduler advances directly to the next time at which something meaningful must happen.

### 10.2 Event windows

Temporal constraints can be intervals, such as "this transition occurs between five and ten seconds from now". Propagators narrow the timing of future events as well as physical quantities.

Example: a low-fidelity furnace predicts that its temperature crosses a critical threshold in 12 to 15 minutes. If another phenomenon changes the airflow, the prediction becomes invalid. The runtime then wakes the furnace model, propagates the new constraints, and calculates a new event window. If nothing disturbs the furnace, the runtime does not execute it.

Interval prediction is particularly useful for variable-fidelity simulation. An abstract phenomenon predicts an interval for its next significant event and does not need constant updates.

### 10.3 Three temporal concepts

Physical time belongs to the semantics of the simulated world. Computational time is the sequence of operations the runtime does. One phenomenon can cover an hour of physical time in one step, and another can need a million steps.

Quadrilateral distinguishes three temporal concepts (Proposed):

- Valid time: when a fact holds in the simulated world.
- Event time: when a transition occurs. Event time can be an interval.
- Evaluation time: when the runtime computes or revises its knowledge of those facts.

Evaluation time supports incremental computation and debugging. The runtime can revise a prediction without a change to the simulated past.

### 10.4 Temporal constraints

Temporal relations propagate backward and forward. Example: A happens before B, B happens within five minutes of A, and B happens before noon. These constraints narrow the possible times of both events.

Simple Temporal Networks represent such relations as difference constraints. Quadrilateral can use that machinery for scheduling, planning, and causal reasoning, without an expensive general-purpose solver.

Physical evolution needs a different mechanism: numerical integration, analytic solutions, or bounded approximations. Temporal constraint solving and numerical integration share interfaces and semantics. They do not share one algorithm.

### 10.5 Representation of evolution

The runtime does not represent each instant of time as a feature-structure node, because the cost is too high. The feature structure of a phenomenon describes a temporal relation or trajectory. Its evaluator supplies operations such as:

- Predict the next event.
- Advance to a requested time.
- Produce observations.

The runtime stores compact representations of evolution, not a collection of world states.

## 11. Concurrency, synchronization, and scheduling

### 11.1 Unit of concurrency

The conversation examined three candidates for the natural unit of concurrency.

| Candidate | Description |
|---|---|
| Actors | Each agent, machine, or institution is an independent process that exchanges messages. Gives isolation, supervision, and an intuitive model of autonomous behavior. BEAM fits well. |
| Processes and phenomena | Independent processes represent combustion, heat transfer, economic exchange, metabolism, or social interaction. They operate on shared conceptual world state through defined interfaces. |
| Reactive constraints | The simulation is typed facts, relations, constraints, and transitions. When something changes, the runtime determines incrementally which computations must run. Concurrency comes from the dependency graph. |

ChatGPT called reactive constraints the most intriguing candidate, and it expected the strongest architecture to combine all three. The project owner selected phenomena as the most interesting answer (Decided).

An autonomous agent can still run as an actor with its own beliefs, goals, and mailbox. The decisions of an agent produce proposals against the simulated world. An agent does not mutate shared state directly.

### 11.2 Concurrency model

The concurrency model has three concepts (Proposed):

- Ownership: each authoritative state variable has one writer.
- Coupling: phenomena communicate through declared inputs, outputs, and transfers.
- Synchronization: the runtime establishes boundaries when interactions need mutually consistent state.

Independent phenomena can run concurrently. Strongly coupled phenomena may need to execute together or use an iterative numerical solver.

Native threads, Go goroutines, or BEAM processes can implement this model. The language does not expose that choice.

### 11.3 Causality across asynchronous phenomena

Phenomena advance asynchronously, so causality needs coordination. Example: the fire has advanced to minute 20, and a person at minute 12 opens a door that changes the oxygen supply.

The design keeps a mechanism like Bruggadung's existing scheduler (Proposed):

- Phenomena declare safe advancement horizons.
- The runtime coordinates phenomena at interaction boundaries.
- Parallel execution is permitted where dependencies permit it.
- The first implementation uses conservative synchronization, not speculative execution and rollback. Conservative synchronization is easier to reason about and keeps the existing commitment model.

The runtime must detect an interaction before it advances past the interaction. Otherwise the runtime commits an invalid future.

### 11.4 Validity contracts

Synchronization requirements are constraints. The language exposes validity contracts and does not expose synchronization barriers (Proposed).

A phenomenon declares when its approximation stays valid. Example: "I can predict this fire for the next ten minutes, if oxygen availability does not change by more than five percent." A second example: the approximation stays valid while external heat input stays within a certain range. A model can also declare a tolerance on the accuracy of its predictions.

If someone opens a door, the airflow phenomenon changes the oxygen conditions. The scheduler detects that the assumptions of the combustion model may not hold, and it invalidates the prediction.

### 11.5 Author control

The principle: authors describe causality, the runtime manages coordination, and advanced users can inspect and constrain the coordination.

The author declares interactions, such as "opening a door changes airflow, and airflow influences combustion". The author does not normally specify that two solvers synchronize at 12.4 seconds. The runtime derives that from the interaction graph.

There are three levels of control:

1. Basic: the author describes phenomena and their interactions. The runtime does the rest.
2. Intermediate: the author specifies tolerances, fidelity requirements, and invariants.
3. Advanced: the author influences scheduling, synchronization, and evaluator selection.

Advanced controls are optional. Ideally, they change only the execution strategy and the approximation guarantees, and not the meaning of the simulation.

The project owner considered explicit exposure of the boundaries complicated, and accepted it as a possible ambition.

### 11.6 Explain facility

Quadrilateral has an equivalent of the database `EXPLAIN` command for simulations (Proposed). A database selects an execution plan for a declarative query, and the user can inspect the plan and give hints.

The explain facility answers questions such as:

- Why does this phenomenon run at high fidelity?
- Which dependencies force synchronization?
- Why can the runtime not advance without an evaluation?
- Why is this approximation valid, and which assumptions and invariants justify it?

The explanations can be typed feature structures.

## 12. Evaluators and kernels

### 12.1 Semantics and algorithm

Quadrilateral owns the semantics of a phenomenon. External code can implement the algorithm that evaluates it.

Kernels are external to Quadrilateral for the first pass (Decided). The project owner also wants everything to be expressible in Quadrilateral eventually.

The principle that covers both: Quadrilateral is semantically self-contained, but it is not necessarily computationally self-hosting (Proposed). The language defines what a simulation means. An implementation can use C, Odin, Rust, GPU kernels, BEAM processes, or later Quadrilateral.

Example: Quadrilateral describes heat transfer through temperature, heat capacity, conductivity, geometry, and conservation of energy. A native kernel implements a finite-volume method. The kernel does not define the meaning of heat, temperature, or conservation.

A native kernel is an implementation of a declared relation.

### 12.2 Kernel contract

A kernel has a declarative contract. The contract declares:

- What the kernel reads.
- What the kernel writes.
- What the kernel conserves.
- The ranges in which the kernel is valid.

The runtime reasons about the dependencies, scheduling, validity, and interactions of a kernel without knowledge of its internal algorithm.

A declaration of conservation does not prove that a native kernel conserves energy. The design needs runtime checks, static verification where feasible, or explicit trust in the implementation.

A kernel written in Quadrilateral and a kernel written in C satisfy the same contract. The runtime treats them as interchangeable. The kernel interface is designed with eventual self-hosting in mind.

The external evaluator interface must be small and independent of the implementation language. ChatGPT named this the most important architectural decision. With such an interface, Quadrilateral can start as a C-based constraint and simulation runtime, experiment with BEAM, and later compile its own numerical kernels, without a redesign of the language.

### 12.3 Evaluator selection

Phenomena declare their participants, invariants, and temporal relations in Quadrilateral. Each phenomenon can have several interchangeable evaluators. Some can be native C or Odin for speed, and others can be written in Quadrilateral later. The runtime selects an evaluator from the declared assumptions and the budget. Propagation keeps the world model coherent.

An evaluator reports its next event, its safe advancement horizon, and its invalidation dependencies.

### 12.4 Path to self-hosting

1. Implement native kernels and a small propagator runtime.
2. Let Quadrilateral define simple propagators in its own constraint language.
3. Compile those propagators into efficient native code.

The language does not need to be Turing-complete at the start to keep this path available.

### 12.5 Summary recommendation

Propagators are the first-class computational abstraction. Phenomena are the first-class simulation abstraction. Native kernels are interchangeable implementations of propagators or of temporal evolution operators.

## 13. Runtime architecture

### 13.1 Components

- Quadrilateral defines temporal relations, invariants, phenomena, and evaluator contracts.
- Propagators refine constraints on values and event times.
- External kernels evaluate continuous or computationally heavy dynamics.
- A scheduler coordinates advancement, dependencies, and fidelity changes.
- A commitment system makes discrete world changes authoritative.

### 13.2 Compilation pipeline

```
Quadrilateral source
  (types, constraints, processes, agents, transitions)
        |
        v
Typed simulation IR
  (dependencies, effects, ownership, scheduling requirements)
        |
        +--> BEAM backend    (agents, coordination, messages, supervision)
        +--> Native backend  (physics, spatial queries, bulk updates, constraints)
        |
        v
Simulation scheduler and transactional world state
```

IR means intermediate representation.

For a declared process such as combustion, the compiler understands the inputs, outputs, units, and dependencies. It can schedule the process only when necessary, group compatible computations, and compile the numerical work into native code.

An earlier diagram in the source, made before the fork decision, has an authoring layer of types, constraints, relations, processes, and probabilities. Below it is a unified semantic representation: a typed intermediate representation, a dependency graph, and effect analysis. Below that are four execution engines:

| Engine | Role |
|---|---|
| Constraint solver | Unification and validation |
| Incremental logic | Relations and derived facts |
| Event engine | Actions and state transitions |
| Numerical kernels | Spatial and physical simulation |

Those engines operate on a persistent simulation world of entities, components, spatial state, and event history. This arrangement keeps a declarative authoring experience while the simulation is optimized for data locality, incremental computation, and sparse updates.

### 13.3 Runtime options

The project owner considered three targets: the BEAM VM, a language like Go with first-class concurrency, or C with a layer that makes it usable from BEAM. The target is Open.

| Runtime | Principal advantage | Main concern |
|---|---|---|
| BEAM | Lightweight processes, supervision, distribution | Numeric and memory-intensive simulation kernels |
| Go | Goroutines, native execution, simple integration | Garbage collection and shared-state coordination |
| C + scheduler | Control of memory, data layout, and performance | Concurrency and supervision must be built |
| Hybrid BEAM + C | Actor orchestration with native simulation kernels | The runtime boundary and state coordination |

ChatGPT's position:

- Do not commit to BEAM, Go, or C first. Give Quadrilateral a language-level concurrency model that different backends can implement.
- Prefer a hybrid architecture, but do not make BEAM processes the authoritative owners of each physical object.
- An agent can run on BEAM while a physical domain executes thousands of updates in a native batch. The two sides interact through typed events, observations, and proposed transitions.
- Physical phenomena have tightly coupled numerical state, shared conservation laws, and high-frequency interactions. For that reason ChatGPT was less inclined to start with BEAM as the central execution model.

### 13.4 Unification at runtime

Triangle's unification machinery stays a first-class runtime facility (Proposed). Agents can exchange partial descriptions, negotiate constraints, and refine their knowledge progressively. This is more expressive than conventional actor message passing.

### 13.5 BEAM integration guidance

- Prototype BEAM integration, but not as the first milestone.
- Erlang processes give isolation, mailboxes, supervision, and distributed coordination. They fit autonomous agents and loosely coupled phenomena.
- Keep high-frequency simulation work in native kernels. BEAM manages coarse-grained orchestration.
- Two interoperation methods exist. An Erlang NIF (native implemented function) gives low-latency native calls. An Erlang port gives process isolation.
- A NIF must not run a long blocking computation. Use ports or dirty schedulers for expensive work.
- Do not convert arbitrary feature structures into BEAM terms on each interaction. That can remove Triangle's performance advantage. Use compact typed handles and exchange deltas.

## 14. Implementation plan

### 14.1 Starting architecture

The starting architecture is Triangle's C core, a propagator engine, typed interaction ports, a conservative discrete-event scheduler, and external numerical kernels (Proposed).

Do not build a new VM, garbage collector, constraint solver, and scheduler at the same time. Assemble a small runtime from established techniques, and keep the option to replace parts later. Start with two or three new dependencies only.

### 14.2 Propagator network

The propagator network is small enough to implement directly, without an embedded constraint-programming framework.

- Each constraint variable has a domain.
- Each propagator has a list of dependencies.
- The runtime keeps a work queue.
- When the domain of a variable narrows, the runtime schedules the propagators that depend on it.
- The runtime runs propagators until the queue is empty or a contradiction appears.

The critical properties are monotonic narrowing, soundness, termination conditions, and no redundant work.

Start with integer intervals, type constraints, equality, addition, and multiplication. Triangle already implements much of this mathematics. The new work is persistent dependency tracking and scheduling.

### 14.3 First experiment: two rooms and a wall

1. Preserve Triangle's existing C engine. Do not change the type hierarchy, unifier, arithmetic, or matcher at first.
2. Introduce phenomenon and interaction schemas. Compile typed ports, ownership, and dependencies into a compact execution graph.
3. Implement a minimal propagator scheduler with a work queue and domain-version tracking. Do not use threads.
4. Add an event-time scheduler. Each evaluator reports its next event, safe advancement horizon, and invalidation dependencies.
5. Integrate one external kernel. Start with a simple analytic heat-transfer model, and compare it against SUNDIALS as the more sophisticated evaluator.
6. Demonstrate fidelity switching between an aggregate room model and a more detailed thermal model. Check energy conservation and approximation error.

The first experiment is single-threaded. This establishes the semantics of interactions and time before scheduling races are possible. Concurrent execution of independent phenomena is a later engineering problem.

### 14.4 Earlier form of the experiment and success criteria

Earlier in the conversation, the experiment was a furnace, a room, and a thermal reservoir, with three representations: detailed spatial heat transfer, aggregate thermal dynamics, and event-based prediction. The runtime switches between them while it preserves energy.

The source states these success criteria for the furnace form. For the two-room form, it names only the checks of energy conservation and approximation error.

- Bounded prediction error.
- Conservation across representation changes.
- Deterministic reconstruction from the same state and seed.
- A measurable reduction in computation with coarser models.

Success shows that a simulation can change its computational representation dynamically and keep a coherent world model.

### 14.5 Initial runtime experiment

Before the phenomenon discussion, ChatGPT's preferred first experiment was a concurrent constraint-and-event runtime built on Triangle's feature structures:

1. Keep the C implementation.
2. Add an intermediate representation for processes and dependencies.
3. Prototype an Erlang-facing runtime, without compilation of the whole language to BEAM bytecode.

The purpose was to learn whether the natural execution model is actors, reactive constraints, or something new.

## 15. Technologies

Some entries are libraries to embed. Others are conceptual models or external tools to study. Do not embed all seven.

| Technology | What it gives Quadrilateral | Priority |
|---|---|---|
| MiniZinc / Gecode | Constraint modeling and solving | High |
| CHR | Rules that propagate constraints | Very high |
| Differential Dataflow | Incremental computation over changing relations | High |
| SUNDIALS | Numerical integration for continuous phenomena | High |
| FMI / FMU | Standard interfaces to external simulation models | Very high |
| Erlang/OTP | Concurrent processes and supervision | Experimental |
| Z3 | SMT solving for validation and difficult constraints | Selective |

Details on the top recommendations:

- FMI 3.0 (Functional Mock-up Interface) is the closest architectural fit. It defines how independently implemented simulation models exchange state and coordinate advancement. It supports model exchange, co-simulation, and scheduled execution through a C interface. Borrow its execution lifecycle, capability negotiation, and communication-point concepts. Implement a smaller, more strongly typed version first, then add FMU interoperability.
- SUNDIALS supplies external numerical kernels for C. CVODE handles ordinary differential equations. IDA handles differential-algebraic equations. ARKODE offers adaptive and multirate integration. Use it for the furnace and heat-transfer prototype, behind a generic evaluator interface.
- Gecode is a C++ toolkit for finite-domain constraint solving with custom propagators, branching, and search. Do not make it the universal solver. Study its propagator scheduling and domain representation. Integration is optional, for difficult combinatorial problems.
- Differential Dataflow is the scalability research target. It updates query results incrementally when records are inserted or removed. Example workloads: which villagers can reach food, which machines have power, which institutions recognize a claim. It is written in Rust, so it adds another runtime boundary.

## 16. Prior art

The project owner's intuition was that typed feature structures express constraints and branching decisions, so they are a good way to write constraint-based programs. The project owner asked whether prior art exists, and asked for a theoretical treatment. ChatGPT confirmed the intuition.

Typed feature structures have a long history as a form of constraint programming. The history comes from computational linguistics, logic programming, and programming-language theory. The new element is their application to deep, concurrent, variable-fidelity simulation.

| Work | Relevance |
|---|---|
| LIFE (Hassan Aït-Kaci and colleagues) | Combined typed feature structures with logic programming. Treated types and feature constraints as fundamental computational concepts. Probably one of the closest ancestors. |
| Oz | Combines logic variables, constraint programming, concurrency, and explicit state. In its dataflow concurrency, a computation waits until the information it needs is available. |
| CHR (Constraint Handling Rules) | Rules that simplify constraints or derive more constraints. A programmable constraint solver. CHR-like rules can define how phenomena interact. |
| Concurrent constraint programming (Vijay Saraswat and others) | Processes communicate through a shared constraint store. A process "tells" the store new information. Another process "asks" and suspends until the information is available. Possibly the most conceptually relevant work, because it is close to a simulation in which phenomena continually contribute information about the world. Largely monotonic: after the store establishes that a variable equals five, it does not later establish that the variable equals six. |
| CLP(X) | Constraint logic programming over a constraint domain X. The pattern for unification that coordinates specialized solvers. |
| Hybrid systems modeling | Systems that combine discrete events with continuous dynamics. This is the combination a Dwarf Fortress-like simulation needs. |
| Simple Temporal Networks | Temporal relations as difference constraints. |
| Differential dataflow | Incremental updates. |
| Erlang | Fault-isolated processes. |
| Modelica | Equation-based modeling and acausal connectors. Describes physical relations without an early choice of evaluation direction. Close to the idea of phenomena described relationally and not procedurally. |

The first research direction is the intersection of LIFE, Oz, concurrent constraint programming, and hybrid systems modeling.

The three most useful influences are LIFE for typed feature structures, Oz for constraint concurrency, and Modelica for declarative physical modeling. Oz and concurrent constraint programming are reading-list items, not libraries to embed. Their treatment of logical variables, suspension, and information flow can directly inform the computational semantics of Quadrilateral.

## 17. Ideas from before the fork decision

ChatGPT proposed these ideas before the project owner defined Quadrilateral. They remain relevant as background.

### 17.1 Five capabilities

This first proposal preceded ChatGPT's reading of the repository.

A conventional content language describes what a creature is. A more ambitious Triangle describes what the creature can do, what it needs, how it interacts with its environment, and which consequences follow.

1. Typed feature structures as the foundation: unify partial descriptions, refine types, express constraints, and compose definitions without inheritance hierarchies. Example domains are creatures, materials, occupations, equipment, and world-generation rules.
2. Relations and logical inference: a Datalog-like layer that derives relations such as reachability, habitability, or task eligibility.
3. Rules for change over time: explicit transitions, preconditions, effects, and event generation.
4. Probabilistic and quantitative semantics: continuous quantities, uncertainty, distributions, and stochastic processes. Temperature, pressure, hunger, fatigue, structural integrity, and creature behavior do not all fit one discrete rule model.
5. Compilation into specialized execution strategies: incremental relational evaluation, event-driven state machines, sparse spatial updates, or optimized numerical loops. ChatGPT said it would put substantial effort here.

Do not implement all five at the same time.

### 17.2 Causal simulation

The world derives behavior from fundamental rules, without procedural code for each case. Example: a hungry dwarf seeks food, finds a blocked passage, and digs another route. The rules are:

- Hunger creates a need.
- Available food can satisfy that need.
- Accessibility determines which food is obtainable.
- Obstacles prevent accessibility.
- Digging capabilities permit removal of some obstacles.
- Agents can select actions that establish the preconditions of their goals.

This combines typed feature structures, Datalog, automated planning, and discrete-event simulation. It can produce rich emergent behavior without hand-authored interactions. The risk is unpredictable performance from unrestricted inference, recursive planning, and arbitrary state transitions. The mitigation is the distinction between pure monotonic deductions and state-changing processes (section 5).

### 17.3 Type system as dependency system

ChatGPT wanted to investigate whether Triangle's type system can also be the dependency system of the simulation. If the compiler knows which facts depend on which other facts, it can build much of the incremental execution graph automatically. ChatGPT said this could be a defining advantage over conventional scripting languages.

### 17.4 Compiled runtime plans for Triangle

After ChatGPT read the repository, it recommended a narrower step for Triangle itself: compile declarative effects and pattern dependencies into specialized runtime plans, without a new rule language.

- A compiler identifies which components and fields an operation reads.
- The compiler generates specialized matchers.
- The runtime invalidates cached results only when those fields change.

The expected benefits are incremental action menus, faster agent reasoning, and larger simulated populations. The three investigations are compiled pattern matching, incremental dependency tracking, and optimization of declarative effects into direct component operations.

### 17.5 Workshop experiment

An early experiment proposal was a workshop with workers, tools, materials, production recipes, and resource needs, implemented entirely in Triangle. It measures three things:

- How naturally the language expresses the simulation.
- How much work is incremental and not recomputed.
- Whether the compiler can turn the hot paths into efficient native code.

The experiment exposes the most important language-design constraints before a commitment to a large runtime.

## 18. Open questions

1. Which runtime does Quadrilateral target: BEAM, Go, C, or a hybrid?
2. Can one typed relational formalism express propagators, phenomena, and kernels? Or does unification of the three lose useful distinctions between logical constraints, numerical evolution, and discrete state transitions? The project owner answered "Yes" to the message that raised this question. That message also proposed the kernel architecture, and ChatGPT's next reply treated the "Yes" as agreement with that architecture. The conversation did not return to this question.
3. Can constraint propagation be the fundamental computational mechanism, with procedural code as an optimization?
4. Is the evolution of a phenomenon a relation between two states, or a constraint over a whole trajectory? ChatGPT suspects trajectory semantics with state-to-state execution.
5. Does an interaction have its own identity and state, or is it purely a relation between ports? The working answer is in section 9.5.
6. How does refinement behave when two asynchronous phenomena at different resolutions suddenly interact? The door-and-fire case is the test.
7. How far does the language expose synchronization boundaries to advanced users?
8. How does the design guarantee that a coarse model's prediction encloses the detailed model's behavior?
9. How does the runtime verify a kernel's conservation claim: runtime checks, static verification, or trust?
10. Which reconstruction policies and retained latent state does refinement need?
11. Does Triangle become a general-purpose simulation language, or does it describe the world while Odin supplies a few general execution mechanisms? ChatGPT asked this before the fork, and said that the second option may give more leverage. The fork decision points to the first answer for Quadrilateral, but the conversation did not answer the question directly.

## 19. Gaps in the source

The export of the source conversation lost several code samples. The surrounding text refers to them, but the code is absent:

- A conceptual authoring example for a combustion process (source section "What Quadrilateral might look like").
- The wood combustion specialization of `#Combustion`.
- A model declaration as a typed feature structure.
- The furnace feature structure and the heat relation, in the constraint-programming discussion.
- A transition example that relates two states.
- The kernel contract pseudocode.
- The propagator network data structures.

The source also contains citation tags for FMI, SUNDIALS, Gecode, and Differential Dataflow. The links did not survive the export.

The project owner asked whether ChatGPT created a document earlier in the conversation. ChatGPT answered that it did not. ChatGPT stated that it then generated a separate file of reconstructed notes (`quadrilateral_conversation_notes.md`). That file is not in this folder.
