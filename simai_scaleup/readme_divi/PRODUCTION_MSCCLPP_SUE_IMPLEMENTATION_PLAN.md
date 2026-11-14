# Production MSCCL++ to SUE Implementation Plan

## Objective

Extend `simai_scaleup` into a kernel-aware collective simulator that consumes
an agreed MSCCL++ schedule representation, translates it into the existing
validated `CollectivePlan`, executes PUT, GET, SIGNAL, WAIT, and REDUCE with
correct completion and semaphore semantics, and sends transport-visible work
through the selected SUE backend.

This plan deliberately extends the existing runtime. The separate
`kernel_ccl_sue_pipeline` toy is not a production dependency.

## Non-negotiable architecture

```text
native / AICB / future Chakra workload
              |
              v
       CollectiveRequest
              |
              v
   CollectiveScheduleProvider
        /              \
       v                v
NativeCollectivePlanner  MscclppScheduleProvider
        \              /
         v            v
       validated CollectivePlan
              |
              v
 dependency and semaphore executor
              |
              v
      transport backend boundary
        /                    \
       v                      v
native deterministic fabric  concrete SUE-Sim/ns-3 bridge
```

Schedule generation must not occur inside a network backend. Packet, queue,
switch, and CBFC code must not invent collective dependencies.

## Current verified baseline

- Native and bounded AICB workloads parse and validate.
- Native planners generate ring AllReduce, ReduceScatter, AllGather, direct
  AllToAll, and a gather-reduce-broadcast AllReduce baseline.
- `CollectivePlan` supports PUT, GET, SIGNAL, WAIT, and REDUCE types.
- The production executor currently executes PUT and REDUCE only.
- PUT completion distinguishes issue, local completion, remote arrival, remote
  visibility, and finalization.
- The native fabric models transactions, PDUs, rails, finite switch queues,
  credits, retries, and destination reassembly.
- The separate SUE-Sim/ns-3 tree is not linked into this build.

## Phase 1: Schedule-provider boundary

### Deliverables

- Introduce an abstract `CollectiveScheduleProvider`.
- Make `NativeCollectivePlanner` implement that interface.
- Allow `ScaleUpSimulator` to receive a provider while preserving its existing
  default-native API.
- Preserve byte-for-byte report output for every existing fixture.
- Record provider source, algorithm, and version in each schedule record.

### Acceptance gates

- Existing contract and runtime suites pass unchanged.
- A test provider can be injected without modifying simulator internals.
- The injected provider's provenance appears in the report.
- Invalid plans are still rejected before execution.

## Phase 2: Freeze the MSCCL++ input contract

### Official repository audit

The official MSCCL++ execution-plan parser and test plans were inspected from
`microsoft/mscclpp`. Current plans are JSON and contain top-level algorithm
metadata, a `gpus` array, per-GPU chunk counts, threadblocks, ordered `ops`,
channels, remote-buffer references, and semaphore initialization. Plans may
also contain nested `pipeline` operations.

Observed operation names include `put`, `get`, `signal`, `wait`, `rlxsignal`,
`rlxwait`, `copy`, `reduce`, `res`, `rre`, `rres`, packet operations, NVLS
operations, semaphore acquire/release, barriers, and no-ops. These names are
not interchangeable; each unsupported operation must be rejected.

The first translator subset is deliberately restricted to:

- memory-channel `put` and `get`;
- memory-channel `signal`, `wait`, `rlxsignal`, and `rlxwait`;
- local `copy` only when it can be represented without network traffic;
- local sum `reduce` where source and destination slices are explicit;
- `nop` as program-order preservation only;
- nested `pipeline` only after iteration expansion is specified exactly.

The first subset explicitly rejects packet operations, switch/NVLS channels,
fused remote-reduce variants, threadblock groups, device semaphore
acquire/release, barriers, unsupported reductions, and ambiguous buffer maps.

### Required decision

Select the exact upstream artifact to consume. Candidates include an official
schedule-plan representation, an agreed exported intermediate representation,
or a deliberately scoped project schema generated from MSCCL++ algorithms.
Do not call a custom schema “MSCCL++” without documenting the translation.

### Required schedule information

- Plan and kernel identity.
- Collective kind and participant mapping.
- Threadblock/workgroup identity and program order.
- PUT, GET, SIGNAL, WAIT, REDUCE, and any fused operation semantics.
- Source and destination buffers, offsets, and byte lengths.
- Explicit dependencies and ordering domains.
- Semaphore endpoint, identifier, value/generation, and memory ordering.
- Required completion stage for releasing dependents.
- Chunk/channel metadata needed for provenance and debugging.

### Acceptance gates

- A schema document and version exist.
- Unknown fields and unsupported operations fail explicitly.
- Rank, buffer, operation, and semaphore mappings are deterministic.
- No topology or algorithm detail is silently synthesized by the backend.

