# SimAI Scale-Up 2.0 Documentation

This directory is the authoritative planning space for the fine-grained,
scale-up-focused redesign of `simai_scaleup`.

The redesign may replace substantial parts of the current runtime. Existing
code is not assumed to be correct for the new objective merely because it
already builds. Every component must be classified, tested, and either reused,
adapted, isolated, replaced, or removed.

## Primary Objective

Build an event-driven GPU execution model for one scale-up domain:

```text
kernel graph
  -> per-GPU kernels
  -> workgroups
  -> wavefronts
  -> GPU operations
  -> Load/Store/control instructions
  -> local or remote cache-line requests
  -> SUE for remote traffic
  -> completions and wake-ups
```

The first target is Load/Store-semantic fidelity, not real GPU ISA or binary
execution.

## Documentation Map

- [`ASTRA_3_FINE_GRAINING_ROADMAP.md`](ASTRA_3_FINE_GRAINING_ROADMAP.md):
  evidence-based ASTRA-sim 3.0 comparison, complete CCL/MSCCL++, GPU, memory,
  topology, trace, SUE, reporting, calibration, and implementation roadmap.
- [`SCALEUP_GPU_MODEL_THEORY_AND_PLAN.md`](SCALEUP_GPU_MODEL_THEORY_AND_PLAN.md):
  beginner-oriented theory, honest current boundary, target data flow, and the
  prioritized implementation roadmap.
- [`SYNTHETIC_TRACE_GUIDE.md`](SYNTHETIC_TRACE_GUIDE.md): completed synthetic
  timestamped Load/Store format, validation rules, direct and Chakra-driven
  execution, examples, and current boundary.
- [`REQUIREMENTS.md`](REQUIREMENTS.md): scope, terminology, functional
  requirements, and non-goals.
- [`BASELINE.md`](BASELINE.md): frozen coarse behavior, golden results, and
  generated-artifact inventory.
- [`STATUS.md`](STATUS.md): completed checkpoints and current implementation
  state.
- [`ARCHITECTURE.md`](ARCHITECTURE.md): proposed layers, ownership, data model,
  and complete execution flow.
- [`CODEBASE_MIGRATION_MATRIX.md`](CODEBASE_MIGRATION_MATRIX.md): what happens to
  every major existing subsystem.
- [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md): ordered checkpoints,
  tests, exit gates, and deletion gates.
- [`OPEN_DECISIONS.md`](OPEN_DECISIONS.md): questions requiring research or PhD
  confirmation before semantics are frozen.
- [`DECISION_LOG.md`](DECISION_LOG.md): accepted architectural decisions and
  future corrections.

## Planning Rules

1. Do not rewrite everything in one patch.
2. Do not force the new model through unsuitable legacy abstractions.
3. Preserve a runnable legacy baseline until the replacement passes equivalent
   correctness tests.
4. Separate static workload/kernel descriptions from dynamic simulator state.
5. Keep local GPU/memory behavior separate from remote SUE transport behavior.
6. Make every probabilistic choice seeded and reproducible.
7. Require request, byte, instruction, and completion conservation tests.
8. Remove old code only after an explicit deletion gate passes.
9. Never claim binary-level or hardware-level accuracy without validation.
10. Prefer small auditable vertical slices before broad feature support.

## Current Status

The fine-model foundations, MSCCL++ lowering, compute templates, native fine
runtime, neutral kernel graph, and Chakra ET importer are present. The
synthetic trace path now executes timestamped, sized local/remote Load and
Store records either directly or as Chakra-node bindings. Persistent
asynchronous SUE-Sim execution, explicit internal GPU topology, hardware-trace
correlation, and calibration remain separate research work. See
[`STATUS.md`](STATUS.md) for checkpoint evidence and
[`SCALEUP_GPU_MODEL_THEORY_AND_PLAN.md`](SCALEUP_GPU_MODEL_THEORY_AND_PLAN.md)
for the next implementation sequence.
