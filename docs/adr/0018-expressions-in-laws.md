---
status: proposed
---

# A law can have inner expressions, numbers and inequalities, and an inner value is a derived cell

ADR 0016 section 4 lowered a law of one shape: `a == b` or `a == b op c`, with references only. A law such as `.power == .conductance * (.hot.temperature - .cold.temperature)` was an error, and the author had to give each inner value a field with a kind. The inner difference needs a cell, and three questions about that cell were open. Does it have a path? Is it state? Which kind does it have? ADR 0016 also made a field with a dimension and no kind an error, such as `power: %energy / %time`. We decided the rules below.

## Decision

### 1. The forms of a law

- A law is an equation (`==`) or an inequality (`<`, `<=`, `>`, `>=`). Each side is a reference, a number with its unit, or an expression of those with `+`, `-`, `*`, `/` and parentheses.
- Each operation gives one propagator. A reference on one side and one operation on the other side give one propagator with no new cell, as in ADR 0016.
- `!=`, the logic operators, and function calls in a law are errors. A relation that is not an equation or an order belongs to the relation layer of stage 5.

### 2. An inner value is a derived cell

- The value of an inner expression is a **derived cell**. A number in a law is a derived cell with a point value.
- The path of a derived cell is the name of its law and a number: `wall.balance.1`. The numbers follow the order in which the expression makes the cells. So the path does not depend on the order of the source. A law is a value with no fields, so no field can have that path.
- A derived cell is not state. Only propagation gives it a value, in each epoch. The state hash does not cover it, it takes no input, and a snapshot does not need it. The world has `qw_derived_cell` for it.
- A derived cell has a path so that a message, a test, and later EXPLAIN (`quad-5xe.3`) can show it: "the inner difference is 10 K".

### 3. The kind of a derived cell

- A sum or a difference keeps the kind of its operands (ADR 0001 section 1). `.hot.temperature - .cold.temperature` is a `%temperature`.
- A product or a quotient has an **anonymous kind**: a dimension and a scale, with no name. Its scale is the exact scale of the operation.
- A number has an anonymous kind with the scale that makes it exact. `273.15K` is 27315 with the scale -2, and `100K` is 1 with the scale 2. So a number is exact in every law.
- An anonymous kind converts to every kind of its dimension, as ADR 0001 says for an intermediate value. The world has `qw_anonymous_kind` for it.

### 4. A field with a dimension and no kind

- `power: %energy / %time` is a cell. It has an anonymous kind with the default scale of its dimension (ADR 0001 section 1). This changes ADR 0015 section 3 and ADR 0016 section 2, where it was an error.
- Such a cell is state, like every field: it is in the state hash, and it takes an input.
- A value that is not a whole number of quanta of that scale is an error. The message tells the author to give the field a kind with another scale.
- A field of type `number`, with no dimension, is still an error.

### 5. Inequalities

- The network gets one new propagator: `a <= b`. It lowers the upper bound of `a` and raises the lower bound of `b`. A bound converts between scales outward, so no value is lost.
- `a < b` removes one quantum of the cell that it narrows. A value is a whole number of quanta (ADR 0001), so the next value below 5 K is one quantum below it.
- `>=` and `>` are the same propagator with the two sides exchanged.

### 6. One text is one law

Two comparisons with the same text are the same value. So one law that a type and an instance both state unifies with itself. Before, the two were two values, and the load reported a conflict of a law with itself.

## Considered options

- **The author names each inner value** (ADR 0016). It needs no new cell. But almost every real law has an inner expression or a number. We rejected it.
- **A derived cell in the state hash.** The world would need no new kind of cell. But two contents with the same state and two spellings of a law would have two hashes. A snapshot would also store values that propagation gives. We rejected it.
- **A derived cell with no path, in the network only.** The world keeps one record for each cell of the network, with a path and an owner. A hidden cell needs a second kind of record. A hidden cell also cannot be shown. We rejected it.
- **A number converts to the kind of the other operand.** `273.15K` next to a temperature in whole kelvin would not be exact. With its own scale, a number is always exact, and the propagator converts outward. We rejected the conversion.
- **A default scale for each derived cell**, as for a field with no kind. The scale of a product is then independent of its operands. But a product of two fine scales would round, and the result of a law would depend on a default. We kept the exact scale.

## Consequences

- The wall law of the stage 2 fixture elaborates, and the world narrows the power to 200 W.
- The exact scale of a product is the sum of the scales of its operands. A long product can have a very fine scale, and its bounds can then widen to the whole range. The result stays sound. The author gives the value a field with a kind to choose the scale.
- The IR has anonymous kinds with names such as `dim[K]@-6`, and cells with the mark "derived".
- A message of the world names an anonymous kind as "anonymous K, scale -6".
- ADR 0015 section 3 and ADR 0016 sections 2 and 4 change with this ADR.
- An unknown kind in a dimension expression, such as `%energi / %time`, now has the message "unknown kind". Before, the message was about a name in an expression.
