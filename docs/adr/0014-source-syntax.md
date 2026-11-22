---
status: proposed
---

# Source has Triangle's grammar, a number keeps its unit as text, and the parser knows no names of units, attributes, or functions

ADR 0011 section 1 decided that Quadrilateral source is a CUE-like language borrowed from Triangle, and it left one detail open: how a literal writes a compound unit such as `J/K`. Triangle's parser also does work that does not fit Quadrilateral. It converts each number to quanta from a fixed table with one scale for each dimension, but in Quadrilateral the kind of the field decides the scale (ADR 0001). It rejects an attribute that is not in a fixed list, and that list serves component generation for Odin, which ADR 0011 does not borrow. It checks function names, compiles regular expressions, and reads files for `@embed`. We decided that the parser is the lexer and the recursive-descent parser of Triangle's `parse.c` (bruggadung `f425574`, under ADR 0003), that a number keeps its unit as text, and that every check of a name moves to a later stage.

## Decision

### 1. The grammar

The grammar is the grammar of Triangle's `docs/REFERENCE.md` at `f425574`:

- Declarations of types (`#Name`), atoms (`"name"`), and instances, with an optional `package` line.
- Structs, lists, unification (`&`), disjunction (`|`), defaults (`*`), bounds, tags (`$t`), and references (`.a.b`).
- `let` and hidden fields (`_name`), comprehensions (`for`, `if`), and computed labels.
- Arithmetic, comparisons, and logic.
- Regular-expression constraints (`=~`, `!~`), string interpolation, and function calls.
- Attributes (`@name(args)`), and docstrings: the comment lines directly above a declaration.

The parser stops at the first syntax error in a file. The diagnostic gives the file, the line, and the column. A file with an error adds no declarations.

### 2. A number and its unit

- A number is digits with an optional fraction and an optional sign. There is no exponent form and no hexadecimal form.
- A unit follows the digits with no space: `250mm`, `293.15K`.
- A unit can be compound. Its terms are joined by `*` or `/`, and a term can have an exponent: `1000J/K`, `5kg*m^2/s^2`, `3m^-1`.
- A `*` or a `/` continues the unit only when a letter follows it at once. So `10mm*2` and `10m / x` are arithmetic, and `10m/x` is one number with the unit `m/x`.
- The parser keeps a literal as three values: the digits as an integer, the number of fraction digits, and the unit as text. It does not know units.
- A later stage converts the literal with the kind of its field, exactly (ADR 0001). The unit text follows the dimension grammar of ADR 0009 section 10, with `*` in place of a space, and the display units of ADR 0001.

### 3. The parser knows no names

- An attribute can have any name. The parser keeps its arguments as text.
- A function call can have any name and any number of arguments.
- The parser does not compile the pattern of a regular expression.
- A later stage checks each of these, and its diagnostics use the locations in the syntax tree.

### 4. What the parser does not take from Triangle

- `ref(n)`, a handle for an entity of Bruggadung's runtime. Quadrilateral identifies a phenomenon by its key (ADR 0013). `ref(12)` now parses as an ordinary function call.
- The file read of `@embed`. The attribute parses like any other, and the parser reads no files.
- The flag that loads the prelude of definite clauses. ADR 0011 section 6 keeps clauses as an explicit query, for later.

### 5. Equations

A comparison parses in any expression, as in Triangle, and it gives a comparison node. So `laws: [ .energy == .capacity * .temperature ]` parses. In Triangle a comparison is a condition that the loader evaluates. In Quadrilateral the elaborator decides what a comparison means where it stands: a condition, or a law that becomes propagators (ADR 0011 section 4).

### 6. The syntax tree

- The tree has Triangle's shape, so that stage 2 can borrow Triangle's `types.c` with few changes.
- The parser replaces each use of a let with the expression that the let binds.
- For each reference `.a.b`, the declaration gains the conjunct `{ a: { b: .a.b } }`. This gives the sharing in both directions of ADR 0011 section 1.
- The tree has a text form, `qf_print`: source that parses to the same tree. Tests and tools inspect the parser's result through it.
- The tree and its header, `src/front/front.h`, are internal. The public product of the front end is the IR (ADR 0011 section 3).

## Considered options

- **A space between the number and the unit, `20 W/K`.** It reads like SI notation and like the sketches in earlier ADRs. The lexer would need a lookahead rule: a name after a number is a unit, unless a `:` follows. The words `for`, `if`, and `in` are not reserved, so `if r > 3 for s in ys` would need exceptions. We rejected it.
- **Simple units only, and compound units later.** The lexer would change again when the two-room world needs `J/K` and `W/K` (stage 3). We rejected it.
- **Conversion in the parser, as in Triangle.** The parser does not know the kind of a field, and one scale for each dimension contradicts ADR 0001. We rejected it.
- **A new parser with a tree that keeps lets and references as written.** It separates parsing from resolution more cleanly. But stage 2 borrows code that expects Triangle's tree. We rejected it for now.
- **Triangle's fixed list of attributes.** The list describes Odin components. The elaborator will have its own attributes, and the parser must not need a change for each one. We rejected the list.

## Consequences

- Stage 2 gets five checks that Triangle's parser did: the conversion of literals, the unit names, the function names and their argument counts, the patterns of regular expressions, and the attribute names.
- `1e5` and `0x10` parse as numbers with the units `e5` and `x10`. Stage 2 rejects them as unknown units.
- The illustrative examples in ADR 0012 write `20 W/K`. The valid form is `20W/K`.
- An author must put spaces around an operator after a quantity when a name follows: `10m / x`. When stage 2 rejects a unit that contains `/` or `*`, its message can say so.
- The front end stops the process when memory runs out. The runtime returns `QW_ENOMEM`. This difference comes from Triangle, and it needs a change if the front end runs inside a long-lived host.
- Triangle's `examples/workshop` parses without change. It is test data in `tests/data/workshop`.
