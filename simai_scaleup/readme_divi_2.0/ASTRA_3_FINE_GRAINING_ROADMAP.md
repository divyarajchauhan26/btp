# ASTRA-sim 3.0 Fine-Graining Comparison and Implementation Roadmap

## 1. Purpose

This document is the authoritative gap analysis and execution plan for the
fine-grained scale-up direction of `simai_scaleup`. It records:

- what ASTRA-sim 3.0 describes;
- what this repository actually implements;
- which pieces are partial or absent;
- how Chakra, CCL/MSCCL++, the GPU model, memory, SUE, and reporting should
  connect;
- what must be implemented before making fidelity or performance claims.

The comparison is based on the ASTRA-sim 3.0 paper:
<https://arxiv.org/abs/2606.10440>. The public ASTRA-sim repository still
identifies its current public implementation as ASTRA-sim 2.0:
<https://github.com/astra-sim/astra-sim>. Therefore, ASTRA-sim 3.0 statements
in this document are paper-level requirements, not source-level verification.

## 2. Target and Fidelity Boundary

The project models scale-up-relevant GPU behavior at semantic Load/Store
granularity. It does not execute GPU binaries and does not decode vendor ISA.

```text
kernel graph
  -> kernel detail source
  -> kernel/workgroup/wavefront operation programs
  -> primitive GPU instructions
  -> local or remote cache-line requests
  -> GPU memory/NoC or scale-up transport
  -> completion, synchronization, retirement, and reporting
```

The intended primitive instruction set is:

```text
Load
Store
SemaphoreAcquire
SemaphoreRelease
Reduce
Waitcnt
TraceWaitUntil
```

`TraceWaitUntil` is a project-specific replay constraint, not an ASTRA-sim 3.0
instruction. Workgroup and kernel barriers are control operations rather than
memory instructions.

## 3. Layer Ownership

### 3.1 Chakra workload layer

Owns:

- kernel nodes and names;
- compute/communication classification;
- dependencies and launch readiness;
- ranks/devices and participants;
- collective kind and logical payload when available;
- stable references to detailed kernel descriptions.

Does not normally own:

- physical CU assignment;
- cache-line request expansion;
- CCL-selected PUT/GET/SIGNAL/WAIT sequences;
- rail selection;
- packet construction;
- memory or network completion times.

### 3.2 Kernel-detail frontends

One frontend supplies detail for each executable Chakra node:

- compute template;
- imported hardware microtrace;
- deterministic synthetic trace;
- MSCCL++ communication plan;
- explicit abstract fallback such as duration-backed Reduce.

All frontends must lower into the same `FineKernelDefinition` representation.

### 3.3 CCL layer

The CCL receives a collective request and chooses its implementation. It owns:

- algorithm/plan selection;
- participant-specific workgroups;
- data movement operations;
- synchronization operations;
- logical channel use;
- plan provenance.

It does not own network packet serialization or physical link timing.

### 3.4 GPU execution layer

Owns:

- workgroup-to-CU scheduling;
- finite residency;
- ready-wavefront selection;
- instruction issue;
- outstanding request accounting;
- Waitcnt, semaphore, and barrier stalls;
- kernel/workgroup/wavefront retirement.

### 3.5 Memory and topology layer

Owns:

- memory-tier placement;
- address-to-channel mapping;
- queueing, latency, bandwidth, and backpressure;
- local GPU NoC traversal;
- GPU I/O ingress/egress;
- local versus remote completion boundaries.

### 3.6 SUE transport layer

Owns:

- remote transaction/PDU construction;
- rails, ports, links, switches, and credits;
- serialization and propagation delay;
- retries and response traffic;
- remote-delivery notifications.

## 4. ASTRA-sim 3.0 Comparison

