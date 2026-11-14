# Collective Planning and Fine-Grained GPU Pipeline

## Purpose

This document explains the intended end-to-end architecture of the clean
SimAI scale-up simulator. It resolves four recurring questions:

1. What happens after workload input?
2. What replaces legacy `Sys.cc`, SimCCL, and MockNCCL?
3. If MSCCL++ is optional, what is the default collective implementation?
4. Where do kernel, workgroup, wavefront, CU, and memory modeling enter?

It also records an important correction to the implementation roadmap: the
current gather-reduce-broadcast AllReduce is a vertical-slice baseline, not the
final collective-planner architecture.

## 1. Legacy SimAI Flow

The old runtime approximately follows this path:

```text
workload input
    -> Workload / Layer
    -> Sys.cc
    -> SimCCL / MockNCCL
    -> send/receive FlowModels
    -> AstraNetworkAPI
    -> selected network backend
```

The responsibilities are:

- the workload describes layers, compute time, communication size, collective
  type, and ordering;
- `Workload` and `Layer` determine when communication is required;
- `Sys.cc` mixes orchestration, groups, logical topology, collective calls,
  event handling, backend calls, and reporting;
- MockNCCL chooses or represents algorithm, protocol, channels, chunks, ranks,
  and dependencies;
- MockNCCL emits abstract byte flows containing mainly source, destination,
  size, and flow dependencies;
- the old network interface executes those flows as matched send/receive work.

MockNCCL is a translator and model. It does not execute real NCCL kernels and
does not move real GPU data.

## 2. Clean Target Flow

The clean simulator does not modify `Sys.cc` into an MSCCL++ driver. It removes
`Sys.cc` from the production build and separates its useful responsibilities:

```text
workload input
    -> workload parser and workload DAG
    -> TP / DP / PP / EP group construction
    -> CollectiveRequest
    -> schedule source
         |-- native collective planner                 [default]
         `-- validated MSCCL++ JSON importer            [optional]
    -> common native MicroOp DAG
         PUT / GET / SIGNAL / WAIT / REDUCE / barriers
    -> fine-grained GPU execution model
         kernels / workgroups / wavefronts / CUs / memory resources
    -> SUE commands and service transactions
    -> transaction segmentation and PDU packing
    -> rails / switch / queues / credits / reliability
    -> completion and memory visibility events
    -> workload dependency release and final report
```

SUE-Sim must never parse an MSCCL++ plan directly. Every schedule source must
first lower into the common native IR.

## 3. What Replaces `Sys.cc`

There is deliberately no `NewSys.cc` monolith. Current responsibilities are
already split as follows:

| Legacy responsibility | Clean component |
|---|---|
| Parse input | `workload_parser.cpp` |
| Execute workload dependencies | `simulator.cpp` |
| Generate current AllReduce | `allreduce_planner.cpp` |
| Represent operation semantics | contracts under `native/include/simai/contracts` |
| Segment transactions and create PDUs | `sue_pdu.cpp` |
| Select and serialize rails | `multi_rail_fabric.cpp` |
| Model finite switch queues | `finite_switch.cpp` |
| Produce deterministic results | `report_writer.cpp` |

The upstream ASTRA/SimAI `Sys.cc` remains unchanged and excluded from the
native build. Its useful ideas may be reviewed, but its two-sided execution
boundary is not retained.

## 4. Default Collective Path

MSCCL++ is not the default. MockNCCL is also not used by the clean runtime.

The default is an internal native collective planner:

```text
CollectiveRequest
    -> native algorithm selection
    -> native buffer/chunk schedule
    -> native MicroOp DAG
```

The original vertical-slice baseline remains available:

```text
seven peers PUT their input to endpoint 1
    -> endpoint 1 performs seven ordered reductions
    -> endpoint 1 PUTs the result to the seven peers
