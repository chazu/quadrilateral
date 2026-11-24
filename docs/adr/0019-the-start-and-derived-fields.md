---
status: accepted
---

# A field that the laws compute can have a start value

## The problem

A room stores energy. Its temperature follows from the energy, by the law `.energy == .capacity * .temperature`. A heat flow changes the energy, and the temperature changes with it.

An author knows the temperature of a room, not its energy. But today the author must write the energy:

```cue
living: #Room & { energy: 3031500J, capacity: 10000J/K }
```

The author found 3031500 J by hand, from 303.15 K and 10000 J/K. The author wants to write this:

```cue
living: #Room & { temperature: 303.15K, capacity: 10000J/K }
```

Today that is an error. The compile says that the energy needs an exact value (ADR 0016 section 2). Each example with heat has this problem (`quad-rf3`).

Two things stop the second form.

1. The compiler does not compute the energy from the temperature, although the law gives exactly one value.
2. A value in source holds at every time. So `temperature: 303.15K` says that the room stays at 303.15 K. The first heat flow changes the energy, and the law then gives a different temperature. The two contradict, and the world stops.

The first is easy to fix. The second is the reason for this ADR. The language has no way to say "303.15 K at the start, and then the value that the law gives".

The problem is not special to heat. A capacitor has a charge and a voltage. A tank has an amount of gas and a pressure. A reservoir has a volume and a level. In each pair, the first quantity has a conserved kind, so it is the state. A person measures the second.

The project already has a term for a value that the laws compute. ADR 0018 calls an inner value of a law a **derived cell**: it is not state, and only propagation gives it a value. The temperature of a room has the same properties. The design document separates the state of a phenomenon from such fields (design section 6.2).

## Decision

### 1. A type marks a field that the laws compute

- The mark is the attribute `@derived`:

```cue
#Room: #Phenomenon & {
    temperature: %temperature @derived
    capacity:    %capacity
    energy:      %energy
    storage:     .energy == .capacity * .temperature
}
```

- A **derived field** is a field that the laws compute from the state, again in each epoch. Here the energy is the state, and the temperature is a derived field.
- A field of a conserved kind cannot be a derived field. A stock is state.
- A wrong name, such as `@derivd`, is an error. The front end knows the names of its attributes.
- The field can be a field of an inner structure, such as a port. The type of that structure gives the mark, or the type around it does: `gauge: #Gauge & { level: >=0m @derived }`.
- The mark in an instance is an error. A mark on a field that is not a quantity is an error too.

### 2. A value for a derived field in an instance is the value at the start

- `living: #Room & { temperature: 303.15K }` says that the room is at 303.15 K when the run starts. After the start, the law gives the temperature.
- This agrees with what an instance value of a stock already means. `energy: 3031500J` is the energy at the start, and transfers change it.
- A value or a bound for a derived field in a type holds at every time, as for any field. `temperature: %temperature & >=0K` in `#Room` says that no room is below 0 K at any time.
- A field with no mark does not change. Its value holds at every time. A capacity is such a field.

### 3. The compiler computes the stock

- The elaborator gives each cell its value from the instance, with the start values of the derived fields. It then runs the propagators of the laws once.
- A stock with no exact value takes the value that the laws give. `temperature: 303.15K` and `capacity: 10000J/K` give `energy: 3031500J`.
- The IR has the result: each stock has its exact value. A backend does not compute the start again.
- An author can give the stock, the derived field, or both. If both are there, they must agree.
- If the laws give a range for a stock, that is an error. The message shows the range, and it tells the author which value to add.
- If the values of an instance contradict a law, that is an error at compile time. The message names the law and shows its values. This is true for each law, also where no stock and no derived field is a part of it.
- A stock has a smallest step, such as one millijoule. If the computed value is between two steps, the stock takes the lower step. The start value of the derived field then holds to within one step.
- The laws round outward. So an inner quotient can give an exact value as a range of two steps, such as 999 mJ to 1001 mJ. The stock then takes the middle step.
- A start value must have an effect. The world does not keep it, so the laws must give the derived field a value again from the state. If no law does, the start value is an error. The message tells the author to remove the mark if the field is a parameter.

