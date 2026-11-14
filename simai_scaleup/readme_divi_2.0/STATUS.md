# Implementation Status

## Phase 0 — Complete

Completed on 2026-10-23.

### Implemented

- Added explicit `--gpu-model coarse|fine` CLI selection.
- Preserved implicit coarse behavior for all existing commands.
- Made `fine` fail explicitly instead of silently using the legacy model.
- Added a frozen four-GPU MSCCL++ golden report.
- Added an exact implicit-versus-explicit coarse comparison test.
- Added an exact golden-report comparison test.
- Added a negative fine-mode availability test.
- Added generated executables, logs, build directories, and local tools to
  `.gitignore`.
- Documented generated source-tree artifacts without deleting them.
- Corrected a stray leading `=` that had corrupted `native/src/simulator.cpp`.

### Verification

```text
simai_contracts_test                  passed
simai_vertical_slice_test             passed
simai_mscclpp_four_gpu_cli_test       passed
simai_coarse_mode_golden_test         passed
simai_fine_mode_unavailable_test      passed

5/5 tests passed
```

### Frozen coarse result

```text
four-GPU direct AllGather
issued/finalized/completed operations: 12/12/12
logical payload bytes:                 12288
PDUs:                                  12
coarse model time:                     602 ns
```

## Next Phase

## Phase 1 — Complete

Completed on 2026-10-23.

### Implemented

- Added typed IDs for instructions, operations, memory requests, compute units,
  barriers, and kernel instances.
- Added typed enums for memory spaces, instruction/operation/request kinds,
  stall reasons, ordering scopes, and scheduling policies.
- Added validated memory references with owner, space, buffer, byte range, and
  alignment.
- Added operation factories and kind-specific validation for Load, Store,
  Memcpy, semaphore acquire/release, Reduce, and barriers.
- Added static wavefront-program, workgroup-resource, workgroup, and kernel
  definitions.
- Added kernel-wide uniqueness validation for workgroups, wavefronts, and
  operations.
- Added local-versus-remote memory ownership validation relative to the target
  kernel device.
- Kept every new contract isolated from the coarse runtime.

### Verification

- Valid communication-kernel construction.
- Invalid alignment rejection.
- Invalid operation-kind state rejection.
- Zero-cycle Reduce rejection.
- Empty workgroup rejection.
- Duplicate wavefront rejection across workgroups.
- Wrong local and remote memory ownership rejection.
- Complete regression suite remains 5/5 passing.

## Next Phase

## Phase 2 — Complete

Completed on 2026-10-23.

### Implemented

- Added validated primitive instruction contracts with parent-operation
  provenance and deterministic sequence numbers.
- Added a lazy `FineInstructionStream`; large memory spans are not expanded
  into stored instruction vectors.
- Added configurable power-of-two cache-line sizes.
- Added deterministic contiguous memory-range partitioning across wavefronts.
- Added LoadOp and StoreOp cache-line expansion.
- Added MemcpyOp expansion into Load, Waitcnt, and Store instructions.
- Added configurable Memcpy unrolling: grouped Loads followed by decreasing
  Waitcnt thresholds and Stores.
- Added partial-final-cache-line support.
- Added singleton semaphore acquire/release and Reduce instruction generation.
- Kept workgroup and kernel barriers as deliberate zero-instruction control
  operations for the future execution engine.

### Verification

- 1,024-byte Memcpy with 128-byte lines emits eight Loads, eight Waitcnts, and
  eight Stores.
- Unroll-by-four begins with four Loads and preserves 1,024 Load and Store
  bytes.
- A 300-byte operation emits `128 + 128 + 44` byte requests.
- Eight cache lines partition across three wavefronts as `3 + 3 + 2`.
- Source and destination offsets remain matched after partitioning.
- Instruction IDs, sequence indices, and parent provenance are preserved.
- All operation-generator kinds have focused coverage.
- Complete regression suite passes 6/6.

## Next Phase

## Phase 3 — Complete

Completed on 2026-10-23.

