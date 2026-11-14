# Future Plan and Current Model Limits

## Honest Current Position

The project is a clean, deterministic, end-to-end research simulator. It is
strong enough for mechanism checks, exact accounting, algorithm comparisons
inside its modeled boundary, and sensitivity studies. It is not yet a
hardware-validated predictor of absolute training or inference performance.

There are three different kinds of correctness to keep separate:

1. **Software correctness:** Do dependencies, queues, credits, retries,
   reassembly, and counters behave as implemented? The current tests provide
   strong evidence for this.
2. **Model correctness:** Are the chosen abstractions appropriate for the
   hardware question? The project has explicit abstractions, but some remain
   deliberately simple.
3. **Empirical accuracy:** Do predicted nanoseconds and bottlenecks match real
   hardware measurements? This has not yet been established.

Lack of calibration does not mean the current simulator is simply “wrong.” It
means its exact internal results are conditional on the supplied parameters
and abstractions. Relative trends can be useful now; absolute performance
claims require validation.

## What “Synthetic GPU Execution” Means

The simulator does not ingest a compiled kernel, GPU instruction trace, or
profiler trace. Today:

- layer compute duration is supplied directly by the workload;
- each collective micro-operation becomes one synthetic communication kernel;
- that kernel becomes one synthetic workgroup;
- every workgroup receives the same configured wavefront count;
- issue time is derived from configured issue width and latency;
- local reduction time is `bytes / configured reduction bytes per ns`;
- finite CU slots create deterministic queuing and stall counters.

This is useful for asking questions such as, “Can limited residency delay the
issue of collective work?” It cannot answer, “How long will this real CUDA or
HIP kernel take?” It currently omits:

- real grid and block dimensions;
- per-kernel register and shared-memory occupancy;
- instruction mix and dependencies;
- cache, HBM, and accelerator-internal NoC traffic;
- memory-system contention between compute and communication;
- measured launch overhead and concurrent-kernel behavior.

The model should therefore be called a finite GPU resource model, not an ISA-
level or cycle-accurate GPU simulator.

## What “Hardware Calibration” Means

Calibration connects configuration values and simplified equations to a real
target system. For example, a nominal 200 Gb/s link does not by itself prove
that a 4096-byte transfer achieves the simulator's predicted latency. Real
systems may add protocol overhead, pipeline startup, alignment effects,
serialization details, arbitration delays, firmware behavior, or effective
bandwidth loss.

A defensible calibration process would:

1. Select a concrete target platform and software version.
2. Measure small point-to-point transfers across message sizes and concurrency
   levels.
3. Measure collective latency for several endpoint counts and algorithms.
4. Measure or obtain reliable values for link overhead, propagation, switch
   latency, buffer/credit behavior, retry cost, kernel launch, and reduction
   throughput.
5. Fit only the parameters intended to be configurable.
6. Freeze those parameters.
7. Validate against separate measurements that were not used for fitting.
8. Report prediction error and uncertainty, not only the best matching case.

Until then, values such as switch latency, credit depth, issue latency, and
reduction throughput are experiment assumptions. The simulator enforces their
consequences consistently, but does not prove that they describe a shipping
device.

## Other Important Current Limits

- One group contains every endpoint; there is no explicit TP/DP/PP/EP mapping.
- Native files cannot yet assign human-readable layer names.
- AICB import is intentionally restricted to the safe single-TP subset.
- Native planners cover ring AllReduce/ReduceScatter/AllGather, direct
  AllToAll, and a simple AllReduce baseline; they are not an algorithm library.
- PUT, GET, SIGNAL, WAIT, and REDUCE are typed contracts, but the production
  executor currently implements only PUT and REDUCE. This boundary is
  deliberate and must be closed before importing one-sided schedules.
- There is no real MSCCL++ JSON-plan parser or rank/buffer mapper.
- The topology is one scale-up domain with a compact rail/switch abstraction,
  not an arbitrary hierarchy of devices and switches.
- Deterministic targeted fault injection supports repeatable retry studies;
  it is not yet a statistical bit-error model.
- Synthetic LLM and MoE traces represent communication shapes, not measured
  end-to-end applications.

## Prioritized Future Checkpoints

Each checkpoint must retain strict builds, full regression, conservation
checks, deterministic output, and the zero-contamination audit.

### CP-16: Input Schema and Experiment Usability

Add a versioned schema reference, native layer names, explicit units, better
diagnostics, and a generated run manifest containing input hash, simulator
version, and resolved configuration.

