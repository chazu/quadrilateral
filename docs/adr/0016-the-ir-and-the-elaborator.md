---
status: accepted
---

# A quantity is a cell, a law is a named field, a reference to a port is a connection, and the IR is plain data

Stage 3 of the front end (ADR 0011 section 3) reads the result of a load and makes the IR. A second step builds a world from the IR through the C API. The acceptance test is the two-room world of phase 4. In source, it must give the same state hash at 1000 s as the world of the C tests. The state hash covers the path of every cell (ADR 0013), so source must give exactly the eight cells of the C build. Four things were open. Which parts of a structure are cells? How does source connect two phenomena, when the resolver copies an instance that another instance names? How does a type state a law, so that a subtype can add one? What is in the IR? We decided the rules below.

The design document gives the endpoint. Each phenomenon owns its state, exposes typed ports, and the runtime links ports with explicit contracts (design section 9.1). The compiler knows which facts depend on which other facts (design section 17.3). The IR has dependencies, effects, ownership and scheduling requirements, and another backend can read it (design section 13.2).

## Decision

### 1. A phenomenon is an instance of a type below `#Phenomenon`

- Each instance whose type is below `#Phenomenon` is a phenomenon. Its name is the first component of the paths of its cells.
- An instance of another type is data. It is not in the IR. A phenomenon that names it gets a copy, and the numbers of the copy are cells of the phenomenon.
- The prelude gains two types: `#Phenomenon: { evaluator: text }` and `#Port: {}`. The other prelude types of ADR 0011 section 2 come with their stages.
- These types are in a package of the prelude, `quad`. So the labels of the prelude, such as `evaluator`, stay free: a type of the content can declare a field with the same label. A content cannot declare in `quad`.
- The resolver looks for a plain name in this order: its own package, the root, the prelude, and then the one package that declares it. So `#Phenomenon` is the prelude's type in every package that does not declare its own, and `quad.#Phenomenon` is its full name.
- A phenomenon inside a phenomenon is an error in this stage. Composition (design section 6.4) is later work.

### 2. Cells

- A number of a kind in a phenomenon is an interval cell. Its path is the name of the phenomenon and the path of the field: `roomA.energy`. Its base is the value of the structure, in quanta of the kind.
- A field of atoms is a set cell (ADR 0012). Its enumeration is every atom that the field can have by its declaration, in the order of the names. The declaration is that of the most general supertype that gives the field an enumeration (ADR 0017 section 5). So the field has the same enumeration in every phenomenon of that type. `bool` is the boolean kind of the world.
- A path has the labels of the fields, without their packages. Two packages can give one phenomenon two fields with one label. Then two cells would have one path, and that is an error.
- A cell of a conserved kind is a stock. Its first value must be a point, because only transfers change a stock.
- A number with no kind is an error (ADR 0015 section 3). The message shows the form `power: %energy`.
- A text and a constraint are not state. They give no cell.
- One value can stand at several paths, because a reference shares one node (ADR 0011 section 1). The value is one cell. The cell has the shortest path outside a port, and each other path is an **alias**.

### 3. Ports and connections

- A port is a field of a type below `#Port`. A port has one of three states.
- **A view.** Each field of the port is a field of the phenomenon: `west: #ThermalPort & { temperature: .temperature, … }`. The fields of the port are aliases: `roomA.west.energy` is `roomA.energy`.
- **Connected.** The field is a reference to a port of another phenomenon: `hot: roomA.west`. The port has no cells of its own. A path through it reaches the cells of the other phenomenon.
- **Open.** A field of the port stands only in the port, and no reference connects the port. The field has no cell. An open port is an error only when a law or a kernel needs it. Then the message names the port.
- The resolver copies an instance that another instance names, and the copy has no link to its source. So the resolver now keeps a **reference record** for each copy: the path where the copy stands, the instance, and the path of the part. A type that names an instance gives the record to its instances. The records are in the text form of the load, so the content hash covers them.
- A phenomenon reaches another phenomenon only through a port (design section 9.1). A reference from a phenomenon to another phenomenon, or to a field of it that is not a port, is an error.
- A connection is a field of the phenomenon, not a field of a nested structure. The world matches a slot of a kernel by the name of the port.
- A connection names a view. A connection to a connected port or to an open port is an error.
- A connection cannot change the port: `hot: roomA.west & { temperature: >=280K }` is an error in this stage. A requirement on the other side is an applicability condition, and ADR 0010 and stage 4 own those.
- The world gains `qw_alias(w, "roomA.west.energy", "roomA.energy")`: a second path for a cell of the same phenomenon. Then `qw_connect(w, wall, "hot", "roomA.west")` works with no other change. An alias is a name, not state, so the state hash does not cover it.

