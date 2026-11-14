# Fine-Grained Scale-Up GPU Model: Implementation Plan

## 1. Objective

Build a deterministic, event-driven GPU execution model focused specifically on
scale-up communication. The model must translate compute and collective kernels
into workgroups, wavefronts, logical GPU operations, and cache-line-sized
instructions; schedule them on finite compute units; model local and remote
memory accesses and synchronization; and send only remote transfers into the
SUE transport backend.

The target abstraction is Load/Store semantic simulation. It is not
instruction-set or GPU-binary simulation.

The intended final path is:

```text
Chakra or native kernel graph
  -> compute or MSCCL++ kernel parser
  -> common fine-grained kernel IR
  -> GPU/CU/workgroup/wavefront execution model
  -> Load/Store/Control instruction engine
  -> local memory path or remote scale-up path
  -> SUE-Sim
  -> completion and wake-up
  -> kernel/workload report
```

## 2. Scope

### In scope

- One scale-up domain containing multiple GPUs/XPUs.
- Compute and collective communication kernels.
- Per-GPU compute units and configurable resident workgroup capacity.
- Workgroups, wavefronts, and operation program order.
- Dynamic workgroup-to-CU assignment.
- Ready/stalled wavefront scheduling.
- Primitive Load, Store, SemaphoreAcquire, SemaphoreRelease, Reduce, and
  Waitcnt instructions.
- LoadOp, StoreOp, MemcpyOp, SemaphoreAcquireOp, SemaphoreReleaseOp,
  ReduceOp, WorkgroupBarrierOp, and KernelBarrierOp abstractions.
- Register-facing instruction semantics and configurable memory spaces.
- L1, L2, HBM, remote HBM, and semaphore/control memory as model-visible
  targets where configured.
- Cache-line request generation.
- Local memory latency/bandwidth/queue modeling.
- Remote request injection into SUE.
- Semaphores, barriers, outstanding-request counters, and memory fences.
- Deterministic round-robin and seeded stochastic scheduling policies.
- Deadlock diagnostics and per-resource utilization reports.
- Chakra ET as a kernel-graph frontend.
- MSCCL++ plans as communication-kernel descriptions.

### Explicit non-goals for the first implementation

- Decoding or executing real GPU ISA binaries.
- Modeling every register or arithmetic instruction.
- Scale-out NIC/network behavior.
- Host CPU, PCIe, storage, or distributed job scheduling.
- Full cache coherence protocol simulation.
- Functional validation of numerical tensor values.
- Exact vendor GPU reproduction.
- Reimplementing SUE-Sim's links, switches, CBFC, LLR, or multi-rail logic.

## 3. Architectural Decisions

### 3.1 Keep the work in `simai_scaleup`

The implementation should be a new subsystem inside `simai_scaleup`, not a
fork of ASTRA-sim 2.0. Existing reusable pieces include:

- deterministic event queue;
- typed IDs and validation patterns;
- buffer ownership and offsets;
- collective and MSCCL++ frontends;
- SUE transport abstraction;
- completion tracking;
- reporting and regression tests.

The existing `GpuExecutionModel` is a useful baseline but should not become the
new architecture through repeated special cases. It currently lowers one
micro-operation into one workgroup, allocates a slot, estimates wavefront issue
time, and adds reduction duration. It has no executable instruction stream or
wavefront scheduler.

### 3.2 Separate trace, static IR, and dynamic state

Do not store runtime CU assignments in Chakra or the static kernel definition.

```text
Trace/kernel description
  what kernels and dependencies exist

Static GPU IR
  workgroups, wavefront programs, operations, instructions, memory spans

Dynamic GPU state
  actual CU assignment, program counters, stalls, outstanding requests
```

This separation enables the same kernel to run under different GPU
configurations and scheduling policies.

### 3.3 Use a common kernel IR

Compute and communication kernels must lower into the same representation.
This is required to model contention naturally when both kinds of kernels are
resident simultaneously.

### 3.4 Keep SUE as the remote transport authority

The GPU model decides that an instruction requires remote access and creates a
scale-up memory request. SUE decides network behavior: rail selection,
switching, CBFC, LLR, transmission, and destination delivery.

### 3.5 Determinism first

Round-robin scheduling is the reference policy. Any probabilistic policy must
use the workload seed and produce repeatable output. Random scheduling without
reproducibility is unsuitable for correctness tests.

## 4. Proposed Source Layout

