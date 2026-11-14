# CP-02 File-by-File Upstream Audit

## Audit Result

The upstream ASTRA/SimAI runtime must **not** be used as the foundation of the
new runtime. Its network boundary is a two-sided `sim_send`/`sim_recv` API, and
that contract propagates into `Sys`, streams, collective algorithms, memory
timing, workload execution, statistics, build scripts, and all three network
frontends.

The clean implementation will remain in the fresh official `simai_scaleup`
repository for provenance, but it will use a new source/build tree and native
SUE contracts. Upstream files classified `REFERENCE` may inform tests or
algorithm structure; they will not be compiled or copied wholesale. There are
**zero upstream production source files approved for KEEP unchanged**.

No production source was deleted or ported during this checkpoint.

## Scope and Method

Audited baseline: official SimAI commit
`a57d765c5478f319c5f5ab6fdc53f9cc7f461743`.

The audit covered all 138 files under
`astra-sim-alibabacloud/astra-sim/{network_frontend,system,workload}`, the 14
ASTRA build/input files outside that source subtree, and the repository-level
integration points for SimCCL, AICB, NS-3, CLEM, examples, documentation, and
the wrapper build script. Evidence included:

- complete path inventory and line counts;
- all project and external `#include` edges;
- class, enum, method, and constructor declarations;
- forbidden dependency searches;
- CMake source globs, compile definitions, and link libraries;
- workload-to-collective-to-network call tracing;
- submodule gitlink state without initializing the submodules.

## Classification Legend

| Disposition | Meaning |
|---|---|
| KEEP | Compile unchanged in the new runtime. No source file qualified. |
| REWRITE | The responsibility is required, but the interface or implementation must be new. |
| REFERENCE | Useful algorithm/workload knowledge only; excluded from the new build. |
| REMOVE | Responsibility or dependency is forbidden or unnecessary. |

## Decisive Dependency Findings

1. `AstraNetworkAPI.hh` exposes only `sim_send` and `sim_recv` for data
   movement and embeds backend selection (`Garnet`, `NS3`, `Analytical`) plus
   NCCL-flow metadata. PUT/GET cannot be made native underneath that contract.
2. `Sys.hh` imports SimCCL types directly and conditionally imports an NS-3
   header. `Sys.cc` constructs topology-specific collective algorithms,
   performs rendezvous send/receive, and generates MockNCCL flow models.
3. `Layer.hh` stores `MockNccl::GroupType`; `Layer.cc` imports SimCCL,
   NS-3/MPI, ratio-table timing, and calls legacy collective generators.
   `Workload.cc` also imports SimCCL logging and NS-3/MPI behavior.
4. `Common.hh` imports `AstraNetworkAPI.hh`, so nominally generic event,
   topology, scheduling, and statistics classes inherit network contamination.
5. The top-level ASTRA CMake file globs every system/collective/topology source
   plus a selected SimCCL mock version. It selects physical or analytical
   behavior with compile definitions rather than building one SUE runtime.
6. `SimpleMemory` asks the network backend for time and has NIC-specific
   methods. It also contains a malformed duplicated scope qualifier in
   `SimpleMemory::SimpleMemory::npu_mem_write`, so it is not a trustworthy
   unchanged baseline.
7. Every existing frontend is the wrong backend: analytical, NS-3, or a
   physical MPI/verbs path. The fast backend predicts two-sided latency and is
   also unsuitable.

## Current Contamination Graph

```text
Workload / Layer
  | imports MockNCCL types and timing shortcuts
  v
Sys + DataSet + BaseStream / StreamBaseline
  | constructs legacy collectives and rendezvous helpers
  v
Algorithm / Ring / HalvingDoubling / tree / NcclFlowModel
  | emits PacketBundle + send/receive callbacks
  v
AstraNetworkAPI::sim_send / sim_recv
  |                |                 |
  v                v                 v
Analytical       NS-3/RDMA       MPI/verbs physical
```

This is a semantic dependency chain, not merely an include problem. Removing
headers while preserving the call structure would leave a disguised
two-sided simulator.

## Exhaustive Source Classification

### Network frontends: 18 files

