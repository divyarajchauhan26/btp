# SimAI Scale-Up: Living Findings

This file records our evolving understanding of the project. New findings and
corrections should be added here as we inspect the implementation.

## 1. Project We Are Presenting

The current main project is `simai_scaleup`.

Its intended complete flow is:

```text
AI workload
  -> collective request
  -> CCL / MSCCL++ execution plan
  -> PUT / GET / SIGNAL / WAIT / REDUCE micro-operations
  -> SUE transactions and PDUs
  -> SUE-Sim network
  -> multi-rail spraying, switching, CBFC, and LLR
  -> completion callbacks
  -> timing and traffic report
```

For conceptual discussions, we are temporarily assuming that the final
SUE-Sim wiring shown above is completed. This helps us understand the intended
architecture as one system.

However, the actual implementation status must remain explicit:

- The workload-to-MSCCL++-micro-op path works.
- The native deterministic SUE backend runs end to end.
- The SUE-Sim packet-tag and bridge adapter exists and has been smoke-tested.
- The normal CLI does not yet create a complete SUE-Sim topology session and
  execute the entire workload through that backend.

## 2. Current Inputs

The current simulator accepts:

1. A workload/configuration file describing layers and simulation parameters.
2. Optionally, an MSCCL++-style JSON execution plan describing the collective
   micro-operations.

Example commands:

```text
simai_scaleup workload.workload
simai_scaleup --mscclpp plan.json workload.workload
```

The simulator supports multiple layers, compute-only layers, communication
layers, and limited compute/communication overlap.

## 3. Workload Information

The workload currently includes four kinds of information:

### Workload-level information

- Number of XPU endpoints.
- Layer index.
- Compute duration.
- Collective type.
- Collective payload size.
- Layer start/overlap policy.

Supported collective types are:

```text
none
allreduce
reducescatter
allgather
alltoall
```

### Collective-planner information

- Algorithm policy such as `ring`, `direct`, or
  `gather_reduce_broadcast`.

### Network information

- Link bandwidth and propagation delay.
- Transaction and PDU sizes.
- PDU overhead.
- Rail count.
- Switch queues and latency.
- Credits and credit-return latency.
- Error injection, retries, and replay latency.

### Optional GPU-resource information

- Number of compute units.
- Workgroup slots.
- Wavefront count, issue width, and issue latency.
- Reduction throughput.

## 4. Endpoint, Switch, and Rail

### Endpoint

An endpoint is an XPU/GPU that originates and receives operations and network
traffic.

`endpoint_count=8` means eight XPUs. A switch is not included in this count.

### Switch

A switch is network infrastructure that forwards PDUs between endpoints. It
may introduce queueing and forwarding latency. It is not an XPU and is not a
collective participant.

### Rail

A rail is an independent network path or port available to an endpoint.
Multi-rail spraying distributes traffic across these paths to use bandwidth and
reduce concentration on one path.

## 5. Collective Type Versus Algorithm

These concepts must not be confused:

```text
collective type = what communication operation is requested
                  examples: AllReduce, AllGather

algorithm       = how that operation is executed
                  examples: ring, direct, an MSCCL++ plan
```

The collective type correctly belongs in the workload, as it does in standard
SimAI-style workloads.

The algorithm should conceptually be selected by the CCL or schedule provider.
Our current workload contains `collective_algorithm` because the project first
used an internal native collective planner. This works, but it mixes workload
and CCL policy.

A cleaner future interface would be:

```text
simai_scaleup --planner ring workload.txt
simai_scaleup --mscclpp plan.json workload.txt
```

The workload would retain the collective type and size, while the command or a
separate CCL configuration would select the algorithm.

## 6. Difference From Standard SimAI Input

A standard SimAI workload primarily describes model execution information such
as compute operations, communication collectives, sizes, and parallel groups.

Our current input also contains detailed network and simulator configuration:

```text
rails
packet sizes
switch queues
credits
retry and replay settings
link timing
GPU resource parameters
```

This happened because `simai_scaleup` was built as a deterministic,
self-contained simulator where one file can reproduce an entire test.

That is convenient for regression tests, but it is not the cleanest final
architecture. Ideally, the inputs should be separated as:

```text
workload.txt    -> layers, compute, collective type and size
ccl/plan.json   -> collective algorithm and micro-operations
network.conf    -> topology, rails, links, CBFC, LLR, queues
```

We should preserve support for a single combined test configuration if useful,
while exposing this cleaner separation for normal simulation runs.

## 7. Credits and CBFC

A switch or receiver has finite buffering. A credit indicates that space is
available for another PDU.

Conceptually:

1. The sender consumes a credit before transmitting.
2. The PDU enters the corresponding queue/buffer.
3. After the PDU leaves, the credit returns after a configured delay.
4. If no credit is available, new traffic must wait.

This prevents unlimited queue growth and approximates credit-based flow
control. It is network configuration, not AI workload information.

The native simulator includes a deterministic credit model. The final intended
SUE-Sim path should use and report SUE-Sim's actual CBFC behavior.

## 8. GPU Execution Model

The finite GPU resource model is implemented, but it is simplified and
optional.

Optional means that the simulator can run in two modes:

- disabled: use the workload's fixed compute durations and focus on
  communication;
- enabled: additionally model contention for finite GPU resources and local
  reduction throughput.

Implemented abstractions include:

- finite compute units;
- workgroup occupancy;
- wavefront issue width and latency;
- contention for those resources;
- local reduction throughput.

It is not a full GPU microarchitecture simulator. It does not currently model:

- real CUDA or MSCCL++ instruction execution;
- tensor-core pipelines;
- detailed cache/HBM contention;
- complete kernel scheduling;
- detailed PUT/GET DMA-engine contention.

Therefore, describe it as a finite GPU resource/timing model, not a complete
GPU execution simulator.

## 9. Meaning of Kernel-Level Operations

In this project, kernel-level collective information mainly means exposing the
communication schedule as micro-operations:

```text
PUT
GET
SIGNAL
WAIT
REDUCE
```

This is different from simulating every machine instruction inside a GPU
kernel. The key contribution is that a collective is no longer represented only
as one high-level AllReduce event; its data movement and synchronization
structure can be simulated explicitly.

## 10. Why SIGNAL and WAIT Are Conditional

SIGNAL and WAIT are needed when a schedule requires explicit synchronization
between endpoints.

They are not mandatory for every operation. For example, a direct AllGather in
which every rank independently PUTs its chunk to every peer may not need an
explicit semaphore sequence. Adding unnecessary SIGNAL/WAIT operations would
change the schedule rather than improve it.

## 11. Current Confidence and Boundary

Current confidence toward the intended complete research prototype is roughly
`7/10`.

Strong and verified:

- Layered workload parsing.
- Native collective planning.
- MSCCL++-style plan parsing and translation.
- PUT, two-leg GET, SIGNAL, WAIT, and REDUCE representations.
- Transaction/PDU generation.
- Native deterministic SUE simulation.
- Four-XPU toy AllGather regression.
- SUE-Sim operation-tag bridge smoke testing.

Still required for the intended complete architecture:

- Full SUE-Sim topology/session ownership from `simai_scaleup`.
- A normal `--backend sue-sim` execution path.
- One shared asynchronous ns-3 event loop for overlapping traffic.
- Completion and deadlock handling across the full workload.
- Export of SUE-Sim CBFC, LLR, queue, and retry statistics.
- Chakra workload import.
- Validation against an external reference or hardware measurements.

## 12. Planned Input Cleanup

The current input is valid and useful for testing. It does not need to be
immediately deleted or rewritten.

The recommended evolution is:

1. Keep collective type, size, layer timing, and dependencies in the workload.
2. Move collective algorithm selection to the schedule-provider/CCL interface.
3. Move topology and transport parameters into a network configuration.
4. Keep MSCCL++ micro-operations in their execution-plan JSON.
5. Optionally retain a combined configuration mode for compact regression
   tests.

This cleanup changes ownership of configuration; it does not require rewriting
the underlying simulator models.

## 13. Workload-Layer Boundary

For code study, the workload layer should be examined independently from the
CCL and network layers.

Its current flow is only:

```text
workload file path
  -> native/app/simai_scaleup.cpp
  -> parse_workload_file(...)
  -> native/src/workload_parser.cpp
  -> validated WorkloadSpec
```

`WorkloadSpec` and `LayerSpec` are defined in
`native/include/simai/runtime/workload.hpp`.

The parser:

1. Opens the workload file.
2. Reads `key=value` entries line by line.
3. Parses numbers and enum-like strings.
4. Constructs each `LayerSpec`.
5. Constructs the complete `WorkloadSpec`.
6. Validates the result and rejects malformed or unsupported values.

At the end of this layer, the simulator has an in-memory description of the
requested run. No collective has yet been decomposed into PUT/GET operations,
no PDU exists, and no network simulation has started.

The important layer structures are conceptually:

```text
WorkloadSpec
  endpoint count
  deterministic seed
  collective algorithm policy
  link configuration
  GPU configuration
  list of LayerSpec objects

LayerSpec
  layer index
  compute duration
  collective type
  payload bytes
  start/dependency policy
```

