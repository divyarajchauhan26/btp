# MockNCCL One-Sided PUT/GET: Research and Implementation Design

Status: research/design only. This document does **not** resume the paused `simai_scaleup` implementation checkpoints.

Date reviewed: 2026-10-05

> New to NCCL, MSCCL++ or one-sided communication? Start with
> [`MOCKNCCL_ONE_SIDED_BEGINNER_GUIDE.md`](MOCKNCCL_ONE_SIDED_BEGINNER_GUIDE.md).
> This file is the detailed engineering reference.

## 1. Executive conclusion

Adding one-sided communication to the current MockNCCL is not an enum change and it is not correctly modeled by reversing an existing flow. The current MockNCCL translates a collective choice into a graph of anonymous byte flows. That representation lacks the concepts needed to state or verify one-sided correctness: registered memory windows, buffer offsets, the operation initiator, PUT versus GET direction, request/response traffic, signal generations, ordering scopes, memory visibility, fences, local and remote completion, and execution-resource ownership.

The recommended architecture is a hybrid:

1. Define a native, backend-neutral one-sided operation IR and execution model in SimCCL/SimAI.
2. Keep the existing `SingleFlow` path intact for legacy collective simulation during migration.
3. Add native PUT/GET collective templates first so the simulator does not depend on Microsoft software.
4. Add an optional MSCCL++ JSON importer that lowers schedules into the same IR.
5. Lower the IR into the selected network backend, eventually SUE-Sim, without exposing MSCCL++ details to the backend.

MSCCL++ is highly useful as a semantic reference, schedule language, source of optimized algorithms, and validation input. It is not required for the simulator to implement correct PUT/GET semantics. Classic MSCCL is not the right abstraction for this work; it primarily supplies custom collective schedules to an NCCL-derived runtime. MSCCL++ is the project that exposes the relevant GPU-driven memory channels, PUT/GET, signal/wait, flush, thread-block schedules, and dependency graph.

There is also an important update to the earlier “standard NCCL has no PUT/GET” assumption. Current standard NCCL has host one-sided `ncclPutSignal`, `ncclSignal`, and `ncclWaitSignal` APIs since NCCL 2.29. It does not expose a corresponding public host `ncclGet` API in the current interface. Therefore:

- MSCCL++ is no longer necessary merely to obtain a real-library one-sided PUT.
- MSCCL++ remains the stronger reference for GPU-driven PUT **and GET**, explicit channels, custom schedules, and fine-grained synchronization.
- The repository's MockNCCL v2.30 does not model NCCL's newer one-sided APIs despite its directory name.

## 2. What the current repository actually implements

### 2.1 MockNCCL is a translator, not NCCL execution

`SimAI/SimCCL/docs/design/architecture.md` describes SimCCL as a translation layer. It chooses an algorithm/protocol/channel/chunk decomposition and emits `FlowModels`. It explicitly does not implement GPU kernels or network I/O.

The relevant path is:

```text
MockNcclGroup::get_algo_proto_info()
  -> MockNcclGroup::genFlowModels()
  -> per-rank FlowModels
  -> AstraSim::NcclFlowModel
  -> front_end_sim_send/front_end_sim_recv
  -> selected network backend
```

### 2.2 The current IR is insufficient

`SimAI/SimCCL/src/mock/v2.30/MockNcclChannel.h` defines `SingleFlow` with:

```text
flow_id, src, dest, flow_size,
prev, parent_flow_id, child_flow_id,
channel_id, chunk_id, chunk_count, conn_type
```

This can describe “N bytes travel from rank A to rank B after flow X,” but cannot describe:

- whether A is pushing data or asking B for data;
- source/destination buffer identity and byte range;
- whether the destination range was registered and remains alive;
- whether an event means issued, locally complete, remotely arrived, remotely visible, or fully complete;
- which signal generation or count a wait consumes;
- release/acquire versus relaxed synchronization;
- a GET request traveling A to B followed by data traveling B to A;
- thread-block/CU ownership or local barriers;
- HBM, on-chip network, copy-engine, proxy, or link contention;
- whether a dependent operation requires local completion or remote visibility.

`FlowModels` is only `map<(channel_id, flow_id), SingleFlow>`. Its dependency edge carries no completion condition.