| File | Disposition | Reason |
|---|---|---|
| `network_frontend/analytical/AnalyticalAstra.cc` | REMOVE | Analytical executable; instantiates legacy `Sys` and SimCCL logging. |
| `network_frontend/analytical/AnalyticalNetwork.cc` | REMOVE | Two-sided analytical backend. |
| `network_frontend/analytical/AnalyticalNetwork.h` | REMOVE | Implements the forbidden legacy network API. |
| `network_frontend/analytical/AnaSim.cc` | REMOVE | Analytical callback/event shim. |
| `network_frontend/analytical/AnaSim.h` | REMOVE | Analytical event queue types. |
| `network_frontend/analytical/CMakeLists.txt` | REMOVE | Builds the analytical executable. |
| `network_frontend/ns3/AstraSimNetwork.cc` | REMOVE | NS-3/MPI executable and backend glue. |
| `network_frontend/ns3/AstraSimNetwork.h` | REMOVE | Obsolete NS-3 frontend declaration. |
| `network_frontend/ns3/common.h` | REMOVE | Imports NS-3 Internet, RDMA, QBB, and switch models. |
| `network_frontend/ns3/entry.h` | REMOVE | NS-3 flow/QP setup and SimCCL glue. |
| `network_frontend/phynet/CMakeLists.txt` | REMOVE | Links MPI and verbs physical mode. |
| `network_frontend/phynet/PhySimAi.cc` | REMOVE | Physical-mode task/callback loop. |
| `network_frontend/phynet/PhySimAi.h` | REMOVE | Physical-mode queue types. |
| `network_frontend/phynet/SimAiEntry.cc` | REMOVE | Physical RDMA/SimCCL entry glue. |
| `network_frontend/phynet/SimAiEntry.h` | REMOVE | QP and physical transport state. |
| `network_frontend/phynet/SimAiMain.cc` | REMOVE | MPI/bootstrap/verbs executable. |
| `network_frontend/phynet/SimAiPhyNetwork.cc` | REMOVE | Physical two-sided backend. |
| `network_frontend/phynet/SimAiPhyNetwork.h` | REMOVE | Implements legacy network API. |

### System root: 73 files