```text
native/include/simai/gpu/
  instruction.hpp
  operation.hpp
  kernel.hpp
  scheduler.hpp
  execution_model.hpp
  state.hpp

native/include/simai/memory/
  address.hpp
  request.hpp
  hierarchy.hpp
  local_backend.hpp

native/include/simai/synchronization/
  semaphore.hpp
  barrier.hpp
  wait_counter.hpp

native/include/simai/workload/
  kernel_graph.hpp
  chakra_importer.hpp
  kernel_lowering.hpp

native/include/simai/scaleup/
  request.hpp
  backend.hpp
  sue_adapter.hpp

native/src/gpu/
native/src/memory/
native/src/synchronization/
native/src/workload/
native/src/scaleup/
```

Existing flat files can remain during migration. Move code only when its new
ownership is established; avoid a large rename-only patch.

## 5. Core Data Model

### 5.1 Memory spaces

Introduce a typed memory-space enum:

```text
RegisterFile
L1
L2
LocalHbm
RemoteHbm
SemaphoreMemory
```

Each memory reference should include:

```text
owner device
memory space
buffer ID or virtual base address
byte offset
length
alignment
access permissions
```

Avoid requiring literal physical addresses initially. A typed
`buffer + offset` representation is easier to validate and can later be mapped
to addresses.

### 5.2 Primitive instructions

```text
Load
  memory source -> CU/register-facing destination

Store
  CU/register-facing source -> memory destination

SemaphoreAcquire
  read/check generation; stall if unsatisfied

SemaphoreRelease
  write/increment generation and wake matching waiters

Reduce
  occupy CU arithmetic resources for a modeled duration

Waitcnt
  stall until matching outstanding requests are at/below a threshold
```

Every instruction requires:

```text
instruction ID
source operation provenance
issuing device/workgroup/wavefront
instruction kind
memory reference where relevant
byte count
ordering scope
completion requirement
```

### 5.3 GPU operations

Operations are static instruction templates rather than timing shortcuts:

```text
LoadOp       -> repeated Load instructions
StoreOp      -> repeated Store instructions
MemcpyOp     -> repeated Load, Waitcnt, Store sequences
AcquireOp    -> SemaphoreAcquire
ReleaseOp    -> SemaphoreRelease
ReduceOp     -> Reduce
WgBarrierOp  -> intra-workgroup synchronization
KernelBarrierOp -> inter-workgroup synchronization
```

Large spans should be lazily expanded by cache line rather than materializing
millions of instruction objects in memory.

### 5.4 Execution hierarchy

```text
GpuKernel
  device
  dependency metadata
  workgroups[]

Workgroup
  resource requirements
  wavefronts[]
  barrier state

Wavefront
  operation sequence
  operation/instruction cursor
  ready/stalled/completed state
  outstanding request counters
```

### 5.5 Wavefront/scale-up request

The cache-line request crossing the memory/backend boundary should contain:

```text
request ID
instruction ID and operation provenance
source GPU and CU
workgroup and wavefront IDs
source and destination memory references
request kind: load, store, acquire, or release
payload bytes
cache-line offset
ordering domain
completion stage
issue timestamp
```

This request is richer than the current PDU. Packetization remains a transport
concern after the request enters SUE.

## 6. End-to-End Execution Semantics

### 6.1 Kernel readiness

The workload engine releases a kernel only when its Chakra/native graph
dependencies are satisfied.

### 6.2 Workgroup dispatch

The GPU scheduler examines free CU residency slots and assigns ready
workgroups. The initial reference policy is deterministic round-robin over CUs.

### 6.3 Wavefront selection

Each cycle/event, a CU selects a ready resident wavefront. A stalled wavefront
is skipped. Selection policy is configurable but deterministic by default.

### 6.4 Instruction issue

- `Reduce` reserves CU execution capacity until completion.
- Local `Load/Store` enters the configured local memory hierarchy.
- Remote `Load/Store` becomes a scale-up request.
- `SemaphoreAcquire` checks or requests the semaphore value and may stall.
- `SemaphoreRelease` updates the owner and wakes eligible acquires.
- `Waitcnt` checks the wavefront's outstanding request classes and may stall.

### 6.5 Local memory path

Model each configured tier with at least:

```text
latency
bandwidth/service rate
maximum outstanding requests
queue depth
cache-line size
```

The first implementation may use analytical fixed-latency queues. It must
still preserve contention and backpressure.

### 6.6 Remote scale-up path

Remote access follows:

```text
issuing CU
  -> optional local cache/HBM read
  -> GPU I/O boundary
  -> SUE request/PDU path
  -> destination GPU I/O boundary
  -> destination memory tier
  -> completion callback
```

A remote store completes at the completion stage requested by the program,
such as local issue or remote visibility. A remote load completes only when the
response reaches the requesting GPU/CU.

