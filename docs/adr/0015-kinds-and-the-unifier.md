---
status: accepted
---

# A kind is a quantity type with the prefix `%`, a literal converts when it meets a kind, and an operation keeps its scratch in its own context

Stage 2 of the front end (ADR 0011 section 3) builds the type hierarchy and unifies each declaration. The code comes from Triangle's `types.c`, `fs.c`, `qc.c`, and parts of `load.c` (bruggadung `f425574`, under ADR 0003). Three things in Triangle do not fit. Triangle has a fixed table of dimensions with one scale each, but in Quadrilateral source declares the kinds, and the kind of a field decides the scale (ADR 0001). Triangle keeps the scratch of an operation in the nodes and in the loaded content, so only one operation can run on a content at a time (ADR 0007). Triangle's API takes descriptions as text. We decided how source declares a kind, how a number gets its kind, where an operation keeps its scratch, and which parts of Triangle's loader stage 2 takes.

## Decision

### 1. A kind has the prefix `%`

```cue
%energy:   { dimension: "J", scale: -3, conserved: true }
%capacity: { dimension: "J/K" }
%coin:     { base: "coin", scale: -2 }

#LumpedRoom: #Phenomenon & {
    energy:   %energy
    capacity: %capacity & 1000J/K
    rate:     %energy / %time
}
```

- A declaration with a `%` label declares a kind. The label tells the parser what the declaration is, as `#Name` does for a type and `"name"` does for an atom.
- The body is literal data, and the resolver reads it before it builds the type hierarchy. The fields are `dimension` (text in the grammar of ADR 0009 section 10), `scale` (an integer, optional), `conserved` (`true` or `false`, optional), and `rounding` (optional, ADR 0001 section 4). A kind without a scale gets the default scale of its dimension (ADR 0001 section 1). Another field, a reference, or a unification in the body is an error.
- A kind has one declaration. A second declaration of the same kind is an error that names the first.
- `base: "coin"` in place of `dimension` declares a new base dimension of the world (ADR 0001 section 1) with the unit symbol `coin`. The kind has that dimension. Other kinds can then write `dimension: "coin/kg"`, and a literal can write `5coin`. The base dimensions get their indices in the order of their symbols, so the order of declarations does not change them.
- `%energy` as a value means any quantity of the kind. `pkg.%energy` names a kind of another package.
- Each SI base dimension has a built-in kind with the default scale of ADR 0001: `%length`, `%mass`, `%time`, `%current`, `%temperature`, `%amount`, and `%luminosity`. None is conserved.
- The angle is a built-in base dimension. Its coherent unit is the degree (`deg`), so that degrees are exact (ADR 0001), and a turn (`turn`) is 360 degrees. There is no radian. Its built-in kind is `%angle`.
- A content can declare a built-in base kind once: `%length: { scale: -9 }` or `%mass: { conserved: true }`. The declaration changes the scale, `conserved`, or the rounding, and it keeps the dimension. A second declaration is an error, as for every kind.
- `%time` has the scale of the world's tick, and a declaration cannot change it. `%count` (dimensionless, scale 0) and `%ratio` (dimensionless, scale -9) are also built in and fixed.
- A package can declare its own kind with a built-in name. In every other package, the plain name is the built-in kind.
- `number`, `text`, and `bool` are built-in bare names, and they are reserved. `number` is any number. They are not kinds, because they have no dimension and no scale.
- The character `%` has no other use. There is no modulo operator and no `50%` literal.

### 2. Numbers in the type hierarchy

- Below `number` there is one **dimension type** for each dimension that the content uses. The resolver generates it. Below a dimension type are the kinds of that dimension. A kind is a leaf.
- Two different kinds have no common subtype, so they never unify (ADR 0001 section 1). A kind unifies with its dimension type and with `number`, and the result is the kind.
- A product or a quotient of kinds, such as `%energy / %time`, is a dimension type. This is the anonymous intermediate kind of ADR 0001.
- A node of a kind holds a closed interval of int64 quanta in the scale of the kind, with the infinities of ADR 0001 section 2.
- A node of a dimension type or of `number` holds a closed interval of exact decimal values in the coherent unit: integers with one power of ten. It has no scale yet.

### 3. A literal converts when it meets a kind