| File | Disposition | Reason |
|---|---|---|
| `system/AstraComputeAPI.hh` | REFERENCE | Compute-delay callback idea is useful; M/K/N-only callback contract is too narrow. |
| `system/AstraMemoryAPI.hh` | REWRITE | Memory timing is needed, but NIC-specific methods and no address ownership are wrong. |
| `system/AstraNetworkAPI.hh` | REMOVE | Foundational two-sided API, backend enum, and NCCL-flow metadata. |
| `system/AstraParamParse.cc` | REWRITE | Replace global/legacy parameter parser with typed SUE-only configuration. |
| `system/AstraParamParse.hh` | REWRITE | Modes include old backends; parsing and validation are entangled. |
| `system/AstraSimDataAPI.hh` | REWRITE | Preserve per-layer reporting intent with explicit typed metrics. |
| `system/BaseStream.cc` | REFERENCE | Stream lifecycle idea only; implementation depends on legacy phases and `Sys`. |
| `system/BaseStream.hh` | REFERENCE | Heavy coupling to topology, streams, statistics, and legacy system. |
| `system/BasicEventHandlerData.cc` | REMOVE | Untyped callback payload for legacy event dispatch. |
| `system/BasicEventHandlerData.hh` | REMOVE | Depends on monolithic legacy `EventType`. |
| `system/BootStrapnet.cc` | REMOVE | MPI/IP bootstrap is forbidden. |
| `system/BootStrapnet.hh` | REMOVE | MPI bootstrap declaration. |
| `system/calbusbw.cc` | REMOVE | Empirical ratio-table timing shortcut. |
| `system/calbusbw.h` | REMOVE | Ratio timing interface. |
| `system/Callable.hh` | REWRITE | Replace raw virtual callback/data pairing with typed scheduled events. |
| `system/CallData.hh` | REMOVE | Empty untyped payload base is unnecessary in native contracts. |
| `system/CollectivePhase.cc` | REFERENCE | Phase sequencing concept can inform the new dependency DAG. |
| `system/CollectivePhase.hh` | REFERENCE | Legacy phase representation lacks native micro-ops/completion semantics. |
| `system/Common.hh` | REWRITE | Required enums are mixed with old algorithms, routing, events, and network API. |
| `system/DataSet.cc` | REFERENCE | Completion aggregation concept is useful but imports SimCCL logging. |
| `system/DataSet.hh` | REFERENCE | Legacy stream/stat ownership does not fit typed completions. |
| `system/DMA_Request.cc` | REFERENCE | Resource-delay concept may inform later GPU/memory modeling. |
| `system/DMA_Request.hh` | REFERENCE | Current request is legacy-event based and not address aware. |
| `system/IntData.cc` | REMOVE | Empty implementation. |
| `system/IntData.hh` | REMOVE | Untyped event integer wrapper. |
| `system/LogGP.cc` | REMOVE | Legacy abstract communication/memory bus protocol. |
| `system/LogGP.hh` | REMOVE | State machine is unrelated to the native SUE transaction model. |
| `system/MemBus.cc` | REFERENCE | Contention concept may inform later memory-resource work. |
| `system/MemBus.hh` | REFERENCE | Current NPU/MA transfer API is coupled to legacy system events. |
| `system/MemMovRequest.cc` | REFERENCE | Memory movement lifecycle concept only. |
| `system/MemMovRequest.hh` | REFERENCE | Legacy callable/stat implementation is unsuitable. |
| `system/MyPacket.cc` | REMOVE | Legacy virtual-network packet wrapper. |
| `system/MyPacket.hh` | REMOVE | Source/destination packet abstraction is not a SUE command/PDU. |
| `system/NetworkStat.cc` | REMOVE | Empty implementation. |
| `system/NetworkStat.hh` | REWRITE | Replace averaged dimension lists with exact SUE counters and latency stages. |
| `system/PacketBundle.cc` | REMOVE | Bundles old packets and SimCCL/physical callbacks. |
| `system/PacketBundle.hh` | REMOVE | Coupled to legacy streams, memory bus, and packet representation. |
| `system/PhyMultiThread.cc` | REMOVE | Verbs completion polling and physical multithreading. |
| `system/PhyMultiThread.hh` | REMOVE | Imports physical RDMA and SimCCL state. |
| `system/QueueLevelHandler.cc` | REFERENCE | Queue-selection concept only. |
| `system/QueueLevelHandler.hh` | REFERENCE | Assumes logical dimensions/ring direction, not SUE VCs/resources. |
| `system/QueueLevels.cc` | REFERENCE | Multi-level scheduling idea only. |
| `system/QueueLevels.hh` | REFERENCE | Bound to legacy dimension topology. |
| `system/README.md` | REMOVE | Documents legacy physical simulator dependencies. |
| `system/RecvPacketEventHadndlerData.cc` | REMOVE | Receive callback payload for two-sided packets. |
| `system/RecvPacketEventHadndlerData.hh` | REMOVE | Directly binds streams to packet receive. |
| `system/RendezvousRecvData.cc` | REMOVE | Forbidden rendezvous receive path. |
| `system/RendezvousRecvData.hh` | REMOVE | Forbidden rendezvous receive metadata. |
| `system/RendezvousSendData.cc` | REMOVE | Forbidden rendezvous send path. |
| `system/RendezvousSendData.hh` | REMOVE | Forbidden rendezvous send metadata. |
| `system/SendPacketEventHandlerData.cc` | REMOVE | Send callback payload for two-sided packets. |
| `system/SendPacketEventHandlerData.hh` | REMOVE | Directly binds streams/system to packet send. |
| `system/SharedBusStat.cc` | REMOVE | Empty implementation. |
| `system/SharedBusStat.hh` | REFERENCE | Metric categories may inform later resources; averaging is unsafe at zero counts. |
| `system/SimAiFlowModelRdma.cc` | REMOVE | Verbs/QP implementation. |
| `system/SimAiFlowModelRdma.hh` | REMOVE | Verbs, sockets, and QP state. |
| `system/SimAiPhyCommon.hh` | REMOVE | Socket/physical transport structures. |
| `system/SimRecvCaller.cc` | REMOVE | Wrapper around `sim_recv`. |
| `system/SimRecvCaller.hh` | REMOVE | Two-sided receive callable. |
| `system/SimSendCaller.cc` | REMOVE | Wrapper around `sim_send`. |
| `system/SimSendCaller.hh` | REMOVE | Two-sided send callable. |
| `system/StatData.cc` | REMOVE | Empty implementation. |
| `system/StatData.hh` | REWRITE | Replace generic scalar payload with typed report records. |
| `system/StreamBaseline.cc` | REFERENCE | Collective stream state-machine idea only; imports SimCCL logging. |
| `system/StreamBaseline.hh` | REFERENCE | Built around packet send/receive completion. |
| `system/StreamStat.cc` | REMOVE | Empty implementation. |
| `system/StreamStat.hh` | REWRITE | Replace inherited aggregate stats with explicit operation/fabric metrics. |
| `system/Sys.cc` | REWRITE | Monolith mixes parsing, scheduling, collectives, rendezvous, flow generation, and backends. |
| `system/Sys.hh` | REWRITE | Direct SimCCL/NS-3 imports and legacy send/receive public API. |
| `system/Usage.cc` | REFERENCE | Time-interval utilization record idea is reusable conceptually. |
| `system/Usage.hh` | REFERENCE | Replace with typed resource-utilization samples. |
| `system/UsageTracker.cc` | REWRITE | Utilization tracking is needed with deterministic interval accounting. |
| `system/UsageTracker.hh` | REWRITE | Currently coupled to `Sys`, `Callable`, `Common`, and CSV writer. |

