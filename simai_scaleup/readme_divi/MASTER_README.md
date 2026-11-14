# SimAI Scale-Up Simulator: Master Record

## Purpose of This Document

This is the authoritative project ledger for the clean `simai_scaleup`
implementation. It records every checkpoint, decision, source reviewed, file
change, validation result, limitation, and next action.

No implementation phase may be marked complete only because code compiles.
Each phase must satisfy its stated acceptance checks and leave reproducible
evidence here.

The detailed engineering roadmap is maintained in
[`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md). This master record reports
what has actually happened against that plan.

The concise restart/handoff state is maintained in
[`context.md`](context.md).

The user-facing input/pipeline/comparison guide and future roadmap are:

- [`project_guide/INPUT_PIPELINE_AND_COMPARISON.md`](project_guide/INPUT_PIPELINE_AND_COMPARISON.md)
- [`project_guide/FUTURE_PLAN_AND_MODEL_LIMITS.md`](project_guide/FUTURE_PLAN_AND_MODEL_LIMITS.md)

Current project state: **CP-00 through CP-15 are complete. The clean native
scale-up simulator has passed its final gate**.

## Project Objective

Build a self-contained end-to-end AI workload simulator for a single
intra-node scale-up domain. It must model model-layer execution, compute and
communication dependencies, parallel groups, collective algorithms, GPU
execution resources, and a native Scale-Up Ethernet (SUE) fabric.

SUE is the only supported communication backend. Preserving compatibility
with upstream SimAI is not a requirement.

## Repository Roles

| Path | Role | Mutation policy |
|---|---|---|
| `simai_scaleup/` | New clean implementation | Active project |
| `simai_fresh/` | Retired prototype | Removed after CP-15 passed every final gate |
| `SimAI/` | Older experimental implementation | Reference only |
| `SUE-Sim/` | Standalone research/reference simulator | Reference only |
| `astra-sim-sue/` | Earlier ASTRA/SUE experiment | Reference only |

The old `simai_fresh` directory was removed on 2026-10-06 after the new
repository passed a clean build, end-to-end workloads, SUE correctness tests,
zero-contamination audit, research sweep, and unique-artifact review.

## Baseline

- Official source: `https://github.com/aliyun/SimAI.git`
- Branch at clone: `master`
- Baseline commit: `a57d765c5478f319c5f5ab6fdc53f9cc7f461743`
- Clone directory: `C:\Users\S15\btp\simai_scaleup`
- Clone policy: top-level repository only; forbidden network submodules were
  not initialized.

## Non-Negotiable Boundaries

The final runtime and build graph must not contain:

- scale-out or multi-node execution;
- RDMA queue-pair or completion-queue models;
- RoCE, InfiniBand, IP, TCP, UDP, PFC, ECN, or DCQCN behavior;
- NS-3, HTSim, Garnet, physical socket, or analytical network backends;
- MPI or socket bootstrap;
- rendezvous/matching send-receive protocols;
- an empirical NVLink ratio-table timing path;
- runtime backend selection;
- a PUT/GET compatibility facade backed by legacy send/receive operations.

Historical audit and migration records may name removed systems. Production
source, build files, configurations, examples, CLI help, and user-facing
documentation may not present them as supported behavior.

## Working Method and Checkpoint Policy

Each checkpoint follows this sequence:

1. State the exact question or behavior under investigation.
2. List files and dependency edges in scope.
3. Record assumptions before editing.
4. Create or identify an objective test.
5. Make one coherent implementation change.
6. Build from a clean state.
7. Run focused tests and the current end-to-end test.
8. Run the forbidden-dependency scan.
9. Record results, known limitations, and the next checkpoint here.
10. Stop for review at architectural or irreversible boundaries.

Large phases will not be implemented in a single uncontrolled pass.

## Planned Checkpoints

| ID | Checkpoint | Required evidence | Status |
|---|---|---|---|
| CP-00 | Clean official baseline | Commit, remote, clean status, submodule state | Complete |
| CP-01 | Research and architecture preflight | Reviewed sources and adopted/rejected ideas | Complete |
| CP-02 | File-by-file upstream audit | `FILE_AUDIT.md` and dependency graph | Complete |
| CP-03 | Native semantic contracts | Tested workload, micro-op, transaction, completion interfaces | Complete |
| CP-04 | Minimal vertical slice | Two-layer trace to deterministic final report | Complete |
| CP-05 | PDU and wire serialization | Analytical equality tests | Complete |
| CP-06 | Multi-rail fabric | Conservation and rail-utilization tests | Complete |
| CP-07 | Finite switch queues | Queue drain, capacity, arbitration, backpressure tests | Complete |
| CP-08 | Credit flow control | Credit conservation and starvation tests | Complete |
| CP-09 | Retry and reassembly | Error/replay and exactly-once completion tests | Complete |
| CP-09A | General collective planner framework | Planner interface, provenance, algorithm validation | Complete |
| CP-10 | GPU execution resources | CU/workgroup/wavefront contention tests | Complete |
| CP-11 | Collective coverage | AllReduce, ReduceScatter, AllGather, AllToAll tests | Complete |
| CP-12 | Full AI workload integration | Bounded AICB/model trace and compute overlap tests | Complete |
| CP-13 | Zero-contamination audit | Source, build, config, docs, symbols, linkage | Complete |
| CP-14 | Research validation | Reproducible workload sweeps and counter definitions | Complete |
| CP-15 | Prototype retirement | Unique-artifact review and removal of `simai_fresh` | Complete |

## Research Review: ASTRA-sim 3.0

### Source

- Paper: *ASTRA-sim 3.0: Next-Level Distributed Machine Learning
  Simulations via High-Fidelity GPU and Infrastructure Modeling*
- Authors: William Won et al.
- Date: June 2026
- arXiv: `2606.10440v1`
- Local copy: `C:\Users\S15\Downloads\astra3.pdf`
- Length: 12 pages
- Review status: complete

### Problems Identified by the Paper

The paper argues that ASTRA-sim 2.0 is insufficient for modern collective and
inference studies because it:

1. supports only a small set of fixed textbook collective algorithms;
2. lacks detailed GPU control/data-path and resource-contention modeling;
3. models large communication operations too coarsely;
4. lacks a common physical infrastructure description across backends.

These observations strongly support our decision not to preserve the old
SimAI/ASTRA execution path merely for compatibility.

### Ideas Relevant to This Project

#### 1. Common Fine-Grained Kernel Representation

ASTRA-sim 3.0 lowers both compute and communication kernels into a shared
hierarchy:

```text
kernel -> workgroups -> wavefronts -> operations -> primitive instructions
```

This is directly relevant. A unified representation allows compute kernels and
communication workgroups to contend for the same GPU resources without
arbitrary one-kernel-at-a-time rules.

Decision: adopt the principle, but define only the operations needed for the
single-domain SUE simulator.

#### 2. Cache-Line-Sized Network Requests

The paper uses a cache-line-sized `WavefrontRequest` (typically 128 bytes) as
the transfer submitted by a CU to the network. This exposes:

- local HBM/register movement;
- outstanding-request limits;
- workgroup and wavefront concurrency;
- control-path latency;
- overlap between communication and reduction.

Decision: adopt a comparable fine-grained request boundary, but do not assume
that a GPU cache line and a SUE wire flit are necessarily the same object. The
GPU request size, SUE payload size, and wire encoding must remain independently
configurable and explicitly translated.

#### 3. Explicit Control Operations

Relevant primitives in the paper include:

- load and store;
- semaphore acquire and release;
- reduction;
- wait-for-outstanding-count;
- intra-workgroup synchronization;
- inter-workgroup barriers.

Decision: incorporate these semantics when defining the native contracts. A
simple `PUT/GET/SIGNAL/WAIT` enumeration is not enough unless dependencies,
buffer readiness, memory fences, and completion scope are precisely defined.

#### 4. MSCCL++ Schedule Translation

The paper translates MSCCL++ per-GPU/per-workgroup schedules into its
fine-grained GPU operation model. This is superior to inventing a collective
schedule from a workload row and calling it “MSCCL++.”

Decision: distinguish clearly between:

- a true imported MSCCL++ schedule;
- an internally generated collective plan;
- the lowered micro-op program executed by the simulator.

Each result must record which source produced its schedule.

#### 5. GPU Resource Contention

ASTRA-sim 3.0 models workgroup placement on finite CUs, ready/stalled
wavefront scheduling, outstanding requests, and configurable loop unrolling.
Its case studies show that these details affect collective performance and
that benefits saturate when dispatch limits are reached.

Decision: include a deliberately scoped GPU execution model. Network-only
timing is insufficient for the intended investigation, particularly when
comparing PUT and GET schedules or compute/communication overlap.

#### 6. PUT/GET Performance Is Collective-Dependent

The paper reports that GET-based ReduceScatter can benefit from immediate
reduction overlap, while PUT-based AllGather may avoid control requests being
blocked behind data responses. Fair arbitration between control and data
traffic can change the result.

Decision: the SUE switch must distinguish control and data traffic and expose
an arbitration policy. We must not assume PUT is always superior to GET or
vice versa.

#### 7. Graph-Based Infrastructure Description

InfraGraph represents components and directed links with annotated physical
properties. The general idea is useful for describing GPUs, CUs, HBM channels,
I/O ports, SUE switch ports, and rails.

Decision: adopt a small SUE-specific attributed graph or equivalent typed
configuration. Do not import multi-backend translators or generic scale-out
blueprints.

#### 8. Fidelity Versus Simulation Cost

The paper deliberately stops at load/store granularity instead of simulating a
complete GPU ISA. Its evaluation shows runtime scaling linearly with message
size at 128-byte granularity and decreasing throughput as system scale grows.

Decision: every fine-grained feature needs a performance budget and an
optional aggregation policy whose accuracy is tested. “More detailed” is not
automatically “better” if realistic workloads become unusably slow.

### Ideas Explicitly Out of Scope

The following ASTRA-sim 3.0 features are not to be carried into the final
runtime:

- multiple interchangeable network backends;
- scale-out fabrics and host-cluster blueprints;
- backend translators for NS-3 or HTSim;
- NIC-centric topology where it is not part of the SUE endpoint model;
- MPI-defined semantics as the simulator’s execution contract;
- the paper’s scale-out Clos validation case.

### Corrections to the Earlier Prototype Direction

This paper reinforces several corrections that must be applied before reusing
old prototype code:

1. A 128-byte GPU request must not automatically be equated with a 128-byte
   wire payload.
2. A real finite switch queue must track departures, enforce capacity, and
   create backpressure; an ever-increasing occupancy counter is insufficient.
3. Semaphore traffic needs memory and arbitration semantics, not only a small
   tagged flit.
4. PUT/GET execution must account for local and remote memory paths.
5. Compute delay cannot always be injected as a single timestamp jump if
   compute and communication overlap is being studied.
6. Aggregate bandwidth must be reported against the correct physical cut and
   must never exceed it.
7. “Cycle accurate” must not be claimed unless the modeled clock domains and
   cycle transitions are explicitly defined and validated.

## Technical Decision: NCCL Versus MSCCL++ for One-Sided Operations

The full beginner-to-implementation architecture connecting legacy SimAI,
MockNCCL reuse, the default native planner, optional MSCCL++ import, `Sys.cc`
replacement, and future GPU modeling is recorded in
[`COLLECTIVE_PLANNER_AND_GPU_PIPELINE.md`](COLLECTIVE_PLANNER_AND_GPU_PIPELINE.md).

### Correct Answer

It is not accurate to say that all versions of standard NCCL have no concept
of one-sided communication.

Historically, NCCL exposed collectives and matched `ncclSend`/`ncclRecv`
operations. Its point-to-point model was explicitly two-sided: a send required
a corresponding receive with matching count and type. This remains an
important part of NCCL's public model.

However, NCCL 2.29 introduced one-sided communication through
`ncclPutSignal()` and `ncclWaitSignal()`. A source rank can write into a
registered remote window without the target issuing a matching receive. Modern
NCCL therefore has a real one-sided PUT-and-signal concept. The target memory
must be registered, and synchronization and communication contexts follow
NCCL's window/communicator rules.

Primary sources:

- [NCCL point-to-point and one-sided communication](https://docs.nvidia.com/deeplearning/nccl/user-guide/docs/usage/p2p.html)
- [NCCL overview](https://docs.nvidia.com/deeplearning/nccl/user-guide/docs/overview.html)
- [MSCCL++ project and primitive interface](https://github.com/microsoft/mscclpp)
- [MSCCL++ paper](https://arxiv.org/abs/2504.09014)

### What NCCL Still Does Not Give Us

NCCL's current one-sided host API does not by itself provide the same general
collective-program representation that this simulator needs. In particular,
it does not replace all of the following MSCCL++ capabilities as one unified
abstraction:

- explicit fine-grained `put`, `get`, `signal`, `flush`, and `wait` primitives;
- per-GPU and per-workgroup custom collective schedules;
- direct expression of dependencies between those micro-operations;
- a schedule that can be lowered into GPU resource operations and then into
  SUE transactions;
- convenient exploration of alternative PUT-based and GET-based algorithms.

NCCL may internally use direct paths and modern releases expose additional
device-initiated mechanisms, but internal transport implementation must not be
confused with an explicit, portable schedule representation.

### Baseline SimAI/SimCCL Limitation

The distinction that matters most to this project is not only what the newest
NCCL release supports. It is what the SimAI/SimCCL model in our baseline
actually represents. The inspected SimCCL MockNCCL v2.30 sources contain no
model for `ncclPutSignal`, `ncclWaitSignal`, registered communication windows,
or a comparable one-sided RMA schedule. Their modeled execution remains based
on existing collective/flow and send/receive abstractions.

Consequently, keeping MockNCCL unchanged would not give the simulator the
one-sided semantics required for SUE research, even though contemporary real
NCCL has begun adding one-sided APIs.

### Is MSCCL++ Strictly Necessary?

No: the final simulator does not need to link against MSCCL++ or make it a
mandatory runtime dependency. We can define and implement our own native,
well-tested intermediate representation for memory operations, synchronization,
GPU execution, and SUE transactions.

MSCCL++ is necessary only in the following qualified sense:

1. Its semantics are an important reference for defining correct fine-grained
   one-sided operations.
2. Its schedule representation is needed if we want to import and faithfully
   simulate real MSCCL++ custom collective programs.
3. It provides a credible basis for comparing PUT-, GET-, and mixed-operation
   algorithms instead of generating an undocumented approximation and calling
   it MSCCL++.

### Project Decision

The clean simulator will therefore:

1. implement a native internal micro-op representation independent of NCCL and
   MSCCL++;
2. give PUT, GET, SIGNAL, WAIT, FLUSH/fence, REDUCE, and barrier operations
   explicit tested semantics;
3. keep the schedule source separate from execution semantics;
4. support an MSCCL++ schedule importer as an optional frontend when the
   required format and semantics have been verified;
5. never label an internally generated schedule as MSCCL++ merely because it
   uses operations with similar names;
6. retain neither legacy MockNCCL send/receive machinery nor MSCCL++ itself as
   an unnecessary runtime dependency.

This decision gives us clean SUE semantics while preserving the ability to
study real MSCCL++ schedules later.

## Research Review: MSCCL++

### Sources

- [MSCCL++ ASPLOS 2026 paper](https://arxiv.org/pdf/2504.09014)
- [Official MSCCL++ repository](https://github.com/microsoft/mscclpp)
- [Official MSCCL++ DSL quick start](https://microsoft.github.io/mscclpp/dsl/quick_start.html)
- [Official MSCCL++ programming documentation](https://microsoft.github.io/mscclpp/)

Review status: complete for the architectural questions needed before the
source audit. Exact importer compatibility will be revisited against a pinned
MSCCL++ version when the optional importer is implemented.

### Findings That Affect the Simulator

MSCCL++ is a hierarchy rather than one API:

1. The Primitive API exposes hardware-near channel operations.
2. The Python DSL describes global per-rank/per-thread-block algorithms.
3. The Collective API offers conventional collective calls implemented using
   lower layers.

The Primitive API separates transfer, notification, and completion. For an
asynchronous transfer:

- `put` begins movement but does not necessarily make the source reusable;
- `signal` notifies the peer;
- `wait` blocks until the expected peer signal and, in the ordered form,
  establishes visibility of preceding memory operations;
- `flush` establishes local completion of preceding transfers.

Relaxed signal/wait synchronizes execution but does not establish memory
visibility. This distinction must be represented explicitly; a single generic
`SIGNAL` event would be insufficient.

MSCCL++ channels describe different hardware access modes. The SUE simulator
will not reproduce PortChannel, MemoryChannel, or SwitchChannel as runtime
transport implementations. Instead, the optional importer will lower their
observable schedule semantics into our hardware-independent micro-ops. It must
reject channel features that have no defined SUE mapping.

An MSCCL++ execution plan includes:

- collective metadata and supported sizes;
- channel types and data-transfer protocol;
- rank and thread-block assignments;
- buffers and chunks;
- semaphore setup;
- operation sequences and loops;
- dependencies and fused operations.

The DSL performs chunk-level data-dependence analysis and operation fusion.
Therefore, faithfully importing an execution plan means preserving its
dependencies and fused-operation meaning, not merely extracting a list of
operation names.

### MSCCL++ Requirements for Native Contracts

The native intermediate representation must include:

- explicit rank and thread-block ownership;
- typed source and destination buffer regions;
- byte ranges and alignment;
- asynchronous issue versus local completion versus remote visibility;
- ordered and relaxed notification semantics;
- expected semaphore values rather than Boolean flags;
- barriers and thread-block dependencies;
- reduction operation and datatype;
- loops or a deterministic expanded form;
- provenance identifying an internal planner or an imported schedule;
- a representation for fused operations without losing their component
  semantics or resource costs.

## Research Review: Broadcom Scale-Up Ethernet Framework

### Authoritative Source

- [Broadcom Scale-Up Ethernet Framework Specification,
  Scale-Ethernet-RM104, September 26, 2025](https://www.globenewswire.com/Tracker?data=4zNtGXPy_Scm0BaDkoBXGpKEdkTlwdCAITRg7GcUxeQZpH-ezIBko2PryVnjzILZD0xRj15e-pRNvjgQT8NPY9079eG1__0Q6wbPVy66MrTwls1MKeBVO12jl9qtVeKz)
- [Broadcom Tomahawk Ultra product description](https://www.broadcom.com/products/ethernet-connectivity/switching/strataxgs/bcm78920-series)

Review status: complete for RM104's public framework-level behavior.

This Broadcom specification, not the earlier prototype comments, is the
primary authority for the SUE transport model. UALink is a useful independent
memory-fabric reference, but it is not interchangeable with SUE and will not
define the runtime protocol.

### Specification Facts

The following are facts stated by RM104 and must be distinguished from our
configurable modeling choices:

| Area | SUE framework fact |
|---|---|
| Purpose | Move memory transactions between XPUs over Ethernet |
| Deployment | Primarily a single switch hop; direct mesh is also possible |
| Operations | Generic command/response capable of put, get, and atomics |
| Memory model | Shared-memory, one-sided load/store/atomic services |
| One-sided meaning | No destination-process awareness, but the operation is acknowledged |
| Transaction size | 256 bytes in the SUE/SUE-Lite comparison |
| Scale | Up to 1,024 XPUs |
| SUE bandwidth | 800 Gb/s, with scaling to 1.6 Tb/s |
| SerDes | 200 Gb/s, with 100 and 50 Gb/s support |
| Ports per SUE | One, two, or four |
| Parallel instances | Multiple SUE instances per XPU are expected |
| Virtual channels | Up to four; used for independent traffic classes and deadlock avoidance |
| Ordering | Strict per source/destination/VC, or unordered multi-port operation |
| Encapsulation | Same-destination/VC commands can be opportunistically packed |
| PDU size | Up to 4096 bytes for full SUE; SUE Lite has a smaller limit |
| Wire unit | Each SUE PDU maps to one Ethernet packet |
| Headers | Standard Ethernet or optimized AFH formats; AFH Gen 2 supports 6- or 12-byte forms |
| Scheduling | Weighted round robin across VCs and arrival order within a VC |
| Reliability | Packet sequence numbers, ACK/NACK, CRC, in-order per-plane delivery, and Go-Back-N |
| Link reliability | FEC plus optional link-level packet retry |
| Losslessness | PFC or CBFC; this project will deliberately select CBFC |
| Load balancing | Above SUE across instances; within one multi-port SUE in unordered mode |
| Completion | Receiver acknowledgment means arrival, not necessarily placement into memory |

### Critical Corrections to the Old Prototype

The earlier SUE implementation must not be copied as the protocol model because
it makes unsupported assumptions:

1. SUE is not defined as a stream of fixed 128-byte flits. RM104 defines
   256-byte transactions that may be packed into larger PDUs, with one PDU per
   Ethernet packet.
2. The previous 12-byte “flit header” conflated a custom header with AFH and
   omitted SUE reliability and transaction encapsulation.
3. A fixed eight-rail, 200-Gb/s topology is only one possible configuration,
   not the SUE architecture itself.
4. Simple round-robin striping by sequence number is not the specified loading
   model. SUE uses per-plane ordering modes, port availability, and a load
   balancer above SUE when multiple instances exist.
5. Link-level retry operates on corrupted packets between peer link devices;
   it is not specified as a 64-flit SRAM replay queue with an 8-ns delay.
6. Full SUE includes an end-to-end reliable transport with ACK/NACK, PSNs, and
   Go-Back-N. Modeling only link retry does not model full SUE reliability.
7. The old credit formula `1 + ceil(payload/32)` is not established by RM104.
   It must not be presented as a SUE fact.
8. XPU-interface credits and Ethernet-link CBFC are distinct flow-control
   loops and require separate models.
9. A completion callback at packet arrival must not automatically mean the
   command's data has been placed into destination memory.
10. A 15-ns central-switch delay contradicts the public Tomahawk Ultra figure
    of 250 ns switch latency. Latency will be configurable and its source
    recorded rather than hard-coded.

### Selected SUE Profile for the First Implementation

The first vertical slice will target full SUE, not SUE Lite:

- single switched hop;
- one SUE instance and one port per XPU initially;
- strict ordering initially;
- command/response transaction model;
- CBFC selected instead of PFC;
- full reliability state and Go-Back-N added only at its checkpoint;
- no in-network collective offload in the first implementation;
- no cache coherence;
- address translation, memory protection, and registration remain outside the
  SUE endpoint, matching the framework boundary.

Multi-port unordered load balancing, multiple SUE instances, mesh deployment,
and switch collective offload are later research extensions. Keeping them out
of the first slice prevents unvalidated complexity while preserving the right
abstraction boundaries.

## Supporting Review: UALink

### Sources

- [UALink official specifications page](https://ualinkconsortium.org/specification/)
- [UALink 200G 1.0 overview](https://ualinkconsortium.org/blog/ualink-200g-1-0-specification-overview-802/)

The full UALink evaluation specification requires explicit license acceptance,
so no automated acceptance or download was performed. Only publicly available
official overview material was used.

Useful cross-checks from the official overview are that scale-up memory fabrics
use read/write/atomic operations, software-managed coherency, explicit address
spaces, and transaction ordering rules. UALink mentions 64-to-256-byte
transactions and ordered request addresses with unordered completions.

Project decision: use these points to test whether our native contracts are
general enough, but do not label SUE behavior as UALink behavior or combine
their packet/protocol details.

## CP-01 Facts-versus-Assumptions Register

Every numerical configuration must name its status and source.

| Item | Status after research preflight |
|---|---|
| 256-byte SUE transaction | Specification fact |
| PDU packing up to 4096 bytes | Specification fact for full SUE |
| One PDU per Ethernet packet | Specification fact |
| Up to four VCs | Specification fact |
| Weighted round robin across VCs | Specification fact |
| Go-Back-N after residual packet loss | Specification fact |
| 200-Gb/s SerDes capability | Specification fact |
| 800-Gb/s SUE instance | Specification fact/example target |
| 250-ns Tomahawk Ultra switch latency | Product-specific public figure |
| CBFC rather than PFC | Project design choice |
| One switch hop, one port, strict ordering | First-slice scope choice |
| Number of XPUs in tests | Experiment configuration |
| CBFC credit unit and buffer capacity | Unresolved modeling parameters |
| Endpoint congestion-control window | Unresolved modeling parameter |
| Cable/propagation delay | Experiment configuration requiring source |
| Error rate and LLR replay latency | Experiment parameters requiring source |
| XPU NoC and HBM service timing | Unresolved device-model parameters |

Unresolved parameters may be introduced only as clearly labeled assumptions
with sensitivity tests. They must not be described as specification values.

## CP-01 Native Interface Requirements

The file audit and contract design must preserve these layer boundaries:

```text
AI workload graph
    -> collective request
    -> schedule plan with rank/thread-block operations
    -> GPU execution and memory-order events
    -> generic XPU commands/responses
    -> SUE per-destination/per-VC queues and packetization
    -> Ethernet links, switch, CBFC, and reliability
    -> destination SUE unpacking
    -> destination XPU NoC/memory service
    -> operation completion and workload dependency release
```

The minimum native data model must include:

- typed local/remote buffers and address ranges;
- rank, thread block, operation order, and dependency identity;
- operation issue, local completion, remote arrival, remote visibility, and
  final workload completion as distinct states;
- command, response, traffic class/VC, data presence, and byte length;
- transaction-to-PDU packing metadata;
- PDU/packet identity, PSN, ACK/NACK state, and ordering domain;
- endpoint, port, plane, link, and switch resources;
- separate XPU-interface credit and link-CBFC accounting;
- deterministic scheduling and random seed provenance;
- counters allowing conservation of commands, bytes, transactions, PDUs,
  packets, credits, retries, and completions.

These requirements are inputs to CP-02 and CP-03, not a license to implement
them all at once.

## Checkpoint Log

### CP-00: Clean Official Baseline

Status: complete.

Actions:

- Verified that `simai_scaleup` did not already exist.
- Cloned the official SimAI repository into a new parallel directory.
- Verified baseline commit `a57d765c5478f319c5f5ab6fdc53f9cc7f461743`.
- Left `simai_fresh` untouched.
- Did not recursively initialize submodules.
- Added `IMPLEMENTATION_PLAN.md`.

Evidence:

- The clone has its own `.git` repository.
- AICB, SimCCL, NS-3, and SimAI-CLEM are uninitialized gitlinks at this
  checkpoint.

Limitations:

- No upstream file classification has occurred.
- No build is expected yet because required approved submodules have not been
  selected or initialized.

### CP-01: Research and Architecture Preflight

Status: complete.

Completed:

- Read the supplied ASTRA-sim 3.0 paper in full.
- Recorded adopted, rejected, and cautionary ideas above.
- Established the checkpoint ledger and retirement requirement for
  `simai_fresh`.
- Reviewed the MSCCL++ paper, official DSL guide, primitive semantics, and
  execution-plan responsibilities.
- Reviewed Broadcom SUE RM104 as the primary fabric framework specification.
- Reviewed public UALink material as a supporting comparison without accepting
  its evaluation-copy license.
- Corrected unsupported assumptions inherited from the earlier prototype.
- Created the facts-versus-assumptions register.
- Converted findings into native-interface requirements for the audit.

No production implementation has started.

### CP-02: File-by-File Upstream Audit

Status: complete.

Deliverable:

- [`FILE_AUDIT.md`](FILE_AUDIT.md)

Completed:

- Inventoried all 138 files under the ASTRA network frontend, system, and
  workload source directories.
- Gave every file an individual `KEEP`, `REWRITE`, `REFERENCE`, or `REMOVE`
  disposition with a reason.
- Audited the 14 adjacent ASTRA build/input files and the repository-level
  SimCCL, AICB, NS-3, CLEM, Vidur, script, example, documentation, and
  container integration points.
- Traced the current semantic dependency chain from `Workload`/`Layer`,
  through `Sys` and collective streams, to the two-sided
  `AstraNetworkAPI::sim_send`/`sim_recv` boundary and its three old backends.
- Recorded a SUE-only proposed build graph and forbidden dependency edges.
- Bounded CP-04 to an eight-endpoint, two-layer, one-AllReduce deterministic
  vertical slice with a hand-calculated serialization-time test.

Decision:

- Zero upstream production source files are approved to compile unchanged.
- The fresh official repository remains the provenance baseline, but the
  runtime will be implemented in a new native source/build tree.
- Upstream workload ordering and collective/topology algorithms may be used
  only as reviewed reference material.
- `Sys`, `AstraNetworkAPI`, `Layer`, and the old collective implementations
  will not be ported before the native contracts are designed and tested.

Evidence:

- An automated path check found `source_files=138` and `missing_rows=0` in
  `FILE_AUDIT.md`.
- The submodules remain uninitialized; no forbidden backend was downloaded or
  built.
- No upstream production source was modified, deleted, or copied during this
  checkpoint.

Limitations:

- CP-02 is an architecture and dependency audit, not a compile/runtime gate.
- Exact C++ namespaces, directory names, schemas, and ownership rules remain
  decisions for CP-03.

### CP-03: Native Semantic Contracts

Status: complete.

Deliverable:

- [`NATIVE_CONTRACTS.md`](NATIVE_CONTRACTS.md)
- `native/include/simai/contracts/`
- `native/src/`
- `native/tests/contracts_test.cpp`

Semantic decisions:

- Simulation time uses unsigned integer nanoseconds.
- All identity domains use distinct strong types with zero reserved as
  invalid.
- Buffer slices are copied descriptors with explicit owner, buffer, range,
  and memory region; no raw pointer crosses a contract.
- `ParallelGroup` defines stable logical-rank order within one scale-up
  domain.
- `CollectivePlan` contains an explicit dependency DAG of PUT, GET, SIGNAL,
  WAIT, and REDUCE micro-operations.
- Thread-block program order and dependency readiness are separate
  constraints.
- Issue, local completion, remote arrival, remote visibility, and finalization
  are recorded as five distinct ordered completion stages.
- SUE commands and responses explicitly carry service, role, endpoints,
  ordering domain, virtual channel, logical bytes, data presence, operands,
  and response status.
- Transaction logical bytes are not treated as on-wire bytes; PDU/packet
  fields remain deferred until their checkpoints.
- Equal-time events are ordered by a monotonic insertion sequence, and the
  event queue rejects scheduling into the past.
- Report counters reserved for unimplemented mechanisms must remain zero; no
  estimated values are permitted.

Implemented validation:

- buffer range overflow and invalid identities;
- workload self/duplicate dependencies and kind-specific operands;
- duplicate group members;
- micro-operation ownership, operands, release stages, and dependencies;
- plan membership, unique identities, missing dependencies, program order,
  and dependency cycles;
- completion stage gaps, duplicates, and time reversal;
- SUE endpoint/slice ownership, role/status, data presence, byte length, and
  virtual-channel range;
- report timing and completion conservation bounds.

Evidence:

- Built from a new out-of-source verification directory with GCC 6.3 using
  C++14 and `-Wall -Wextra -Wpedantic -Werror`.
- The unit executable reported `All native contract tests passed` in both the
  initial and clean verification builds.
- Compiler-generated dependency output contained only standard headers and
  files under `native/include/simai/contracts`.
- The CP-03 forbidden-source scan returned `zero forbidden matches` across
  the native tree and new CMake files.
- `git diff --quiet -- astra-sim-alibabacloud` returned exit code zero; legacy
  ASTRA source remains unchanged.
- No submodule was initialized or added to the native build graph.

Limitations and deferrals:

- The environment provides GCC 6.3 but no CMake executable. The CMake project
  metadata is present; verification used the equivalent direct compiler
  invocation.
- CP-03 validates a collective plan but does not generate one.
- The event queue orders typed event records but executes no callbacks.
- No workload parser, link timing, PDU/packet layout, fabric resource, or CLI
  exists yet.

### CP-04: Minimal End-to-End Vertical Slice

Status: complete.

Deliverable: [`CP04_VERTICAL_SLICE.md`](CP04_VERTICAL_SLICE.md)

Completed:

- added a strict versioned workload parser for exactly two layers, eight
  endpoints, one tensor-parallel group, and one AllReduce;
- lowered AllReduce to a validated 21-operation native DAG containing 14 PUTs
  and 7 local reductions;
- executed the DAG through the deterministic event queue and a single shared
  payload-only SUE link;
- separated issue, local serialization completion, arrival, visibility, and
  finalization timestamps;
- added an executable and deterministic report writer;
- added exact layer timing, exposed-communication, payload-conservation, and
  operation-conservation results;
- proved and tested a 580 ns AllReduce and 730 ns end-to-end golden result.

Evidence:

- warning-as-error GCC 6.3 C++14 build passed for contracts, runtime, tests,
  and CLI;
- contract and CP-04 vertical-slice tests passed;
- independently executed CLI output matches the committed golden report;
- dependency output contains only standard and native headers;
- forbidden production-source scan, ASTRA integrity check, and whitespace
  check passed.

Limitations:

- each CP-04 PUT is one logical transaction regardless of its size;
- link time includes logical payload serialization and propagation only;
- no PDU/packet representation or wire overhead exists yet;
- only the internal gather-reduce-broadcast AllReduce is executable;
- reduction consumes zero modeled time until GPU resources are added.

### CP-05: Bounded Transactions, PDUs, and Wire Serialization

Status: complete.

Deliverable: [`CP05_PDU_SERIALIZATION.md`](CP05_PDU_SERIALIZATION.md)

Completed:

- enforced the 256-byte service-transaction limit;
- added compatible transaction packing into bounded PDUs;
- added explicit PDU identity, validation, payload bytes, configured overhead,
  and on-wire bytes;
- changed link serialization to operate on PDU wire bytes;
- added distinct operation, transaction, PDU-payload, and wire-byte counters;
- added analytical tests for a 1000-byte single-PDU transfer and a 9000-byte
  multi-PDU transfer;
- preserved the CP-04 timing regression while improving its counters from 14
  coarse transfers to 56 bounded transactions and 14 PDUs.

Evidence:

- warning-as-error contract/runtime/test/CLI build passed;
- contract, vertical-slice, packetization, boundary, analytical, deterministic,
  and CLI golden checks passed;
- dependency isolation, forbidden-source, ASTRA integrity, and whitespace
  checks passed.

Limitations:

- packetization currently packs segments from one PUT operation only;
- PDU overhead is configured, defaults to the explicit input value, and is not
  presented as an authoritative header size;
- there is still one shared physical serialization resource.

### CP-06: Deterministic Multi-Rail Fabric

Status: complete.

Deliverable: [`CP06_MULTI_RAIL.md`](CP06_MULTI_RAIL.md)

Completed:

- added one to four independent rail serialization resources;
- used deterministic source/destination/ordering-domain flow mapping so one
  ordered flow stays on one rail;
- recorded exact PDU, payload, wire-byte, and busy-time counters per rail;
- added report validation that per-rail totals conserve global totals;
- analytically proved the current two-rail 5/9 PDU split, 200/360 ns busy
  times, 380 ns communication time, and 530 ns end-to-end time after the
  CP-14 flow-hash correction;
- preserved the one-rail 730 ns golden result.

Evidence:

- warning-as-error builds and all contract/runtime tests passed;
- deterministic replay and CLI golden comparison passed;
- dependency isolation, forbidden-source scan, ASTRA integrity, and whitespace
  checks passed.

Limitations:

- all rails currently connect endpoints directly;
- selection is a deterministic research policy, not a hardware-specific load
  balancer model;
- no switch queue, arbitration, or backpressure is modeled yet.

### CP-07: Finite Switch Queues

Status: complete.

Deliverable: [`CP07_FINITE_SWITCH_QUEUES.md`](CP07_FINITE_SWITCH_QUEUES.md)

Completed:

- added optional finite per-egress switch queues;
- scheduled switch admission at simulated ingress-arrival events across rails;
- modeled capacity, processing latency, service, departure, and destination
  arrival as distinct points;
- recorded accepted/departed PDUs, bytes, maximum/final occupancy,
  backpressure events, admission delay, and busy time per egress;
- proved deterministic equal-time arbitration and capacity-one queue drain;
- proved an integrated 710 ns result with six root-queue backpressure events
  and 400 ns total admission delay.

Evidence: every warning-as-error build, focused test, regression, CLI golden
comparison, conservation check, dependency scan, forbidden-source scan, ASTRA
integrity check, and whitespace check passed.

### CP-08: Explicit Credit Flow Control

Status: complete.

Deliverable: [`CP08_CREDIT_FLOW_CONTROL.md`](CP08_CREDIT_FLOW_CONTROL.md)

Completed:

- added optional per-egress credit ownership and timed returns;
- consumes one credit per admitted PDU and returns it only after departure plus
  configured return latency;
- checks `initial = available + outstanding` at every transition;
- records consumption, return, maximum outstanding depth, starvation events,
  and starvation time;
- proves the one-credit 45 ns starvation case and the integrated 740 ns case;
- preserves every credit-disabled CP-04 through CP-07 regression.

Evidence: warning-as-error builds, contract/runtime tests, CLI golden
comparison, isolation scan, ASTRA integrity, and whitespace checks passed.

### CP-09: Bounded Retry and Exactly-Once Reassembly

Status: complete.

Deliverable: [`CP09_RETRY_REASSEMBLY.md`](CP09_RETRY_REASSEMBLY.md)

Completed: sequence identity, deterministic error injection, bounded replay,
logical-versus-physical byte accounting, switch duplicate rejection, unique-PDU
reassembly, retry exhaustion, and exactly-once operation completion.

Evidence: all warning-as-error builds, focused/replay/regression tests, CLI
golden comparison, dependency isolation, forbidden-source scan, ASTRA integrity,
and whitespace checks passed.

### CP-09A: General Collective Planner

Status: complete.

Deliverable: [`CP09A_COLLECTIVE_PLANNER.md`](CP09A_COLLECTIVE_PLANNER.md)

Completed: reusable request/capability/planner interface, explicit algorithm
policy, stable schedule provenance, retained baseline lowering, native ring
AllReduce lowering, and explicit rejection of unsupported requests.

Evidence: strict builds, analytical operation and byte counts, deterministic
ring execution, all earlier regressions, CLI golden comparison, dependency
isolation, forbidden-source scan, ASTRA integrity, and whitespace checks
passed.

## Current Stop Point

### CP-10: Finite GPU Execution Resources

Status: complete for the scoped first GPU model.

Deliverable: [`CP10_GPU_EXECUTION_RESOURCES.md`](CP10_GPU_EXECUTION_RESOURCES.md)

Completed: kernel/workgroup/wavefront lowering, deterministic finite-CU
residency, wavefront issue timing, byte-dependent reduction time, explicit
resource stalls, integrated execution, and per-endpoint counters.

Evidence: exact unit and end-to-end contention results, strict builds, every
earlier regression, CLI golden comparison, isolation scans, subtree integrity,
and whitespace checks passed.

### CP-11: Collective Coverage

Status: complete.

Deliverable: [`CP11_COLLECTIVE_COVERAGE.md`](CP11_COLLECTIVE_COVERAGE.md)

Completed: native ring ReduceScatter and AllGather, direct AllToAll, workload
support for all four collectives, 2–1024 endpoint groups, exact byte semantics,
algorithm selection/provenance, and tiny/medium/contention/negative tests.

### CP-12: Multi-Layer and Bounded AICB Integration

Status: complete for the documented TP-only single-domain subset.

Deliverable: [`CP12_WORKLOAD_INTEGRATION.md`](CP12_WORKLOAD_INTEGRATION.md)

Completed: arbitrary named layer counts, explicit serial/overlap policies,
compute/communication overlap, ordered collective fencing, simulation-global
transaction/PDU identity, a strict AICB phase adapter, CLI execution, and
preserved layer names in reports.

CP-12 does not claim support for AICB EP/DP/PP group suffixes; those inputs are
rejected explicitly.

### CP-13: Zero-Contamination and Reachability Audit

Status: complete.

Deliverable: [`CP13_ZERO_CONTAMINATION.md`](CP13_ZERO_CONTAMINATION.md)

Completed: removal of 388 tracked legacy files/gitlinks, replacement of the
old product README, simplified ignore rules, complete native source
reachability, dependency isolation, binary symbol/linkage inspection, and a
repeatable audit script.

### CP-14: Reproducible Research Validation

Status: complete.

Deliverable: [`CP14_RESEARCH_VALIDATION.md`](CP14_RESEARCH_VALIDATION.md)

Completed: deterministic payload/endpoint/rail/queue/credit/replay sweeps,
metric definitions, analytical byte and serialization bounds, machine-readable
results, synthetic LLM/MoE communication-shape traces, and correction of a
rail-hash collision discovered by the sweep.

### CP-15: Prototype Retirement

Status: complete.

Deliverable: [`CP15_PROTOTYPE_RETIREMENT.md`](CP15_PROTOTYPE_RETIREMENT.md)

Completed: artifact-by-artifact review of the uncommitted prototype, explicit
adopt/reject decisions, preservation of useful architectural lessons, fresh
strict builds, full regressions, native and AICB CLI gates, reproducible
research sweep, dependency and zero-contamination audits, and removal of only
`C:\Users\S15\btp\simai_fresh` after all gates passed.

## Supplementary Research While Paused

The MockNCCL PUT/GET and MSCCL++ investigation is recorded in
[`MOCKNCCL_ONE_SIDED_RESEARCH.md`](MOCKNCCL_ONE_SIDED_RESEARCH.md). It is a
research/design artifact only and does not advance the implementation beyond
CP-03. The note audits the current SimCCL flow representation and executor,
defines the missing memory/order/completion/barrier semantics, compares
standard NCCL, classic MSCCL, and MSCCL++, incorporates ASTRA-sim 3.0's
fine-grained modeling lessons, and proposes independently gated future
implementation checkpoints.

For a fundamentals-first explanation of the same material, begin with
[`MOCKNCCL_ONE_SIDED_BEGINNER_GUIDE.md`](MOCKNCCL_ONE_SIDED_BEGINNER_GUIDE.md)
before reading the engineering reference.
