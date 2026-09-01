# Code Documentation and Clarity Standards

## 1. Method and Function Documentation
- Every function and method MUST have a preceding function comment describing:
  - The purpose of the function.
  - Its input arguments and their meaning/constraints.
  - Its return value (if any).
  - Any architectural context or hardware/emulator specifics.

## 2. In-Function Step Descriptions
- Inside function bodies, non-trivial lines and logic blocks MUST include single-sentence descriptions explaining what the code is doing and why.
- For opaque operations (e.g., stack manipulation, bitfield offsets, hardware register emulation, memory bank remaps, JIT barriers), provide explicit rationale and domain explanations.

## 3. High-Level Concept Blocks
- If a subsystem, module, or algorithm requires deeper context (e.g., 680x0 CPU context switches, JIT write-protect toggling, exception stack frames, return hook mechanics), add an expanded comment block above the relevant functions or at the top of the file.