### Collective algorithms: 12 files

| File | Disposition | Reason |
|---|---|---|
| `system/collective/Algorithm.cc` | REFERENCE | Lifecycle concept only; base invokes stream callbacks. |
| `system/collective/Algorithm.hh` | REFERENCE | Legacy callback/topology base, not a micro-op planner. |
| `system/collective/AllToAll.cc` | REFERENCE | Peer/chunk ordering can inform new AllToAll lowering. |
| `system/collective/AllToAll.hh` | REFERENCE | Inherits legacy Ring and packet execution. |
| `system/collective/DoubleBinaryTreeAllReduce.cc` | REFERENCE | Tree schedule may be checked against a future native planner. |
| `system/collective/DoubleBinaryTreeAllReduce.hh` | REFERENCE | Bound to PacketBundle and legacy tree topology. |
| `system/collective/HalvingDoubling.cc` | REFERENCE | Algorithm ordering may inform tests; execution is packet/send-receive based. |
| `system/collective/HalvingDoubling.hh` | REFERENCE | Bound to memory bus, packets, and ring direction. |
| `system/collective/NcclFlowModel.cc` | REMOVE | SimCCL/MPI/RDMA flow execution. |
| `system/collective/NcclFlowModel.hh` | REMOVE | MockNCCL QP and flow-model dependency. |
| `system/collective/Ring.cc` | REFERENCE | Ring step/chunk math may inform new micro-op lowering tests. |
| `system/collective/Ring.hh` | REFERENCE | Execution uses legacy packets, bus, and callbacks. |

### Fast backend, memory, and scheduling: 6 files

| File | Disposition | Reason |
|---|---|---|
| `system/fast-backend/FastBackEnd.cc` | REMOVE | Predicts latency for paired send/receive calls and wraps another backend. |
| `system/fast-backend/FastBackEnd.hh` | REMOVE | Inherits the legacy network API. |
| `system/memory/SimpleMemory.cc` | REWRITE | Useful bandwidth/serialization intent, but tied to network time and contains malformed method qualification. |
| `system/memory/SimpleMemory.hh` | REWRITE | Needs address spaces, ownership, visibility, and independent event-clock access. |
| `system/scheduling/OfflineGreedy.cc` | REFERENCE | Load-balancing heuristic may inform later planner experiments. |
| `system/scheduling/OfflineGreedy.hh` | REFERENCE | Directly depends on monolithic `Sys` and dimension scheduling. |

### Logical topology: 23 files

