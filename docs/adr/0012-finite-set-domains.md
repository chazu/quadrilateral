---
status: proposed
---

# Cells can hold a finite set of atoms, and element and reification propagators connect sets to quantities

Some state is not a quantity: a door is open or closed, a material is solid or liquid, a phenomenon is burning or not. Content needs such state as cells, so that propagation, kernels, and later relations can use it (ADR 0011 consequences). Unification already coordinates several constraint domains in Triangle (types, atoms, intervals), which is the CLP(X) pattern of design section 5. We decided to add one more cell domain to the network: a finite set of atoms, stored in the same 16 bytes as an interval. Two propagators connect it to quantities: element, which maps each atom to an interval, and reification, which links a boolean to the membership of a quantity in an interval.

## Decision

### 1. The set domain

- An enumeration kind has an ordered list of 1 to 64 atom names. Atom i is bit i of a 64-bit mask. A boolean is an enumeration with the atoms `false` and `true`, in that order.
- A set cell holds the mask of the atoms that are still possible. Narrowing is a bitwise AND. An empty mask is a contradiction. A mask with one bit is a point.
- A set cell uses the same 16-byte storage as an interval cell: the mask is in the first word and the universe mask (all atoms of the kind) in the second. Code that only copies cell values, such as the change log and snapshots, works for both domains without change.
- The network rejects an interval operation on a set cell, and a set operation on an interval cell (`QN_EDOMAIN`).
- More than 64 atoms is reserved for later. A larger kind needs a different storage, and no content needs it yet.

### 2. Propagators

| Propagator | Relation | Narrowing |
|---|---|---|
| Set equality | a = b, same kind | a ∩= b and b ∩= a |
| Element | x = table[a] | x ∩= hull of table[i] for each possible atom i; a ∩= the atoms whose table interval meets x |
| Reification | b ⇔ x ∈ I | x ⊆ I gives b = true; x ∩ I = ∅ gives b = false; b = true gives x ∩= I; b = false gives x ∩= the complement of I when that complement is one interval |

All three are monotonic and contracting, so the order-independence of ADR 0008 holds. The element propagator connects discrete state to quantities: a door's state selects the wall's conductance. The reification propagator connects quantities to discrete state: a temperature above the ignition point makes "burning" true. Instantiation rules (ADR 0011 section 7) will use it.

### 3. Kernels

- A slot whose dimension text is `atoms` binds to a set cell. The binding rejects any other combination.
- The host table gains three functions at its end. The table's size field records them, and ADR 0009 section 8 allows this without a new major version:
  - `atom(call, slot, name)` gives the index of an atom by name, or −1. A kernel looks names up once, at create, so it never depends on the order in which a world declares atoms.
  - `read_set(call, slot)` gives the mask.
  - `write_atom(call, slot, index)` writes a point. It follows the same rules as `write`: `OUT` and `STATE` slots only, and only during advance or map.
- A set slot cannot be a stock. Atoms are not conserved.

### 4. The world

The world declares enumeration kinds with their atom names, creates set cells by path, reads them, and takes inputs for them, beside the interval functions.

## Considered options

- **Encode atoms as integer intervals.** An interval over atom indices cannot represent "open or locked but not closed", because it has no holes. Narrowing would lose information. We rejected it.
- **A general domain interface with a function table for each domain.** It allows any domain later, but it costs an indirect call on every narrowing, also for intervals, which are the common case. Two built-in domains with a tag are enough now. We rejected the general interface for this step.
- **Unbounded sets.** No content needs more than 64 atoms in one kind yet. We reserved it.

## Consequences

- The change log, snapshots, and EXPLAIN treat a set cell like any other cell.
- The front end can lower `door: "open" | "closed"` to a set cell, and `conductance: { open: 20 W/K, closed: 10 W/K }[door]` (illustrative) to an element propagator.