### 4. Laws

- A law is a field with a name: `storage: .energy == .capacity * .temperature`. A subtype adds a law with a new field, so unification specializes a phenomenon with more laws (design section 6.2). Two declarations of one law must agree, as for every field.
- The prelude has no `laws` list. A list has a fixed length, so a subtype that writes `laws: [ b ]` conflicts with a supertype that writes `laws: [ a ]`.
- Each constraint record of a phenomenon is a law (ADR 0015 section 5). A record is a comparison at a field, a comparison in a list that a type declares, or an expression at a field.
- The IR has the name of each law, which is its path: `roomA.storage`. A message and, later, EXPLAIN (`quad-5xe.3`) can name the law.
- This stage lowers a law of the form `a == b` or `a == b op c`. Here `a`, `b` and `c` are references, and `op` is `+`, `-`, `*` or `/`. The law gives one propagator: equality, sum, difference, product or quotient. For an expression at a field, the field is the result.
- A reference can go through a connected port. Then the propagator names a cell of the other phenomenon. It can only narrow that cell, so ownership holds: a narrowing is a deduction, not a write.
- The elaborator checks the kinds of a law with the rules of ADR 0001, so the error has the place of the law in source.
- A nested expression, a literal operand, and an inequality are errors that name `quad-ijx.12`. They need an intermediate cell with an anonymous kind, a constant cell, and an inequality propagator. Until then, the author gives an intermediate value a field with a kind.
- A record that stands under a connection is a law of the other phenomenon. The elaborator makes it once, for its owner.

### 5. Kernels

- `evaluator: "native.conduction"` names the kernel of a phenomenon, as in design section 9.3. The caller gives the kernels that source can name (`QiOptions`). The native kernels that need no parameter are always known.
- The elaborator binds each slot of the contract to a cell: the slot `hot.temperature` of the wall is the cell `roomA.temperature`. A slot with no cell is an error at compile time.
- The transfers of the IR come from the contract of the kernel in this stage. Stage 4 adds interactions in source and checks them against the contract.
- The world checks the rest of the contract (dimensions, roles, ownership) when it attaches the kernel, as it does for C callers.

### 6. The IR

- The IR is plain data in a public header, `include/quad/ir.h`. It has these parts:
  - the tick, the base dimensions, and the kinds and the enumerations that cells use
  - the phenomena, the cells with their owners, the aliases, and the ports
  - the propagators, and the kernels with the cell of each slot and their transfers
  - the dependency graph
- The dependency graph has one edge for each use of a cell. A propagator narrows a cell. A kernel reads it, writes it, or transfers a stock out of it or into it. This is the first form of the "type system as the dependency system" of design section 17.3. The scheduler does not read it yet.
- Steps in an epoch and steps at a boundary are separate from the start (ADR 0008, `quadrilateral-review-fable.md` section 3.1). A propagator is monotonic. A kernel acts at a boundary.
- Everything has a defined order. Phenomena, kinds and enumerations are in the order of their names. Cells are in the order of their paths, and propagators in the order of their laws' names. The order does not depend on the order of the files or of the declarations. A property test compiles shuffled copies of a content and compares the text.
- The IR has a text form, `qi_text`, for a person and for tests. The text has no places in source, so two contents with the same meaning give the same text. The data keeps the place of each law.
- The IR owns its memory. It lives after the front end that made it.

### 7. A world from the IR