```

This gather-reduce-broadcast AllReduce contains 14 PUT operations and 7 local
REDUCE operations. CP-09A also added a native ring AllReduce and routes both
algorithms through `CollectiveRequest`, capability checks, explicit policy,
and schedule provenance. Broader collective coverage remains CP-11.

## 5. Why MockNCCL Is Attractive

MockNCCL already contains mature collective knowledge, including concepts for:

- collective and algorithm selection;
- rings, trees, and collective phases;
- ranks and peer ordering;
- channels;
- chunks and slices;
- protocol selection;
- flow dependencies;
- multiple collective types.

If the target remained:

```text
collective -> coarse send/receive flows -> conventional backend
```

then reusing MockNCCL would be the easiest path.

## 6. Why MockNCCL Cannot Remain Unchanged

The current flow representation mainly states:

```text
source rank, destination rank, bytes, flow dependency
```

Correct one-sided and fine-grained execution additionally requires:

```text
operation kind: PUT / GET / SIGNAL / WAIT / REDUCE
initiating endpoint
source and destination buffers
offset, length, and ownership
thread-block and program order
ordering domain
semaphore and expected generation
local completion
remote arrival
remote memory visibility
final dependency release
memory fences and barriers
GPU execution-resource ownership
```

Adding only new enum values to `SingleFlow` would therefore be incorrect.
Using MockNCCL as the core would require substantial redesign of its flow IR,
QP/busy state, dependency engine, completion handling, synchronization, GPU
ownership, and backend interface. At that point it has effectively become the
new native planner and IR while retaining old coupling.

The conclusion is not that extending MockNCCL is impossible. The conclusion
is:

- MockNCCL is easier for coarse send/receive coverage;
- native planning is cleaner for one-sided memory and GPU semantics;
- proven MockNCCL algorithm mathematics should be reused rather than ignored.

## 7. Adopted Hybrid Strategy

The project will avoid both extremes: it will neither keep the old MockNCCL
runtime unchanged nor reinvent every collective algorithm without reference.

```text
                         CollectiveRequest
                                |
                         planner interface
                +---------------+---------------+
                |               |               |
        native generators  adapted baseline  MSCCL++ importer
                +---------------+---------------+
                                |
                       native MicroOp DAG
                                |
                    GPU executor -> SUE fabric
```

The adopted rules are:

1. The native MicroOp IR and SUE executor remain the core.
2. Established rank, ring, tree, channel, chunk, and phase logic from MockNCCL
   and upstream collective implementations may be translated after review.
3. A legacy-flow adapter may be used only as a coarse baseline for byte counts,
   rank ordering, and schedule comparison.
4. A legacy flow must not be presented as proof of PUT/GET, memory visibility,
   semaphore, barrier, or GPU semantics that it does not contain.
5. MSCCL++ remains an optional schedule frontend.
6. Every plan records its provenance.

Example provenance values:

```text
native:gather_reduce_broadcast
native:ring
native:recursive_halving_doubling
adapted:mocknccl_baseline
imported:mscclpp:<schema-version>
```

## 8. Required General Planner Framework

Before broad collective coverage or the detailed GPU model, the hardcoded
planner must be replaced by a reusable framework.

### Collective request

```text
CollectiveRequest
  kind
  parallel group and ordered members
  payload and datatype
  reduction operation
  source and destination buffers
  allowed/forced algorithm
  fabric capabilities
```

### Planner responsibilities

- validate the request and group;
- select a supported algorithm using explicit policy;
- divide buffers into typed chunks;
- assign ranks, thread blocks, and program order;
- generate operation dependencies and synchronization;
- produce a valid native `CollectivePlan`;
- reject unsupported cases rather than silently falling back;
- record algorithm and schedule provenance.

### Planned native algorithm families

```text
AllReduce
  - gather-reduce-broadcast baseline
  - ring reduce-scatter + all-gather
  - recursive halving/doubling where valid

ReduceScatter
  - ring
  - recursive halving

AllGather
  - ring
  - recursive doubling

AllToAll
  - direct exchange
  - deterministic pairwise exchange