### Implemented

- Added validated scheduler configuration for devices, CUs, residency slots,
  scheduling policy, and deterministic seed.
- Added finite per-CU workgroup residency accounting.
- Added deterministic round-robin and least-occupied policies.
- Added a reproducible seeded-random policy using an internal deterministic
  generator.
- Added FIFO workgroup waiting queues and retirement-driven admission.
- Added device-local occupancy and waiting queries.
- Added distinct runtime kernel/workgroup-instance IDs so repeated static
  kernels and workgroup IDs remain safe.
- Added atomic submission validation: an impossible later workgroup cannot
  leave earlier workgroups partially admitted.
- Kept the scheduler independent of instruction execution and the coarse path.

### Verification

- Exact round-robin CU assignment.
- Waiting and retirement-driven redispatch.
- Residency-slot conservation and capacity enforcement.
- Least-occupied balancing with deterministic tie-breaking.
- Same seed reproduces identical random CU assignments.
- Separate GPUs schedule independently.
- Repeated static kernels receive distinct runtime identities.
- Unknown completion IDs are rejected.
- Failed kernel submission leaves the scheduler drained.
- Complete regression suite passes 7/7.

## Phase 4 - Complete

Completed on 2026-10-23.

### Implemented

- Added dynamic wavefront-instance IDs and runtime state separate from static
  wavefront definitions.
- Added per-CU ready-wavefront selection with deterministic round-robin
  interleaving.
- Enforced workgroup admission time, CU issue width, and CU issue latency.
- Preserved per-wavefront operation and instruction order through lazy streams.
- Skipped instruction-stalled wavefronts so another resident wavefront can
  issue.
- Added wavefront snapshots with operation cursor and explicit stall state.
- Added wavefront-to-workgroup completion aggregation and scheduler retirement
  handoff.
- Retired completed dynamic workgroup and wavefront state promptly.

### Verification

- A workgroup cannot issue before its admission timestamp.
- Two wavefronts interleave on one CU while one remains instruction-stalled.
- Incorrect instruction completions are rejected.
- A workgroup completes only after every wavefront completes.
- Workgroup retirement releases residency to queued work immediately.

## Phase 5 - Complete

Completed on 2026-10-23.

### Implemented

- Added independently outstanding Load and Store instructions per wavefront.
- Added total and request-class outstanding counters.
- Implemented internal Waitcnt threshold evaluation and completion-driven
  wake-up; Waitcnt is not exported as a memory request.
- Allowed out-of-order memory completions while preserving instruction issue
  order.
- Prevented operation/workgroup retirement until every final memory request
  completes.
- Added configurable Reduce cycle duration and deterministic modeled
  completion timestamps.
- Rejected Reduce completion before its modeled completion time.
- Added fine-instruction issue and completion event kinds for later event-loop
  integration.

### Verification

- Four-way unrolled Memcpy issues four independent Loads.
- Waitcnt blocks at four outstanding requests with threshold three and wakes
  exactly when the count reaches three.
- Subsequent Waitcnt thresholds account for both Loads and Stores.
- Out-of-order completion preserves exact Load/Store counters.
- A four-line Memcpy emits exactly four Loads and four Stores and retires only
  after the last Store completes.
- Reduce cycle count produces an exact completion timestamp and cannot retire
  early.
- Complete regression suite passes 8/8, including coarse golden and existing
  SUE-backed tests.

## Phase 6 - Complete

Completed on 2026-10-23.

### Implemented

- Added typed asynchronous memory request/completion contracts carrying
  kernel, workgroup, wavefront, instruction, issuer, target, and timing
  provenance.
- Added configurable analytical L1, L2, and local-HBM tiers per device.
- Modeled base latency and serialized service time using configured bytes per
  nanosecond.
- Added finite waiting-queue and maximum-outstanding limits with explicit
  `MemoryBackpressure` rejection and retry support.
- Kept per-device tier capacity and timing state independent.
- Added strict Load/Store lowering based on source/destination memory space.
- Rejected remote targets, remote request kinds, semaphore memory, and
  unconfigured local tiers instead of silently redirecting traffic.