### What we have in hand

The current workload layer is implemented and validated. It supports multiple
layers, compute-only layers, collective layers, payload sizes, and limited
compute/communication overlap policies.

Its current `WorkloadSpec` is broader than a pure AI workload because it also
contains CCL policy, network configuration, and optional GPU-resource model
configuration. This is functional and useful for reproducible tests, but its
responsibilities are mixed.

### Planned ownership

The intended cleaner ownership is:

```text
WorkloadSpec
  layers, compute, collective type/size, dependencies, parallel groups

CclConfig or execution plan
  algorithm selection and collective micro-operation schedule

NetworkConfig
  topology, bandwidth, rails, switches, CBFC, and LLR

GpuModelConfig
  finite compute resources and reduction throughput
```

We do not need to immediately break the existing parser. The current combined
format can remain as a reproducible test/configuration format while cleaner
interfaces are introduced around it.

## 14. Critical Finding: Importing a Plan Is Not Generating a Plan

The MSCCL++ schedule provider currently consumes a plan that already contains
PUT, GET, SIGNAL, WAIT, and REDUCE operations. It parses and translates those
operations into the simulator's common `MicroOp` representation.

Therefore, the current MSCCL++ provider performs:

```text
prewritten MSCCL++ plan
  -> validation and interpretation
  -> internal MicroOp dependency graph
```

It does not currently perform:

```text
high-level AllReduce request
  -> choose an algorithm
  -> automatically generate the MSCCL++ operation sequence
```

This distinction is central. If a user must manually know and write every
micro-operation, the simulator has not itself solved collective decomposition.
The operation sequence must instead come from one of these sources:

1. Official MSCCL++ plan-generation tooling.
2. A plan library containing schedules generated ahead of time.
3. A new algorithm/compiler component that generates compatible plans.
4. The existing native planner for its limited built-in algorithms.

This is a major difference from SimCCL. SimCCL's role in the standard SimAI
pipeline is to take a high-level collective and automatically produce its
communication-flow decomposition. Our native planner performs a small,
simulator-specific version of that role. Our MSCCL++ provider currently imports
an already-decomposed schedule; it is not yet a SimCCL-equivalent decomposer.

### Threadblocks and compute-resource interpretation

The MSCCL++ importer preserves threadblock identity and program order. The
finite GPU model also has compute-unit, occupancy, wavefront, and reduction
parameters. This does not mean that the simulator faithfully executes real
MSCCL++ threadblocks on modeled GPU compute units.

Currently:

- threadblock IDs organize operations and create sequential dependencies;
- REDUCE operations may consume the simplified GPU execution model;
- PUT/GET/SIGNAL/WAIT use the operation and transport models;
- detailed threadblock scheduling, DMA engines, HBM pipelines, caches, and
  actual MSCCL++ kernel execution are not modeled.

Threadblock metadata is therefore partly semantic/organizational rather than a
complete GPU execution model.

### Why native plans can use PUT and REDUCE without SIGNAL/WAIT

The native planner directly attaches operation-ID dependencies to its graph.
For example, a REDUCE can explicitly depend on a PUT becoming remotely visible,
and a later PUT can depend on that REDUCE completing. These simulator-internal
dependencies enforce ordering without a separate semaphore exchange.

SIGNAL and WAIT are necessary when modeling a schedule whose synchronization is
itself explicit and transport-visible, as in an imported MSCCL++ plan. They are
not required when equivalent ordering is already encoded directly in the DAG.

### Required plan-generation evolution

The recommended evolution is:

1. Keep the common `CollectiveScheduleProvider -> PlannedCollective` boundary.
2. Keep the native planner as a limited baseline and test oracle.
3. Import plans generated by official MSCCL++ tooling rather than expecting
   users to manually author micro-operations.
4. Add a plan registry selected by collective type, rank count, payload/chunk
   constraints, protocol, and topology.
5. If runtime generation is required, integrate or build a dedicated
   MSCCL++-compatible schedule generator before the existing importer.
6. Validate imported plans against official plan examples and, where possible,
   hardware execution traces.

## 15. Synchronization Exists Even Without Explicit Semaphores

Every correct collective schedule requires synchronization or ordering. The
important distinction is between:

```text
logical synchronization requirement
```

and:

```text
the concrete mechanism used to enforce that requirement
```

The native planner does have synchronization. It expresses it directly through
operation dependencies and completion stages.

Example:

```text
PUT operation 1
  -> must become remotely visible
  -> REDUCE operation 2 may begin
  -> must complete locally
  -> PUT operation 3 may begin
```