### 6.7 Completion propagation

```text
request completion
  -> decrement wavefront outstanding count
  -> release Waitcnt if eligible
  -> complete primitive instruction
  -> advance operation
  -> advance wavefront
  -> retire workgroup when all wavefronts finish
  -> retire kernel when all workgroups finish
  -> release dependent workload kernels
```

## 7. Better-Than-Baseline Research Features

These features should be designed for, but implemented only after the correct
baseline exists:

### 7.1 Explicit provenance

Every generated request should be traceable back through:

```text
SUE PDU
-> cache-line request
-> primitive instruction
-> GPU operation
-> workgroup/kernel
-> Chakra node or MSCCL++ operation
```

This enables debugging and research attribution.

### 7.2 Mechanism-aware synchronization

Retain both the logical dependency and its mechanism:

```text
program order
remote visibility
semaphore generation
Waitcnt threshold
workgroup barrier
kernel barrier
```

Do not convert every dependency into a semaphore.

### 7.3 Memory-placement experiments

Allow the same operation to be evaluated with data in L1, L2, local HBM, or
remote HBM. Report latency components separately.

### 7.4 Deterministic stochastic modeling

Kernel templates may specify distributions for workgroup counts, compute
duration, or memory locality when traces lack details. Samples must be seeded,
reported, and reproducible.

### 7.5 Deadlock explanations

On lack of progress, report:

```text
blocked GPU/CU/workgroup/wavefront
current operation/instruction
required semaphore generation or barrier participants
outstanding request IDs
last completion timestamp
```

### 7.6 Dual-fidelity mode

Retain the existing coarse mode and add the fine mode:

```text
--gpu-model coarse
--gpu-model fine
```

This supports scalability studies and regression comparison.

## 8. Detailed Implementation Checkpoints

## CP0 — Freeze and Characterize the Existing Baseline

### Work

- Preserve current native and MSCCL++ tests.
- Capture golden reports for two-, four-, and eight-XPU examples.
- Document which current fields are coarse-only.
- Add a CLI/config selection for `coarse` versus future `fine` mode without
  changing behavior.

### Verification

- Existing test suite remains unchanged and passing.
- Coarse mode produces byte-identical existing reports.

### Exit criterion

The current simulator remains a trustworthy fallback throughout development.

## CP1 — Typed Fine-Grained Contracts

### Work

- Add IDs for GPU instruction, GPU operation, memory request, and barrier.
- Add memory-space and request-kind enums.
- Add validated memory references.
- Add immutable kernel/workgroup/wavefront/operation definitions.
- Add dynamic state enums separately.

### Verification

- Unit tests reject zero/duplicate IDs, invalid owners, zero lengths,
  out-of-range memory tiers, and invalid remote/local combinations.
- No transport or scheduling code is introduced yet.

### Exit criterion

A fine-grained kernel can be represented and validated in memory.

## CP2 — Lazy Operation-to-Instruction Expansion

### Work

- Implement instruction generators for all baseline operations.
- Split memory spans into configurable cache-line requests.
- Implement `MemcpyOp` ordering and configurable loop unrolling.
- Preserve source-plan provenance.

### Verification

- A 1,024-byte MemcpyOp with 128-byte lines produces eight logical loads and
  eight logical stores.
- Unrolling changes allowed in-flight ordering but not total bytes.
- Partial final cache lines are handled correctly.

### Exit criterion

Operations deterministically expose the correct primitive instruction stream.

## CP3 — CU Residency and Workgroup Scheduler

### Work

- Represent GPUs containing configurable CUs.
- Represent workgroup slots per CU.
- Implement deterministic round-robin assignment.
- Implement a seeded-random policy behind the same interface.
- Queue workgroups when capacity is exhausted.

### Verification

- Exact CU assignments are asserted for deterministic examples.
- No CU exceeds resident-workgroup capacity.
- Fixed seeds reproduce stochastic assignments.
- Different seeds may alter assignment but not correctness invariants.

### Exit criterion

Multiple workgroups contend for and retire from finite CUs correctly.

## CP4 — Wavefront Execution Engine

### Work

- Add wavefront program counters and states.
- Implement ready-wavefront selection.
- Implement instruction issue width and issue latency.
- Allow another wavefront to run when one stalls.
- Retire workgroups only after all wavefronts finish.

### Verification

- A stalled wavefront does not block another ready wavefront on the same CU.
- Program order is preserved per wavefront.
- Workgroup/kernel completion conservation holds.

### Exit criterion

A no-memory synthetic kernel executes through workgroups and wavefronts.

