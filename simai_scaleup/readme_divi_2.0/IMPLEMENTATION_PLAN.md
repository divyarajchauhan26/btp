# Implementation Plan

## Strategy

Use a side-by-side replacement strategy:

```text
freeze legacy baseline
→ build independent typed contracts
→ build execution core without Chakra or SUE-Sim
→ add local memory and synchronization
→ integrate native scale-up backend
→ lower MSCCL++ plans
→ add compute kernels
→ add Chakra
→ add persistent SUE-Sim
→ migrate CLI and remove superseded paths
```

Every checkpoint must compile, have focused tests, and leave the repository in a
runnable state.

## Phase 0 — Baseline and Safety Rails

### 0.1 Inventory

- Record every current executable, test, fixture, and backend.
- Mark generated binaries/logs that should not be source-controlled.
- Capture current four-XPU and multi-layer reports.

### 0.2 Compatibility mode

- Introduce explicit `coarse` behavior without changing output.
- Reserve `fine` as unavailable until its first vertical slice works.

### 0.3 Testing split

- Preserve current regression tests.
- Create focused test targets for GPU contracts, scheduling, memory,
  synchronization, lowering, and integration.

### Gate

- Existing tests pass unchanged.
- Golden baseline reports are archived.
- No 2.0 code is yet on the production path.

## Phase 1 — Static Contracts

### 1.1 IDs and enums

Add strongly typed IDs for:

```text
GpuInstruction
GpuOperation
MemoryRequest
ComputeUnit
Barrier
KernelInstance
```

Add enums for memory space, instruction kind, operation kind, request kind,
stall reason, ordering scope, and scheduling policy.

### 1.2 Memory references

Implement validated local/remote references using owner, space, buffer, offset,
and length.

### 1.3 Static kernel IR

Implement immutable kernel, workgroup, wavefront-program, and operation
definitions.

### Tests

- Valid minimal kernel.
- Duplicate and zero IDs rejected.
- Invalid owner/space combinations rejected.
- Overflow and alignment checks.
- Static structures contain no runtime CU assignment.

### Gate

An in-memory fine-grained kernel can be constructed and validated.

## Phase 2 — Lazy Instruction Generation

### 2.1 Operation generators

Implement generators for:

- LoadOp;
- StoreOp;
- MemcpyOp;
- SemaphoreAcquireOp;
- SemaphoreReleaseOp;
- ReduceOp;
- barriers.

### 2.2 Cache-line splitting

- Support configurable line size.
- Handle unaligned start/end and partial final lines.
- Partition spans across wavefronts deterministically.

### 2.3 Memcpy sequencing

Baseline:

```text
Load → Waitcnt → Store
```

Add configurable unrolling without changing logical bytes.

### Tests and gate

- Exact instruction sequences for small spans.
- Correct counts for partial lines.
- Correct per-wavefront subranges.
- Lazy generation proven for large spans.

## Phase 3 — Dynamic GPU State and CU Scheduling

### 3.1 GPU/CU state

Add CU residency slots, ready queues, active workgroups, counters, and policy
state.

### 3.2 Workgroup assignment

- Deterministic round-robin reference implementation.
- Queue when all slots are occupied.
- Retirement immediately enables waiting work.

### 3.3 Optional policies

Add seeded-random only after round-robin tests pass.

### Tests and gate

- Exact assignment sequence.
- Capacity invariants.
- Fairness/progress.
- Seed reproducibility.
- Multiple workgroups execute and retire without instructions that access
  memory.

## Phase 4 — Wavefront Engine

### 4.1 Runtime state

Track operation cursor, instruction generator state, ready/stalled/completed
state, stall reason, and outstanding counts.

### 4.2 Scheduling

- Select ready wavefronts.
- Respect issue width and latency.
- Skip stalled wavefronts.
- Preserve per-wavefront order.

### 4.3 Completion hierarchy

Retire wavefront, workgroup, and kernel at their correct boundaries.

### Tests and gate

- Interleaving of two wavefronts.
- One stalled wavefront does not block another.
- Completion conservation.
- A synthetic kernel executes to completion without memory backends.

## Phase 5 — Outstanding Requests, Waitcnt, and Reduce

### Work

- Add request-class counters per wavefront.
- Implement Waitcnt threshold checks and wake-ups.
- Implement modeled Reduce duration and CU occupancy.
- Add event kinds for instruction issue and completion.

### Tests and gate

- Out-of-order completions.
- Waitcnt threshold edge cases.
- Reduce contention timing.
- No early wake-up.

## Phase 6 — Local Memory System

### 6.1 Interface

Create asynchronous local-memory request/completion contracts.

### 6.2 Initial backend

Implement configurable analytical tiers with latency, service bandwidth, queue
capacity, and outstanding limits.

### 6.3 Routing

Route by typed memory space. Do not silently redirect unsupported spaces.

### Tests and gate

- Analytical single-access timing.
- Queue contention timing.
- Backpressure behavior.
- Per-tier request/byte conservation.
- Local requests never enter SUE.

## Phase 7 — Synchronization Engine

### 7.1 Semaphores

- Owner, ID, generation, location.
- Acquire wait lists indexed by semaphore.
- Release visibility and wake-up.