### 4. A derived field is a derived cell in the world

- The world already has the derived cell (ADR 0018). A derived field becomes one, with the range of its types as its base. The world does not keep the start value.
- So a derived field has the three properties of a derived cell. The laws give its value. The state hash does not cover it. The world refuses an input to it.
- A kernel cannot write a derived field. The elaborator reports a slot with the role `out` or `state` that binds one. The world gets the same check, which it does not have today.
- A kernel can read a derived field. A kernel that narrows (ADR 0009) can narrow one, because that step is monotonic, as a law is.
- The world of the C tests declares its temperatures as derived cells. The two-room world in source then has the same state hash as that world (ADR 0011, acceptance test 1).

## Considered options

- **The author computes the stock** (ADR 0016). It needs no rule. But each example with heat is then hard to write and to read. We rejected it.
- **Compute the stock, and change nothing more.** The temperature is then a value for every time. This is the small change that `quad-rf3` first asked for. The first transfer then contradicts the law. We rejected it.
- **The compiler finds the derived fields.** It cannot. In `.energy == .capacity * .temperature`, the energy is the state, and one of the other two fields must follow it. Nothing in the law says that the capacity stays and the temperature follows. Only the author of the type knows. We rejected it.
- **A mark on the value in the instance: `temperature: 303.15K @start`.** A type would need no mark. But each instance must repeat the mark, and an instance without it loads and then fails at the first step. We rejected it.
- **Sections in the type, as in the sketch of design section 6.2: `state: { … }` and `observables: { … }`.** The path of a cell would change to `roomA.observables.temperature`, and a slot of a kernel names a path. An attribute keeps the paths. We rejected the sections.
- **A value for a derived field in a type is also a start value.** That is one rule for all values of a derived field. But a bound in a type, such as `>=0K`, reads as a rule for every time, and it would hold at the start only. We rejected it.
- **The name `@observation` or `@observable`.** The design document uses "observation" for a measurement that arrives and narrows a value (design section 9.4). It uses "observability" for what a player can see (design section 7.1). The mark says how the value comes, not who looks. We rejected these names.
- **The field stays an ordinary cell in the world.** The runtime would not change. But the hash would cover a value that the state determines, and an input to the field could only contradict a law. The word "derived" would also have two behaviors. We rejected it.
- **Compute the start in the step that builds the world, not in the elaborator.** The IR would have stocks with no value, and each backend would need the same solver. We rejected it.

## Consequences

- An example with heat reads as the author thinks: `living: #Room & { temperature: 303.15K, capacity: 10000J/K }`.
- The author of a type has one more thing to decide: which fields get the mark. An author of an instance has nothing new to learn.
- ADR 0016 section 2 changes. The first value of a stock is exact: the author gives it, or the laws give it at the start.
- A type can give a derived field an exact value: `#WarmRoom: #Room & { temperature: 303.15K }`. That says that each such room stays at that temperature. The first transfer then contradicts it, and the message of the tool names the law.
- The elaborator runs the propagators at compile time. It uses the network of the runtime for that, so the start has the same arithmetic as the world.
- A kernel that narrows (ADR 0009) has no part in the start. A stock that only such a kernel can determine needs a value from the author.
- The state hash of a world changes when its types get the mark, because the hash no longer covers the derived fields.
- A derived field with no law has no value at any time. The elaborator does not report it, because a kernel that narrows can give the value.
- `@derived` is the first attribute that the elaborator reads. The attributes of Triangle's workshop example (`@field`, `@component`, `@vary`) are not names of the front end, so the port of that example (`quad-ijx.8`) must decide what they become.
- The IR has two sorts of derived cell: a derived field, and an inner value of a law. It marks them apart. `quad run` prints each field, and it does not print an inner value.
- A world that contradicts a law at the start no longer loads. Before, the tool found the contradiction when it ran the world. A contradiction that comes later, from an input or a transfer, is still an error of the run.
