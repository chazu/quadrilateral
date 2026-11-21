---
status: accepted
---

# Changing state is in cells, outside the feature-structure graph

Feature structures describe phenomena, models, ports, and interactions. A propagator network needs persistent links between values, and the simulation changes values at each boundary. In Triangle, each operation overwrites per-node scratch fields, also on loaded content, so persistent links cannot be stored on nodes. Nodes are also shared between structures, so an in-place change would change every structure that shares the node. Design section 10.5 already rejects one node for each instant. We decided that the run-time state is in cells, outside the feature-structure graph, and that feature structures supply the schema and the initial domains.

## Decision

- A cell holds a domain value, for example a `QInterval`, and a version. The cell's quantity type is in a static table that the compiler builds (ADR 0001).
- The compiler creates the cells of a phenomenon instance from its feature structure: one cell for each state value and each port value. A cell is identified by (phenomenon instance, feature path) and is indexed by an integer at run time.
- Propagators and evaluators refer to cells by index. Cells are stored as arrays of values, not as linked nodes.
- After load, feature structures do not change. Unification at run time is still available. For example, agents can exchange partial descriptions, and an operation can be matched against a description. Such unification creates new structures and never changes a cell.
- To match a pattern against current state, the runtime builds a description from the cells. Bruggadung's runtime does the same, but through text. Quadrilateral builds the description through handles.
- Cells are invisible to the author. Quadrilateral source, error messages, traces, and EXPLAIN output name a value by its phenomenon and feature path, never by a cell index. When source reads a state value, it gets a feature-structure value. The split between cells and feature structures is an implementation choice, and the language does not expose it.

## Consequences

- The unifier and the propagator network are separate components with a defined boundary: building cells from a structure, and building a description from cells.
- The unifier can keep Triangle's quasi-destructive design with per-operation scratch, because nothing persistent depends on node scratch. Each unification needs its own context, not one arena for each loaded content, so that unifications can later run in parallel.
- The propagator engine, the kernel interface, and the scheduler can be built and tested before the language front end exists (`quadrilateral-review-fable.md` section 6).
