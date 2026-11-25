---
status: accepted
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

This ADR gives the details of those points. The section "What this ADR adds to the four points" lists each detail that the short form did not have.

The first version of this ADR allowed a condition on one cell only. The project owner asked on 2026-11-25 why a condition has no "and", "or" and "not", and no comparison of two fields. The design needs each of them:

- A model applies only when oxygen is plentiful and the material is approximately homogeneous (design section 6.2).
- A fire becomes detailed when a character enters the building or when its prediction becomes unreliable (design section 7.1).
- A combustion exists for each combustible whose temperature is above its ignition temperature (ADR 0011 section 7).

So this version makes a condition a truth-value expression. The project owner chose the operators of CUE: `&&`, `||` and `!`.

## Decision

### 1. A condition is the field `when` of a map

```cue
#DetailRoom: #Phenomenon & {
    visible: bool                                  // an input of the application
    outside: %temperature
    refine:   #Map & { from: "coarse", to: "fine", evaluator: "native.room.refine",
                       when: .visible || .west.temperature > .outside + 20K }
    abstract: #Map & { from: "fine", to: "coarse", evaluator: "native.room.abstract",
                       when: !.visible && .west.temperature <= .outside + 20K }
}
```

- The prelude has `#Map: { from: text, to: text, evaluator: text, when: _ }`. A map is a transition, and the name of its field is the name of the transition.
- A condition is a truth-value expression. It has these forms:
  - a path to a field with the type `bool`: `.visible`
  - a comparison of a path with an atom, with `==` or `!=`: `.door != "open"`
  - a comparison of two quantities, with `<`, `<=`, `>`, `>=`, `==` or `!=`: `.temperature > 300K`, `.inside > .outside + 2K`. Each side is an expression as in a law (ADR 0018): paths, quantities, and `+ - * /`. The two sides have the same dimension.
  - `!c`, `c && c`, `c || c` and `(c)`, where each `c` is a condition
- The operators bind as in CUE. `!` binds most. Then come the comparisons, then `&&`, then `||`. A comparison does not chain: `.a < .b < .c` is an error.
- A path can go through the port of the active model: `.west.temperature`.
- A map with no `when` has no condition. Only the application starts its switch.

### 2. A condition has three results

A condition is true, false or unknown. An unknown condition changes nothing (ADR 0008).

- A path to a `bool` field is true or false if the field has that one value. If not, it is unknown.
- A comparison is true if propagation shows that it holds for each possible value of its fields. It is false if propagation shows that it holds for none. If not, it is unknown.
- `!c` is true if `c` is false, and false if `c` is true. If `c` is unknown, `!c` is unknown.
- `c && d` is false if one of the two is false, also if the other is unknown. It is true if the two are true. If not, it is unknown.
- `c || d` is true if one of the two is true, also if the other is unknown. It is false if the two are false. If not, it is unknown.

So `!` is not "the condition has no proof". No operator changes an unknown result into false. If an epoch has a true or false result, the result stays the same in that epoch.

### 3. A condition with an operator is a cell

- The world keeps the form of a condition from story `quad-ijx.4.4`: the value of one cell is in a set. The set is a range of a quantity or a set of atoms.
- A condition from source with one path of the phenomenon and one constant has that form. Examples are `.visible`, `.door != "open"` and `.temperature > 300K`. The elaborator gives it to the world as it is.
- For each other condition, the elaborator makes a cell with the type `bool` at the field `when` of the map, such as `roomB.refine.when`. It makes propagators that compute the cell from the fields in the expression. The condition of the world is then "the cell `roomB.refine.when` is true".
- The cell is a derived field (ADR 0019). Propagation gives it a value in each epoch. The IR and the inspection tools show it as each other cell. The change log has no entry for it, as for each derived field. The log has the changes of state, and the cell is a function of state.
- The world reads the cell at a boundary, after the fixed point of propagation. That is the rule for each condition.

### 4. A law can give a name to a truth value

```cue
#DetailRoom: #Phenomenon & {
    visible: bool
    busy: bool
    laws: [ .busy == (.visible || .people > 0) ]
}
```

- A law can be an equation of a path to a `bool` field and a condition. The condition is in parentheses if it has an operator, because `==` binds more than `&&` and `||`.
- The elaborator makes the same propagators as for a `when`. The field then gets its value from the law in each epoch.
- Such a law is a relation, as each law. If the field has a value from another source, propagation can narrow the fields of the condition.
- A condition that is alone in `laws` is still an error, for example `laws: [ .visible || .people > 0 ]`. That is a statement that a disjunction is true, and it belongs to the relation layer (ADR 0018).
- An existence condition (ADR 0013) names a cell with the type `bool`. So this law also gives source the condition for a phenomenon that starts and ends (`quad-ijx.7`).

### 5. A named input is a field

- An input of the application is a field of the phenomenon that no law and no kernel writes, such as `visible: bool`. The application sets it with `qw_input` or `qw_input_set`, which exist.
- Each instance has its own field, so two rooms with one rule can make different decisions.

### 6. One switch for each phenomenon at a boundary