- Preserved exact byte range and dynamic execution provenance through
  completion.

### Verification

- A 64-byte request at 16 bytes/ns has four nanoseconds of service time.
- Two same-time requests serialize on the tier service timeline.
- A full one-entry waiting queue rejects a third request without consuming
  outstanding capacity.
- An early completion is rejected and a blocked request succeeds after retry.
- Per-tier outstanding limits are independent across devices.
- Local Load and Store lowering selects the correct source/destination tier.
- Remote Load and remote-kind submission are explicitly rejected.
- Duplicate and nonlocal tier configurations are rejected.
- Complete regression suite passes 9/9, including coarse golden and existing
  SUE-backed tests.

## Phase 7 - Complete

Completed on 2026-10-23.

### Implemented

- Added semaphore state keyed by owner and semaphore ID.
- Added monotonically advancing generations and generation-specific waiters.
- Added release-driven wake-up for every satisfied acquire.
- Connected semaphore instruction issues from the wavefront engine to the
  synchronization service through explicit completions.
- Added reusable workgroup barriers with explicit wavefront participants.
- Added reusable kernel barriers with explicit workgroup participants.
- Rejected duplicate arrivals, nonparticipants, and repeated/regressing
  semaphore releases.
- Added deterministic blocked-state diagnostics for semaphore and barrier
  waits.

### Boundary

Barrier state is implemented and tested as a synchronization service. Barrier
operations are not yet consumed automatically by the unified wavefront event
loop because that loop is a later integration checkpoint.

## Phase 8 - Complete

Completed on 2026-10-23.

### Implemented

- Added typed semantic remote requests retaining kernel/workgroup/wavefront and
  instruction provenance.
- Lowered remote Store to one-way PUT traffic with remote-visible completion.
- Lowered remote Load to GET command plus exact data response traffic.
- Lowered remote semaphore release to an atomic command.
- Lowered remote semaphore acquire to a read request plus value response.
- Adapted semantic requests to the existing SUE transaction/PDU layer without
  exposing packet construction to the GPU engine.
- Rejected local traffic at the remote boundary.

### Boundary

Semantic request generation and native SUE packet adaptation are complete.
Persistent asynchronous SUE-Sim session execution remains Phase 12.

## Phase 9 - Complete

Completed on 2026-10-23.

### Implemented

- Reused the existing MSCCL++ JSON parser while adding a separate fine-kernel
  lowering path.
- Lowered each GPU to a communication kernel and each threadblock to an
  ordered workgroup/wavefront program.
- Lowered PUT/GET/COPY to Memcpy operations, SIGNAL/WAIT to generation-aware
  semaphore operations, and REDUCE to configured Reduce operations.
- Preserved local/remote buffer ownership, offsets, sizes, and threadblock
  provenance.
- Added deterministic plan selection by collective, participant count,
  protocol, payload range, and payload multiple.
- Rejected unsupported operations, invalid ownership, missing plans, and
  ambiguous plan matches explicitly.

### Verification

- The two-GPU fixture lowers to two four-operation communication kernels.
- The four-GPU AllGather fixture preserves 12 remote PUT operations and 12,288
  logical bytes.

## Phase 10 - Complete

Completed on 2026-10-23.

### Implemented

- Added a typed compute-kernel template with workgroup count, wavefront count,
  residency, operation sequence, and deterministic memory strides.
- Expanded local Load, Store, Memcpy, and Reduce templates into validated fine
  kernels.
- Assigned deterministic global wavefront and operation IDs.
- Applied independent workgroup and wavefront byte strides with overflow
  checks.
- Rejected remote communication and synchronization operations in compute
  templates.
- Verified compute and communication kernels compete for the same finite CU
  residency and retirement-driven admission.

## Phase 11 - Complete

Completed locally on 2026-10-23:

- Added a frontend-neutral kernel graph with compute/collective nodes, device
  ownership, descriptors, payloads, and dependencies.
