---
status: accepted
---

# An interaction with a rate changes a stock over time, with no C kernel

## The problem

Only one thing changes a value over time today: the native kernel `native.conduction`. So each example of change is two rooms and a wall. An author cannot write a heater, a leak, a pump or growth (`quad-znr`).

The author wants to write a heater like this:

```cue
#Heater: #Phenomenon & {
    room:  #ThermalPort
    power: %energy / %time
    heat:  #Interaction & { sink: .room.energy, rate: .power, conserved: false }
}

heater: #Heater & { room: living.west, power: 1500W }
```

And a tank that leaks faster when it is fuller, with a law for the rate:

```cue
%volume: { dimension: "m^3", conserved: true }

#Tank: #Phenomenon & {
    volume:  %volume
    empties: %time                       // the time constant of the leak
    outflow: %volume / %time
    speed:   .outflow == .volume / .empties
    leak:    #Interaction & { source: .volume, rate: .outflow, conserved: false }
}

tank: #Tank & { volume: 2m^3, empties: 600s }
```

Neither needs C. The laws already give the rate. The language lacks only the statement "move this stock at this rate".

The sources already name the construct. An interaction is a typed structure with a source, a sink and a conserved flag (ADR 0011 section 4, design section 9.3). A stock changes only through a transfer, so conservation is exact by construction (ADR 0009 section 3). A kernel in Quadrilateral and a kernel in C satisfy the same contract (design section 12.2).

## Decision

### 1. The prelude has the type `#Interaction`

```cue
#Interaction: {
    source:    _            // the stock that loses the quantity
    sink:      _            // the stock that gets the quantity
    rate:      _            // the quantity for each second
    tolerance: *1pct | %ratio
    conserved: *true | bool
}
```

- An interaction is a field of a phenomenon. A phenomenon can have more than one.
- The two defaults use the `*` of `quad-ijx.10`.

### 2. The source and the sink are stocks of one kind

- Each is a path to a stock: a stock of the phenomenon (`.volume`), or a stock behind a port (`.room.energy`).
- The two have the same kind. A different kind is an error.
- An interaction with a source and a sink conserves the quantity. `conserved: false` there is an error.
- An interaction with only one of them brings the quantity into the world, or takes it out. The author must then write `conserved: false`. Without it, the compile gives an error. This stops a forgotten `sink` from silently destroying energy.

### 3. The rate is a field

- The rate is a path to a field, or a field of the interaction with a law for `.leak.rate`.
- Its dimension is the dimension of the stock for each second. Another dimension is an error.
- An instance gives the rate a value (`power: 1500W`), or the laws compute it.
- A negative rate moves the quantity from the sink to the source.
- If the rate is a range, the interaction uses the middle of the range, as `native.conduction` does. If the rate has no bound, the run stops with a message that names the interaction.
- A stock of the phenomenon can be the rate of another interaction. The speed of a body is a stock that an acceleration changes, and it is the rate of the height.
- A rate with a dimension and no kind gets the scale of its stock with 6 more decimals. The default scale of the dimension is too coarse for a slow flow: the rate then reads as zero, and the flow stops. An inner quotient of a law has 6 more decimals for the same reason (ADR 0018). A rate with a kind keeps the scale of its kind.
- The source, the sink and the flag of an interaction are not cells. A law that reads `.volume` finds the cell, also where the same value stands at `leak.source`.

### 4. The runtime moves the rate times the step

- At each step, the runtime moves `rate × step` from the source to the sink. It uses the rate at the start of the step. This is the explicit coupling of ADR 0009 section 9.
- The runtime keeps the part of the amount that is smaller than one quantum of the stock, and adds it to the next step. So a small rate loses nothing to rounding.
- The arithmetic uses integers only, so a run gives the same result on each platform (ADR 0004).

### 5. The tolerance sets the step, and the author does not