## CP5 — Reduce, Waitcnt, and Outstanding Requests

### Work

- Implement CU occupancy duration for Reduce.
- Track outstanding requests by wavefront and class.
- Implement Waitcnt threshold semantics.
- Wake stalled wavefronts on request completion.

### Verification

- Waitcnt does not release early.
- Out-of-order request completions update counts correctly.
- Reduce contention produces analytically expected timing.

### Exit criterion

Non-memory execution and request-dependent stalls work without a network.

## CP6 — Local Memory Hierarchy

### Work

- Add local memory backend interface.
- Implement configurable L1, L2, HBM service stages.
- Add finite queues, outstanding limits, latency, and bandwidth.
- Add simple placement/routing rules.
- Return asynchronous completions to the wavefront engine.

### Verification

- Single-request latency matches analytical expectations.
- Concurrent requests show expected contention.
- Local traffic never enters SUE.
- Request and byte conservation holds per tier.

### Exit criterion

Local Load/Store operations execute with modeled memory contention.

## CP7 — Semaphore and Barrier Semantics

### Work

- Add semaphore owner, ID, generation, and memory location.
- Implement acquire/release and waiter indexing.
- Model control operations as one configurable cache-line access.
- Implement intra-workgroup barrier.
- Implement inter-workgroup/kernel barrier with explicit participant sets.
- Add deadlock/progress detector.

### Verification

- Old generations cannot satisfy newer acquires.
- Wavefront-zero-only control issue works when configured.
- Barriers release exactly when all required participants arrive.
- Missing releases produce actionable deadlock output.

### Exit criterion

Control-path operations behave correctly before remote transport integration.

## CP8 — Scale-Up Request and Native Backend Adapter

### Work

- Convert remote Load/Store/control instructions into typed scale-up requests.
- Adapt requests to the current native SUE transaction/PDU backend.
- Preserve request and instruction provenance through completion.
- Model remote load as request and response.
- Honor local-complete versus remote-visible release stages.

### Verification

- Remote store, remote load, signal, and wait tests pass.
- Cache-line, operation, and collective byte conservation holds.
- Local and remote latency components are separately reported.

### Exit criterion

A fine-grained communication kernel runs end to end on the native SUE model.

## CP9 — MSCCL++ Fine-Grained Lowering

### Work

- Replace direct MSCCL++ MicroOp execution in fine mode with GPU operations.
- Map PUT/GET/COPY to MemcpyOp variants with correct memory ownership.
- Map SIGNAL/WAIT to release/acquire operations.
- Map REDUCE to ReduceOp.
- Preserve workgroup identity and per-workgroup order.
- Add explicit barriers when present in supported plans.

### Verification

- Compare imported plan structure against hand-audited fixtures.
- Verify operation, cache-line request, and byte counts.
- Verify semaphore generations and completion ordering.

### Exit criterion

Official-style MSCCL++ fixtures execute through the fine GPU model.

## CP10 — Compute-Kernel Templates

### Work

- Define a compute-kernel lowering interface.
- Add deterministic templates describing workgroups, wavefronts, memory spans,
  and ReduceOp durations.
- Add optional calibrated/statistical templates for missing detail.
- Record all sampled values in the report.

### Verification

- Template expansion is deterministic under fixed parameters/seed.
- Compute and communication kernels contend on the same CUs.
- Concurrent kernels do not exceed resource capacities.

### Exit criterion

Compute and communication execute through one common GPU model.

## CP11 — Chakra ET Frontend

### Work

- Integrate the official Chakra protobuf schema/library or a narrowly versioned
  compatible reader.
- Parse per-rank ET files and graph dependencies.
- Map compute nodes to compute-kernel templates.
- Map collective nodes to plan-registry lookups.
- Map point-to-point/memory nodes only when semantics are explicit.
- Reject unsupported nodes with node ID/name/type diagnostics.

### Verification

- Parse official small Chakra examples.
- Preserve node IDs and graph edges exactly.
- Detect cycles, missing dependencies, and rank mismatches.
- Run a small compute-collective-compute trace.

### Exit criterion

Chakra drives kernel readiness while parsers provide fine-grained kernels.

## CP12 — SUE-Sim Backend Integration

### Work

- Construct one persistent SUE-Sim topology/session per simulation.
- Add a normal backend selector.
- Inject cache-line-derived remote requests asynchronously.
- Preserve concurrent outstanding requests in one ns-3 event loop.
- Return destination visibility and response callbacks.
- Export CBFC, LLR, queues, rails, and retries into shared reports.

### Verification