- Added deterministic topological ordering.
- Added cycle, missing-edge, duplicate, and self-dependency validation.
- Added an explicitly versioned text sidecar importer preserving node IDs,
  names, kinds, devices, descriptors, collective kinds, payloads, and edges.

Completed locally on 2026-10-24:

- Added a self-contained reader for official length-delimited Chakra ET
  protobuf streams, pinned to `astra-sim/chakra` schema commit
  `bf222b23caf7b6972da17ab7b419d02083c2d2e0`.
- Added support for metadata versions represented either by the direct
  `version` field or the `schema` metadata attribute.
- Mapped metadata, compute, memory load/store, collective, send, and receive
  nodes into the validated neutral graph.
- Preserved zero-based source IDs, data dependencies, device ownership,
  microsecond-to-nanosecond durations, known payloads, and point-to-point peers.
- Kept absent communication attributes unresolved rather than inventing a
  collective type or payload.
- Added `--chakra <trace.et> <device-id>` import/inspection mode.
- Verified import against Chakra's packaged 3,664-node `chakra.0.et` fixture.

Chakra graph import is complete. Automatic graph-node lowering and dependency-
aware execution are separate runtime/frontend work: Chakra does not itself
provide wavefront Load/Store/semaphore instruction streams.

## Phase 12 - Native Runtime Complete, Persistent SUE-Sim Pending

Completed locally on 2026-10-23:

- Added one deterministic fine event runtime connecting CU scheduling,
  wavefront issue, analytical local memory, Waitcnt, reductions, semaphore
  visibility, semantic remote requests, PDU transport, and retirement.
- Added transport-factory injection so the fine runtime can use the same
  backend abstraction as the coarse runtime.
- Added request-plus-response handling for remote Load/acquire and one-way
  handling for Store/release.

The native multi-rail SUE backend is executable. The optional ns-3/SUE-Sim
backend still invokes `Simulator::Run()` per PDU and therefore does not satisfy
the planned persistent concurrent-session gate.

## Phase 13 - Complete

- Added versioned fine reports with end-to-end time, instruction conservation,
  local/remote requests, command/response PDUs, payload/wire bytes, analytical
  memory assumptions, CU dispatch lifetimes, per-rail counters, execution trace
  records, and blocked-state diagnostics.
- Added report validation reconciling instruction and rail counters.

## Phase 14 - Correctness Validation Complete

- Repeated two-GPU execution produces byte-identical reports.
- Two-GPU PUT/signal/wait/reduce execution completes through the unified fine
  runtime.
- Four-GPU execution conserves 12 PUTs as 192 cache-line remote Stores and
  completes 384/384 external Load/Store instructions.
- Coarse golden behavior remains unchanged.

Calibration against hardware measurements or published timing datasets remains
future research work; no accuracy percentage is claimed.

## Phase 15 - Scoped Migration Complete

- Enabled `--gpu-model fine --mscclpp <plan> <workload>` for one collective
  layer with explicit GPU resources.
- Retained coarse native, AICB, and MSCCL++ modes unchanged.
- Fine CLI reports its built-in analytical memory parameters rather than hiding
  them.
- Native/AICB fine modes fail explicitly because their graph/template mapping
  is not yet defined.

## Phase 16 - Chakra-Driven Native Fine Execution Complete

Completed locally on 2026-10-24:

- Preserved Chakra start-time presence, duration presence, I/O descriptions,
  raw attributes, and control dependencies in the neutral graph.
- Added collision-safe multi-rank graph merging using `(source rank, source
  node ID)` identity while retaining rank-local dependency edges.
- Added a versioned execution manifest with rank trace declarations,
  duration-backed compute bindings, and grouped MSCCL++ collective bindings.
- Added graph lowering that rebases compute kernels to target devices, assigns
  globally unique kernel IDs, validates collective participants/payloads, and
  collapses per-rank collective nodes into one launch.
- Added dependency-aware graph execution to the existing fine event loop.
  Successor kernels are released at predecessor completion instead of all
  kernels being submitted at time zero.