## Phase 3: MSCCL++ parser and translator

### Deliverables

- Add `MscclppScheduleProvider` implementing `CollectiveScheduleProvider`.
- Parse an external schedule into an intermediate typed representation.
- Translate supported instructions into existing `MicroOp` values.
- Preserve threadblock order and explicit cross-threadblock dependencies.
- Attach provenance identifying input file, schema, algorithm, and translator
  version.

### Validation

- Unique nonzero operation IDs.
- Participants match the requested parallel group.
- Buffers are owned by legal source and destination endpoints.
- Data ranges are nonzero and do not overflow.
- Every dependency exists and the dependency graph is acyclic.
- Every ordered WAIT has a reachable SIGNAL producer.
- Semaphore values are monotonic within their defined reuse domain.
- Requested operation kinds are supported by backend capabilities.

### Acceptance gates

- A two-rank imported plan produces the expected operation table.
- Reordering the file does not change dependency semantics.
- Missing producers, stale generations, invalid buffers, and cycles fail.
- Native planner results remain unchanged.

## Phase 4: GET execution

### Required semantics

GET is not PUT with reversed labels. It requires a request from the consumer,
a remote read, a data response, placement into requester-local memory, and
local completion after the response is reassembled.

### Deliverables

- GET request and response transaction types.
- Packetization and traffic accounting for both directions.
- Requester-side reassembly and destination placement.
- Completion timeline ending only after local visibility.
- Retry and duplicate handling for request and response PDUs.

### Acceptance gates

- Two-rank GET completes only after local placement.
- Request and response bytes appear separately in counters.
- Duplicate or retried PDUs do not cause double completion.
- PUT regressions remain unchanged.

## Phase 5: SIGNAL, WAIT, and semaphores

### Runtime state

Maintain endpoint-local semaphore state keyed by a typed identity and storing a
monotonic visible value/generation. Maintain blocked waits indexed by endpoint
and semaphore so arrival can wake only relevant operations.

### Ordered SIGNAL

An ordered signal may become remotely visible only after all required earlier
memory operations in its ordering domain reach remote visibility. A relaxed
signal may overlap only where the imported schedule explicitly permits it.

### WAIT

WAIT is local and generates no network data. It completes when the endpoint's
visible semaphore value is at least the requested value. Otherwise it remains
blocked without consuming backend bandwidth.

### Acceptance gates

- WAIT never passes before its matching generation is remotely visible.
- An old generation cannot release a later epoch.
- Ordered data is visible before its release signal.
- Fan-in signals and multiple semaphore IDs remain independent.
- A wait-for graph reports intentional deadlocks with blocked operation IDs.

## Phase 6: Fused operations and barriers

Add only operations present in the selected MSCCL++ input contract. A fused
PUT-with-signal must retain data-before-signal ordering while permitting the
backend to model optimized transport. Define workgroup, grid, and rank barriers
separately from remote semaphores.

Acceptance requires generation-safe reuse and deterministic release exactly
once per barrier epoch.

## Phase 7: Backend abstraction

### Deliverables

- Introduce a transport interface below the executor, not below the planner.
- Keep the current native deterministic fabric as one implementation.
- Define operations for PUT, GET request/response, SIGNAL, progress, and
  completion notification.
- Keep local WAIT and dependency evaluation above the transport boundary.

### Acceptance gates

- The same imported plan can run against a deterministic conformance backend.
- Backend selection does not change schedule provenance or dependencies.
- Completion stages have identical definitions across backends.

## Phase 8: Concrete SUE-Sim/ns-3 bridge

### Discovery gate

Before coding, identify the concrete SUE-Sim classes for endpoint injection,
packet/PDU construction, switch forwarding, CBFC, event advancement, and
destination completion. Existing CBFC-like code in `simai_scaleup` does not by
itself constitute linkage to the separate SUE-Sim tree.

### Repository audit result

The local `SUE-Sim` tree contains a dedicated ns-3 `sue-sim-module`. Its
public model includes `TrafficGenerator`, `TraceTrafficGenerator`,
`LoadBalancer`, `SueClient`, `SueServer`, SUE headers, point-to-point SUE
devices/channels, switches, queue management, LLR, and CBFC.

`LoadBalancer::DistributeTransaction` and `SueClient::AddTransaction` provide a
possible packet-injection path. However, the public API is traffic-generator
oriented: it accepts raw ns-3 packets, destinations, and virtual channels,
while `SueServer` exposes aggregate receive handling rather than an
operation-ID completion callback. `SueHeader` carries PSN, XPU, VC, and
data/ACK/NACK opcode, but no simulator micro-operation identity.

The bridge therefore requires a small SUE-Sim integration extension:

- deterministic injection with explicit source XPU, destination XPU, VC,
  operation ID, and byte count;