- The author states the accuracy with `tolerance`, as for the wall. The author does not state a step.
- The rule: in one step, the rate changes by about `tolerance` of its value.
- The runtime measures how much the rate changed since its last change and sets the next step from it. A step is at most two times that time. The first step is one tick.
- For a rate that falls in proportion to its stock, the step becomes `tolerance` times the time constant. This is the step of `native.conduction`. The stock does not go below zero.
- A constant rate, such as the heater, needs no step of its own. Its result is exact at each time.
- Near a balance of two flows, each rate changes little, and the rule alone gives steps longer than the time constant. The stock then goes above and below the balance in turn. The runtime sees that a rate goes up and down in turn. It then stops the growth of the step, and after two turns it sets a cap of half the last step. The cap grows slowly while the rate goes in one direction.
- The first version of this ADR measured the change against the largest rate that the interaction had. With that rule, a stock that falls to zero goes below zero, because the allowed change does not become smaller with the rate. The implementation showed this, and the rule changed before the merge.

### 6. In the IR and in the world, the interactions of a phenomenon are one kernel

- The elaborator gives each phenomenon with such interactions one kernel with the name `native.rate`. Its slots are the rates, the tolerances and the stocks. Each interaction is one transfer.
- In the IR, a transfer of this kernel names the cell of its rate and the cell of its tolerance. `qi_instantiate` makes the kernel from that data and gives it to the world. The world frees it (`qw_adopt_kernel`).
- The scheduler and the state hash do not change. They see an ordinary kernel. The world has two small changes. It can own a generated kernel. Its message for a kernel that fails has the reason that the kernel gave.
- For each interaction, the kernel keeps the last rate, with the time and the direction of its last change. It also keeps the cap of the step and the kept amount. It gives `save` and `restore`, and a snapshot must use them (`quad-5xe.1`).
- A phenomenon with an interaction that has a rate cannot also name an `evaluator`. The compile gives an error.

## Acceptance tests

1. A heater of 1500 W gives a room exactly 1500 J more for each second, at each time of the run.
2. The tank follows `volume × exp(-t / empties)` within the tolerance.
3. A wall written with an interaction and a law for the rate gives the temperatures of `native.conduction` within the tolerance. The sum of the two energies stays exactly the same.
4. Two runs with the same seed give the same state hash at each time.

## Considered options

- **A derivative in a law, such as `d(.energy) == .power`.** Rejected. A derivative names one stock and no other end. Conservation is then a property that someone must check. With a source and a sink, it is exact by construction (ADR 0009 section 3).
- **The author states the step (`step: 5s`).** Rejected. The author must then know the time constant of the system. The end state of the design is a solver that takes a tolerance (design section 10.1, ADR 0009 section 9, `quad-h21.4`). With `tolerance` now, the source does not change when that solver arrives.
- **A new type name, such as `#Transfer` or `#Flow`.** Rejected. ADR 0011 already names `#Interaction` for this structure. An interaction without a rate, for a native kernel, comes with `quad-ijx.4`.
- **One end missing means "from outside", with no flag.** Rejected. A forgotten end then destroys a conserved quantity with no message.
- **A step from the size of the stock (move at most 1 % of the stock in a step).** Rejected. The steps become very small when a stock is near zero. For a temperature, the energy above 0 K is the wrong measure.
- **The change of the rate against the largest rate that the interaction had.** This was the rule of the first version. Rejected after the implementation: a stock that falls to zero goes below zero.

## Consequences

- The examples have a heater (`examples/heater.quad`) and a tank with a tap and a leak (`examples/tank.quad`). Neither needs C.
- The first method is explicit, with no repeat of a step (ADR 0006 has no rollback). A step can therefore change the rate by up to two times the tolerance. A stiff system, or two phenomena with a strong coupling, needs the group solver of `quad-h21.4`. The source stays the same.
- A rate that goes through zero, as in an oscillation, costs many short steps near zero, because the allowed change becomes small with the rate. The result is right. The group solver of `quad-h21.4` removes this cost.
- A rate law has no condition. An author cannot yet write "a constant flow until the tank is empty": the stock goes below its bound, and the world stops. The fix is a model with a condition for each regime. Time: `quad-ijx.4`.
- The author writes three lines for one flow: a field, a law and an interaction. The fix is an expression as the value of a field (`rate: .volume / .empties`), which the elaborator changes into a field and a law. This helps each computed field, not only a rate. Time: `quad-pr0`, after this work.
- Only a stock can change through an interaction. A position that follows a speed must have a kind with `conserved: true`, and an interaction with `conserved: false`. The word "conserved" reads wrong there. The fix is a rate for a state field of a kind that is not conserved. Time: `quad-iil`, with `quad-ijx.4`.
