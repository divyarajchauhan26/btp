# Scale-Up GPU Modeling: Theory and Implementation Plan

## Purpose

This document explains the theory behind the planned fine-grained scale-up GPU
model and then defines how `simai_scaleup` should implement it.

The project is not intended to execute a real GPU binary or reproduce a named
GPU microarchitecture instruction-for-instruction. Its target is an
event-driven model at Load/Store and synchronization granularity:

```text
workload graph
  -> kernel description
  -> workgroups
  -> wavefronts
  -> GPU operations
  -> cache-line-sized instructions and requests
  -> local GPU memory or remote scale-up fabric
  -> completions, wake-ups, and kernel retirement
```

The design is inspired by the workload and GPU-model separation described by
[ASTRA-sim 3.0](https://arxiv.org/abs/2606.10440), but it is not intended to be
a line-for-line reproduction of ASTRA-sim. Every behavior implemented here
must be supported by our own contracts, tests, configuration, and stated
assumptions.

## Part I: Theory

## 1. What We Are Modeling

A scale-up system contains multiple accelerator devices connected through a
high-bandwidth fabric inside one scale-up domain. A useful simulation must
model more than the time required to send a packet between two endpoint IDs.

For a communication operation, the complete path may include:

```text
source CU
  -> source register or local memory access
  -> source GPU internal interconnect
  -> source I/O port
  -> scale-up link and optional switch
  -> destination I/O port
  -> destination GPU internal interconnect
  -> destination memory
  -> visibility/completion notification
```

The simulator therefore needs two connected models:

1. **GPU execution model:** kernels, workgroups, wavefronts, CUs, memory
   operations, synchronization, and local memory service.
2. **Scale-up transport model:** remote requests, packetization, rails, links,
   switches, flow control, retries, delivery, and responses.

SUE-Sim belongs to the second category. It cannot by itself determine which CU
issued an operation, whether a source-side HBM load was required, or whether a
wavefront was blocked on a semaphore. Those decisions belong above its
transport boundary.

## 2. The Required Levels of Representation

The simulator needs several representations because one format cannot cleanly
express every level of detail.

### 2.1 Workload graph

The workload graph describes coarse execution structure:

- which kernel or operation runs on which device;
- whether a node is compute, memory, collective, send, or receive;
- dependencies between nodes;
- tensor or communication sizes when recorded;
- optional captured duration and attributes.

Chakra ET is a suitable source for this layer. It is a graph of operations; it
is not normally a complete stream of GPU Load, Store, Waitcnt, and semaphore
instructions.

### 2.2 Kernel description or plan

Each executable graph node needs more detail than Chakra necessarily contains.
That detail comes from a kernel-specific description:

- a communication kernel may reference an MSCCL++ plan;
- a compute kernel may reference a compute template;
- a memory node may reference an explicit source/destination descriptor;
- an unsupported node must fail clearly instead of receiving invented detail.

This binding layer answers the question:

> Which fine-grained program should execute when this Chakra node becomes
> ready?

### 2.3 Fine-grained GPU intermediate representation

The selected description is lowered into a frontend-independent hierarchy:

```text
KernelDefinition
  WorkgroupDefinition[]
    FineWavefrontProgram[]
      FineGpuOperation[]
```

Operations include:

- `Load`;
- `Store`;
- `Memcpy`;
- `SemaphoreAcquire`;
- `SemaphoreRelease`;
- `Reduce`;
- workgroup barrier;
- kernel barrier.

These are semantic GPU operations. They are not packets and are not assumed to
be literal ISA opcodes.

### 2.4 Primitive instruction stream

Operations generate primitive instructions lazily:

- Load;
- Store;
- SemaphoreAcquire;
- SemaphoreRelease;
- Reduce;
- Waitcnt.

Large memory operations are split into configurable cache-line-sized requests.
Lazy generation avoids storing millions of instruction objects before they are
needed.

### 2.5 Semantic scale-up request

Only remote memory/control instructions cross the transport boundary. The GPU
model emits a semantic request containing provenance, source and destination
devices, memory references, byte count, operation class, and required
completion stage.

The transport adapter—not the workload parser or wavefront—turns this request
into transactions and PDUs.

## 3. Where Fine-Grained Information Comes From

Fine-grained information should be derived from explicit sources rather than
guessed from node names.

| Information | Recommended source |
| --- | --- |
| Kernel dependency | Chakra ET |
| Node type and device | Chakra ET |
| Collective type and payload | Chakra ET attribute, when present |
| Communication algorithm | MSCCL++ plan registry |
| Threadblock/workgroup program | MSCCL++ plan or compute template |
| Wavefront count | Kernel descriptor/template |
| CU assignment | Dynamic GPU scheduler |
| Source/destination buffer owner | Lowered kernel description |
| Local versus remote route | Memory owner compared with issuing device |
| Cache-line requests | Fine instruction generator |
| HBM/channel/NoC timing | Hardware configuration and memory model |
| Rails, links, switches, CBFC, LLR | Scale-up transport configuration |
| Simulated timestamps | Event-driven execution |
| Captured duration | Calibration or fallback compute model |

The workload trace should not contain switch credits, rail bandwidth, HBM
channel count, or other hardware configuration. Those values describe the
machine being simulated, not the application graph.

## 4. Local and Remote Memory Semantics

A memory reference should identify data semantically:

```text
MemoryReference
  owner_device
  memory_space
  buffer_id
  byte_offset
  byte_length
  alignment
```

Local versus remote is resolved by ownership:

```text
owner_device == issuing_device  -> local access
owner_device != issuing_device  -> remote access
```

Chakra does not need to label every access as local or remote. The parser or
kernel template provides the owner and memory reference; the runtime compares
that owner with the issuing GPU.

### Initial memory spaces

The contracts permit:

- register file;
- L1;
- L2;
- local HBM;
- remote HBM;
- semaphore memory.

However, the existence of these enum values does not prove that a real trace
contains cache behavior. Unless cache hit/miss information is supplied or a
cache policy is explicitly modeled, L1/L2 must be treated as analytical tiers,
not as validated hardware cache behavior.

### Recommended first fidelity target

Use these meaningful paths first:

```text
RegisterFile <-> LocalHbm
RegisterFile <-> RemoteHbm
SemaphoreMemory on an explicit owner device
```

Add cache placement and replacement behavior only after its inputs and
validation method are defined.

## 5. Example: PUT from GPU 0 to GPU 1

A high-level `PUT` should not instantly become one network packet. Its semantic
expansion is approximately:

```text
GPU 0 wavefront
  1. Load source cache line from GPU 0 local memory
  2. Waitcnt until source data is ready
  3. Issue remote Store to a GPU 1-owned destination
  4. Traverse the scale-up transport
  5. Service destination memory/visibility
  6. Complete the Store at the requested completion stage
```

If the communication algorithm requires synchronization:

```text
  7. Release a semaphore owned by GPU 1
  8. A dependent wavefront acquires/waits for that generation
```

`SIGNAL` and `WAIT` are therefore not mandatory after every PUT. They are used
when the algorithm needs an explicit dependency that is not already guaranteed
by program order or completion semantics.

## 6. Example: GET from GPU 1 to GPU 0

A remote GET is a request-response operation:

```text
GPU 0 issues remote Load request
  -> request travels to GPU 1
  -> GPU 1 services its memory
  -> response data travels to GPU 0
  -> GPU 0 receives/stores the data
  -> requesting instruction completes
```

This differs from a PUT because the data-bearing transport is the response,
not the original command.

## 7. Synchronization Concepts

### Program order

Instructions from one wavefront remain ordered unless an operation explicitly
allows multiple requests to be in flight.

### Waitcnt

Waitcnt is local wavefront control. It blocks until selected outstanding
request counts reach a threshold. It is not itself a network packet.

### Semaphores

A semaphore needs:

- owner device;
- identifier;
- generation or value;
- memory location;
- waiters;
- release visibility semantics.

A local semaphore stays within the GPU model. A remote semaphore operation
becomes scale-up control traffic.

### Barriers

- A workgroup barrier synchronizes participating wavefronts.
- A kernel/inter-workgroup barrier synchronizes an explicit workgroup set.

Barriers, Waitcnt, semaphores, and graph dependencies are related but not
interchangeable. They operate at different levels.

## 8. CU and Wavefront Execution

The kernel descriptor defines workgroups and wavefront programs, but it should
not normally hardcode runtime CU assignments.

During simulation:

1. A graph node becomes ready after its dependencies complete.
2. Its kernel is submitted to the target GPU.
3. Workgroups wait for CU residency slots.
4. The scheduler assigns ready workgroups to CUs.
5. Each CU selects ready resident wavefronts.
6. Wavefronts issue instructions until they stall or finish.
7. Memory and synchronization completions wake blocked wavefronts.
8. All wavefronts retire, then the workgroup and kernel retire.
9. Kernel retirement satisfies dependent graph edges.

Round-robin should remain the deterministic reference policy. Seeded-random or
architecture-specific assignment can be optional experiments, but random
assignment is not automatically more realistic.

## 9. Timing Rules

The simulation should produce time from modeled events and resource
contention. Trace timestamps must not be added blindly to simulated latency.

Recommended rules:

- use graph dependencies to determine readiness;
- use simulated resources to determine execution and completion time;
- preserve captured Chakra duration for calibration and reporting;
- use captured duration as a fallback abstract compute cost only when no
  detailed compute descriptor exists;
- never both replay a captured kernel duration and simulate its complete
  fine-grained duration on the same execution path;
- keep source-trace time and simulated time as separate fields.

## 10. What Accuracy We Can and Cannot Claim

The model can support correctness claims such as:

- dependency ordering is preserved;
- requests and bytes are conserved;
- semaphores wake the correct waiters;
- finite CU and memory capacities are respected;
- local requests do not accidentally enter the scale-up network;
- remote loads generate command and response traffic;
- execution is deterministic for a fixed configuration and seed.

It cannot yet support claims such as:

- cycle-accurate reproduction of a real GPU;
- exact execution of a GPU binary;
- validated cache hit rate;
- hardware-accurate latency or utilization;
- measured agreement with a named accelerator.

Those claims require hardware parameters, measurements, and calibration that
are not currently available.

## Part II: Current Implementation

## 11. What Already Exists

The following foundations are implemented in `simai_scaleup`:

- typed kernel, workgroup, wavefront, operation, instruction, and memory
  contracts;
- semantic memory references with owner, space, buffer, offset, and length;
- lazy cache-line instruction generation;
- deterministic workgroup-to-CU scheduling;
- wavefront issue, stall, completion, and retirement state;
- outstanding Load/Store tracking and Waitcnt;
- analytical L1/L2/HBM services;
- semaphore and barrier services;
- semantic remote Store, Load, acquire, and release requests;
- native request-to-PDU transport;
- MSCCL++ plan parsing and fine-kernel lowering;
- compute-kernel templates;
- a frontend-neutral kernel graph;
- official length-delimited Chakra ET import into that neutral graph;
- fine execution reports and conservation checks.

Important code boundaries include:

- `native/include/simai/contracts/fine_gpu.hpp`: fine GPU contracts;
- `native/include/simai/runtime/fine_kernel_graph.hpp`: neutral kernel graph;
- `native/src/chakra_et_importer.cpp`: Chakra ET import;
- `native/src/mscclpp_plan.cpp`: MSCCL++ parsing and lowering;
- `native/src/fine_compute.cpp`: compute-template lowering;
- `native/src/fine_microtrace.cpp`: lane-trace normalization and Load/Store
  lowering;
- `native/src/fine_synthetic_trace.cpp`: synthetic timestamped Load/Store
  parsing, validation, and kernel lowering;
- `native/src/fine_simulator.cpp`: unified fine event runtime.

## 12. Current End-to-End Boundary

The following path is now implemented:

```text
multi-rank Chakra ET
  -> collision-safe merged graph
  -> versioned execution manifest
  -> duration-backed compute or grouped MSCCL++ lowering
  -> dependency-aware kernel release
  -> unified fine execution
  -> native deterministic scale-up transport
```

An additional native API path now exists below the graph layer:

```text
dynamic block/thread TSV records
  + explicit instruction-type legend
  + block/warp geometry
  + allocation ownership map
  -> dynamic instructions grouped by block and warp
  -> contiguous lane addresses coalesced into memory spans
  -> semantic local/remote HBM references
  -> fine compute or communication Load/Store kernel
  -> explicit binding to one Chakra executable node
```

This path intentionally does not guess what numeric instruction types mean or
which GPU owns a virtual address. It requires both facts from capture metadata.
It preserves kernel-elapsed cycle values for calibration but does not replay
them as latency. Programmatic Chakra-node binding is implemented; file-based
selection is not yet available in the Chakra execution manifest.

The agreed synthetic path is separately complete:

```text
timestamp + Load/Store + address + local/remote owner + size
  -> strict synthetic trace parser
  -> TraceWaitUntil and semantic memory operations
  -> direct execution or Chakra manifest binding
  -> local memory or remote native SUE transport
  -> deterministic report
```

Synthetic timestamps are nanosecond earliest-issue offsets from kernel
admission. They are executable constraints, not hardware calibration claims.

The importer preserves source rank/ID, start and duration presence, I/O text,
raw attributes, control dependencies, data dependencies, payloads, and peers.
The manifest identifies nodes by `(zero-based rank, source node ID)`. Several
per-rank collective nodes can bind to one MSCCL++ plan launch.

The remaining gaps are narrower but important:

1. Hardware microtrace selection and correlation are not yet available in the
   manifest. Synthetic trace files are supported; duration-backed Reduce
   remains the explicit fallback when neither synthetic nor hardware detail is
   supplied.
2. Chakra control dependencies are preserved separately but are not promoted
   automatically to execution dependencies without a defined semantic policy.
3. The local memory model is analytical and does not yet represent an explicit
   CU-to-NoC-to-HBM-channel topology.
4. Fine CLI memory-tier constants are still constructed by the application
   rather than loaded from a dedicated hardware schema.
5. Persistent asynchronous SUE-Sim execution is not complete; the native
   deterministic transport is the current executable backend.
6. Timing remains uncalibrated against hardware.

Therefore, the current system may be described as a Chakra-driven native fine
simulation prototype, but not as binary-accurate, hardware-calibrated, or a
persistent SUE-Sim/ns-3 run.

## Part III: Planned Design

## 13. Target End-to-End Flow

```text
1. Read one Chakra ET file per rank
2. Preserve source IDs, dependencies, timing, tensors, and attributes
3. Merge rank graphs using (rank, source_node_id) identity
4. Match each executable node to a kernel descriptor
5. Select an MSCCL++ plan for communication nodes
6. Select a compute/memory template for noncommunication nodes
7. Lower the selected description into FineKernelDefinition objects
8. Release only dependency-ready kernels to the GPU runtime
9. Schedule workgroups onto finite CUs
10. Execute wavefront instructions
11. Route local requests to local memory
12. Route remote requests through the scale-up backend
13. Process completions and synchronization wake-ups
14. Retire the kernel and release dependent Chakra nodes
15. Report source, simulated, GPU, memory, and transport timing separately
```

## 14. Configuration Ownership

Configuration should be separated into three logical documents even if one CLI
eventually loads them together.

### Workload configuration

Contains:

- Chakra trace path/prefix;
- rank count and rank mapping;
- optional source-time replay policy;
- graph validation policy.

### Kernel binding manifest

Contains rules such as:

```text
if node is AllReduce with four participants and payload in range X
  -> use MSCCL++ plan A

if node name/type matches compute kernel B
  -> use compute template B

if memory detail is unavailable
  -> use an explicit tagged fallback policy or fail
```

The manifest should reference descriptions rather than embedding every
instruction into Chakra. This keeps standard Chakra compatibility while
allowing our model to add detail.

### Hardware/system configuration

Contains:

- GPU count and device mapping;
- CU count and residency;
- issue width and instruction timing;
- cache-line size;
- local memory tiers/channels and capacities;
- internal NoC parameters when implemented;
- I/O ports;
- scale-up topology, rails, links, switches, bandwidth, latency, and flow
  control.

## 15. Binding and Fallback Policy

Each graph node must have one unambiguous handling rule.

### Communication node

Preferred:

```text
collective metadata
  -> plan-registry key
  -> one matching MSCCL++ plan
  -> per-device communication kernels
```

The plan registry should match at least:

- collective kind;
- participant count/group;
- protocol or topology class;
- payload range and alignment constraints;
- optional user-selected plan name.

The importer is not responsible for inventing a collective algorithm.

### Compute node

Preferred:

```text
node descriptor/name/type
  -> compute template
  -> workgroups, wavefronts, memory spans, and Reduce costs
```

Fallback, when explicitly enabled:

```text
captured duration
  -> abstract compute/Reduce operation
```

This fallback must be marked in the report because it contains no validated
memory-access pattern.

### Memory node

The descriptor must provide or resolve:

- source/destination owner;
- memory space;
- buffer and byte range;
- load/store direction.

If location is absent, the default may be `LocalHbm` only when the user enables
and the report records that assumption. Silent inference is not acceptable.

## 16. Multi-Rank Graph Model

Chakra node IDs may repeat across rank files. Internal graph identity should
therefore be composite:

```text
GraphNodeKey = (rank, source_node_id)
```

Collective instances spanning ranks also need a correlation key. Prefer an
explicit collective sequence/tag and participant group. If traces do not
provide one, matching by order, collective type, group, and payload is only a
documented fallback and must detect ambiguity.

Dependencies remain rank-local unless the imported format or binding manifest
defines a cross-rank relationship. Communication-kernel completion creates the
runtime synchronization across participating ranks.

## 17. Dependency-Aware Kernel Scheduler

The graph runtime should maintain:

- unresolved dependency count per node;
- ready-node queue;
- node state: pending, ready, running, complete, failed;
- mapping from runtime kernel instances back to graph nodes;
- completion count when one graph node lowers to multiple device kernels.

Algorithm:

```text
validate graph
initialize unresolved counts
enqueue zero-dependency executable nodes

while work remains:
  lower and submit ready nodes
  process next GPU/memory/transport event
  on kernel completion:
    update its graph node
    when all kernels for that node complete:
      mark node complete
      decrement dependent-node counts
      enqueue newly ready nodes

if no event or ready node exists while work remains:
  emit blocked/deadlock diagnostics
```

This replaces unconditional time-zero submission for Chakra-driven runs. The
existing direct-kernel test path may remain for focused unit tests.

## 18. Scale-Up Hardware Evolution

### First stage: semantic correctness

Use the existing analytical memory and native transport to prove:

- correct ownership classification;
- request/response behavior;
- synchronization;
- dependency ordering;
- byte and request conservation;
- deterministic execution.

### Second stage: explicit internal GPU topology

Introduce configurable components:

```text
CU ports
  -> internal NoC routers/links
  -> HBM channels or GPU I/O ports
```

This permits contention between CUs, HBM channels, and scale-up egress without
mixing those concerns into the workload format.

### Third stage: persistent SUE-Sim

Create one SUE-Sim/ns-3 topology and one asynchronous event session per
simulation. Multiple in-flight requests must coexist, and delivery callbacks
must return to the GPU runtime without calling a separate `Simulator::Run()`
for each PDU.

## Part IV: Ordered Implementation Plan

## 19. Phase A: Complete Chakra Preservation

**Status: implemented for the fields retained by the current schema reader.**

### Work

- preserve node start time separately from duration;
- preserve inputs, outputs, tensor/device information, and arbitrary
  attributes needed by binding rules;
- distinguish absent values from zero values;
- maintain source schema/version provenance;
- add multi-rank import using `(rank, source_node_id)` keys.

### Exit gate

- an imported graph can be round-tripped into a diagnostic representation
  without losing fields required by the next stages;
- repeated source IDs across ranks do not collide;
- no missing field is silently fabricated.

## 20. Phase B: Define the Kernel Descriptor Manifest

**Status: version 1 implemented.** It supports rank trace declarations,
duration-backed compute bindings, and grouped MSCCL++ collective bindings.
Detailed compute memory-operation templates remain an extension to this phase.

### Work

- define a versioned external manifest;
- support exact node-ID bindings for reproducible tests;
- support validated matching rules for real traces;
- reference MSCCL++ plans and compute templates by path or registry name;
- define explicit fallback and unsupported-node policies;
- validate missing, duplicate, and ambiguous matches before simulation starts.

### Exit gate

- every executable node in the demonstration graph resolves to exactly one
  description or one explicit skip/fallback policy.

## 21. Phase C: Graph-Node Lowering

**Status: implemented for version 1 manifest bindings.** Compute bindings lower
to abstract Reduce kernels; collective bindings lower through the existing
MSCCL++ fine path.

### Work

- lower collective nodes through the MSCCL++ plan registry;
- instantiate plan payloads, participants, ranks, buffers, and semaphore
  generations for the specific graph node;
- lower compute nodes through compute templates;
- lower memory nodes through explicit descriptors;
- maintain graph-node-to-kernel provenance;
- reject incompatible participant counts, payloads, owners, and memory ranges.

### Exit gate

- a small mixed graph lowers into validated fine kernels;
- each fine instruction can be traced back to rank, Chakra node, kernel,
  workgroup, wavefront, and operation.

## 22. Phase D: Dependency-Aware Execution

**Status: implemented and covered by graph-runtime and CLI tests.**

### Work

- add graph-node runtime state;
- submit only ready kernels;
- aggregate completion when one graph node creates multiple kernels;
- release dependents at simulated completion time;
- preserve independent rank-local concurrency;
- add graph-level blocked-state diagnostics.

### Exit gate

- a compute-to-collective-to-compute graph runs in dependency order;
- independent nodes overlap when resources allow;
- no Chakra-driven kernel is submitted unconditionally at time zero.

## 23. Phase E: Separate and Expand Hardware Configuration

### Work

- remove hardcoded fine-CLI memory assumptions;
- load CU, memory, and fabric settings from explicit configuration;
- report every active parameter;
- keep workload, kernel descriptors, and machine configuration separate;
- add optional explicit HBM channels and internal interconnect resources.

### Exit gate

- the same workload and kernel plans can run against two machine
  configurations without modifying the trace or plans.

## 24. Phase F: Persistent SUE-Sim Integration

### Work

- create one persistent ns-3/SUE-Sim session;
- inject concurrent semantic requests asynchronously;
- preserve request provenance through packetization and callbacks;
- map logical channels/ports to rails through configuration;
- expose CBFC, LLR, retry, queue, switch, and rail counters;
- define exact completion points for PUT, GET, and semaphore traffic.

### Exit gate

- concurrent remote requests overlap inside one SUE-Sim event timeline;
- native and SUE-Sim backends agree on logical request/byte conservation;
- no endpoint count is hardcoded to two GPUs.

## 25. Phase G: Validation and Calibration

### Correctness validation

- graph ordering;
- workgroup/CU capacity;
- instruction and request conservation;
- local/remote routing;
- command/response pairing;
- semaphore generation correctness;
- deterministic replay.

### Performance validation

- compare simulated kernel and collective times with measured or published
  references;
- tune only documented hardware parameters;
- report error and uncertainty;
- retain both captured and simulated timelines;
- measure host runtime and memory scaling.

No accuracy percentage should be reported before this phase has real reference
data.

## 26. First Demonstration

The first end-to-end proof should be small but genuinely graph-driven:

```text
two or more GPUs

per-rank compute node
  -> one collective node using an MSCCL++ plan
  -> per-rank compute node
```

It should demonstrate:

- multi-rank Chakra import;
- descriptor binding;
- collective-plan selection;
- workgroup and wavefront creation;
- deterministic CU assignment;
- local HBM Load and Store instructions;
- remote PUT or GET traffic;
- any synchronization required by the selected algorithm;
- dependency-based release of the second compute kernel;
- complete provenance and conservation reporting.

The test should be parameterized by rank count and must not contain logic that
only works for two devices.

## 27. Recommended Development Order

The immediate order is:

```text
Chakra field preservation
  -> multi-rank graph identity
  -> descriptor manifest
  -> node-to-kernel lowering
  -> dependency-aware graph scheduler
  -> configurable hardware model
  -> persistent SUE-Sim
  -> calibration
```

This order is deliberate. Building a more detailed network before the graph
can produce and schedule correct fine-grained requests would optimize the wrong
boundary.

## Part V: Decisions Still Requiring Confirmation

The following decisions should be confirmed with the research supervisor
before their semantics are frozen:

1. Is Load/Store granularity the final target, or is actual ISA/binary replay
   required?
2. Is the target a generic configurable accelerator or a named GPU?
3. Which Chakra schema and trace producer must be supported?
4. Where will compute memory-access descriptions come from?
5. Should the binding information remain in a sidecar/manifest or eventually
   be encoded as Chakra attributes?
6. Who supplies or generates the MSCCL++ plans?
7. Which semaphore and memory-ordering semantics should match the target
   system?
8. Is the first required memory model local/remote HBM only, or must it include
   functional L1/L2 caches?
9. Which internal GPU NoC and HBM-channel effects are required?
10. At what point is a remote operation complete: destination ingress, cache
    visibility, HBM visibility, or an explicit acknowledgement?
11. Which measured or published results will be used for calibration?
12. Which output metrics are the primary research deliverables?

Until answered, these remain configurable assumptions or explicit model
limitations rather than hidden implementation choices.

## Final Summary

The central design principle is:

> Chakra determines which coarse kernel is ready. A descriptor or MSCCL++ plan
> determines what that kernel does. The GPU runtime determines when its
> wavefronts can issue. Memory ownership determines whether each request is
> local or remote. The transport backend determines how remote requests move.

The current repository contains most of the isolated machinery below the graph
layer. The next milestone is to build the missing bridge from multi-rank Chakra
nodes to fine kernels and to schedule those kernels according to graph
dependencies. Only after that bridge works should persistent SUE-Sim and
hardware calibration become the primary focus.