### 7.2 Barriers

- Workgroup barrier participants.
- Explicit inter-workgroup participant sets.
- Reusable barrier generations where required.

### 7.3 Deadlock diagnostics

Detect no-progress states and produce blocked-state explanations.

### Tests and gate

- Generation correctness.
- Multiple waiters.
- Barrier reuse.
- Missing release/barrier participant diagnostics.

## Phase 8 — Semantic Scale-Up Requests

### Work

- Define request above the PDU layer.
- Convert remote Load/Store/acquire/release instructions.
- Preserve full provenance.
- Model remote loads as request plus response.
- Respect requested completion stage.

### Initial backend

Adapt to the existing native SUE backend first.

### Tests and gate

- Remote store visibility.
- Remote load response.
- Remote semaphore release/acquire.
- Request, byte, and completion conservation.
- Two- and four-GPU tests without hardcoded endpoint assumptions.

## Phase 9 — MSCCL++ Kernel Lowering

### Work

- Preserve per-GPU and per-workgroup structure.
- Lower PUT/GET/COPY into memory operations.
- Lower SIGNAL/WAIT into semaphore operations.
- Lower REDUCE into ReduceOp.
- Preserve program order and explicit synchronization.
- Reject unsupported plan features clearly.

### Plan generation boundary

The importer is not a collective synthesizer. Add a plan registry so high-level
collective nodes select pre-generated plans by collective type, participant
count, protocol, topology class, and payload/chunk constraints.

### Tests and gate

- Hand-audited two- and four-GPU fixtures.
- Official-style generated fixtures when available.
- Provenance and operation-count checks.
- One communication kernel executes through the fine GPU and native SUE paths.

## Phase 10 — Compute-Kernel Lowering

### Work

- Define a compute-kernel descriptor/template format.
- Support workgroup count, wavefront count, memory spans, operation sequences,
  and arithmetic duration.
- Add optional seeded statistical fields only when detail is unavailable.
- Record generated values.

### Tests and gate

- Deterministic template expansion.
- Compute and communication kernels compete for the same CUs.
- Resource capacities remain valid during overlap.

## Phase 11 — Kernel Graph and Chakra Frontend

### 11.1 Internal kernel graph

Build and test frontend-neutral node/dependency structures first.

### 11.2 Chakra reader

- Prefer official schema/protobuf support.
- Version the accepted schema.
- Preserve rank, node ID, name, type, and dependencies.
- Map compute nodes to compute descriptions.
- Map collective nodes to plan-registry queries.
- Reject unsupported nodes explicitly.

### Sidecar decision

If Chakra cannot carry all required kernel detail cleanly, use a versioned
sidecar referenced by node ID rather than creating incompatible Chakra fields
prematurely.

### Tests and gate

- Official small ET parsing.
- Graph cycle and missing-edge diagnostics.
- Multi-rank consistency.
- Compute → communication → compute execution.

## Phase 12 — Persistent SUE-Sim Integration

### Work

- Build one topology/session per simulation.
- Use one asynchronous ns-3 event loop.
- Inject multiple concurrent requests.
- Return request-level completion callbacks.
- Preserve ports/rails and request provenance.
- Export CBFC, LLR, queues, retries, and rail counters.

### Tests and gate

- Logical equivalence against native backend.
- Concurrent requests overlap.
- Multi-rail behavior.
- Retry/failure behavior.
- No two-GPU assumptions.

## Phase 13 — Reporting and Debugging

### Required outputs

- kernel graph timeline;
- CU assignment and occupancy;
- workgroup queue and lifetime;
- wavefront instruction timeline;
- stalls by reason;
- local/remote bytes by memory tier;
- semaphore/barrier state;
- request latency decomposition;
- transport counters;
- selectable provenance table.

### Gate

Every aggregate counter can be reconciled against lower-level counters.

## Phase 14 — Validation and Calibration

### Correctness before accuracy

Validate ordering, synchronization, conservation, and completion first.

### External validation

- Compare semantics with ASTRA-sim 3.0 descriptions/examples.
- Compare plan lowering with official MSCCL++ plans.
- Use measured or published data when available.
- Report assumptions and error instead of claiming universal accuracy.

### Scalability

Measure host runtime and memory for increasing GPUs, workgroups, payloads, and
cache-line counts.

## Phase 15 — Migration and Deletion

### Work

- Add final unified CLI modes.
- Split configuration ownership.
- Migrate documentation and examples.
- Apply deletion gates from the migration matrix.
- Remove duplicate runtime logic and accidental binaries/logs separately.

### Final gate

- Fine mode is fully tested and documented.
- Coarse mode is either intentionally retained or cleanly removed.
- No supported path depends on superseded direct MicroOp packetization.

## First Vertical Slice

Build before Chakra or full SUE-Sim:

```text
2 GPUs
2 CUs/GPU
2 workgroups/GPU
2 wavefronts/workgroup
256-byte remote MemcpyOp
128-byte cache line
one remote semaphore release/acquire
native SUE backend
```

The demonstration must show deterministic CU assignments, wavefront switching
during a semaphore stall, cache-line requests, remote completion, and complete
counter conservation.

