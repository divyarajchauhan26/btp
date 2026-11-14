# Requirements

## 1. Research Goal

Model GPU execution details that determine scale-up communication timing and
traffic generation. The model should explain which CU and wavefront generated a
local or remote memory request, what synchronization blocked progress, which
memory tier was accessed, and how the resulting remote request traversed SUE.

## 2. Scope

### Required

- One configurable scale-up domain.
- Multiple GPUs/XPUs.
- Multiple CUs per GPU.
- Multiple resident workgroups per CU.
- Multiple wavefronts per workgroup.
- Workgroup-to-CU assignment.
- Wavefront readiness, execution, stalls, and retirement.
- Compute and communication kernels using a shared execution model.
- Primitive Load, Store, SemaphoreAcquire, SemaphoreRelease, Reduce, and
  Waitcnt instructions.
- Operation wrappers such as LoadOp, StoreOp, MemcpyOp, semaphore operations,
  ReduceOp, and barriers.
- Configurable cache-line size and lazy range expansion.
- Typed local and remote memory references.
- Local memory timing and contention.
- Remote request handoff to SUE.
- Remote load request/response behavior.
- Remote store visibility semantics.
- Semaphore generations and waiter wake-up.
- Workgroup and inter-workgroup barriers.
- Deterministic event scheduling.
- Deadlock/progress diagnostics.
- Detailed provenance and utilization reports.

### Later but architecturally supported

- Official Chakra ET frontend.
- Official MSCCL++ plan fixtures and broader schema support.
- Statistical compute-kernel expansion.
- Alternative CU and wavefront scheduling policies.
- Configurable cache-hit/miss models.
- Complete SUE-Sim topology/session integration.
- Calibration against measurements.

### Excluded initially

- Scale-out networks.
- CPU or host-runtime simulation.
- Actual CUDA/ROCm binary execution.
- Vendor ISA decoding.
- Functional tensor-value correctness.
- Full cache coherence.
- Reimplementation of SUE-Sim networking features.

## 3. Fidelity Boundary

The unit of detailed execution is a semantic GPU instruction, commonly
generating cache-line-sized requests:

```text
Load
Store
SemaphoreAcquire
SemaphoreRelease
Reduce
Waitcnt
```

The simulator does not need every arithmetic instruction in a real binary. A
`Reduce` instruction may represent a configured amount of arithmetic work.

## 4. Input Ownership

### Workload/kernel graph

Contains:

- kernel nodes;
- compute/communication classification;
- kernel dependencies;
- collective kind and logical bytes;
- parallel groups;
- references to kernel descriptions or plan-registry keys.

### Kernel description

Contains:

- workgroups and wavefront programs;
- operations and memory spans;
- resource requirements;
- synchronization structure;
- provenance.

### GPU configuration

Contains:

- CU count;
- workgroup residency;
- issue width/latency;
- scheduling policies;
- arithmetic throughput;
- outstanding-request limits.

### Memory configuration

Contains:

- memory tiers;
- cache-line size;
- latency, bandwidth, queues, and capacities;
- routing and placement policy.

### Scale-up network configuration

Contains:

- topology and endpoint mapping;
- rails and ports;
- link parameters;
- SUE/CBFC/LLR configuration.

Dynamic CU assignment must not normally be embedded in the workload trace.

## 5. Correctness Requirements

- Every static and dynamic object has a stable typed ID.
- Per-wavefront program order is preserved.
- An instruction is issued at most once unless retry semantics explicitly apply.
- A workgroup never exceeds one CU assignment at a time.
- CU residency limits are never exceeded.
- Waitcnt never releases above its configured outstanding threshold.
- Semaphore generation `N` cannot be satisfied by generation `N-1`.
- A barrier releases only after every required participant arrives.
- Remote load completes only after response delivery.
- Remote store completion respects the requested completion stage.
- Local accesses never enter the scale-up backend.
- Logical bytes are conserved through requests and transport packetization.
- Every accepted kernel eventually completes or produces a diagnostic deadlock.

## 6. Reproducibility Requirements

- Event ordering is deterministic for equal timestamps.
- Random policies require an explicit seed.
- Reports include policy names, seeds, and sampled values.
- Golden tests must avoid dependence on unordered container iteration.

## 7. Performance Requirements

- Large memory spans are expanded lazily.
- Completed dynamic state is retired promptly.
- The event engine must not advance one cycle at a time when no event occurs.
- Fine and coarse modes may coexist to study fidelity/performance tradeoffs.