| File | Disposition | Reason |
|---|---|---|
| `system/topology/BasicLogicalTopology.cc` | REMOVE | Empty implementation. |
| `system/topology/BasicLogicalTopology.hh` | REFERENCE | Basic ring/tree classification only. |
| `system/topology/BinaryTree.cc` | REFERENCE | Parent/child math may inform collective planner tests. |
| `system/topology/BinaryTree.hh` | REFERENCE | Legacy logical topology and node coupling. |
| `system/topology/ComplexLogicalTopology.cc` | REMOVE | Empty implementation. |
| `system/topology/ComplexLogicalTopology.hh` | REMOVE | Multi-dimensional composition is not required for the first scale-up domain. |
| `system/topology/ComputeNode.hh` | REMOVE | Empty marker class. |
| `system/topology/DoubleBinaryTreeTopology.cc` | REFERENCE | Tree construction may inform a future scale-up collective planner. |
| `system/topology/DoubleBinaryTreeTopology.hh` | REFERENCE | Legacy composite topology representation. |
| `system/topology/GeneralComplexTopology.cc` | REMOVE | Generic multi-dimensional composition exceeds target fabric contract. |
| `system/topology/GeneralComplexTopology.hh` | REMOVE | Generic complex topology abstraction is unnecessary. |
| `system/topology/LocalRingGlobalBinaryTree.cc` | REMOVE | Explicit local/global hierarchy carries scale-out assumptions. |
| `system/topology/LocalRingGlobalBinaryTree.hh` | REMOVE | Explicit local/global hierarchy. |
| `system/topology/LocalRingNodeA2AGlobalDBT.cc` | REMOVE | Explicit node/global hierarchy and mixed algorithms. |
| `system/topology/LocalRingNodeA2AGlobalDBT.hh` | REMOVE | Explicit node/global hierarchy. |
| `system/topology/LogicalTopology.cc` | REFERENCE | Minimal abstraction concept only. |
| `system/topology/LogicalTopology.hh` | REFERENCE | Replace logical dimensions with `ParallelGroup` and explicit fabric endpoints. |
| `system/topology/Node.cc` | REFERENCE | Node identity concept only; target entity is an accelerator endpoint. |
| `system/topology/Node.hh` | REFERENCE | Legacy topology node wrapper. |
| `system/topology/RingTopology.cc` | REFERENCE | Ring neighbor/order math is relevant inside a scale-up domain. |
| `system/topology/RingTopology.hh` | REFERENCE | Remove dimension/direction assumptions from the new planner contract. |
| `system/topology/Torus3D.cc` | REMOVE | Unneeded scale-out-style topology. |
| `system/topology/Torus3D.hh` | REMOVE | Unneeded scale-out-style topology. |

### Workload: 6 files

| File | Disposition | Reason |
|---|---|---|
| `workload/CSVWriter.cc` | REWRITE | Reporting is required, but current writer imports legacy common types and mutates CSV cells in place. |
| `workload/CSVWriter.hh` | REWRITE | Replace POSIX-heavy writer with deterministic report serialization. |
| `workload/Layer.cc` | REWRITE | Mixes layer timeline, SimCCL, MPI/NS-3, ratio timing, collectives, and reporting. |
| `workload/Layer.hh` | REWRITE | Stores MockNCCL group types and directly owns legacy datasets/system. |
| `workload/Workload.cc` | REWRITE | Preserve AI execution ordering, but remove SimCCL logging, NS-3/MPI, and legacy collective calls. |
| `workload/Workload.hh` | REWRITE | Replace direct `Sys` ownership with native workload events and scheduler interface. |

The tables above contain one row for every one of the 138 files in the three
audited source directories.

## Build and Input Classification

| File | Disposition | Reason |
|---|---|---|
| `astra-sim-alibabacloud/CMakeLists.txt` | REWRITE | Globs all legacy sources and SimCCL; selects obsolete backends. |
| `astra-sim-alibabacloud/build.sh` | REWRITE | Exposes only NS-3, physical, and analytical modes and writes under `/etc`. |
| `build/astra_ns3/CMakeLists.txt` | REMOVE | NS-3 build shell. |
| `build/astra_ns3/build.sh` | REMOVE | Copies NS-3/SimCCL trees and builds forbidden backend. |
| `build/simai_analytical/CMakeLists.txt` | REMOVE | Analytical and SimCCL build. |
| `build/simai_analytical/build.sh` | REMOVE | Analytical build wrapper. |
| `build/simai_phy/CMakeLists.txt` | REMOVE | MPI/physical build and SimCCL linkage. |
| `build/simai_phy/build.sh` | REMOVE | Physical backend build wrapper. |
| `inputs/config/SimAI.conf` | REMOVE | Contains QCN/PFC and old packet/network simulation configuration. |
| `inputs/ratio/ata_ratio.csv` | REMOVE | Empirical communication ratio timing. |
| `inputs/ratio/nic_ratio.csv` | REMOVE | Multi-node/NIC empirical ratio timing. |
| `inputs/ratio/nvlink_ratio.csv` | REMOVE | Non-SUE empirical ratio timing. |
| `inputs/topo/gen_Topo_Template.py` | REMOVE | Generates data-center/cluster topologies, not the SUE target fabric. |
| `astra-sim-alibabacloud/LICENSE` | KEEP | Preserve upstream license and provenance. |

## Repository Integration Classification

> **CP-13 outcome:** Every path classified `REMOVE`, every old product
> documentation/configuration surface, all four gitlinks, and the vendored
> inference tree were removed from the clean repository. The top-level Apache
> license remains. This table is retained as the historical evidence for those
> decisions, not as a description of the current tree.