- A literal gives a node of a dimension type. The unit gives the dimension and an exact factor: `5000J` is 5000 in the dimension of the joule, and `3min` is 180 in the dimension of the second. A literal without a unit is dimensionless.
- When unification gives a kind to a node that has a decimal value, the value converts to quanta of the kind. So `%energy & 5000J`, `5000J & %energy`, and a literal in an instance whose type declares the kind all give the same result.
- A point value that is not a whole number of quanta makes the unification fail, and the failure names the value and the quantum (ADR 0001 section 4). Example: `0.15mm` for a kind with a quantum of 0.1 mm.
- A bound that is not a whole number of quanta moves inward to the next quantum. This removes no value of the kind: for a quantum of 0.1 mm, `>=0.15mm` and `>=0.2mm` allow the same values.
- A value that does not fit in the finite range of the kind makes the unification fail (ADR 0001 section 5).
- A node can end the load with only a dimension type. That is valid for a structure. The first version of this ADR said that the elaborator reports a value with no kind. Since ADR 0018, such a value is a cell with the default scale of its dimension.

### 4. Units

- One table gives each unit symbol its dimension and its exact factor. The table has the symbols of `q_parse_dim` (m, kg, s, A, K, mol, cd, N, J, W, Pa, Hz, and for electricity C, V, ohm, F, H, S, T, Wb), the gram, the SI prefixes from pico to tera, and min, h, d, and L. It has `deg` and `turn` for the angle. The symbols of declared base dimensions are added to it.
- Two symbols have no dimension: `pct` is 10^-2 and `ppm` is 10^-6. So `50pct` is 0.5.
- A kind that is not a base dimension has no unit symbol of its own, such as `gp` for a count of gold pieces. That waits for a need.
- A symbol is first looked up as a whole (`min`, `mol`, `cd`), and then as a prefix with a symbol (`mm`, `kJ`).
- A compound unit follows ADR 0014 section 2, and its factor is the product of the factors of its terms. A factor that is not an exact decimal, such as `min^-1`, is an error. The author writes the value in seconds.
- Every term after the first `/` divides, and each divisor has its own `/`: `J/kg/K`. A `*` after a `/` is an error, because `J/kg*K` has two readings: (J/kg)·K to a programmer, and J/(kg·K) in the dimension grammar of ADR 0009 section 10.
- Stage 2 makes the checks that ADR 0014 gave to it: an unknown unit, a let or a variable of a comprehension with the name of a unit symbol, and the hints for `10m/x` and `0.1/s`.

### 5. Structures, types, and equations

- The type hierarchy, its completion with generated meet types, features, enums, disjunction types, typed lists, and packages work as in Triangle. A structure cannot have a feature that its type does not allow. The first version of this ADR also kept Triangle's rule that one type introduces each feature. ADR 0017 reversed that rule: unrelated types can each have a field with the same name.
- The constraint of a type is its expansion: one structure, built once. An instance has one structure, which is the unification of its declarations and of the types that it names.
- A struct states its type. In Triangle, a struct gets the type that introduces its fields, so `head: { size: .size }` silently becomes a full `#Tool`, with the laws and the stocks of that type. In Quadrilateral, the place where a struct stands must already have that type: the declared type of its field, the element type of its list, or a type or an instance that the author names with `&`. If the place has no type, or a less specific type, the struct is an error, and the message gives the type to write: `#Tool & { … }`. The order of the conjuncts and of the declarations does not count.
- A reference is a path that the author wrote, so it can still go through a field with no type.
- An arithmetic expression or a comparison is not evaluated. It gives a **constraint record**: the expression, the path where it stands, and the path of each reference in it. The records stay with the structure, and they move with it when another declaration includes it. Stage 3 makes propagators from them (ADR 0011 section 4).
- A comparison that stands as a value, as in `laws: [ .energy == .capacity * .temperature ]`, gives a node of the built-in type `constraint` that refers to its record.

### 6. What stage 2 takes from Triangle

- Taken: `types.c`, `fs.c` (nodes, unification, copy, subsumption, generalization, the text form), `qc.c`, and from `load.c` the build of a syntax tree into a structure, type expansions, instance structures, the resolution of names across packages, and the content hash.
- Later, each in its own issue: comprehensions and built-in functions (`quad-ijx.9`), defaults and the variants of an instance (`quad-ijx.10`), and text constraints and interpolation (`quad-ijx.11`). Until then, each of these in source is an error that names the missing stage.
- Not taken: Triangle's arithmetic settling (the propagator network does this work), `@vary` and random construction, derived fields, families, the TDL front end, component generation, and the fixed dimension table.
- Matching with three results, and explanations of a match, are for stage 6.
- Since ADR 0017, a structure that is an alternative of a disjunction is below only the types that it names. Triangle also puts it below each type that declares one of its fields.

### 7. An operation keeps its scratch in its own context

- After load, the content does not change. Every type expansion is built during load, not on first use.
- An operation (unification, copy, subsumption, generalization) runs in a context. The context owns the scratch arena, the forwarding and copy marks, the path stack, the failure record, and the caches that an operation fills, such as the cache of meets.
- A node does not hold scratch. A node has an index in its store, and the context has one table of scratch for each store that the operation reads. The content is one store. A structure that an operation gives to its caller is in a store that the caller names.
- So two contexts can work on the same content at the same time, and they need no lock. Stage 2 does not start threads. It only keeps this property.
- A generation counter is local to a context.

