---
status: accepted
---

# Quadrilateral is a successor to Triangle, written as new C code

Triangle has parts that contradict Quadrilateral's goals: a non-incremental settling pass, per-node scratch that blocks persistent links, one scratch arena for each loaded content, a text-shaped API, and no kernel interface (`quadrilateral-review-fable.md` section 2.4). A fork would start with those parts and then remove them. We decided that Quadrilateral is a new C11 codebase that copies code from Triangle only where it fits (decisions D1, D2, and D3).

## Decision

- Quadrilateral is new code. When a Triangle file or function fits the design, we copy it. The file header names the Triangle source and revision, and from then on Quadrilateral owns the copy.
- Quadrilateral does not track upstream Triangle. Fixes are not merged in either direction.
- Compatibility with Triangle source files is not a goal. The syntax stays close to CUE, as Triangle's does, but Triangle content does not need to load. Where compatibility costs nothing, we keep it.
- The language is C11. The code uses the `__int128` extension of Clang and GCC.
- The kernel interface is a C interface with opaque handles and identifiers. No structure layout from an internal header appears in it. For example, a kernel refers to a quantity type by an identifier (ADR 0001). This rule keeps other host languages possible later: BEAM, Go, Odin, or Quadrilateral itself.

## Considered options

- **A fork of Triangle with its history.** It would give a working parser, type hierarchy, and unifier at once. Most of the early phases (propagation, kernel interface, scheduler) do not use those parts, and the parts that conflict with the design would need removal. We rejected the fork.
- **Odin, the language of Bruggadung's runtime.** The propagator engine touches code that comes from Triangle's C internals, and a C interface is the most portable boundary. We rejected Odin.