| Path | State | Disposition |
|---|---|---|
| `SimCCL` | Uninitialized gitlink at `fd7cd57d16f9bd42e3ccb70911c977e18ec294b9` | Remove as runtime/build dependency. A future optional importer must target native `CollectivePlan`, never network execution. |
| `aicb` | Uninitialized gitlink at `23eec3c48ca2d2d93dd888a4c7b22ab4421e782f` | Defer; later optional workload import after native schema is stable. |
| `ns-3-alibabacloud` | Uninitialized gitlink at `3e0c7c1bfbbe9f77890ddcf5e5b9c79fc6dd7437` | Remove. It must never enter the new build graph. |
| `SimAI-CLEM` | Uninitialized gitlink at `4a91f8b655224c0b1d1c300aa09c7058f4da2707` | Remove from product tree; unrelated scale-out extension. |
| `vidur-alibabacloud` | Vendored inference integration | Reference only until workload import checkpoint. Do not compile in the vertical slice. |
| `scripts/build.sh` | Legacy wrapper | Rewrite as one SUE-only configure/build/test entry point. |
| `example/*.txt`, `example/busbw.yaml` | Legacy examples | Replace with native two-layer golden input and typed SUE configuration. |
| `README*.md`, `docs/` | Legacy product documentation | Replace before final release; historical material may remain only in migration/research records. |
| `Dockerfile` | Installs legacy full-stack dependencies | Rewrite only after the native local build is stable. |

## Proposed Retained Build Graph

This is the allowed dependency direction for CP-03 onward. Arrows mean
“depends on”; no upward or sideways backend dependency is allowed.

```text
simai_cli
  -> workload_parser
  -> simulation

workload_parser
  -> contracts::WorkloadEvent / ParallelGroup

simulation
  -> workload_executor
  -> collective_planner
  -> sue_backend
  -> report_writer

workload_executor
  -> contracts::WorkloadEvent / Completion
  -> event_engine

collective_planner
  -> contracts::CollectivePlan / MicroOp

sue_backend
  -> contracts::SueTransaction / Completion
  -> event_engine
  -> sue_link_model                 (CP-04)
  -> flit_and_pdu                   (CP-05)
  -> rails_and_switch              (CP-06/CP-07)
  -> credit_control                (CP-08)
  -> retry_and_reassembly          (CP-09)

report_writer
  -> contracts::SimulationReport

optional_aicb_importer              (later)
  -> contracts::WorkloadEvent

optional_collective_importer        (later, if justified)
  -> contracts::CollectivePlan
```

Forbidden edges:

- contracts to parser, CLI, or backend implementation;
- collective planner directly to fabric queues or flits;
- workload executor directly to SUE packets;
- SUE backend to SimCCL, AICB, MPI, sockets, or another network simulator;
- any native PUT/GET lowering through send/receive compatibility calls.

## Minimal CP-04 Vertical Slice

Only the following behavior is required for the first working end-to-end
slice:

1. A typed, versioned input with exactly eight accelerator endpoints and two
   model layers.
2. A deterministic integer-nanosecond event engine with stable tie-breaking.
3. One compute event per layer.
4. One eight-endpoint tensor-parallel group.
5. One AllReduce request lowered into an explicit dependency DAG containing
   native memory-oriented micro-operations; no compatibility send/receive.
6. A simple SUE transaction/link model with configured payload bytes,
   bandwidth, fixed propagation delay, and completion on destination arrival.
7. Per-layer compute time, communication issue time, completion time, exposed
   communication time, transferred payload bytes, and end-to-end time.
8. A golden test and a hand-calculated serialization-time equality test.

Explicitly deferred after the vertical slice: PDU packing, detailed flits,
multiple rails, switch arbitration, finite queues, CBFC, retry, reassembly,
GPU execution resources, AICB import, and additional collectives.

## CP-02 Exit-Gate Evidence

- Every backend/system/workload file has a recorded disposition and reason.
- All existing frontend and build choices are classified for removal.
- The legacy dependency chain from workload to two-sided backend is recorded.
- A SUE-only proposed build graph has been defined.
- The first vertical slice has a bounded feature list and explicit deferrals.
- No upstream production file was modified, removed, or copied.

## Decision for CP-03

Proceed with a clean native contract skeleton. Do **not** port `Sys`,
`AstraNetworkAPI`, `Layer`, or any collective implementation first. CP-03 must
define and test value types, ownership, dependency/completion semantics, and
the event-engine boundary before implementing fabric behavior.