`MockNccl.h` also fixes `NCCL_NUM_FUNCTIONS` at five and its `ncclFunc_t` has
collective and SEND/RECV values but no RMA operation. In
`MockNcclGroup::get_algo_proto_info`, an ASTRA communication type is cast by
ordinal to `ncclFunc_t`. Inserting PUT/GET values into that enum without
separating collective selection from memory operations could silently corrupt
tuning-table indexes and operation mapping. This is another reason to create a
new typed operation layer rather than extending the enum in place.

### 2.3 The consumer assumes matched send/receive behavior

`SimAI/astra-sim-alibabacloud/astra-sim/system/collective/NcclFlowModel.cc`:

- posts receives through `front_end_sim_recv`;
- sends through `front_end_sim_send`;
- decrements `parent_flow_id` indegrees when packets arrive;
- treats `PacketSentFinshed` as the point at which a peer/channel QP is reusable;
- tracks expected receives with `free_packets`;
- considers the algorithm done when send streams and posted receives have drained.

Those assumptions are appropriate for its current flow abstraction, but not for genuine one-sided memory semantics. A PUT target must not need to post a matching receive. A GET needs a request and response even though the payload direction alone resembles a reversed PUT. A “packet sent” callback is not automatically remote memory visibility.

The current `MockNcclQps.h` also only records a busy bit and waiting flow IDs per `(channel, src, dst)`. A one-sided model needs context/order-domain state, outstanding limits, operation sequence numbers, and independent request/data/control resources.

### 2.4 Existing barriers are not the required barriers

- `MPI_Barrier` calls in physical-mode initialization/exiting synchronize simulator processes. They do not model application or GPU memory synchronization.
- `CollectiveBarrier::{Blocking, Non_Blocking}` is workload scheduling policy. It does not establish data visibility.
- `parent_flow_id` is a graph dependency, but has no memory-order meaning.

None of these can substitute for an ordered signal, acquire wait, memory fence, workgroup barrier, or collective rendezvous.

### 2.5 A correctness issue worth fixing before extension

`NcclFlowModel.cc` contains `assert(flow_exist = true)`, an assignment rather than a comparison. This can hide invalid dependency state. It is unrelated to PUT/GET semantics, but a new executor should not be built on top of untrusted dependency assertions without first adding tests and correcting this class of issue.

## 3. Terminology that must remain separate

### 3.1 Two-sided SEND/RECV

The sender issues SEND and the target posts a matching RECV. Progress and buffer matching involve both ranks. This is the model closest to the current `front_end_sim_send/recv` path.

### 3.2 One-sided PUT

The initiator reads a local source range and writes a registered range in the target's memory. The target does not issue a matching receive for the data movement. It still normally participates in setup and synchronization, for example window registration and waiting for a completion signal.

### 3.3 One-sided GET

The initiator reads a registered range from the target and stores the returned data in its own local range. Correct modeling needs at least:

```text
initiator -> target: read request/control
target: remote memory read
target -> initiator: data response
initiator: local memory placement and visibility
```

GET is therefore not “PUT with `src` and `dest` swapped.” The control direction, data direction, completion location, resource use, and synchronization needs differ.

### 3.4 Signal/wait, fence, flush, and barrier

These terms are not interchangeable:

- **Signal:** changes a peer-visible synchronization counter/token.
- **Wait:** blocks an execution agent until a specified token/count/generation is observed.
- **Release signal:** guarantees specified prior memory operations are complete/visible before the signal becomes observable.
- **Acquire wait:** guarantees corresponding data is safe to consume after the wait completes.
- **Relaxed signal/wait:** orders execution only; it must not be used to infer data visibility.
- **Fence/Waitcnt:** constrains local outstanding memory operations or establishes an ordering point.
- **Flush:** forces asynchronously issued/proxied operations to the completion point promised by the channel.
- **Workgroup barrier:** synchronizes wavefronts/threads in a block.
- **Inter-workgroup barrier:** synchronizes multiple blocks/CUs in one kernel or program phase.
- **Collective barrier:** all ranks reach a rendezvous. It may be built from signals/waits, but is not a primitive replacement for every dependency.

An implementation that exposes only a generic `BARRIER` will either over-serialize execution or allow incorrect early reads.

## 4. What current NCCL and MSCCL++ provide

### 4.1 Current standard NCCL