- The world reads the conditions at the start of a run and after each boundary, after the step for the instances (ADR 0013). It decides each switch from the values at that time. Then it makes the switches in the order of the names of the phenomena.
- Only the transitions from the active model count.
- A phenomenon makes one switch at most at a boundary. The conditions of its new model have their turn at the next boundary. So two conditions that are each true after the other switch cost one switch at each boundary, and the run does not stop there.
- If two transitions from the active model are true at one boundary, the run stops with an error. The message names the phenomenon and the two transitions. The author must make the conditions exclusive.

### 7. The application can replace a condition

- The world keeps the exact type of each phenomenon and the condition from source of each transition.
- `qw_override_condition` gives one instance its own condition for a transition.
- `qw_override_type_condition` gives a condition to each instance of an exact type.
- `qw_clear_condition` and `qw_clear_type_condition` remove them.
- A condition from the application is a statement about one cell: a path in the phenomenon, and a range or a set of atoms. A condition can also be "never".
- For a condition with an operator, the application names a `bool` field that source computes (section 4). It can also name the cell of another transition, such as `refine.when`.
- The world uses the condition of the instance. If there is none, it uses the condition of the exact type, and then the condition from source. `qw_condition` gives the condition that is in effect and its level.
- A call takes effect at the current time, which is a boundary. The world reads the new condition at once.
- The conditions of the application are state. The state hash and a snapshot have them.

### 8. The world records the calls of the application

- The world can record each call of the application that changes it. The calls are:
  - a run to a time
  - an input
  - a start or an end of an instance
  - a switch
  - a change of a condition
- `qw_replay` makes the same calls in a new world. The new world then has the same state hash at each time, and each switch at the same boundary.
- A run to a time is in the record because each stop of a run is a boundary.

## Why incremental evaluation stays valid

ADR 0011 section 6 has the rules that keep incremental and differential evaluation valid. A condition with operators follows them.

- **Each deduction in an epoch is monotonic (rule 1).** The cell of a condition starts with the two values `false` and `true`. A propagator can only remove one of them. Section 2 gives each operator a result that does not change when a field gets a smaller range.
- **The step that is not monotonic is at a boundary (rule 2).** The switch reads the cell after the fixed point. A decision from an unknown result does not exist, because an unknown condition changes nothing.
- **No result depends on an order (rule 3).** The propagators of a condition have no order.
- **No condition reads a previous value (rule 4).** "The oxygen changed by more than five percent" needs a field that holds the reference value.
- **The change log is sufficient (rule 6).** The cell of a condition is a function of the fields of its expression. The log has each change of those fields, so a later differential engine can maintain the cell from the log.

## What this ADR adds to the four points

- **The form of a condition.** The short form said "an expression". This ADR makes it a truth-value expression with comparisons, `&&`, `||` and `!`. The project owner chose those three operators. Sections 1 to 4 add the forms of a comparison, the three results, the cell at `when`, and the law for a named truth value.
- **A condition from the application is a statement about one cell.** It is less than a condition from source.
- **The time of the decision.** The world decides each switch of a boundary first, and then makes the switches in the order of the names.
- **The names of the functions**, and the condition "never".
- **The conditions of the application are state**, in the state hash and in a snapshot.
- **The record also has each run to a time**, a start and an end of an instance, and a switch. The short form named inputs, switches and changes of conditions.

## Acceptance tests

1. Acceptance test 5 of ADR 0021: change a source condition through the C API. Check the instance and the exact type, the removal of both, a subtype, and the replay at the same boundary. Two rooms with different inputs make different decisions with one rule. (In the code.)
2. Each operator gives the result of section 2 for each pair of true, false and unknown.
3. A room with `when: .visible || .inside > .outside + 20K` makes its switch at the first boundary where one of the two parts is true. While `inside` has no value and `visible` is false, the room makes no switch.
4. The application replaces a condition with one that names a `bool` field from a law. The switch follows that field.
5. A replay of a run with such conditions has the same state hash after each call.
6. A change of a cell that a condition does not read runs no propagator of that condition.

## Considered options

- **A condition on one cell only.** That was the first version of this ADR. Rejected. The design needs "and", "or" and a comparison of two fields, and the limit protects no rule of ADR 0011.
- **The words `and`, `or` and `not`.** Rejected. The language is like CUE, and the parser has `&&`, `||` and `!`.
- **A tree of conditions in the world, which the world evaluates at a boundary.** The application could then build each condition in C. Rejected. An existence condition and a later automatic selection read cells. The change log and the inspection tools show a cell with no new code. A snapshot would need the tree.
- **The text of an expression from the application, compiled at the time of the call.** Rejected. The world would need the front end in a running program.
- **The first true transition in the order of the names wins.** Rejected. The author gets no message that two conditions are true together.
- **A switch again at the same boundary until the conditions settle.** Rejected. Two conditions that are true in turn would not end.

## Consequences

