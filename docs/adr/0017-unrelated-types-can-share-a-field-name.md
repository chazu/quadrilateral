---
status: accepted
---

# Unrelated types can each have a field with the same name

ADR 0015 section 5 kept a rule of Triangle: a type introduces each feature once. In one package, a field name belonged to one type. If `#Wall` declared `hot`, an unrelated type could not declare `hot`, and the author had to write a common supertype. ADR 0016 made a law a named field, so two unrelated types could also not each have a law named `storage`. The reason in ADR 0015 was that one label could be a quantity in one type and text in another, with no message. The project owner decided on 2026-11-22 that this reason does not justify the rule, and that we reverse it. We decided how.

The design document gives the endpoint. Typed feature structures compose definitions without inheritance hierarchies (design section 17.1). The core semantics follow CUE (ADR 0011 section 1), and in CUE two definitions can each declare a field with one name.

## Decision

### 1. Two types, one name, two fields

```cue
#Wall:  #Phenomenon & { hot: #ThermalPort, cold: #ThermalPort, conductance: %conductance }
#Glass: #Phenomenon & { hot: #ThermalPort, cold: #ThermalPort, tint: text }
#Gate:  #Phenomenon & { state: "open" | "closed" }
#Lamp:  #Phenomenon & { state: "on" | "off" }
```

- The fields have only the name in common. Each type states its own type for its field: `state` is one enumeration in `#Gate` and another in `#Lamp`. `hot` is the same port type in `#Wall` and in `#Glass`, because the author wrote it so.
- The name matters in two cases. A path, a reference, or the slot of a kernel finds a field by its name. A type below both types has one field with that name.
- A structure still cannot have a field that its type does not declare. A wrong label in an instance is an error, as before.
- That type below both types has the unification of the two declarations. If they do not unify, that is an error at that type: "conflict at x: %length vs text".

### 2. The mechanism: a name has a list of types

- A field name is one feature in a package, as before. So a path, a reference, and the slot of a kernel (`hot.temperature`) find the field with that name in every type.
- A feature now has a list of the types that introduce it: each type that declares the field and has no supertype that declares it. A type has the field if it is below one of them. Triangle allows one type in that list.
- The hierarchy gets no new type. A content that loaded before this ADR has the same hierarchy and the same content hash. The one exception is an alternative that got a supertype from its fields (section 4).
- A supertype that declares the field is the one type that introduces it for its subtypes, as before.
- A package still has its own field names (ADR 0015 section 5). A field of a package and a field of the root with one label are two features.

### 3. A struct still states its type

- The rule of ADR 0015 section 5 stays: a struct does not get a type from its fields. The place where the struct stands must have a type with every field of the struct.
- In Triangle, the type of a struct is the meet of the types that introduce its fields. A name that several types have says nothing about the type, so it has no part in that meet.
- Several types can have the fields of a struct. If the place has no type, the message names them: "The types with these fields: #A, #B".

### 4. Alternatives

- A structure that is an alternative of a disjunction declares its own fields. Triangle puts such an alternative below each named type that declares one of its fields. That is a type from fields, and with this ADR several types can have a field with that name. The alternative is now below only the types that it names.
- A struct that stands where the type is a disjunction chooses an alternative by its fields: `form: { side: 1m }` for `form: { x: text, r: %length } | { side: %length }`. Before, this was an error that told the author to write a type with no name. If the fields fit more than one alternative, that is an error.

### 5. The enumeration of a set cell

ADR 0016 section 2 took the enumeration of a set cell from the type that introduces the field. Several types can now introduce it. The enumeration is now the declaration of the most general supertype of the owner that gives the field an enumeration. For a field that one type introduces, the result is the same.

## Considered options

- **Keep the rule of ADR 0015.** The author writes each common supertype. A large content has many of them, only to satisfy the compiler. The project owner rejected it.
- **A feature for each type that declares the label.** `hot` of `#Wall` and `hot` of `#Glass` would be two features. A path, a kernel slot, and a reference would then need a type to find their field. We rejected it.
- **Every type can have every field.** It is the smallest change. But a wrong label in an instance would load with no message. We rejected it.
- **The same name only when the declared types are equal.** One label would have one meaning in a package. But `state` could not be two enumerations in two types, and the author would get the old error again. We rejected it.
- **A generated supertype of the types that have the name.** The compiler would write the supertype that the old error message asked for, and the rest of the type code would not change. The first version of this change did that. But the completion of the hierarchy then adds a meet type for each set of types that two generated types share. We measured it with generated contents. For 100 types and 30 names, each on 10 to 40 types, the load made 2440 meet types. For 40 names, each on 30 to 70 types, the load did not end in 10 minutes. With a list of types for each name, the same content loads in 5 ms and makes no type. We rejected the generated supertype.

## Consequences

- ADR 0015 changes in three places: section 5 (the rule), a considered option, and the consequence about `#Thermal`. A supertype such as `#Thermal` is now a choice of the author, not a need.
- ADR 0016 loses the consequence about two wall types and law names.
- Three places in the type code change. A feature has a list of types. The test that a type has a feature reads that list. A written struct gets its type from its place.
- A test loads 80 types with 40 common names, so a change that makes the hierarchy grow again does not pass.
- One label can have two kinds in two types. A message that names only a path, such as `x.state`, does not say which. The type of the phenomenon is in the IR.
- Content from Triangle that expects an alternative to get a supertype from its fields must name the type: `#Circle & { … }`.
- The old error, "feature … is introduced by both …", is gone.
- The port of the workshop example (`quad-ijx.8`) needs no supertype for a shared field.