- Added graph-node readiness, start, completion, source-node count, and kernel
  count to the fine report.
- Added `--gpu-model fine --chakra-fine <manifest> <hardware-workload>`.

### Verification

- Chakra field-preservation and multi-rank merge test passes.
- In-memory compute-to-collective-to-compute graph execution test passes.
- File-based manifest import/lowering test passes.
- Full CLI fixture with two ET rank files and one grouped MSCCL++ plan passes.
- Complete current regression suite passes 23/23.

### Boundary

The manifest's compute path currently uses one abstract Reduce operation based
on explicit cycles or captured Chakra duration. Detailed compute Load/Store
templates are not yet represented by the external manifest. The executable
backend remains the native deterministic transport; persistent SUE-Sim and
hardware timing calibration are still pending.

## Phase 17 - Initial Dynamic Microtrace Import Complete

Completed locally on 2026-11-03:

- Added a typed importer for the observed tab-separated dynamic trace shape:
  probe, block coordinates, thread coordinates, clock value, address, and
  encoded instruction type.
- Required callers to provide an explicit instruction-type legend, block
  dimensions, warp size, access width, timestamp semantics, and allocation
  ownership map. Unknown instruction types and unmapped addresses fail instead
  of being guessed.
- Grouped thread records into block/warp dynamic instructions and coalesced
  contiguous lane accesses into semantic memory spans.
- Preserved raw records, coalesced access clocks, active-lane counts, and
  timestamp semantics for later calibration.
- Converted allocation-relative addresses to validated buffer offsets and
  classified HBM references as local or remote from allocation ownership.
- Lowered observed Load/Store accesses into the existing fine workgroup,
  wavefront, and kernel contracts for either compute or communication kernels.
- Added a graph binding for pre-lowered traced kernels so an imported kernel
  can replace one Chakra executable node without reconstructing a duration-
  based compute template. Device mismatches are rejected.

### Verification

- The 64-record one-block/one-warp fixture coalesces into two 128-byte memory
  accesses and two ordered GPU operations.
- Remote allocation ownership produces `RemoteHbm` references.
- Sparse lane addresses split into separate spans with correct active-lane
  counts.
- Unknown instruction encodings and unmapped addresses are rejected.
- The complete native source set compiles with warnings treated as errors.
- All 16 directly executable native unit tests pass with the available
  fallback compiler. The prior 23/23 CTest result remains the latest full CLI
  and fixture-orchestrated regression because the original local CMake/LLVM-
  MinGW installation is no longer present on this machine.

### Boundary

The microtrace importer and Chakra-node binding are currently native APIs, not
yet a file-manifest or CLI path. The importer supports observed fixed-width
Load/Store records only. It
does not infer ALU work, atomics, barriers, semaphores, cache hits, exact HBM
service, or network packets. Clock values are preserved but are not charged as
simulation latency. Kernel-elapsed cycle semantics must be confirmed by the
trace producer before import.

## Phase 18 - Synthetic Timestamped Load/Store Path Complete

Completed locally on 2026-11-03:

- Added a versioned, self-contained synthetic trace format whose records carry
  timestamp, Load/Store operation, address, local/remote placement, owner GPU,
  buffer identity, data size, workgroup, and wavefront.
- Added strict parsing for nanosecond timestamps, nonzero identities and sizes,
  non-overflowing address ranges, and owner/locality consistency.
- Added `TraceWaitUntil` as an explicit GPU operation/instruction and stall
  reason. It treats each timestamp as an earliest issue offset from kernel
  admission without abusing Reduce timing.
- Allowed independent memory operations to remain outstanding while later
  operations advance; explicit Waitcnt still enforces dependency thresholds.
- Lowered each record to one timestamp wait plus one semantic local or remote
  Load/Store operation. Existing cache-line expansion handles arbitrary sizes.