| Capability | ASTRA-sim 3.0 paper | Current repository | Classification |
| --- | --- | --- | --- |
| Chakra kernel DAG | Kernel-granularity ET | Official ET import, rank merge, dependencies | Implemented |
| Fine kernel parser | Compute and MSCCL++ parsers | Compute, trace, and MSCCL++ frontends | Implemented with manual binding |
| GPU hierarchy | Kernel/workgroup/wavefront/operation/instruction | Same hierarchy | Implemented |
| Primitive instructions | Load, Store, semaphore, Reduce, Waitcnt | Same plus TraceWaitUntil | Implemented |
| Memcpy lowering | Load, Waitcnt, Store | Same with configurable unrolling | Implemented |
| Finite CUs | Round-robin assignment and residency | Finite CUs plus multiple policies | Implemented |
| Wavefront switching | Skip stalled wavefronts | Ready-wavefront round robin | Implemented |
| Outstanding memory | Tunable Wavefront Requests | Per-wavefront tracking and memory queues | Partial architectural limits |
| Compute/communication contention | Shared GPU model | Shared scheduler and runtime | Implemented |
| Workgroup barrier | NopOp-style wavefront rendezvous | Synchronization service exists | Not integrated in event loop |
| Kernel barrier | Inter-workgroup synchronization | Synchronization service exists | Not integrated in event loop |
| MSCCL++ translation | PUT/GET/COPY and SIGNAL/WAIT | Same safe subset plus Reduce | Partial schema support |
| Multiple communication wavefronts | Data over wavefronts, control on wavefront zero | One wavefront per MSCCL++ threadblock | Missing fidelity |
| Local memory traffic | Detailed local requests | Analytical L1/L2/HBM queues | Partial |
| Internal GPU NoC | CU, HBM channels, I/O ports | No explicit internal topology | Missing |
| Remote destination HBM | Network plus destination-side memory path | Remote visibility ends at transport completion | Missing |
| Infrastructure graph | InfraGraph and translators | Flat workload/link configuration | Missing |
| Network backends | Simple, ns-3, HTSim through API | Native transport; optional SUE-Sim adapter | Partial |
| Persistent packet simulation | Concurrent network events | SUE-Sim adapter runs per PDU | Missing |
| Timing calibration | Evaluated design studies | No hardware calibration | Missing |

## 5. Existing Implementation Evidence

- Fine contracts: `native/include/simai/contracts/fine_gpu.hpp`.
- Cache-line instruction generation: `native/src/fine_instruction_stream.cpp`.
- CU scheduling: `native/src/fine_gpu_scheduler.cpp`.
- Wavefront issue and Waitcnt: `native/src/fine_wavefront_engine.cpp`.
- Local memory queues: `native/src/fine_local_memory.cpp`.
- Semaphore/barrier state: `native/src/fine_synchronization.cpp`.
- Remote request adaptation: `native/src/fine_remote_adapter.cpp`.
- MSCCL++ parser and lowering: `native/src/mscclpp_plan.cpp`.
- Chakra graph binding: `native/src/fine_graph_execution.cpp`.
- Synthetic trace input: `native/src/fine_synthetic_trace.cpp`.
- Unified fine runtime: `native/src/fine_simulator.cpp`.
- Optional ns-3 bridge: `native/src/sue_sim_transport_backend.cpp`.

## 6. CCL and MSCCL++ Plan

### 6.1 Correct flow

```text
Chakra collective node
  -> collective request
     {kind, payload, participants, in-place, topology capabilities}
  -> CCL plan selector
  -> generated or stored MSCCL++ plan
  -> schema and semantic validation
  -> per-GPU/per-workgroup operations
  -> FineKernelDefinition
  -> common GPU runtime
```

The Chakra workload should not normally contain a fully compiled sequence of
PUT/GET/SIGNAL/WAIT. Those operations are the chosen CCL implementation.

Synthetic communication traces may bypass CCL intentionally. Such execution
must be labelled trace replay and must not be described as CCL planning.

### 6.2 Current MSCCL++ support

Current mapping:

```text
PUT, GET, COPY -> Memcpy operation
SIGNAL         -> SemaphoreRelease operation
WAIT           -> SemaphoreAcquire operation
REDUCE         -> Reduce operation
NOP            -> skipped
```

Current restrictions:

- protocol must be `Simple`;
- resource reuse is rejected;
- each data operation requires one source and destination;
- semaphore operations require one MemoryChannel peer;
- only sum reduction is accepted;
- each threadblock lowers to one workgroup with one wavefront;
- logical data channels do not drive physical rail selection;
- PortChannel/proxy and flush semantics are absent;
- the CLI fine path supports one direct collective layer;
- Chakra plan association is explicit in the manifest.

This is plan ingestion, not execution of the official MSCCL++ runtime. The
official project is <https://github.com/microsoft/mscclpp>.

### 6.3 Required CCL work

1. Pin one official MSCCL++ schema/tool version.
2. Add official compiler-generated fixtures.
3. Validate plan collective, participants, chunk ranges, buffers, channels,
   semaphore balance, and program order.
4. Extend plan selection with collective, world size, payload range, payload
   multiple, in-place state, protocol, channel capabilities, and topology.
5. Wire the registry into Chakra execution rather than requiring every plan to
   be selected manually.
6. Preserve logical channel IDs through lowering.
7. Map logical channels to topology ports/rails outside the CCL parser.
8. Model or explicitly reject `flush`.
9. Stop silently discarding semantically meaningful NOP/barrier operations.
10. Support multiple wavefronts in communication workgroups.
11. Execute control operations only on the designated control wavefront when
    required by the plan semantics.
12. Produce a deterministic operation-table dump for debugging.
13. Retain the native planner only as a baseline and lower it into the same
    fine IR when used with the fine model.

### 6.4 CCL acceptance gates

- At least one official two-GPU and one four-GPU plan parses unchanged.
- Plan selection is deterministic and records provenance.
- PUT/GET byte ranges conserve plan bytes.
- Every WAIT has an identifiable matching signal/generation.
- Unsupported channel or operation forms fail explicitly.
- Communication workgroups can contain multiple wavefronts without duplicate
  data movement.

## 7. Synthetic and Captured Trace Plan

### 7.1 Schema version 1

Version 1 contains timestamp, workgroup, wavefront, Load/Store, address,
locality, owner, buffer, and size. Timestamps are earliest-issue offsets from
kernel launch.

### 7.2 Schema version 2 foundation

Version 2 retains the same record fields and requires:

- `trace_producer`;
- `launch_id`;
- `hardware_id`;
- `timestamp_semantics=earliest_issue`.
- one or more `buffer=buffer-id|owner-device|allocation-size-bytes`
  declarations.

Version 1 remains supported. Unsupported timing semantics fail explicitly.
The `--inspect-synthetic-trace` command validates and summarizes a trace
without running a simulation. Version-2 records must reference a declared
buffer, match its owner, and remain within its allocation-relative bounds.

### 7.3 Future trace fields

Add only with an explicit schema revision:

- allocation declarations and bounds;
- raw versus allocation-relative address mode;
- kernel launch, rank, stream, and iteration identity;
- grid, block, wavefront, and active-lane metadata;
- opcode/PC and access width;
- coalesced transaction size and cache-line count;
- resolved memory tier or cache outcome;
- source and destination references for memory copies;
- Reduce duration/work quantity;
- Waitcnt class and threshold;
- semaphore owner, ID, generation, and value;
- barrier scope, generation, and participants;
- atomic/fence kind and ordering scope;
- logical CCL channel;
- observed completion timestamp and measured latency;
- trace confidence/source tags.

### 7.4 Timestamp modes

Future modes must remain distinct:

- `earliest_issue`: simulator may delay issuance further;
- `observed_issue`: replay a measured issue timeline under explicit rules;
- `dependency_only`: preserve order but do not charge trace gaps;
- `calibration`: compare modeled and measured timing without forcing replay.

