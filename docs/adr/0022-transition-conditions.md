---
status: proposed
---

# A transition has a condition, and the application can replace it

## The problem

ADR 0021 section 5 states that source can give a condition to a transition between two models. It also states that the application can inspect, replace and clear that condition in a running world. Four points are open (`quad-ijx.4.4`):

- the syntax of a condition
- what an input of the application is
- what happens if two conditions are true
- how the world records a change for replay

The project owner agreed to four points on 2026-11-25, in this short form:

1. A condition is a field `when` on the map, as in `when: .visible`.
2. A named input is a field of the phenomenon that the application sets with `qw_input`. There is no new construct.
3. A phenomenon makes one switch at most at a boundary. If two transitions from the active model are true, the run stops with an error.
4. The world gets a record of the calls of the application, for replay.

This ADR gives the details of those points. The details are a proposal. The section "What this ADR adds to the four points" lists each detail that the short form did not have.

## Decision

### 1. A condition is the field `when` of a map

```cue
#DetailRoom: #Phenomenon & {
    visible: bool                                  // an input of the application
    refine:   #Map & { from: "coarse", to: "fine", evaluator: "native.room.refine", when: .visible }
    abstract: #Map & { from: "fine", to: "coarse", evaluator: "native.room.abstract", when: .visible == false }
}
```

- The prelude has `#Map: { from: text, to: text, evaluator: text, when: _ }`. A map is a transition, and the name of its field is the name of the transition.
- A condition is a statement about one cell of the phenomenon: the value of the cell is in a set. It has three forms:
  - a path to a field with the type `bool`: `when: .visible`. The condition is true while the field is true.
  - a comparison of a path with an atom: `.visible == false`, `.door != "open"`.
  - a comparison of a path with a quantity: `.temperature > 300K`, with `<`, `<=`, `>`, `>=` or `==`.
- The path can go through the port of the active model: `.west.temperature`.
- A condition has three results. It is true if each possible value of the cell is in the set. It is false if no possible value is in the set. If not, it is unknown, and an unknown condition changes nothing (ADR 0008).
- A map with no `when` has no condition. Only the application starts its switch.

### 2. A named input is a field

- An input of the application is a field of the phenomenon that no law and no kernel writes, such as `visible: bool`. The application sets it with `qw_input` or `qw_input_set`, which exist.
- Each instance has its own field, so two rooms with one rule can make different decisions.

### 3. One switch for each phenomenon at a boundary

- The world reads the conditions at the start of a run and after each boundary, after the step for the instances (ADR 0013). It decides each switch from the values at that time. Then it makes the switches in the order of the names of the phenomena.
- Only the transitions from the active model count.
- A phenomenon makes one switch at most at a boundary. The conditions of its new model have their turn at the next boundary. So two conditions that are each true after the other switch cost one switch at each boundary, and the run does not stop there.
- If two transitions from the active model are true at one boundary, the run stops with an error. The message names the phenomenon and the two transitions. The author must make the conditions exclusive.

### 4. The application can replace a condition

- The world keeps the exact type of each phenomenon and the condition from source of each transition.
- `qw_override_condition` gives one instance its own condition for a transition.
- `qw_override_type_condition` gives a condition to each instance of an exact type.
- `qw_clear_condition` and `qw_clear_type_condition` remove them.
- A condition from the application has the same form as one from source. It is a path in the phenomenon, and a range or a set of atoms. A condition can also be "never".
- The world uses the condition of the instance. If there is none, it uses the condition of the exact type, and then the condition from source. `qw_condition` gives the condition that is in effect and its level.
- A call takes effect at the current time, which is a boundary. The world reads the new condition at once.
- The conditions of the application are state. The state hash and a snapshot have them.

### 5. The world records the calls of the application

- The world can record each call of the application that changes it. The calls are:
  - a run to a time
  - an input
  - a start or an end of an instance
  - a switch
  - a change of a condition
- `qw_replay` makes the same calls in a new world. The new world then has the same state hash at each time, and each switch at the same boundary.
- A run to a time is in the record because each stop of a run is a boundary.

## What this ADR adds to the four points

- **The form of a condition.** The short form said "an expression". This ADR allows a statement about one cell: a `bool` field, or a comparison of one path with an atom or a quantity. It has no "and", "or" and "not", and no comparison of two fields.
- **Three results.** A condition that is unknown changes nothing.
- **The time of the decision.** The world decides each switch of a boundary first, and then makes the switches in the order of the names.
- **The names of the functions**, and the condition "never".
- **The conditions of the application are state**, in the state hash and in a snapshot.
- **The record also has each run to a time**, a start and an end of an instance, and a switch. The short form named inputs, switches and changes of conditions.

## Acceptance test

Acceptance test 5 of ADR 0021: change a source condition through the C API. Check the instance and the exact type, the removal of both, a subtype, and the replay at the same boundary. Two rooms with different inputs make different decisions with one rule.

## Considered options

- **A boolean cell that laws compute, as for an existence condition (ADR 0013).** The network would need a law that gives a truth value from a comparison, for each form. Rejected for now: a statement about one cell needs no new law, and the application can state it in C with no compiler.
- **The text of an expression from the application, compiled at the time of the call.** Rejected. The world would need the front end in a running program.
- **The first true transition in the order of the names wins.** Rejected. The author gets no message that two conditions are true together.
- **A switch again at the same boundary until the conditions settle.** Rejected. Two conditions that are true in turn would not end.

## Consequences

- A condition reads one cell. A condition on two fields, such as `.inside > .outside`, needs a field for the difference and a law. The fix is an expression as the value of a field. Time: `quad-pr0`.
- A condition has no "and", "or" and "not". The fix is a field with the type `bool` that laws compute from other fields. The network has no such laws yet. Time: a new issue, after this work.
- The record has one entry for each call of `qw_run`. An application that calls `qw_run` for each frame gets a long record. The fix is a run that stops at a time and makes no boundary. Time: with the asynchronous clocks of `quad-h21.3`.
- Automatic selection by cost (`quad-ft5.3`) can use the same step at a boundary and the same switch.