- Added direct CLI execution with `--gpu-model fine --synthetic-trace`.
- Added a standalone dependency-aware multi-GPU execution manifest with
  `--gpu-model fine --synthetic-manifest`, avoiding fake Chakra inputs for
  synthetic-only studies.
- Added `synthetic=name|rank:node|path` to the Chakra execution manifest and
  reused the validated pre-lowered-kernel binding.
- Added a checked two-GPU fixture containing local Load, remote Store, remote
  Load, and local Store records at four timestamps.

### Verification

- Every native source compiles with C++14 warnings treated as errors.
- All 18 directly executable native unit tests pass with the available
  fallback compiler.
- The direct synthetic CLI completes 10/10 instructions, three local memory
  requests, and three remote requests.
- The Chakra-driven fixture combines the synthetic kernel with its existing
  collective and reports nine remote requests.
- Timestamp tests prove that a wait is relative to kernel admission, cannot
  complete early, and releases the memory operation at its earliest issue time.
- Queued workgroups retain the original kernel timestamp origin rather than
  restarting their trace clock when a CU slot becomes available.
- Fine reports conserve 192 local and 192 remote logical bytes for the
  synthetic fixture and reject memory owners outside the simulator domain.
- The standalone two-GPU manifest launches concurrent kernels on devices 1 and
  2 and conserves four local requests, four remote requests, 256 local bytes,
  and 256 remote bytes.
- Local/remote owner mismatches, unsupported operations, invalid units, empty
  traces, and malformed identities are rejected.

### Boundary

The agreed synthetic input target is complete end to end. It models
timestamped, sized local/remote Load and Store traffic through the native fine
runtime and transport. Hardware-derived trace correlation, additional GPU
instruction classes, calibrated H200 accuracy, explicit internal GPU topology,
and persistent asynchronous SUE-Sim remain separate research extensions.

## Phase 19 - Synthetic Trace Version 2 Foundation

Started locally on 2026-11-03:

- Added backward-compatible synthetic trace schema version 2.
- Required trace producer, launch correlation ID, hardware/configuration ID,
  and explicit timestamp semantics for version 2.
- Kept version 1 accepted without reinterpretation.
- Kept `earliest_issue` as the only executable timestamp semantics and reject
  unsupported modes explicitly to prevent accidental timing double counting.
- Added a trace summary formatter reporting provenance, kernel structure,
  declared allocations, Load/Store counts, local/remote bytes, and timestamp
  span.
- Added required version-2 buffer declarations with owner and allocation size.
- Rejects undeclared buffers, owner mismatches, and allocation-relative access
  ranges that exceed the declared buffer.
- Added `--inspect-synthetic-trace <trace>` to validate and inspect a trace
  without requiring a hardware workload or running the simulator.
- Added an executable version-2 trace fixture and CLI regression registration.
- Recorded the ASTRA-sim 3.0 comparison and full implementation roadmap in
  `ASTRA_3_FINE_GRAINING_ROADMAP.md`.

### Boundary

Version 2 currently adds provenance, timing semantics, and allocation-relative
buffer owner/size validation. Raw-address/base-address mapping, richer
instruction records, measured completion timestamps, and additional replay
policies remain later versioned extensions.

## Final Verification

- The previously recorded 19-test suite passed after Chakra import, unified
  runtime, and the first CLI integration. New graph-driven tests are recorded
  separately above; run the current complete suite for the live total.
- Verified fine CLI sample: 76 ns, 14 issued/completed instructions, four local
  requests, six remote requests, six command PDUs, and balanced two-rail use.

## Remaining Work

1. Define the stable launch/correlation identity used to select hardware microtrace
   files and expose the existing native binding through the execution manifest.
2. Replace the synchronous SUE-Sim adapter with one persistent asynchronous
   ns-3 session supporting multiple in-flight fine requests.
3. Extend trace capture/import to access width, opcode/PC, atomics, barriers,
   fences, synchronization, issuing GPU, and allocation ownership metadata.
4. Add measured calibration data and report model error.
5. Move CU, memory-tier, and fabric assumptions into a dedicated hardware
   configuration schema.