Never combine observed stall-inclusive timestamps with modeled contention
without defining how double counting is avoided.

### 7.5 Synthetic generators

Useful deterministic generators:

- local Load/Store streams;
- remote PUT and GET;
- PUT/SIGNAL and WAIT/GET/Reduce;
- multi-wavefront memcpy;
- ring AllGather;
- ring ReduceScatter;
- AllReduce;
- bidirectional traffic;
- memory-channel hotspot and striping;
- semaphore and Waitcnt stress;
- CU-residency contention;
- concurrent compute and communication kernels.

Synchronization must be generated from an explicit program or CCL plan. It
must not be inferred from timestamp gaps.

## 8. GPU Execution Work

### 8.1 Barriers

Barrier state and tests exist, but barrier operations are not consumed by the
unified event loop. Required work:

- register participants at kernel/workgroup admission;
- stall arrivals with explicit stall reasons;
- release all required participants atomically;
- support generations and repeated barriers;
- diagnose missing participants and deadlock;
- enforce legal inter-workgroup residency semantics.

### 8.2 Semaphores

Remote semaphore traffic is packetized. Local semaphore operations currently
do not receive equivalent local-memory timing. Required work:

- route local semaphore reads/writes through an appropriate memory tier;
- separate memory visibility from logical semaphore publication;
- preserve generation and ordering semantics;
- define acquire polling/retry behavior;
- report semaphore traffic and stall duration.

### 8.3 Wavefront and workgroup fidelity

- Add configurable communication wavefront count.
- Partition data spans without overlap or loss.
- Identify the control wavefront explicitly.
- Add optional per-CU request-buffer limits.
- Add separate outstanding Load/Store thresholds if required.
- Add optional register/shared-memory occupancy inputs.
- Keep dynamic CU assignment in the scheduler, not the trace.

### 8.4 Compute model

The duration-backed Reduce fallback remains useful but abstract. Detailed
compute may come from templates or traces. Do not invent memory behavior from
Chakra duration alone.

## 9. Memory and Internal GPU Topology

The target path is:

```text
CU -> L1 -> L2/internal fabric -> HBM channel -> GPU I/O
   -> scale-up fabric
   -> remote GPU I/O -> remote HBM channel
```

Required work:

1. Move cache line, unroll, tier, issue, and capacity assumptions into a
   dedicated hardware configuration.
2. Represent multiple HBM channels.
3. Define address-to-channel mapping.
4. Represent CU-to-memory and CU-to-I/O routes.
5. Add internal NoC latency, bandwidth, queues, and contention.
6. Add GPU ingress and egress queues.
7. Charge remote source-HBM service for GET.
8. Charge remote destination-HBM service for PUT.
9. Define completion at ingress, GPU delivery, HBM visibility, and local
   acknowledgement.
10. Add optional cache-hit/miss input or a deliberately simple cache model.
11. Keep register-file data movement conceptual until a capacity model is
    explicitly selected.

An internal topology schema can be implemented before adopting or recreating
all InfraGraph capabilities.

## 10. SUE and Network Work

The native deterministic transport is executable. The optional ns-3/SUE-Sim
adapter currently invokes the ns-3 simulator for each PDU and therefore does
not model a persistent concurrent session.

Required work:

- create one persistent simulation session;
- allow multiple in-flight requests and PDUs;
- use asynchronous completion callbacks;
- preserve operation/request/PDU provenance;
- map CCL channels to topology ports and rails;
- define CBFC/credit ownership exactly once;
- include response traffic for remote reads/acquires;
- distinguish network delivery from HBM visibility;
- conserve logical, payload, wire, retry, and completion counters;
- support deterministic shutdown and drained-state verification.

## 11. Reporting

Add a formal execution table containing:

- graph node and kernel;
- GPU, CU, workgroup, and wavefront;
- operation and instruction IDs;
- issue, service-start, service-end, and completion times;
- source/destination buffer, owner, address, size, and memory tier;
- local/remote classification;
- outstanding request counts;
- Waitcnt threshold;
- semaphore/barrier identity and generation;
- logical channel, port, rail, transaction, and PDU;
- stall reason and stall duration.

