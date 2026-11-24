---
status: proposed
---

# Source states the models of a phenomenon, the maps between them, and reactions

## The problem

The world in C has models since ADR 0010. A phenomenon has several models at different levels of detail. One model is active, and a map kernel moves the state when the model changes. Source cannot state any of this (`quad-ijx.4`). The experiment of phase 5 exists only as C in `tests/test_models.c`.

The author wants to write the room of phase 5 like this. It has one temperature when nobody looks, and four cells in a row when somebody does:

```cue
// one part of a room, with the law of its heat
#Lump: #Thermal & { storage: .energy == .capacity * .temperature }

#CoarseRoom: #Model & #Lump & {
    west: #ThermalPort & { temperature: .temperature, capacity: .capacity, energy: .energy }
}

#FineRoom: #Model & {
    c0: #Lump, c1: #Lump, c2: #Lump, c3: #Lump
    conductance: %conductance
    tolerance:   *1pct | %ratio
    evaluator:   "native.diffusion"
    west: #ThermalPort & { temperature: .c0.temperature, capacity: .c0.capacity, energy: .c0.energy }
}

#DetailRoom: #Phenomenon & {
    west:   #ThermalPort              // each model gives this port
    spread: %temperature              // a field of every model
    model:  *"coarse" | text          // the model at the start

    coarse: #CoarseRoom
    fine:   #FineRoom

    refine:   #Map & { from: "coarse", to: "fine", evaluator: "native.room.refine" }
    abstract: #Map & { from: "fine", to: "coarse", evaluator: "native.room.abstract" }
}

roomB: #DetailRoom & {
    coarse: { temperature: 293.15K, capacity: 10000J/K }
    fine:   { c0: { capacity: 2500J/K }, c1: { capacity: 2500J/K }, c2: { capacity: 2500J/K }, c3: { capacity: 2500J/K },
              conductance: 100W/K }
    spread: 0.5K
}

wall: #Wall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K }
```

The wall connects to `roomB.west`. It does not know which model is behind the port (design section 7.5).

Two smaller parts of stage 4 are also open. An interaction of a phenomenon with a native kernel says nothing that the compiler checks. And ADR 0009 requires a construct for a reaction: a set of flows with fixed ratios and one rate.

## Decision

### 1. A model is a field of a phenomenon with a type below `#Model`

- The prelude has `#Model: { evaluator: text }`.
- The name of the model is the name of the field: `coarse`, `fine`.
- The cells of a model have paths below the model: `roomB.fine.c0.energy`. This is the layout of ADR 0010. A cell directly below the phenomenon, such as `roomB.spread`, belongs to every model.
- A model has its own laws, its own interactions with a rate (ADR 0020), and its own `evaluator`. They work only while the model is active.
- A phenomenon with models has no kernel of its own: an `evaluator` or an interaction with a rate directly below it is an error.
- A model inside a model is an error.

### 2. The phenomenon names the model at the start

- `#Phenomenon` gets the field `model: text`. Its value is the name of the model that is active at the start.
- A phenomenon with models must give it. A type can give it as a default, as in the example.
- ADR 0010 says that the first declared model is active. Source has no order of declarations (ADR 0011), so the name is explicit.

### 3. A model that is not active at the start has no state

- Each stock of such a model is zero. This is the rule of ADR 0010.
- The start pass (ADR 0019) runs the laws of the active model only.
- A value for a stock or for a derived field of such a model is an error. Its other fields, such as a capacity, have their values.

### 4. A port of the phenomenon is the port of the active model

- The phenomenon declares the port with no view: `west: #ThermalPort`.
- Each model declares a port with the same name, as a view of its own state.
- The elaborator maps the port of the phenomenon to the port of each model. The world then resolves `roomB.west` to the active model (ADR 0010 section 2).
- A model that lacks the port, or has it with another type, is an error.

### 5. A map is a field of the phenomenon with a type below `#Map`

- The prelude has `#Map: { from: text, to: text, evaluator: text }`.
- `from` and `to` are names of models of the phenomenon. `evaluator` names a map kernel (ADR 0009 section 5).
- Two maps with the same `from` and `to` are an error.
- The start of a switch stays explicit (ADR 0010 section 4). A new function, `qi_switch(ir, world, "roomB", "fine")`, finds the map from the active model to the named model and calls `qw_switch`. With no such map, it gives an error.

