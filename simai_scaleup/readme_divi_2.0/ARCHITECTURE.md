# Proposed Architecture

## 1. Layering

```text
Frontend layer
  native tests | Chakra ET
        ↓
Kernel graph layer
  dependencies and per-rank kernel instances
        ↓
Kernel lowering layer
  compute templates | MSCCL++ plans
        ↓
Fine-grained GPU IR
  kernels → workgroups → wavefronts → operations
        ↓
GPU execution layer
  CU residency, wavefront scheduling, instruction issue
        ↓
Memory and synchronization layer
  local tiers, outstanding requests, semaphores, barriers
        ↓
Scale-up request layer
  remote cache-line Load/Store/control requests
        ↓
Transport layer
  native test backend | SUE-Sim
        ↓
Completion and reporting
```

## 2. Static Versus Dynamic State

### Static IR

Reusable across simulations:

```text
KernelDefinition
WorkgroupDefinition
WavefrontProgram
GpuOperationDefinition
MemorySpan
SynchronizationRequirement
```

### Dynamic state

Created for one run:

```text
KernelInstanceState
WorkgroupState
WavefrontState
ComputeUnitState
OutstandingRequestState
SemaphoreState
BarrierState
```

Static definitions must not contain mutable runtime progress or accidental CU
assignments.

## 3. Fine-Grained Intermediate Representation

### Kernel

```text
KernelDefinition
  ID and source provenance
  target device
  kind: compute or communication
  resource requirements
  workgroups[]
```

### Workgroup

```text
WorkgroupDefinition
  ID
  source threadblock/workgroup identity
  wavefront count
  wavefront program/template
  local barrier participation
  resource requirements
```

### Wavefront program

```text
GpuOperationDefinition[]
```

The first implementation may use the same program for every wavefront while
assigning different memory subranges. The schema must permit different programs
later.

### GPU operation

```text
LoadOp
StoreOp
MemcpyOp
SemaphoreAcquireOp
SemaphoreReleaseOp
ReduceOp
WorkgroupBarrierOp
KernelBarrierOp
```

Operations generate instructions lazily.

### Primitive instruction

```text
Load
Store
SemaphoreAcquire
SemaphoreRelease
Reduce
Waitcnt
```

Each instruction carries enough provenance to reconstruct its source kernel,
workgroup, wavefront, and operation.

## 4. Scheduling

### Kernel scheduler

Releases a kernel instance when workload-graph dependencies are satisfied and
the target GPU may accept it.

### Workgroup scheduler

Maps ready workgroups to free CU residency slots.

Required initial policy:

```text
deterministic round-robin
```

Optional policies:

```text
least occupied
seeded random
architecture-specific policy
```

### Wavefront scheduler

Selects one or more ready wavefronts from resident workgroups according to
issue width and policy. Stalled wavefronts yield execution opportunities.

## 5. Memory Model

### Address/reference representation

Prefer:

```text
device owner
memory space
buffer ID
byte offset
byte length
```

Literal virtual/physical addresses can be added through a mapper without
changing operation semantics.

### Memory spaces

Initial typed spaces:

```text
RegisterFile
L1
L2
LocalHbm
RemoteHbm
SemaphoreMemory
```

The initial implementation may enable only selected tiers, but unsupported
tiers must fail clearly rather than silently collapsing.

### Local requests

Handled entirely by local memory backends. Each tier can model:

- latency;
- bandwidth/service interval;
- finite queue;
- outstanding-request capacity;
- optional hit/miss routing.

### Remote requests

Remote traffic enters SUE only after any required source-side local memory
access completes.

Remote store:

```text
local source Load
→ Waitcnt/fence
→ remote Store
→ optional remote-memory service
→ visibility completion
```

Remote load:

```text
remote Load request
→ owner memory service
→ response data
→ requester completion
```

## 6. Synchronization

### Program order

Operations and instructions in one wavefront respect program order except where
unrolling explicitly permits multiple in-flight requests.

### Waitcnt

Tracks outstanding request classes and stalls until the selected count reaches
the threshold.

### Semaphores

Semaphores are identified by owner, ID, and generation. Acquire and release are
modeled as control-memory operations with explicit visibility.

### Barriers

- Workgroup barrier: synchronizes wavefronts within one workgroup.
- Kernel/inter-workgroup barrier: synchronizes an explicit set of workgroups.

Logical dependencies remain separate from the mechanism used to enforce them.

## 7. Backend Boundary

The GPU model emits a semantic `ScaleUpMemoryRequest`, not a prebuilt PDU.

```text
ScaleUpMemoryRequest
  request/instruction provenance
  source and destination device
  memory references
  bytes
  operation class
  ordering and completion requirement
```

The transport adapter is responsible for transactions, PDUs, rails, switches,
CBFC, LLR, and delivery callbacks.

## 8. Complete Example

```text
Chakra AllReduce kernel becomes ready
→ plan registry selects four-GPU MSCCL++ plan
→ parser creates per-GPU communication kernels
→ PUT becomes a remote MemcpyOp
→ workgroup waits for a CU slot
→ scheduler assigns CU 2
→ CU selects wavefront 0
→ wavefront issues a 128-byte local HBM Load
→ local completion increments ready data
→ Waitcnt confirms Load completion
→ wavefront issues remote Store
→ scale-up request enters SUE
→ destination HBM visibility callback arrives
→ Store instruction completes
→ wavefront advances to SemaphoreReleaseOp
→ semaphore update travels remotely
→ destination acquire becomes ready
→ all wavefronts retire
→ workgroup, kernel, and Chakra node complete
```

## 9. Observability

Required report levels:

- workload/kernel dependency timing;
- kernel dispatch and completion;
- workgroup queue, assignment, and retirement;
- CU occupancy and active/stalled time;
- wavefront issue and stall reasons;
- instruction counts and latency;
- bytes and requests by memory tier;
- semaphore/barrier/Waitcnt stalls;
- scale-up request latency decomposition;
- SUE rail, queue, retry, and delivery counters;
- complete provenance for a selected request.