Summary metrics should include kernel latency, exposed communication, CU
occupancy, HBM traffic, synchronization stalls, fabric congestion, queue
occupancy, and critical path.

## 12. Validation and Calibration

### 12.1 Functional validation

- deterministic repeated reports;
- instruction/request/byte/PDU conservation;
- malformed trace and plan rejection;
- semaphore generation tests;
- barrier deadlock and release tests;
- remote Load request/response tests;
- remote Store visibility tests;
- concurrent compute/communication contention;
- topology routing tests.

### 12.2 Scalability validation

- 2, 4, and 8 GPUs initially;
- increasing workgroups and wavefronts;
- increasing payload and cache-line count;
- bounded simulator memory;
- event throughput and wall-clock runtime reporting.

### 12.3 Hardware calibration

- record exact GPU/link configuration;
- collect repeatable kernel and collective measurements;
- separate launch, local memory, synchronization, and network contributions;
- tune only documented parameters;
- validate on measurements not used for tuning;
- report absolute and percentage error with workload coverage;
- never claim H200/NVLink accuracy before this gate passes.

## 13. Ordered Implementation Phases

### Phase A: trace provenance and inspection

- Version-2 provenance metadata.
- Explicit earliest-issue semantics.
- Backward-compatible version-1 parsing.
- Inspection CLI and summary counters.

Exit gate: both versions parse; malformed semantics fail; inspection requires
no hardware workload.

### Phase B: trace allocations and richer instructions

- Allocation declarations and address bounds. Initial buffer-owner/size
  declarations are implemented; raw-address/base-address mapping remains.
- Instruction-class schema extension.
- Waitcnt, Reduce, semaphore, and barrier records.
- Trace-to-Chakra launch correlation.

### Phase C: production CCL frontend

- Official MSCCL++ fixtures and pinned schema.
- Complete plan validation and selection.
- Multi-wavefront communication lowering.
- Channel preservation and operation-table output.

### Phase D: synchronization completion

- Runtime workgroup/kernel barriers.
- Timed local semaphores.
- Ordering/fence semantics and diagnostics.

### Phase E: hardware configuration and GPU topology

- Dedicated GPU/memory/topology configuration.
- HBM channels, internal NoC, and GPU I/O.
- Remote-HBM service and completion stages.

### Phase F: persistent SUE-Sim

- One asynchronous ns-3 session.
- Multiple in-flight PDUs.
- Topology-aware ports/rails and drained completion.

### Phase G: reporting, calibration, and research evaluation

- Detailed execution/stall table.
- Hardware measurements and error report.
- Fidelity/performance comparisons against coarse mode.

## 14. Immediate Next Actions

1. Finish and verify Phase A.
2. Add raw-address/base-address mapping without changing version-1.
3. Add a trace inspection table, not only aggregate summary.
4. Obtain one official compiler-generated MSCCL++ fixture.
5. Design the CCL selection key and Chakra binding contract.
6. Integrate workgroup barriers into the event loop.
7. Move hard-coded cache-line and memory timing into hardware configuration.

## 15. Explicit Non-Claims

Until the relevant gates pass, do not describe the project as:

- GPU-binary or ISA accurate;
- cache accurate;
- H200 calibrated;
- complete MSCCL++ runtime integration;
- complete ASTRA-sim 3.0 parity;
- persistent asynchronous SUE-Sim execution;
- validated at ASTRA-sim 3.0 scales.

The accurate current description is: a deterministic, event-driven,
fine-grained scale-up GPU simulation prototype with Chakra kernel graphs,
synthetic and imported memory traces, safe-subset MSCCL++ lowering, finite CU
scheduling, local analytical memory, semaphore-aware remote operations, and a
native SUE-compatible packet transport.