- Native and SUE-Sim backends conserve identical logical requests and bytes.
- Multi-GPU traffic overlaps in one event loop.
- Multi-rail and failure/retry tests complete without hidden two-GPU limits.

### Exit criterion

Fine-grained remote memory operations execute on the real SUE-Sim backend.

## CP13 — Reporting and Research Observability

### Work

- Add per-GPU/CU occupancy and active time.
- Add wavefront ready/stall breakdowns.
- Add instruction counts by type.
- Add local/remote bytes by memory tier.
- Add semaphore/barrier/Waitcnt stall time.
- Add request latency decomposition.
- Add provenance-oriented execution table export.

### Verification

- Counters reconcile from kernel to instruction to request to PDU.
- Reports are deterministic.
- Table export can explain one complete request path.

### Exit criterion

Results are interpretable enough for debugging and research evaluation.

## CP14 — Validation and Calibration

### Work

- Analytical unit tests for every timing component.
- Cross-check selected semantics against ASTRA-sim 3.0 documentation/examples.
- Compare official MSCCL++ plan structure.
- Calibrate memory/CU parameters where measurements are available.
- Quantify simulation speed and memory use as cache-line scale grows.

### Verification

- Correctness invariants pass before timing comparisons.
- Calibration data, assumptions, and error are documented.
- No unsupported accuracy claims are made.

### Exit criterion

The model has an evidence-backed fidelity statement and known limitations.

## CP15 — Migration and Cleanup

### Work

- Keep coarse mode available for fast simulation.
- Make fine mode selectable and documented.
- Remove duplicated logic only after equivalent tests exist.
- Split combined workload/network/GPU configuration ownership.
- Update the main README and architecture diagrams.

### Verification

- Both modes build and pass their suites.
- Existing examples have documented migration paths.

### Exit criterion

The new model is the supported detailed scale-up path without breaking the
coarse baseline.

## 9. Recommended First Demonstration

Do not begin with a complete AllReduce. Build this minimal vertical slice:

```text
2 GPUs
2 CUs per GPU
1 communication kernel
2 workgroups per GPU
2 wavefronts per workgroup
256-byte remote MemcpyOp
128-byte cache line
SemaphoreRelease on GPU 0
SemaphoreAcquire on GPU 1
native SUE backend
```

Expected observable behavior:

- deterministic CU assignment;
- two cache-line loads and two remote stores for the copy;
- acquire stalls until release is remotely visible;
- another ready wavefront executes during the stall;
- all instruction/request/PDU byte counts reconcile;
- report shows CU occupancy and semaphore stall time.

After this passes, generalize endpoint, CU, workgroup, wavefront, cache-line,
and payload counts before adding Chakra and SUE-Sim.

## 10. Implementation Order and Dependency Chain

```text
CP0 baseline
  -> CP1 contracts
  -> CP2 lazy instruction expansion
  -> CP3 CU scheduler
  -> CP4 wavefront engine
  -> CP5 outstanding/Waitcnt
  -> CP6 local memory
  -> CP7 synchronization
  -> CP8 native scale-up integration
  -> CP9 MSCCL++ lowering
  -> CP10 compute templates
  -> CP11 Chakra
  -> CP12 SUE-Sim
  -> CP13 reporting
  -> CP14 validation
  -> CP15 migration
```

Chakra and full SUE-Sim integration deliberately come after the execution core.
Otherwise, frontend and backend complexity would hide GPU-model correctness
bugs.

## 11. Decisions Requiring PhD Confirmation

Before CP2/CP6 becomes final, confirm:

1. Is the required target Load/Store semantic simulation or actual ISA/binary
   simulation? The paper and recommended design use Load/Store semantics.
2. Which memory tiers must be modeled in the first result: L1, L2, HBM, and
   remote HBM, or only local/remote HBM?
3. Should caches model hits/misses, or should traces/templates specify the
   resolved tier directly?
4. What is the intended source of compute-kernel memory behavior: profiling,
   static templates, Chakra attributes, or probabilistic generation?
5. Must official Chakra files be modified with new attributes, or may the
   simulator maintain a sidecar kernel-description file?
6. Which MSCCL++ plan version and operations are required?
7. Is CU scheduling intended to model a named architecture or remain
   configurable/abstract?
8. What reference data will establish accuracy?

## 12. Definition of Success

The project is successful when a Chakra communication kernel can be expanded
through an MSCCL++ plan into per-GPU workgroups and wavefronts, dynamically
scheduled on finite CUs, executed as cache-line-sized local/remote Load/Store
and control instructions, transported through SUE for remote accesses, and
reported with complete provenance, synchronization, contention, and latency
breakdowns.