Exit gate:

- old schema-version-1 inputs remain reproducible or receive a documented
  migration tool;
- invalid or ambiguous units fail before simulation;
- every report identifies the exact input and build used.

### CP-17: Real Parallel-Group Modeling

Represent explicit device membership for TP, DP, PP, and EP groups. Each
workload communication phase must reference a group rather than implicitly
using all endpoints. Add validation for overlap, membership, collective size,
and single-domain boundaries.

Exit gate:

- hand-checkable mixed TP/EP and TP/DP traces use only their declared ranks;
- bytes and operation counts match analytical expectations per group;
- unsupported cross-domain communication fails explicitly.

### CP-18: Complete One-Sided Execution Semantics

Implement runtime GET, SIGNAL, and WAIT rather than treating their existing
contracts as executable. Add registered buffer state, GET request/response
causality, semaphore values and generations, ordered data-before-signal
release, wait blocking, memory visibility, and explicit completion stages.

Exit gate:

- GET accounts separately for request and response traffic;
- a WAIT cannot pass before its matching SIGNAL generation;
- a SIGNAL cannot expose ordered data before remote visibility;
- stale signals, invalid buffers, and dependency cycles fail deterministically.

### CP-19: External Collective-Plan Import

Define a documented adapter boundary and parse a real, versioned external
plan—such as a supported MSCCL++-derived JSON schema—instead of creating a
class named “parser” that generates a hard-coded schedule. Map plan ranks to
the groups from CP-17 and plan buffers/dependencies to native contracts.

Exit gate:

- schema validation rejects unknown or incomplete operations;
- imported and equivalent native plans produce matching logical results;
- rank, buffer, semaphore, and dependency mappings are visible in provenance;
- the external library is not a runtime dependency of the SUE engine.

### CP-20: Trace-Driven GPU Execution

Extend workload phases with kernel records: kernel identity, grid/workgroup
count, wavefronts, resource usage, measured or modeled duration, memory bytes,
and dependencies. Start with profiler-derived summaries; do not jump directly
to an unvalidated instruction-level simulator.

Exit gate:

- multiple workgroups per real kernel contend for finite resources;
- supplied kernel timings are traceable to measurements or a named model;
- compute/communication overlap follows explicit stream/dependency data;
- disabling the detailed model preserves existing network-only baselines.

### CP-21: Memory-System and Compute/Communication Interference

Add only the contention needed by the research question: HBM bandwidth pools,
DMA/reduction traffic, and possibly a compact accelerator-internal NoC model.
Avoid adding cache or instruction details without evidence that they materially
change the target conclusions.

Exit gate:

- byte conservation holds across GPU memory and fabric movement;
- isolated and contended cases have hand-calculated bounds;
- the model makes no unsupported claim of full GPU microarchitecture fidelity.

### CP-22: Hardware Calibration and Validation

Create versioned platform profiles from real measurements or authoritative
published data. Separate fitted workloads from held-out validation workloads
and report absolute and percentage error.

Exit gate:

- every calibrated number has provenance;
- validation includes message-size, collective, endpoint-count, and contention
  sweeps;
- reports state the valid platform/software range and uncertainty;
- failed validation changes the model or narrows the claim rather than being
  hidden by retuning each experiment.

### CP-23: Research Experiments

Only after the relevant model is validated, study collective selection,
rail/switch sensitivity, credit sizing, failure/replay effects, TP/EP layouts,
and compute/communication overlap for measured workloads.

Exit gate:

- every plot is reproducible from source-controlled configuration;
- every metric maps to a documented counter;
- synthetic, modeled, calibrated, and measured results are labeled separately.

## Recommended Order

The most important next work is not adding more topology knobs. The preferred
order is:

```text
input provenance
  -> parallel groups
  -> executable GET/SIGNAL/WAIT semantics
  -> real external plan import
  -> trace-driven GPU kernels
  -> selected memory contention
  -> hardware calibration
  -> research conclusions
```

This order prevents a sophisticated-looking input or GPU model from resting
on incorrect rank mapping or incomplete one-sided synchronization.

## Project Assessment

The current codebase is a strong foundation because it is small, deterministic,
strictly tested, explicit about unsupported cases, and free of the legacy
multi-backend runtime. Its main scientific risk is not software instability;
it is overclaiming fidelity beyond the abstractions and evidence listed above.
Following the checkpoints in this document can improve fidelity without losing
the clean architecture that was just established.