### 6. An interaction with no rate states a transfer of the native kernel

- A phenomenon or a model with an `evaluator` can have interactions with no rate. Each states one transfer that the kernel makes.
- The compiler checks each against the contract of the kernel. The kernel must have a transfer from the cell of `source` to the cell of `sink`, with the same `conserved` flag. If not, the compile gives an error.
- The statement is optional. A kernel can have transfers that no interaction states.

### 7. A reaction is a rate and a set of interactions with ratios

```cue
burn: #Reaction & {
    rate: .burning                                           // kg of wood for each second
    fuel: #Interaction & { source: .wood, ratio: 1, conserved: false }
    heat: #Interaction & { sink: .room.energy, ratio: .heating, conserved: false }   // J for each kg
}
```

- The prelude has `#Reaction: { rate: _, tolerance: *1pct | %ratio }`, and `#Interaction` gets the field `ratio`.
- Each interaction below the reaction is one flow. It has a `ratio` and no rate. Its rate is the ratio times the rate of the reaction.
- The ratio is a number, or a quantity with the dimension of the flow's stock for each unit of the reaction. It is a value or a path to a field.
- The kernel `native.rate` computes the amount of the reaction for a step once, and then each flow from its ratio. So the flows keep their ratios, and each transfer is exact (ADR 0009).
- The tolerance of the reaction sets the step, as for an interaction (ADR 0020 section 5).

### 8. The IR

- A phenomenon lists its models and names the active one. Each cell, propagator and kernel names its model, or none.
- A port lists its target for each model.
- A phenomenon lists its maps: the two models and the kernel.
- A transfer of `native.rate` can name the cell of a ratio.
- `qi_instantiate` builds the models with the calls of ADR 0010: `qw_model`, `qw_model_begin`, `qw_port` and `qw_model_attach`.

## Acceptance tests

1. The world of phase 5 from source has the same state hash as the world from C, at each time and after each switch of `tests/test_models.c`.
2. A switch from source conserves the total energy exactly, and the same seed gives the same detail.
3. An interaction that states a transfer that the kernel does not make gives a compile error.
4. A reaction that burns wood moves the energy and the mass in the stated ratio, at each time of a run.

## Considered options

- **The fields of ADR 0011 section 5 for a model: its conditions, the information that it keeps, and its cost.** Not now. Nothing reads them until automatic selection (`quad-ft5.3`), so they would have no test and no meaning. They come with that work. This changes ADR 0011 section 5, which wanted the fields from the start.
- **The first model in source is active at the start.** Rejected. Source has no order.
- **A model as a separate declaration, outside its phenomenon.** Rejected. The paths of ADR 0010 put the cells of a model below its phenomenon, and a field gives that path with no new rule.
- **The port of the phenomenon follows from the ports of the models, with no declaration.** Rejected. A reference to `roomB.west` needs a field with that name in the type of `roomB`.
- **A reaction as separate interactions with laws for the ratios.** This works today. Rejected as the only form. Each interaction then has its own step and its own kept amount, and the author keeps the flows consistent by hand. ADR 0009 requires the construct for that reason.
- **Each transfer of a native kernel must have an interaction.** Rejected for now. Each existing content would need the statements. A later check can require them.

## Consequences

- The room of phase 5 needs one field for each cell: `c0`, `c1`, `c2`, `c3`. The fix is a comprehension. Time: `quad-ijx.9`.
- A switch needs a map kernel in C. The fix is a map in source: a set of transfers that happen at once, at a boundary. Time: a new issue, after this work.
- Nothing starts a switch but the host. The fix is automatic selection. Time: `quad-ft5.3`.
- ADR 0020 named this stage for a rate law with a condition, such as "a constant flow until the tank is empty". That was wrong. The fix is a phenomenon that exists only while a condition holds (ADR 0013). Time: `quad-ijx.7`.
- The acceptance test of `quad-ijx.4` (phase 5 in source) is test 1 here.
