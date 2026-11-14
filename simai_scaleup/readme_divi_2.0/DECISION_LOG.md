# Decision Log

## Accepted Initial Decisions

### D001 — Focus on one scale-up domain

Scale-out modeling is outside the immediate project scope.

### D002 — Build inside `simai_scaleup`

Create modular 2.0 subsystems within the current project. Do not fork ASTRA-sim
2.0 unless upstream ASTRA compatibility becomes an explicit requirement.

### D003 — Target Load/Store semantics

Model semantically meaningful GPU instructions and cache-line requests rather
than decoding exact GPU binaries.

### D004 — Preserve the old path temporarily

The current GPU/executor/native-SUE path remains a coarse reference until
replacement gates pass.

### D005 — Separate static and dynamic information

Kernel definitions contain no default runtime CU assignments. The scheduler
creates assignments during simulation.

### D006 — Use deterministic scheduling first

Round-robin is the reference CU scheduling policy. Probabilistic policies must
be seeded and reproducible.

### D007 — Send only remote traffic to SUE

Local GPU and local-memory accesses remain in the GPU/memory model. Remote
scale-up requests enter the SUE transport boundary.

### D008 — Chakra remains kernel-level

Chakra supplies kernel nodes and dependencies. Fine-grained descriptions are
produced by parsers/templates/plans and may use a sidecar if necessary.

### D009 — Importing MSCCL++ is not plan generation

The existing parser interprets an already generated plan. Plan generation or
selection is a separate responsibility.

### D010 — No deletion without a replacement gate

Legacy code may be substantially removed, but only after tested replacement,
caller migration, and isolated deletion review.

## Pending Decisions

See [`OPEN_DECISIONS.md`](OPEN_DECISIONS.md). Add accepted answers here with a
date, evidence, and consequences before implementation depends on them.

## Implementation Records

### 2026-10-23 — Phase 0 completed

The CLI now recognizes explicit coarse and fine GPU-model modes. Coarse remains
the only executable model, and fine fails clearly until its vertical slice is
implemented. Exact report comparison confirms that the new boundary did not
change legacy output.

### 2026-10-23 — Phase 1 completed

Fine-grained static contracts now exist independently of the legacy runtime.
Memory-space ownership is validated relative to the kernel device, and IDs are
globally unique within a kernel. No new contract is yet used for execution.

### 2026-10-23 — Phase 2 completed

GPU operations now expose lazy primitive instruction streams. Memcpy unrolling
uses grouped Loads followed by Waitcnt/Store pairs, and memory spans are
partitioned deterministically by cache line across wavefronts. Barriers remain
zero-instruction control operations to be handled by the execution engine.

### 2026-10-23 — Phase 3 completed

Static kernel/workgroup IDs are definitions, not runtime identities. Every
submission now creates distinct kernel and workgroup instance IDs. Round-robin
is the reference scheduler; least-occupied and seeded-random policies use the
same finite-residency interface.

### 2026-10-23 - Phase 4 completed

Dynamic wavefront state now belongs to a standalone fine execution engine.
The engine consumes scheduler dispatches, interleaves ready wavefronts per CU,
enforces issue timing, and emits workgroup completions that release scheduler
residency. Static definitions remain reusable and carry no runtime assignment.

### 2026-10-23 - Phase 5 completed

Load and Store instructions are nonblocking external completions tracked by
class and instruction ID. Waitcnt remains an internal wavefront control
instruction over the combined outstanding Load/Store count. Reduce completion
time is derived deterministically from configured nanoseconds per cycle. These
mechanisms are execution-core semantics only; they do not claim a completed
local-memory or SUE-Sim request path.

### 2026-10-23 - Phase 6 completed

Local L1, L2, and HBM requests use typed asynchronous contracts and a finite
analytical backend with service serialization, base latency, queue capacity,
and outstanding limits. Local routing is derived from instruction memory
references. Remote and semaphore traffic is rejected explicitly until their
dedicated backends exist; it is never approximated as local memory.

### 2026-10-23 - Phase 7 completed

Semaphore visibility is an external event: local or remote completion invokes
the synchronization engine, which advances generations and wakes waiters.
Workgroup and kernel barriers use explicit runtime participant sets and
generations. The service reports blocked state deterministically.

### 2026-10-23 - Phase 8 completed

Fine GPU instructions lower to semantic remote requests before any SUE
transaction or PDU exists. Remote loads and semaphore acquires require response
traffic; stores and releases request remote visibility. The existing SUE
packetizer is an adapter below this semantic boundary.

### 2026-10-23 - Phase 9 completed

MSCCL++ plan parsing remains separate from fine execution lowering. GPU and
threadblock structure are preserved, and a registry selects existing plans by
collective/topology/protocol/payload constraints. The importer never invents a
collective algorithm when no registered plan matches.

### 2026-10-23 - Phase 10 completed

Compute kernels use deterministic typed templates rather than pretending to
decode vendor binaries. Templates expand local memory and Reduce operations
across workgroups and wavefronts, and use the same scheduler and residency
limits as communication kernels.

### 2026-10-23 - Unified fine runtime enabled

The production CLI now exposes a scoped fine path for one MSCCL++ collective
layer. The runtime connects all locally implemented subsystems through a
deterministic event loop and preserves the coarse CLI unchanged. Analytical
memory parameters are emitted in every fine report.

### 2026-10-23 - Chakra and persistent SUE-Sim remain explicit gaps

The repository contains no Chakra schema files or generated protobuf runtime,
so only a versioned neutral sidecar is supported. The SUE-Sim adapter still
runs ns-3 once per PDU through a synchronous backend interface. Neither gap is
described as complete or silently approximated.

### 2026-10-24 - Chakra ET import completed without protobuf runtime coupling

The importer reads official length-delimited Chakra protobuf wire data directly
and maps it into the frontend-neutral graph. Compatibility is pinned to Chakra
commit `bf222b23caf7b6972da17ab7b419d02083c2d2e0` and verified against Chakra's
packaged 3,664-node ET fixture. Simulation dependencies use `data_deps`, matching
Chakra's ET feeder; host call-tree `ctrl_deps` are not promoted into execution
ordering. Missing `comm_type`, `comm_size`, or tensor-size attributes remain
unknown. Import/validation is deliberately separate from graph lowering and
fine-runtime execution.