NCCL 2.29 introduced host one-sided RMA using `ncclPutSignal`, `ncclSignal`, and `ncclWaitSignal`. The target window is registered with `ncclCommWindowRegister`; the current API also requires the local PUT buffer to be registered. Same-peer, same-context operations are ordered. Operations to different peers or contexts can overlap and complete out of order. A completed wait guarantees the corresponding data is visible locally. NCCL 2.31 added configurable RMA contexts and signal indexes. See the official [one-sided usage guide](https://github.com/NVIDIA/nccl/blob/master/docs/userguide/source/usage/p2p.rst), [API semantics](https://github.com/NVIDIA/nccl/blob/master/docs/userguide/source/api/p2p.rst), and [public header](https://github.com/NVIDIA/nccl/blob/master/src/nccl.h.in).

The API exposes several different completion observations: returning to the
CPU from `ncclPutSignal` means the work was enqueued; completion on the CUDA
stream makes the local source safe to reuse; the remote signal update means
the corresponding data was delivered; and completion of `ncclWaitSignal` on
the target stream makes the data visible locally. Grouped operations may
overlap across peers/contexts, while the documented group completion covers
all operations in that group. These distinctions should appear in the
simulator rather than being collapsed into one callback.

The current public host interface contains PUT+signal but no symmetric host GET API. NVIDIA describes the implementation as zero-SM: copy engines serve scale-up movement while a CPU proxy coordinates network movement in its [NCCL 2.29 announcement](https://github.com/NVIDIA/nccl/discussions/2007). That makes current NCCL a valid comparison point, but not a full PUT/GET schedule language.

### 4.2 Classic MSCCL

Classic MSCCL is an NCCL-based custom-collective platform. MSCCLang or synthesis tools generate an XML schedule that the MSCCL runtime executes, with fallback to generic NCCL algorithms when no plan applies. It is valuable for topology- and message-size-specific scheduling, but it is not the best semantic template for this PUT/GET simulator extension. See the [MSCCL runtime](https://github.com/microsoft/msccl) and [MSCCL tools](https://github.com/microsoft/msccl-tools).

### 4.3 MSCCL++

MSCCL++ is a separate, GPU-driven communication stack. Its relevant abstractions are:

- registered memory and exchanged memory metadata;
- `MemoryChannel` for GPU-driven direct peer-memory PUT and GET;
- `PortChannel` for proxied/offloaded PUT, signal, and flush;
- ordered and relaxed signal/wait operations;
- thread-block assignment;
- local/device semaphores and cross-block synchronization;
- chunk-level dependency DAGs;
- operation fusion, redundant-synchronization removal, and pipelining;
- execution-plan JSON that can be imported independently of the simulator backend.

MSCCL++ documentation explicitly distinguishes ordered signal/wait from relaxed synchronization. Relaxed operations coordinate execution but do not prove memory completion. Its memory-channel tutorial also notes that a GET may avoid a post-copy remote notification because the result completes at the initiator, while the initiator must still ensure that the remote source is ready and stable. See [MemoryChannel PUT/GET](https://microsoft.github.io/mscclpp/tutorials/03-memory-channel.html), [C++ channel semantics](https://microsoft.github.io/mscclpp/cpp_api.html), and [DSL dependency processing](https://microsoft.github.io/mscclpp/dsl/concepts.html).

The MSCCL++ paper reports maximum speedups of up to 3.8x for collective
communication and up to 15% for evaluated end-to-end inference workloads
against its stated baselines. Those are evidence that the abstraction can be
valuable, not a prediction for this simulator, topology, or every collective.

## 5. Lessons from ASTRA-sim 3.0

The supplied paper is *ASTRA-sim 3.0: Next-Level Distributed Machine Learning Simulations via High-Fidelity GPU and Infrastructure Modeling*, arXiv:2606.10440. Its design directly addresses this question.

ASTRA-sim 3.0 argues that a large flow hides the control and data path of a remote write. It models operations at cache-line-sized load/store granularity and adds:

- `Load`, `Store`, `SemaphoreAcquire`, `SemaphoreRelease`, `Reduce`, and `Waitcnt` instructions;
- `LoadOp`, `StoreOp`, `MemcpyOp`, semaphore operations, `NopOp`, and `BarrierOp`;
- kernel, workgroup, wavefront, and CU scheduling;
- local HBM, on-chip network, I/O, and inter-GPU transfers;
- outstanding-request limits and loop unrolling;
- a parser that converts MSCCL++ PUT/GET/copy into memory-copy operations and signal/wait into semaphore release/acquire.

This matters because a one-sided simulator used for architecture research must capture the latency and contention that distinguish two algorithms. Modeling PUT and GET as equal-sized payload flows makes many research questions impossible to answer.

The paper's case studies are especially important:

- In its generic 32-GPU Reduce-Scatter experiment, GET performed better for large collectives because data became locally consumable at the initiator without a post-PUT receiver notification, enabling finer compute/communication overlap.
- In its 16-GPU All-Gather experiment, PUT performed better because GET control requests were delayed behind data responses. Fair control/data arbitration reduced the gap.
- The authors explicitly present these as demonstrations on a generic modeled system, not universal product measurements.

Therefore the simulator must expose PUT/GET choice, control/data arbitration, synchronization cost, outstanding depth, and local-resource contention as experimental variables. “One-sided is better” and “GET is better” are both invalid general conclusions. See the [ASTRA-sim 3.0 paper](https://arxiv.org/abs/2606.10440).

## 6. Required semantic contract

### 6.1 Registered memory windows

Add a simulator-level registry independent of actual virtual addresses:

```cpp
struct MemoryWindow {
  WindowId id;
  int owner_rank;
  BufferId buffer;
  uint64_t bytes;
  WindowFlags flags;
  uint64_t registration_epoch;
};

struct BufferSlice {
  int rank;
  WindowId window;
  BufferId buffer;
  uint64_t offset;
  uint64_t bytes;
};
```

The compiler/importer must reject out-of-range slices, stale/deregistered windows, integer overflow, invalid owners, and unintended aliasing. Window lifetime must extend until all outstanding operations that reference it are complete.

### 6.2 An explicit operation IR

Do not overload `SingleFlow` with a few optional fields. Create a separate representation and retain a compatibility adapter for existing flows:

```cpp
enum class OpKind {
  Put, Get, Signal, Wait, Fence, Flush,
  WorkgroupBarrier, GridBarrier,
  LocalCopy, Reduce, Nop
};

enum class Ordering {
  Relaxed, Acquire, Release, AcqRel, Sequential
};

enum class CompletionPoint {
  Issued,
  SourceReadComplete,
  Injected,
  RemoteArrived,
  DestinationVisible,
  SignalVisible,
  LocalComplete,
  FinalComplete
};

struct Dependency {
  OpId predecessor;
  CompletionPoint required_point;
};

struct SyncToken {
  int peer;
  int context;
  int signal_index;
  uint64_t generation;
  uint64_t count;
};

struct OneSidedOp {
  OpId id;
  OpKind kind;
  int initiator_rank;
  int target_rank;
  BufferSlice source;
  BufferSlice destination;
  SyncToken token;
  Ordering ordering;
  int channel;
  int threadblock;
  int wavefront;
  std::vector<Dependency> dependencies;
};
```

The exact C++ layout may differ, but these meanings must exist. In particular, `initiator_rank` cannot be inferred from payload direction for GET.

### 6.3 Typed dependency edges

The current `parent_flow_id` means only “wait for this flow.” One-sided execution needs “wait for which event of this operation?” Examples:

- a later PUT may require only prior source-read completion before reusing the local source;
- a release signal must wait for destination visibility;
- a receiver reduce must wait for the acquire wait to complete;
- a new epoch must wait for the old operation's final completion before reusing a signal slot.

Typed edges are necessary to model overlap without sacrificing correctness.

### 6.4 Signal counters and generations

Use monotonic counters or `(epoch, count)` tokens, not booleans. A boolean signal suffers stale wakeups/ABA when an algorithm pipelines chunks or repeats iterations. State should be keyed by at least:

```text
(communicator, receiver_rank, sender_rank, context, signal_index, epoch)
```

Wait must specify whether it waits for equality, at-least count, or a particular generation. Multiple waiters and fan-in waits need defined consumption semantics.

### 6.5 Ordering domains

The executor needs a defined ordering key. For compatibility with current NCCL a reasonable default is:

```text
(communicator, initiator, target, context)
```

Operations within that domain can preserve program order; independent peers/contexts may overlap. MSCCL++ schedules may supply stricter thread-block dependencies or explicitly relaxed synchronization. The model should not accidentally serialize all ranks through one global queue.

### 6.6 Separate completion milestones

At minimum track:

1. **Enqueued/issued** — accepted by the simulated stream or workgroup.
2. **Source safe/local completion** — local source can be reused where applicable.
3. **Network arrival** — bytes/control reached the remote interface.
4. **Destination visible** — data is placed in the destination memory domain and can be consumed after required acquire semantics.
5. **Signal visible** — ordered completion token is observable.
6. **Final completion** — all protocol responses/acknowledgments and resources required by the API contract are retired.

For the eventual SUE backend, a transport ACK or successful PDU arrival must not automatically imply HBM visibility. Keep transport reliability, destination placement, and application synchronization as distinct events.

## 7. Required execution behavior

### 7.1 PUT state machine

```text
validate windows/ranges
  -> satisfy source readiness and dependency edges
  -> acquire execution/channel/outstanding resources
  -> local source read
  -> issue/inject PUT data
  -> traverse network
  -> remote placement
  -> destination-visible event
  -> optional ordered signal release
  -> optional protocol completion/ack
  -> local/final completion and resource retirement
```

A fused `PUT_WITH_SIGNAL` is legal only if the signal cannot become visible before the corresponding destination range is visible. A separate relaxed signal must not imply this ordering.

### 7.2 GET state machine

```text
validate windows/ranges
  -> establish remote source readiness/stability
  -> acquire execution/channel/outstanding resources
  -> issue GET request
  -> request traverses initiator -> target
  -> remote source read
  -> data response traverses target -> initiator
  -> local destination placement
  -> local destination-visible event
  -> waitcnt/fence if the consumer requires it
  -> local/final completion and resource retirement
```

GET often avoids the *post-transfer receiver notification* needed by PUT, because the initiator observes its own completion. It does not universally eliminate synchronization: the initiator still needs proof that the remote producer has finished writing the source, and the remote source cannot be modified during an unsafe read.

### 7.3 Barrier constructions

Implement barriers from explicit primitives rather than a magic fixed delay:

- **rank barrier:** every rank signals every required peer (or a tree/central counter), then waits for its expected generation/count;
- **workgroup barrier:** all wavefronts in a block reach the phase before any proceeds;
- **grid/inter-workgroup barrier:** all participating blocks reach the phase, consuming modeled synchronization resources;
- **data-ready barrier:** release signal after producer visibility, acquire wait before consumer access;
- **execution-only rendezvous:** relaxed signal/wait when no data visibility is inferred.

Barrier topology matters. An all-to-all signal mesh, tree barrier, switch-assisted barrier, and centralized counter generate different traffic and contention.

### 7.4 Memory hazards

The schedule validator/executor should detect or order:

- read-after-write (RAW);
- write-after-read (WAR);
- write-after-write (WAW);
- overlapping PUTs from several ranks;
- GET racing a remote producer;
- buffer/window reuse before local or final completion;
- signal-slot reuse before the previous generation retires.

The model can allow intentional races only when a plan explicitly declares the behavior and the backend supports it.

### 7.5 Resources and contention

For useful scale-up research, model at least configurable tokens/queues for:

- communication workgroups/CUs or copy engines;
- HBM read and write bandwidth;
- local NoC/I/O injection and ejection;
- per-peer/context outstanding operations;
- request, data, response/ack, and signal traffic classes;
- link/switch arbitration and virtual channels;
- proxy/doorbell/command submission where the transport requires them;
- reduction execution.

Without these, PUT and GET may collapse to identical latency formulas and the main research value disappears.

## 8. Concrete repository changes

The names below are a design proposal, not changes already implemented.

### 8.1 SimCCL front end and IR

Add a version-independent directory such as:

```text
SimAI/SimCCL/src/ir/
  OneSidedProgram.hh/.cc
  MemoryWindow.hh/.cc
  ProgramValidator.hh/.cc
```

Then modify/add:

- `MockNccl.h`: only public operation/API descriptors and feature/version metadata; do not put execution state here.
- `MockNcclGroup.h/.cc`: generate native one-sided collective programs and choose PUT/GET algorithms.
- `MockNcclChannel.h/.cc`: keep `SingleFlow` stable; provide an adapter only if legacy flows need to be wrapped in the new program.
- `MockNcclQps.h`: replace/augment the busy bit with ordering contexts, sequence numbers, outstanding limits, and separate traffic classes.
- new `MscclppPlanImporter.hh/.cc`: parse and validate a supported MSCCL++ JSON subset into `OneSidedProgram`.

The importer must be a front end. No network backend should parse MSCCL++ JSON directly.

### 8.2 ASTRA/SimAI executor

Prefer a new executor over deeply branching `NcclFlowModel`:

```text
astra-sim/system/collective/
  OneSidedProgramModel.hh/.cc
  RmaMemoryModel.hh/.cc
  RmaSynchronization.hh/.cc
```

Responsibilities:

- dependency and typed-completion tracking;
- PUT/GET state machines;
- window/range validation;
- signal/wait generations;
- barriers and fences;
- operation/resource scheduling;
- deadlock detection and diagnostics;
- completion and metrics.

`NcclFlowModel` can remain the legacy executor until equivalence tests pass.

### 8.3 Network API and backend

Extend `AstraNetworkAPI.hh`/the `Sys` front end with typed RMA requests rather than pretending they are matched sends:

```text
RmaPutData
RmaGetRequest
RmaGetResponse
RmaSignal
RmaAck/Completion (when required)
```

Each request needs operation ID, initiator, target, context/order key, traffic class, byte count, and callback milestone. Backends may lower these differently, but must return the same semantic completion events.

For SUE-Sim, later lowering should map high-level operations onto SUE command/response, PUT/GET service traffic, transaction/PDU packetization, ordering, acknowledgments, retry, and flow control. Those transport details belong below the semantic IR.

### 8.4 Dumps and observability

Replace/extend the flow CSV with an operation/event trace containing:

```text
op_id, kind, initiator, target,
src_window, src_offset, dst_window, dst_offset, bytes,
context, signal_index, generation, ordering,
threadblock, channel, dependency_event,
issue_time, source_safe_time, arrival_time,
destination_visible_time, signal_visible_time, final_complete_time
```

Also report control bytes separately from payload bytes. Otherwise GET request cost and barrier/signaling overhead disappear from results.

## 9. Fidelity levels and what each can answer

### Level 0: payload-flow approximation

Represent PUT as A-to-B data and GET as B-to-A data, with a fixed sync delay.

Useful for: rough bandwidth accounting.

Not valid for: ordering, barriers, memory visibility, request contention, GPU resource effects, PUT-vs-GET research conclusions.

### Level 1: operation-level one-sided model — recommended first implementation

Model windows/ranges, PUT/GET request/data/control traffic, typed completion, signals/waits/fences, outstanding limits, HBM bandwidth, and configurable resource queues. Chunk transfers at a tunable transaction size rather than every GPU cache line.

Useful for: collective algorithm comparison, synchronization overhead, control/data arbitration, scale-up fabric studies, and integration with SUE-Sim at manageable runtime.

### Level 2: ASTRA-sim 3.0-style GPU model

Add workgroups, wavefronts, CUs, cache-line-sized load/store requests, waitcnt, loop unrolling, HBM/NoC endpoints, and detailed local execution.

Useful for: GPU/network co-design, CU occupancy, register/outstanding-depth studies, fine-grained compute/communication overlap.

Cost: substantially more events, configuration/calibration burden, and simulation time.

The sensible path is to design the IR so Level 1 can later lower into Level 2 without changing schedule semantics.

## 10. Should the project use MSCCL++?

### 10.1 Benefits

- It already represents arbitrary custom collective schedules at rank and thread-block granularity.
- PUT, GET, signal, wait, copy, reduction, and synchronization are first-class rather than inferred from byte flows.
- Its DSL performs chunk-level dependency analysis, fusion, redundant synchronization removal, and pipelining.
- ASTRA-sim 3.0 demonstrates that importing this representation into a simulator is practical.
- It provides realistic schedules and an external correctness/performance comparison point.
- It avoids inventing a user-facing schedule language before the internal semantics are understood.

### 10.2 Costs and risks

- MSCCL++ plan/schema evolution can couple the simulator to a moving external format.
- Not every real transport has identical MemoryChannel/PortChannel behavior.
- A plan optimized for one topology, GPU, message range, or protocol may perform poorly elsewhere.
- Importing syntax without implementing visibility and resource semantics creates false confidence.
- Building/running MSCCL++ for hardware validation adds toolchain and GPU requirements.
- An execution plan can contain more detail than a scalable simulation wants to model.

### 10.3 Recommendation

Use MSCCL++ as an **optional schedule front end and semantic oracle**, not as the simulator's core or its only way to generate algorithms. The internal IR should be owned by SimCCL/SimAI and remain backend-neutral. Native generators ensure reproducibility and permit experiments that do not correspond exactly to an MSCCL++ runtime feature. The importer enables reuse of real schedules and comparison with ASTRA-sim 3.0.

Classic MSCCL should only be supported later if there is a concrete need to import its XML algorithms. It should not drive the initial PUT/GET design.

## 11. Is one-sided better than the current model?

### Better when

- the target should not post a matching receive;
- producer and consumer scheduling need to be decoupled;
- direct registered-memory access reduces target-side execution overhead;
- GET permits the consumer to pull ready data and immediately reduce/process it;
- PUT allows producers to stream data into known remote locations;
- topology-specific schedules, chunking, and pipelining expose more overlap;
- zero-SM/copy-engine or GPU-driven paths free compute resources;
- the research goal is memory/fabric/GPU co-design rather than only aggregate link traffic.

### Worse or more difficult when

- registration/setup dominates small or infrequent communication;
- signals, readiness handshakes, and barriers add more control traffic than matched P2P;
- weak ordering produces subtle races;
- plans require topology- and size-specific tuning;
- GET requests contend with data responses or head-of-line block;
- PUT requires a receiver notification before consumption;
- extra execution states make simulation and validation much more expensive;
- a simple textbook collective already saturates the relevant link with less software complexity.

### Final comparison

The current flow model remains useful as a fast baseline. A correct one-sided model is better for the intended scale-up investigation because it exposes the mechanisms being studied. It is not guaranteed to predict lower latency, and MSCCL++ is not guaranteed to beat NCCL for every collective, size, topology, or hardware generation. Both paths should remain available and be compared under the same memory, link, arbitration, and completion assumptions.

## 12. Implementation checkpoints with exit criteria

These are proposed future checkpoints. None is started by this research note.

### R0 — Freeze semantics

- Write the operation/completion/ordering/window specification.
- Add invalid-program tests.
- Decide the exact promise of local and final completion.

Exit: two reviewers can independently derive the same legal event order for PUT, GET, signal/wait, and barrier examples.

### R1 — IR, validation, and trace-only executor

- Implement windows, buffer slices, typed dependencies, signal generations, and event trace output.
- Execute without a timed network backend.

Exit: deterministic golden traces and hazard rejection pass.

### R2 — Correct PUT

- Implement data placement, local/destination completion, ordered and relaxed signals, and target wait.
- No matching receive is posted.

Exit: two-rank PUT, fan-in PUT, pipelined epochs, and stale-signal negative tests pass.

### R3 — Correct GET

- Implement request, remote read, response, local placement, and local completion.
- Model readiness and control/data arbitration.

Exit: GET cannot consume an unready remote range; request and response bytes/resources are visible in metrics.

### R4 — Barriers and resource model

- Add workgroup/grid/rank barriers, HBM queues, outstanding-depth limits, and traffic classes.
- Add deadlock detection with a wait-for graph and useful diagnostics.

Exit: intentional deadlocks are detected; valid barriers release exactly once per generation.

### R5 — Native collectives

- Implement PUT- and GET-based All-Gather and Reduce-Scatter first.
- Add All-to-All and All-Reduce compositions after those are validated.

Exit: final buffer ownership/content maps are correct for multiple ranks/chunks and in-place/out-of-place cases.

### R6 — MSCCL++ importer

- Pin a documented supported schema/version.
- Translate a small supported subset, reject unknown semantics, and preserve thread-block dependencies.

Exit: imported and hand-authored equivalents produce the same normalized IR and event trace.

### R7 — SUE-Sim lowering and calibration

- Map operations to SUE command/response and packet behavior.
- Keep transport ACK, destination placement, and signal visibility distinct.

Exit: conservation/order invariants pass and selected microbenchmarks are calibrated against an external reference where hardware is available.

### R8 — Research experiments

- Sweep PUT/GET, chunk size, contexts, outstanding depth, barrier topology, arbitration, HBM bandwidth, and link parameters.
- Report uncertainty and sensitivity, not just a single speedup number.

Exit: conclusions identify the mechanism responsible for each observed crossover.

## 13. Validation matrix

### Semantic tests

- PUT without matching receive completes.
- A release signal never precedes destination visibility.
- A relaxed signal does not release a data-dependent consumer without an additional fence/order edge.
- GET generates request and response traffic in opposite directions.
- GET completion occurs only after local placement.
- Window bounds and lifetime violations fail deterministically.
- Two epochs cannot consume a stale signal.
- Same-context ordering and cross-context overlap behave as configured.
- RAW/WAR/WAW hazards are either ordered or rejected.

### Collective tests

- two- and N-rank All-Gather by PUT;
- two- and N-rank All-Gather by GET;
- Reduce-Scatter with correct reduction ownership;
- All-to-All with readiness barrier;
- All-Reduce composed from Reduce-Scatter + All-Gather;
- in-place and out-of-place buffers;
- non-divisible sizes and tail chunks;
- several thread blocks/channels and pipelined iterations.

### Invariants

- payload bytes written/read equal the algorithm specification;
- no destination is consumed before visibility;
- every acquired resource is released once;
- every non-relaxed wait has a reachable producer;
- every operation reaches exactly one terminal state;
- completion time is monotonic across its milestones;
- control traffic is never counted as payload bandwidth;
- no hidden global serialization occurs across independent contexts.

### External comparisons

- NCCL `ncclPutSignal`/`ncclWaitSignal` and `nccl-tests -H` for host PUT semantics and supported collectives;
- MSCCL++ memory-channel PUT/GET examples for operation ordering;
- ASTRA-sim 3.0 traces/results for normalized MSCCL++ schedules where configurations can be matched;
- SUE-Sim command/response traces once that backend is integrated.

## 14. Research questions worth presenting

1. At what message size and link/HBM ratio does PUT overtake GET for each collective?
2. How much of a crossover is caused by control/data arbitration rather than raw bandwidth?
3. What is the minimum synchronization needed for correctness, and what is the cost of conservative barriers?
4. How do outstanding GET requests, contexts, and signal slots affect throughput and fairness?
5. When does HBM or local NoC, rather than the scale-up link, become the bottleneck?
6. Does a zero-SM/copy-engine path improve end-to-end workload time even when communication latency is similar?
7. How much benefit comes from MSCCL++ schedule structure versus the one-sided primitive itself?
8. How portable is an optimized schedule across topology, message size, GPU resource count, and transport?
9. What simulation fidelity is required before the predicted PUT/GET choice stops changing?
10. How do barrier topology, signal coalescing, and relaxed synchronization change latency and correctness risk?

## 15. Anti-patterns to reject during review

- Adding only `ncclFuncPut`/`ncclFuncGet` enum values.
- Treating GET as a reversed payload flow.
- Requiring a target `front_end_sim_recv` for one-sided data placement.
- Treating packet send completion or transport ACK as remote HBM visibility.
- Using one boolean as a reusable semaphore.
- Treating `MPI_Barrier` as an algorithmic barrier.
- Making every signal an implicit global fence.
- Parsing MSCCL++ inside SUE-Sim or another network backend.
- Claiming MSCCL++ or one-sided is always faster based on one collective/configuration.
- Importing MSCCL++ plans without schema/version checks or unsupported-op rejection.
- Hiding control, signal, request, response, and retry bytes inside payload statistics.

## 16. Primary references

- William Won et al., [ASTRA-sim 3.0](https://arxiv.org/abs/2606.10440), 2026.
- Aashaka Shah et al., [MSCCL++: Rethinking GPU Communication Abstractions for Cutting-edge AI Applications](https://arxiv.org/abs/2504.09014), NSDI 2025.
- Microsoft, [MSCCL++ documentation](https://microsoft.github.io/mscclpp/) and [source repository](https://github.com/microsoft/mscclpp).
- NVIDIA, [NCCL one-sided communication guide](https://github.com/NVIDIA/nccl/blob/master/docs/userguide/source/usage/p2p.rst) and [one-sided API semantics](https://github.com/NVIDIA/nccl/blob/master/docs/userguide/source/api/p2p.rst).
- NVIDIA, [nccl-tests](https://github.com/NVIDIA/nccl-tests), including host RMA test modes.
- Microsoft, [classic MSCCL runtime](https://github.com/microsoft/msccl) and [MSCCL tools](https://github.com/microsoft/msccl-tools).