```

Algorithm selection must eventually consider collective kind, group size,
payload size, chunking, available rails, ordering requirements, and explicitly
configured policy. It must not use undocumented magic thresholds.

## 9. MSCCL++'s Exact Role

An MSCCL++ plan is generated outside this simulator by an MSCCL++ algorithm or
DSL toolchain. A future importer will:

- validate the supported schema/version;
- map MSCCL++ ranks to the members of a SimAI TP/DP/PP/EP group;
- map buffers, chunks, and offsets;
- preserve thread-block assignments and dependencies;
- translate supported PUT, GET, SIGNAL, WAIT, REDUCE, copy, and fused behavior;
- expand or represent loops deterministically;
- reject unsupported channel or execution features;
- produce the same native `CollectivePlan` used by native generators.

The project does not need to recreate the complete MSCCL++ runtime, CUDA
executor, or hardware implementation. Importing real plans is not trivial, but
it is a bounded frontend problem once the native IR is stable.

MSCCL++ is valuable because it supplies real fine-grained schedules and a
semantic reference. It is not required for the simulator to run.

## 10. Fine-Grained GPU Modeling

CP-10 inserted a deliberately scoped GPU execution layer between ready
micro-operations and SUE issue:

```text
ready collective MicroOp
    -> communication kernel
    -> workgroups/thread blocks
    -> wavefront operations
    -> finite CU and memory-resource scheduling
    -> executable network instruction
    -> SUE command
```

Planned representations include:

```text
GpuKernel
  -> Workgroup
     -> Wavefront
        -> GpuOperation
```

Relevant operations include load, store, local reduce, PUT, GET, SIGNAL, WAIT,
wait-for-outstanding operations, workgroup barriers, and grid/inter-workgroup
barriers.

Planned finite resources include:

- compute units and workgroup slots;
- wavefront issue capacity;
- outstanding memory-request depth;
- local reduction execution capacity;
- HBM/NoC access paths;
- SUE command issue capacity;
- semaphore and barrier state.

Wavefronts may be ready, running, stalled on memory, stalled on semaphore,
stalled on a barrier, or complete. A network command is generated only when
the issuing GPU operation becomes executable.

The implemented first gate validates finite CU/workgroup residency, wavefront
issue timing, local reduction time, and deterministic stalls with controlled
synthetic kernels. It deliberately does not yet claim HBM/NoC or full GPU ISA
fidelity. Real-trace integration remains CP-12.

## 11. Where Kernel Information Comes From

The current SimAI/ASTRA-sim 2.0-style workload is primarily layer-level. It
provides information such as compute duration, collective kind, bytes, and
ordering, but normally does not provide enough information to derive true CU
occupancy or wavefront execution.

Fine-grained modeling therefore needs richer sources:

- an explicit kernel/workgroup/wavefront trace;
- an analytical or synthetic kernel generator;
- profiler-derived kernel parameters;
- a future extended Chakra/AICB-style importer;
- MSCCL++ plans for communication kernels.

MSCCL++ can help describe communication kernels. It does not automatically
describe GEMM, attention, activation, or other model-compute kernels.

The CU/workgroup/wavefront execution model discussed by this project comes
from the ASTRA-sim 3.0 direction. Chakra is a workload/execution-trace format;
the trace representation does not itself perform GPU resource scheduling or
establish memory visibility.

Full real-workload/kernel import remains CP-12, after the controlled GPU model
is correct.

## 12. Revised Implementation Order

The implementation sequence is now:

```text
CP-08  explicit credit flow control
CP-09  retry and reassembly
CP-09A general collective planner framework
       + schedule provenance
       + reviewed MockNCCL/upstream algorithm translation
       + ring AllReduce foundation
CP-10  fine-grained GPU execution resources
CP-11  collective coverage and algorithm expansion
CP-12  full workload and kernel-trace integration
```

Placing CP-09A before CP-10 prevents the GPU scheduler from being designed
only around the temporary gather-reduce-broadcast schedule.

## 13. Final Decision Summary

```text
Default schedule source: native collective planner
Optional schedule source: MSCCL++ JSON importer
MockNCCL role: reviewed algorithm reference and optional coarse baseline
Core representation: native buffer-aware MicroOp DAG
Legacy Sys.cc: unchanged and excluded from the native build
Fine-grained GPU model: planned for CP-10 after planner generalization
Real kernel/workload import: planned for CP-12
Network execution: native SUE backend only
```

The current hardcoded AllReduce is therefore accepted only as a verified
vertical slice. The project is not complete until it has a general planner,
multiple validated algorithms, GPU-resource execution, and real workload
integration.
