# SimAI Scale-Up Clean-Room Implementation Plan

> **Resume notice (2026-10-05):** The user resumed this plan and requested
> continuous checkpoint-by-checkpoint progress. CP-00 through CP-15 are
> complete, including final prototype retirement. See
> [`context.md`](context.md) for the verified handoff state.

## 1. Objective

Build a self-contained, end-to-end AI workload simulator for intra-node
scale-up research. The simulator will retain only the useful workload,
compute, grouping, collective, scheduling, and reporting concepts from SimAI.
All communication will execute through a native Scale-Up Ethernet (SUE)
backend.

This repository starts from the official Alibaba Cloud SimAI commit:

`a57d765c5478f319c5f5ab6fdc53f9cc7f461743`

The earlier `simai_fresh` tree is reference material only. Code must not be
copied from it without a file-level review.

## 2. Non-Negotiable Architecture Rules

1. The supported simulated system is one scale-up domain, not a cluster.
2. SUE is the only network/fabric backend.
3. The core execution interface is one-sided and memory-oriented.
4. A legacy send/receive API must not sit underneath PUT/GET operations.
5. No compatibility layer will be retained merely to preserve upstream SimAI.
6. Every retained source file must be reachable from the final build or have a
   documented non-runtime purpose.
7. Generated binaries, logs, and results must not live in source directories.
8. Claims such as cycle accuracy, zero-copy, or hardware fidelity require a
   test and a documented modeling assumption.

## 3. Forbidden Runtime Concepts

The final source and build graph must contain no implementation of:

- scale-out or multi-node networking;
- RDMA queue pairs or completion queues;
- RoCE, InfiniBand, IP, TCP, UDP, PFC, ECN, or DCQCN;
- NS-3, HTSim, Garnet, physical socket, or analytical network backends;
- MPI/bootstrap coordination;
- rendezvous send/receive protocols;
- empirical NVLink ratio-table timing;
- runtime backend selection.

Historical migration documents may name removed concepts, but production
documentation, configuration, examples, and CLI help must describe only the
new simulator.

## 4. Target Pipeline

```text
Model/AICB workload
        |
        v
Layer and compute timeline
        |
        v
TP/EP/DP group and collective planner
        |
        v
Scale-up micro-op program
PUT / GET / SIGNAL / WAIT / REDUCE
        |
        v
SUE transaction interface
        |
        v
SUE event engine
flits / rails / switch / credits / retry / reassembly
        |
        v
End-to-end and per-layer reports
```

## 5. Work Phases and Exit Gates

### Phase 0: Freeze and Baseline

Tasks:

- Preserve `simai_fresh` unchanged as a reference.
- Record the upstream commit and submodule revisions.
- Do not initialize unapproved network submodules.
- Establish separate `src`, `configs`, `tests`, `examples`, and `results`
  responsibilities.

Exit gate:

- Clean Git baseline is reproducible.
- No old prototype file has entered the new build.

### Phase 1: Complete Dependency Audit

Classify every relevant upstream backend file as:

- KEEP unchanged;
- REWRITE for scale-up semantics;
- REFERENCE only;
- REMOVE.

Audit at minimum:

- workload parsing and layer execution;
- `Sys` and event scheduling;
- collective algorithms and logical topologies;
- network API boundaries;
- build scripts and compile definitions;
- configuration readers;
- SimCCL and AICB integration points;
- examples, tests, and documentation.

Deliverables:

- `FILE_AUDIT.md` with a row for every backend/system source file.
- Include/dependency graph for the proposed retained build.
- A minimal feature list for the first working vertical slice.

Exit gate:

- No file is removed or ported without a recorded classification and reason.

### Phase 2: Define Native Simulator Contracts

Define small interfaces before implementing the fabric:

- `WorkloadEvent`: compute or collective activity for a model layer.
- `ParallelGroup`: ranks belonging to TP, EP, or DP within one scale-up domain.
- `CollectivePlan`: algorithm-independent collective request.
- `MicroOp`: PUT, GET, SIGNAL, WAIT, or REDUCE.
- `SueTransaction`: fabric-visible transfer or control operation.
- `Completion`: event returned to the scheduler.
- `SimulationReport`: per-layer and end-to-end metrics.

Required semantic decisions:

- address-space ownership;
- buffer lifetime;
- dependency and semaphore rules;
- reduction timing and resource use;
- concurrent threadblock scheduling;
- whether DP groups larger than the scale-up domain are rejected or split
  before simulation.