Operations 2 and 3 cannot run early because their dependency IDs are not yet
complete. The simulator's dependency scheduler is acting as an abstract
synchronization mechanism.

An imported MSCCL++ schedule may instead make synchronization part of the
program itself:

```text
PUT
  -> SIGNAL remote semaphore
  -> remote WAIT observes required generation
  -> dependent work begins
```

In that case, SIGNAL and WAIT must be represented explicitly because they may
create control traffic, consume ordering resources, and affect timing.

Therefore:

- synchronization is required for both native and MSCCL++ schedules;
- native schedules currently encode it as abstract DAG dependencies;
- MSCCL++ schedules can encode it as explicit semaphore operations;
- absence of SIGNAL/WAIT does not mean absence of synchronization.

### Future detailed GPU execution

Adding a detailed compute-unit/threadblock model will require stronger
synchronization semantics, but not every dependency must become a semaphore.
Different scopes require different mechanisms:

```text
within one thread/threadblock
  -> program order or local barrier

between threadblocks on one GPU
  -> scheduler event, local barrier, atomic, or local semaphore

between different GPUs
  -> remote completion, SIGNAL/WAIT, atomic, or another CCL protocol primitive
```

The future model should preserve the synchronization mechanism specified by the
source schedule instead of automatically converting every dependency into
SIGNAL/WAIT. Artificially inserting semaphores everywhere would change the
algorithm and overestimate control traffic and latency.

The recommended future representation is:

1. Keep graph dependencies as the universal correctness relationship.
2. Label each dependency with its synchronization scope and mechanism.
3. Model explicit SIGNAL/WAIT when required by the imported plan.
4. Add local barriers/atomics when detailed threadblock scheduling is modeled.
5. Release dependencies only at the correct completion stage, such as local
   issue, local completion, or remote visibility.

## 16. Revised Primary Goal: Fine-Grained Scale-Up GPU Modeling

After clarification, the immediate research goal is no longer to prioritize a
complete end-to-end network simulation. The first priority is a fine-grained
GPU execution model designed around scale-up Load/Store semantics.

The intended hierarchy is:

```text
kernel
  -> workgroups
  -> wavefronts
  -> GPU operations
  -> primitive Load/Store/control instructions
  -> cache-line-sized memory requests
```

The required dynamic model includes:

- finite GPUs and compute units;
- workgroup residency and CU assignment;
- ready and stalled wavefront scheduling;
- local and remote memory spaces;
- Load, Store, SemaphoreAcquire, SemaphoreRelease, Reduce, and Waitcnt;
- operation wrappers such as MemcpyOp and barrier operations;
- outstanding-request tracking;
- local memory and remote scale-up request routing;
- synchronization latency and wake-up behavior;
- per-CU, per-wavefront, per-memory-tier, and per-request reporting.

### Important fidelity boundary

The target should initially be Load/Store semantic simulation, not exact GPU
binary or ISA simulation. ASTRA-sim 3.0 explicitly chooses this boundary to
retain scalability. The model can be more configurable and observable without
claiming binary-level accuracy.

### Scale-up-only ownership

The project should focus on one scale-up domain. SUE-Sim remains responsible
for remote fabric behavior such as rails, switches, CBFC, LLR, and delivery.
The GPU model is responsible for determining when and why a local or remote
memory request is generated.

### Placement in the codebase

The recommended implementation is a new modular subsystem inside
`simai_scaleup`, not direct modification of ASTRA-sim 2.0. Reusable current
components include the deterministic event queue, typed IDs, buffer ownership,
MSCCL++ parser, completion contracts, transport abstraction, and tests.

The current `GpuExecutionModel` is only a coarse baseline. It models finite CU
slots, workgroup admission, wavefront issue cost, and reduction duration, but
does not execute wavefront instruction streams. It should remain available as
a coarse mode while a separate fine mode is built.

### Chakra ownership

Chakra should describe the kernel graph and dependencies. It should not normally
hardcode dynamic CU assignments. Fine-grained kernel descriptions may come
from MSCCL++ plans, compute-kernel templates/profiles, Chakra attributes, or a
versioned sidecar format. The GPU model should decide runtime CU assignment.

### Key architectural rule

Only remote memory requests should enter SUE. Register and local-memory
operations should be handled by the GPU/local-memory model. The complete
provenance chain should remain visible:

```text
Chakra node or MSCCL++ operation
-> GPU operation
-> primitive instruction
-> cache-line request
-> SUE PDU, when remote
```

The complete phased design is recorded in
`readme_divi/FINE_GRAINED_SCALEUP_GPU_IMPLEMENTATION_PLAN.md`.