### 8. The API uses handles

- The API is internal to the front end, in `src/front/`. The public product of the front end is still the IR (ADR 0011 section 3).
- A caller refers to a type, a feature, a kind, and a node by a handle, and to a place by a path of features. No call takes a description as text or parses a path from text.
- A builder makes a structure from handles: a node of a type, an arc, a number of a kind, an atom, a text value, and a shared node. Stage 6 uses it to build a description from cells (ADR 0007).
- A structure has a text form, for tests and tools.

### 9. The result does not depend on order

- The result of a load is the kinds, the type hierarchy, the features, the expansion of each type, the structure of each instance, and the constraint records. The content hash covers all of them.
- The result is the same for every order of the files and for every order of the declarations in a file. A property test loads shuffled copies of each test content and compares the hash and the text form.
- A content with an error fails in every order. The diagnostics are sorted by location.

## Considered options

- **A struct gets its type from its fields, as in Triangle.** It is short to write. But the type comes with constraints that the author did not write, and two of the test contents of stage 2 had a wrong type that nobody saw. We rejected it.
- **A generated supertype when two types declare one field.** The author would not need a type such as `#Thermal`. But one label could then be a quantity in one type and text in another, with no message. We rejected it at first. ADR 0017 then removed the need: unrelated types can each have a field with the same name, and the compiler generates no supertype.
- **A bare name for a kind, as in the sketch of ADR 0011: `energy: #Kind & {…}`.** It reads well and matches the design document. But the declaration looks like an instance and makes a type, the resolver must find kinds before the hierarchy exists, and a kind named `gold` takes that name from instances. We rejected it.
- **A `#` type for a kind: `#Energy: #Kind & {…}`.** Then `x: #Energy` means a structure with a `dimension` field, but the value is a number. A kind declaration is data about a type, not a supertype. We rejected it.
- **Exact decimals in every structure, and conversion to quanta in the elaborator.** The unifier would need no kinds. But ADR 0011 section 1 puts the conversion in the unifier, two kinds of one dimension would unify, and an inexact literal would be found late and far from its source. We rejected it.
- **The kind from the context while a structure is built, as Triangle resolves a label against the type on its left.** The result would depend on the order of conjuncts. We rejected it.
- **Scratch pointers in the nodes, as in Triangle.** The port is smaller, and an access to scratch has one indirection less. But every operation writes to the nodes of the content, so all operations on one content run one at a time, for the life of the design. ADR 0007 asks for the opposite. We rejected it.
- **All of Triangle's load semantics in one stage.** Stage 3 needs only the core. We split the rest into three issues.

## Consequences

- The parser gets a token for `%name` and a fourth class of declaration. ADR 0014 section 1 gains kinds in its list.
- The sketch in ADR 0011 section 2 changes: a kind is `%energy: {…}`, a field is `energy: %energy`, and `#Kind` leaves the list of prelude types. I edit ADR 0011 when this ADR is accepted.
- The port of the workshop example (`quad-ijx.8`) declares its kinds and writes `%mass` where Triangle writes `mass`.
- An access to scratch goes through a table of the context. The scale benchmark (`quad-5xe.2`) or a later profile shows if this costs too much.
- A temperature in degrees Celsius has an offset, and it is ambiguous for a temperature difference. Stage 2 has no unit with an offset. Source writes kelvin.
- A unit with a factor that is not an exact decimal is rejected, also when the value of the literal is exact. Example: `3min^-1` is an error, although it is 0.05 in the dimension of the hertz. The rule is simple to state: a minute cannot be a divisor.
- Constant arithmetic such as `2 * 500J/K` is a constraint record like every other expression. The world computes the value when it settles.
- The sketch of ADR 0011 declared `temperature`, `capacity`, and `energy` in the port and also in the room. Under the first version of this ADR that was an error, so one supertype, `#Thermal`, declares them in the sketch. Since ADR 0017 the supertype is a choice of the author.
- The text of a dimension in a kind declaration and in a kernel contract still follows ADR 0009 section 10, where `"J/kg K"` is J/(kg·K). Only the unit of a literal rejects the form with two readings.
- A world can declare 24 base dimensions, because the angle takes one of the 25.
- A disjunction of numbers, such as `1m | 2m`, is an error in stage 2. Triangle accepts one that has no gaps.
- `qf_build_text` interns its string in the front end, so only one thread at a time can call it. All other operations of a context are safe next to other contexts. Stage 6 must remove this limit, because it builds descriptions with text from cells.
- Stage 2 cannot load the whole workshop example. The three later issues close that gap before `quad-ijx.8`.