Exit gate:

- Unit tests demonstrate operations without using legacy send/receive calls.

### Phase 3: Minimal End-to-End Vertical Slice

Implement one narrow path first:

1. Parse a two-layer workload.
2. Apply compute delays.
3. Create an eight-device TP group.
4. Lower one AllReduce into explicit micro-operations.
5. Pass operations through a simple deterministic SUE link model.
6. Produce per-layer and end-to-end timing.

Do not add detailed retry, switch, or congestion behavior yet.

Exit gate:

- A deterministic golden test passes from workload file to final report.
- Hand-calculated serialization time matches simulation output.

### Phase 4: SUE Fabric Fidelity

Add and validate one feature at a time:

1. Flit format and serialization.
2. Multiple physical rails and rail selection.
3. Switch ingress, arbitration, egress queues, and finite buffers.
4. Credit accounting and actual backpressure.
5. Destination placement and reassembly.
6. Link error detection and bounded replay.
7. Concurrent traffic and resource contention.

Each feature requires:

- a written assumption;
- a focused unit test;
- an analytical expected result;
- counters proving that the modeled mechanism was exercised.

Exit gate:

- Queue occupancy drains correctly.
- Buffer limits affect scheduling.
- Conservation checks pass for bytes, flits, credits, and completions.

### Phase 5: Collective and Workload Coverage

Before detailed GPU-resource work, complete checkpoint CP-09A as specified in
[`COLLECTIVE_PLANNER_AND_GPU_PIPELINE.md`](COLLECTIVE_PLANNER_AND_GPU_PIPELINE.md):

- replace the vertical-slice AllReduce entry point with a general planner
  interface and explicit `CollectiveRequest`;
- record native, adapted, or imported schedule provenance;
- translate reviewed algorithm mathematics rather than retaining MockNCCL's
  flow executor;
- establish ring AllReduce as the first non-baseline generator;
- keep MSCCL++ as an optional importer into the common native IR.

Add in this order:

1. AllReduce;
2. ReduceScatter and AllGather;
3. AllToAll for expert parallelism;
4. Broadcast and point-to-point memory operations only if required by real
   workloads;
5. full AICB trace support;
6. overlapping compute and communication.

Exit gate:

- Every supported collective has tiny, medium, and contention-heavy tests.
- Unsupported workload operations fail explicitly rather than falling back.

### Phase 6: Zero-Contamination Audit

Run automated checks across tracked files and the compiled dependency graph.

Checks include:

- forbidden vocabulary outside the historical migration record;
- forbidden headers and compile definitions;
- unexpected submodules;
- unreachable/dead backend files;
- legacy network CLI options;
- generated artifacts committed beside source;
- binary symbol and linkage inspection.

Exit gate:

- Audit script reports zero unexplained violations.
- A clean checkout builds and runs the golden test.

### Phase 7: Research Validation

Workloads:

- micro AllReduce;
- micro AllToAll;
- LLaMA tensor-parallel trace;
- MoE expert-parallel trace.

Sweeps:

- message size;
- device count within one scale-up domain;
- rails and per-rail bandwidth;
- switch latency and buffer capacity;
- credit depth and return latency;
- error rate and replay latency.

Validation:

- lower-bound serialization calculations;
- topology/algorithm byte-count checks;
- bandwidth never exceeding the applicable physical cut;
- deterministic fixed-seed runs;
- sensitivity trends with documented explanations.

Exit gate:

- Results are reproducible from one documented command.
- Every plotted metric has an exact simulator counter definition.

## 6. Change Protocol

For every implementation step:

1. State the behavior being introduced or removed.
2. Identify affected files and dependency edges.
3. Add or update a test first where practical.
4. Make the smallest coherent change.
5. Build from a clean state.
6. Run relevant unit and end-to-end tests.
7. Run the forbidden-dependency audit.
8. Update the implementation log with evidence, limitations, and next action.

No milestone is complete merely because the program compiles.

## 7. Completion State

The planned clean-room implementation and retirement sequence is complete.
The unique prototype artifacts were reviewed, useful lessons were preserved
in [`CP15_PROTOTYPE_RETIREMENT.md`](CP15_PROTOTYPE_RETIREMENT.md), every final
gate passed, and `simai_fresh` was removed. Future work should begin as a new,
explicitly scoped checkpoint rather than reopening the completed migration.