- a packet tag or header field preserving operation/transaction identity;
- a destination-visible completion trace or callback;
- explicitly separated acknowledgement and memory-visibility semantics;
- lifecycle control that advances ns-3 without the random traffic generator;
- statistics lookup by operation identity.

CBFC is genuinely implemented in `sue-sim-module`, but it is not invoked by
`simai_scaleup`; the build graphs and completion APIs remain disconnected.

### Deliverables

- Rank-to-SUE-endpoint mapping.
- Translation of operation buffers and bytes into SUE requests.
- Correct control representation for SIGNAL.
- ns-3 event scheduling and progress.
- Completion callbacks at defined visibility points.
- SUE-Sim statistics translated into the common report.

### Acceptance gates

- Build graph demonstrably links the selected SUE-Sim/ns-3 components.
- A two-rank PUT traverses actual modeled endpoints and links.
- A PUT-SIGNAL-WAIT chain preserves remote visibility ordering.
- A finite-credit test demonstrably exercises SUE-Sim CBFC, not the native
  `FiniteSwitch` substitute.
- Byte, completion, and credit conservation checks pass.

## Phase 9: Workload and Chakra integration

Map a ready workload collective node into `CollectiveRequest`. Chakra remains
the workload/dependency description; the selected provider remains responsible
for the collective algorithm. Preserve compute/communication overlap and
cross-layer communication fencing.

Acceptance requires at least one multi-layer trace where compute readiness,
collective scheduling, micro-op completion, and next-layer release are visible
in a single deterministic trace.

## Phase 10: Validation and research experiments

### Correctness ladder

1. Two-rank PUT.
2. Two-rank PUT-SIGNAL-WAIT.
3. Two-rank GET.
4. Multi-chunk semaphore generations.
5. ReduceScatter.
6. AllGather.
7. AllReduce composition.
8. AllToAll fan-in and congestion.
9. Multi-layer overlap.

### Comparison matrix

- Native planner versus imported MSCCL++ plan.
- Dependency-only synchronization versus explicit SIGNAL/WAIT.
- Native deterministic fabric versus SUE-Sim/ns-3.
- Link bandwidth, rail count, queue capacity, credit depth, and return latency.
- Chunk size, operation concurrency, semaphore strategy, and algorithm.

No latency, bandwidth, speedup, or accuracy claim is acceptable until the
corresponding backend and schedule provenance are recorded and validated.

## Verified bridge status and remaining gaps

Verified on Windows with a configured ns-3/SUE-Sim build:

- The SUE-Sim module compiles with operation/PDU identity tags, identified
  load-balancer injection, and destination-visible server callbacks.
- Metadata round trips for every directed pair in an eight-endpoint set.
- The optional `SueSimTransportBackend` compiles and links against SUE-Sim.
- Native and SUE-enabled SimAI contract/runtime regressions pass.
- Completion matching checks operation ID, PDU ID, source, destination, VC,
  payload bytes, and destination SUE port before releasing an operation.

The bridge is endpoint-count agnostic. It maps SimAI's one-based `DeviceId`
values to SUE-Sim's zero-based XPU IDs and validates both against the workload's
configured endpoint count. No two-XPU assumption belongs in the backend.

Remaining production work must not be described as complete:

1. Add a reusable topology-session owner that constructs `TopologyBuilder`,
   `ApplicationDeployer`, and `SimaiSueBridge` from workload link/topology
   configuration and controls `Simulator::Destroy()` lifecycle.
2. Replace one-PDU-at-a-time `Simulator::Run()` calls with batched asynchronous
   submission so independent micro-ops can overlap in ns-3.
3. Export authoritative SUE-Sim rail, retry, CBFC, queue, drop, and transmitted
   byte counters instead of deriving only completion-level counters.
4. Encode PUT, GET request, GET response, and SIGNAL service roles explicitly
   at SUE injection; the current bridge transports payloads but does not expose
   service-specific behavior to SUE-Sim internals.
5. Add bounded deadlock/timeout detection for missing completion callbacks.
6. Add topology-backed tests for arbitrary endpoint and rail counts, including
   non-power-of-two endpoint counts, multi-rail selection, CBFC pressure, GET
   request/response, and semaphore visibility.
7. Define how MSCCL++ channel IDs map to SUE ports/rails. Channel identity and
   physical path identity must not be conflated.

## Immediate implementation order

1. Complete Phase 1 without changing outputs.
2. Obtain supervisor confirmation of the exact MSCCL versus MSCCL++ artifact.
3. Write the Phase 2 schema/translation contract before implementing a parser.
4. Implement parser tests using one minimal two-rank plan.
5. Add GET, then SIGNAL/WAIT and semaphores to the existing executor.
6. Add a transport interface while preserving native-fabric regressions.
7. Audit SUE-Sim APIs and implement the concrete bridge.
8. Add Chakra only after the collective and transport contracts are stable.