- `qi_instantiate` builds a world with the calls of `quad/world.h`, in the order of the IR. So two equal IRs give two equal worlds. The seed is an argument, not a part of the IR.
- The world takes names of letters, digits and `_`. A kind such as `shop.%coin` gets the name `shop_coin` in the world, and a number makes a name different from an earlier one. Only messages show these names.

## Considered options

- **A link in the structure: `hot: roomA.west` shares the nodes of `roomA`.** It matches "references share in both directions". But an instance is one structure (ADR 0015 section 6). A node that two instances share makes every operation on one of them touch the other. We rejected it. A reference record keeps the fact without a change to structures.
- **Find connections in the syntax tree.** The elaborator would read `hot: roomA.west` from the declaration. It misses a connection that a type states, and it repeats the name resolution of the resolver. We rejected it.
- **A connection to the phenomenon, with no port: the world call of the C tests, `qw_connect(w, wall, "hot", "roomA")`.** It needs no alias. But then a port is only a prefix of paths. A room cannot expose a boundary temperature that is not its own temperature (design section 9.4). We rejected it.
- **A cell for each path.** `roomA.west.energy` and `roomA.energy` would be two cells with an equality between them. A stock would exist two times, and the state hash would differ from the C build. We rejected it.
- **Intermediate cells in this stage.** The wall law of the stage 2 fixture has a nested expression. An intermediate cell needs decisions about its path, its kind in the world, and the state hash. The acceptance test does not need it. We made it an issue of its own (`quad-ijx.12`).
- **An attribute for the kernel: `@evaluator("native.conduction")`.** The design document and ADR 0011 write a field. A field can be set by a type and read with the structure. We kept the field.
- **A text IR with a reader.** A backend in another language would need it. The C data is enough for the C world, and the text form is stable enough to become that format later. We wait for the second backend.
- **The prelude in the root package.** The first version had it there. Then `evaluator` and each later label of the prelude (`source`, `sink`, `cost`) belong to a prelude type, and no type of the content can declare them. We rejected it.
- **A `laws` list in `#Phenomenon`, as in the sketches of the design document and of ADR 0011.** The first version had it. A subtype cannot add a law to it. We rejected it.
- **A struct of laws, `laws: { storage: … }`.** A struct states its type (ADR 0015 section 5), so each phenomenon type would need a type for its laws. A field of the phenomenon needs nothing more. We rejected it.

## Consequences

- The first version of this ADR met the rule that a type introduces each feature once (ADR 0015 section 5). Two wall types could not each declare `hot` and `cold`, and two types could not each have a law named `storage`. ADR 0017 reversed the rule. The test content still has `#Wall`, and `#ConductiveWall: #Wall & { evaluator: "native.conduction" }`, as a choice.
- A type that sets `evaluator` fixes the kernel for its subtypes, because two different texts do not unify. Defaults (`quad-ijx.10`) remove this limit, and stage 4 moves evaluators to models (design section 12.3).
- The sketch of ADR 0011 section 2 changes: a law is a named field, and the prelude is in the package `quad`.
- The content hash of stage 2 changes, because the text form of a load now has reference records.
- The stage 2 fixture declares `#Phenomenon` and `#Port` in the root, with a `laws` list. The prelude adds to a type that the root declares, so the fixture loads as before. Its wall law has a nested expression, so the fixture does not elaborate until `quad-ijx.12`.
- A kernel that needs a parameter, such as `native.diffusion` for n cells, has no name in source yet. Stage 4 needs it for the phase 5 experiment.
- A phenomenon in a package works when one package has its name. Two phenomena with the same plain name in two packages are an error, because the world has one name space.
- An atom in a set cell needs a name of letters, digits and `_`. An atom such as `"two words"` is an error where it becomes a cell.
- A kind with a rounding other than half to even compiles, and the world rejects it, because the world has no other rounding yet.
- The elaborator walks a shared structure once for each path to it. A content with much sharing can make this slow. The scale benchmark (`quad-5xe.2`) can measure it.
- `qi_compile` stops the process if memory runs out, as the front end does (ADR 0014). `qi_instantiate` returns `QW_ENOMEM` from the world.