- The application cannot build a new condition with operators at run time. It names a `bool` field that source computes. The fix is a C function that makes a truth-value cell from other cells at a boundary. Time: `quad-hu6`, when an application needs it.
- A named truth value takes two lines: the field and its law. The fix is an expression as the value of a field. Time: `quad-pr0`.
- A comparison can be unknown where a reader sees the result, because the network computes with ranges. The elaborator removes the same text from the two sides of a comparison, so `.t > .t - 1K` is true. It does no other algebra: `2 * .t >= .t + .t` is unknown while `.t` has a range. The author then writes each path on one side only.
- ADR 0018 states that a logic operator in a law is an error. Section 4 changes that for one form of a law, and ADR 0018 has a pointer to this ADR.
- The record has one entry for each call of `qw_run`. An application that calls `qw_run` for each frame gets a long record. The fix is a run that stops at a time and makes no boundary. Time: with the asynchronous clocks of `quad-h21.3`.
- Automatic selection by cost (`quad-ft5.3`) can use the same step at a boundary and the same switch. Its applicability conditions can be cells of the same kind.

## Notes from the implementation (`quad-ijx.4.4`)

- **The functions.** `include/quad/world.h` has the condition (`QwCondition`) and `qw_transition_when` for the condition of the content. It has the four functions for the conditions of the application, and `qw_condition`. The record has `qw_record`, `qw_calls` and `qw_replay`. `qw_set_type` gives the world the exact type of a phenomenon. `qi_instantiate` calls it for each phenomenon.
- **The time of the decision.** The world reads the conditions at the start of each `qw_run`, also a run to the current time. So a change of an input or of a condition takes effect when the application runs the world again. That is at the same time, with no new boundary.
- **A path that is not a cell.** The world finds the cell of a condition when the application gives the condition, and again after a switch of that phenomenon. A condition of a type must fit each phenomenon of the type that has the transition. If a path is not a cell after a switch, the condition is unknown.
- **A number between two steps.** `.temperature > 294.0000005K` on a field with 6 decimals is `>= 294.000001`. A comparison with `==` needs a number on a step.
- **An atom in an expression.** A comparison with `==` or `!=` can have an atom: `.visible == false`. Before, each expression with an atom was an error. A law with an atom is still an error.
- **A quantity with `!=`.** `.temperature != 300K` is an error in this story, because one range cannot state it. With `quad-esj` it is `!(.temperature == 300K)`.
- **The snapshot.** The format of a snapshot has a new number, because it has the conditions of the application. A snapshot from before this work does not load.
- **A switch from `qw_switch` in the record.** The record has the phenomenon and the model. The replay finds the transition between the two models. So a switch with a kernel that is not a transition of the world does not replay.
- **The tool.** `quad run --set study.visible=true@5min` gives a cell a value at a time. `examples/detail-room.quad` has a room that is fine while `visible` is true.
- A condition `when: false` is an error, because the map then has no use for a condition. `when: true` is always true.

## Notes from the implementation (`quad-esj`)

- **Two statements of the accepted text were wrong, and this text has the correction.** Section 3 and the section on incremental evaluation said that the change log shows the cell of a condition. The change log has only the changes of state: an input, a write of a kernel and a transfer. A derived field has no entry. Acceptance test 6 used the change log, so it is now a test of the network. A change of a cell that the condition does not read runs no propagator of the condition.
- **The network.** `include/quad/network.h` has `qn_and`, `qn_or` and `qn_not` for truth-value cells, and `qn_reify_set` for "a set cell is in some of its atoms". Each one works in each direction: a result that is true or false can make an input smaller.
- **The IR.** A propagator has five new operations: `QI_IN_RANGE`, `QI_IN_SET`, `QI_AND`, `QI_OR` and `QI_NOT`. `QI_EQUAL` also makes two set cells of one enumeration equal. The text form writes `roomB.refine.when <=> roomB.visible || roomB.refine.when.5`.
- **The world.** `qw_derived_set_cell` makes the cell of a condition. It takes no input, and the state hash and a snapshot do not have it.
- **A comparison of two quantities.** The elaborator moves each part to one side and removes the same text from the two sides. One cell and one number are a range of that cell. Each other comparison is a range of an inner difference. `!=` is the negation of `==`.
- **A number between two steps.** `.temperature == 300.0000005K` on a field with 6 decimals is an error, also with `!=`: the two are never equal.
- **Two forms on one cell that were errors now load.** `when: .temperature != 300K` loads. A condition that reads a field of another phenomenon through a connection loads too. Each one gets a cell at `when`.
- **A comparison of two truth values is an error in a condition.** `.a == .b` in a condition gets a message. The law `.r == .a` makes two `bool` fields equal.
- **A set of atoms through the port of the active model.** A condition or a law for a truth value reads a copy, as a law does for a quantity (ADR 0021). One propagator for each model gives the copy its value.
- **The start.** The propagators of a truth value run at the start too. If the values of an instance contradict the law, the load stops with a message that names the law and the values.
- **A law is a relation.** In `box: #Box & { r: false }` with the law `.r == (.a || .t > 300K)`, propagation makes `a` false and `t` at most 300 K.
- **The example.** `examples/detail-room.quad` has a limit: the room is fine while somebody looks at it, or while it is warmer than the limit.
