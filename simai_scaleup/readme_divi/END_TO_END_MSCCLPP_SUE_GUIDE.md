# Workload to MSCCL++ Micro-Ops to SUE: Complete Guide

## 1. What This Project Is

This repository implements a research prototype for simulating collective
communication within one scale-up accelerator domain. Its key architectural
change is a schedule-provider boundary: workload parsing is separated from
collective decomposition, and collective decomposition is separated from the
transport backend.

The implemented path is:

```text
native workload or bounded AICB trace
  -> layer and collective request
  -> schedule provider
       A. native ring/baseline planner, or
       B. MSCCL++ JSON parser and translator
  -> dependency graph of PUT / GET / SIGNAL / WAIT / REDUCE micro-ops
  -> dependency-aware operation executor
  -> SUE transaction segmentation
  -> PDU packetization and operation tags
  -> transport backend
       A. deterministic native SUE fabric (runnable CLI), or
       B. SUE-Sim/ns-3 bridge (compiled and smoke-tested adapter)
  -> completion, reassembly, timing, and counters
  -> text report
```

This is not a replacement implementation of MSCCL++. It is a simulator-side
interpreter for the safe subset of MSCCL++ execution-plan JSON needed to turn a
collective schedule into simulated operations.

## 2. Relationship to Original SimAI

The local upstream snapshot describes SimAI-Simulation as:

```text
AICB workload
  -> SimCCL collective flow decomposition
  -> ASTRA-Sim execution/scheduling layer
  -> ns-3 network simulation
```

Evidence is in `../SimAI/README.md` and `../SimAI/docs/Tutorial.md`. Upstream
documents AICB as the workload generator, SimCCL as the collective decomposition
component, ASTRA-Sim as the simulation front end, and ns-3 as the detailed
network backend.

This prototype preserves that separation but changes the scale-up research
path:

| Concern | Original SimAI path | This prototype |
|---|---|---|
| Workload | AICB-generated workload | Native format plus bounded AICB import |
| Collective decomposition | SimCCL/NCCL-derived flow model | Native planner or MSCCL++ JSON interpreter |
| Execution | ASTRA-Sim | Local dependency-aware micro-op executor |
| Network | ns-3-alibabacloud | Native deterministic SUE model; optional SUE-Sim adapter |
| Scale-up operations | Send/receive-style flows | Memory-semantic PUT/GET plus synchronization |

Therefore the verified toy run is analogous in *layering* to original SimAI,
but it is not a bit-for-bit reproduction of an upstream SimAI example.

## 3. What Each Input Contains

### 3.1 Workload file

The workload describes simulation-wide and layer-level facts:

- endpoint count and deterministic seed;
- compute duration per layer;
- collective kind and payload size;
- dependency/overlap policy;
- link bandwidth, propagation delay, payload sizes, and rails;
- optional switch queue, credit, retry, and replay settings;
- optional finite GPU execution-resource settings.

The workload says *what collective occurs and how much data it represents*. It
does not need to hardcode every PUT or WAIT when an external plan supplies those
operations.

Example: `native/tests/data/four_gpu_allgather.workload`.

### 3.2 MSCCL++ plan

The plan describes *how that collective is executed*:

- participating GPU/rank IDs;
- input, output, and scratch chunk counts;
- threadblocks and ordered operation lists;
- memory channels and connected peers;
- remote buffers;
- operation buffer indices and chunk sizes;
- synchronization operations.

Example: `native/tests/data/mscclpp_four_gpu_allgather.json`.

The parser validates the supported schema and the translator emits the local
`PlannedCollective` DAG. Unsupported or inconsistent plans fail rather than
silently degrading into a different algorithm.

### 3.3 Why both files are needed

The workload and execution plan answer different questions. A workload layer
may say "4,096-byte all-gather across four endpoints." The plan says which rank
performs each PUT, which remote output buffer receives each chunk, and what
dependencies must hold. The link configuration then determines simulated
transport timing.

## 4. Micro-Operation Semantics

### PUT

`PUT(source, destination, bytes)` models a one-sided write. The source issues
payload traffic to a remote memory target. Completion occurs after the PDU path
and reassembly complete.

### GET

`GET(requester, owner, bytes)` is modeled as two transport legs:

1. a request travels from requester to data owner; and
2. a response carrying the requested payload travels back.

This is intentionally not reduced to a local delay or a single PUT.

### SIGNAL

`SIGNAL(source, destination, semaphore)` models a remote synchronization update.
It is represented as transport-visible control traffic and advances a semaphore
generation at the receiver.

### WAIT

`WAIT(endpoint, semaphore, generation)` has no payload and performs no ALU work.
It becomes ready only after the required semaphore generation is visible.

### REDUCE

`REDUCE(endpoint, bytes)` models local reduction work. Its duration and GPU
resource behavior are handled by the operation/GPU execution path rather than
the network.

### Are SIGNAL and WAIT always required?

No. They are required only when the schedule needs explicit cross-rank ordering
that cannot be represented by local DAG dependencies alone. The four-GPU direct
all-gather fixture consists only of independent PUTs, so adding SIGNAL/WAIT
would add synchronization that the algorithm does not need. The two-GPU fixture
`mscclpp_two_gpu.json` exists specifically to exercise PUT, SIGNAL, WAIT, and
REDUCE translation.

## 5. Four-XPU Toy Simulation

The included plan is a direct all-gather. Every GPU writes its one logical chunk
to each of the other three GPUs:

```text
4 sources x 3 remote destinations = 12 PUT operations
4096-byte collective / 4 chunks = 1024 bytes per PUT
12 x 1024 bytes = 12288 transferred payload bytes
```

This fixture demonstrates that endpoint count is input-driven and not fixed at
two. It is intentionally small and auditable; it is not claimed to be an
optimized all-gather algorithm.

Verified report facts on 2026-10-19:

```text
schedule source:       mscclpp
algorithm:             simai_four_gpu_direct_allgather
issued operations:     12
finalized operations:  12
completions:            12
transferred payload:   12288 bytes
PDUs:                   12
end-to-end time:        602 ns under the fixture's model parameters
```

The 602 ns result is a deterministic simulator output for this toy
configuration, not a hardware performance measurement.

## 6. Build and Test

### Portable native build

Requirements:

- CMake 3.16 or newer;
- a C++14 compiler;
- a build tool supported by CMake.

From `simai_scaleup`:

```text
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

On multi-config generators, add `--config Release` to build and test commands.

### Verified Windows toolchain in this workspace

The ns-3-enabled build was compiled with LLVM-MinGW and the workspace-local
CMake executable. When launching that binary manually, the LLVM-MinGW runtime
directory and `SUE-Sim/build/lib` must be on `PATH` so Windows can resolve DLLs.

The simpler native build does not require SUE-Sim. `SIMAI_ENABLE_SUE_SIM` is
`OFF` by default.

### Tests

The CTest suite includes:

- contract-level data structure tests;
- vertical-slice workload, planner, transport, retry, GPU, and parser tests;
- a four-GPU MSCCL++ CLI regression run.

Run only the toy CLI regression:

```text
ctest --test-dir build -R simai_mscclpp_four_gpu_cli_test --output-on-failure
```

## 7. Run Commands

Paths below assume a single-config build where executables are under
`build/native`.

### Four-XPU MSCCL++ schedule

```text
build/native/simai_scaleup --mscclpp \
  native/tests/data/mscclpp_four_gpu_allgather.json \
  native/tests/data/four_gpu_allgather.workload
```

### Native collective planner

```text
build/native/simai_scaleup native/tests/data/ring_allreduce.workload
```

### AICB subset importer

```text
build/native/simai_scaleup --aicb \
  native/tests/data/aicb_scaleup_subset.txt \
  native/tests/data/aicb_base.workload 1
```

The final argument converts AICB ticks to nanoseconds. The importer deliberately
rejects unsupported parallel-group layouts rather than guessing.

## 8. Reading the Report

The report contains:

- schema version and deterministic seed;
- end-to-end simulated duration;
- per-layer compute, communication issue, completion, overlap, and finish times;
- transferred payload bytes;
- per-rail PDU, wire-byte, utilization, attempt, and retry counters;
- schedule source, algorithm name, and schedule version;
- global operation, transaction, PDU, packet, credit, retry, and completion
  counters.

Use counter conservation as the first sanity check: issued and finalized
operations should match for a successful run, and expected payload bytes should
match the algorithm's communication volume.

## 9. Native SUE Fabric Versus SUE-Sim

### Completed and runnable

The default native backend models:

- payload segmentation into SUE transactions and PDUs;
- multiple rails;
- propagation and serialization timing;
- optional finite switch queues;
- credits and credit return;
- deterministic error injection, retry, and replay;
- reassembly and operation completion.

This backend is what the CLI commands above currently use.

### Completed adapter work

The optional SUE-Sim integration contains:

- operation metadata tags attached to ns-3 packets;
- identified injection through `SimaiSueBridge`;
- destination visibility callbacks;
- source, destination, VC, payload, PDU, operation, and port validation;
- endpoint- and rail-aware bridge behavior;
- a passing smoke test over all directed pairs in an eight-endpoint set.

Relevant code:

- `native/src/sue_sim_transport_backend.cpp`;
- `../SUE-Sim/model/simai-sue-bridge.cc`;
- `../SUE-Sim/model/sue-operation-tag.cc`.

### Not yet completed

The CLI does not yet own and construct a full SUE-Sim topology session. It does
not currently instantiate `SueSimulationConfig`, `TopologyBuilder`,
`ApplicationDeployer`, and `SimaiSueBridge` from the workload and then select
`SueSimTransportBackendFactory` for a normal command-line run.

Also outstanding:

- batch multiple in-flight PDUs in one ns-3 run rather than calling the event
  loop per PDU;
- map logical channels to physical SUE ports explicitly;
- export SUE-Sim CBFC, queue, drop, retry, and replay statistics into the SimAI
  report;
- add timeout/deadlock diagnostics;
- represent PUT, GET request, GET response, and SIGNAL service roles explicitly
  inside SUE-Sim;
- validate timing against a hardware trace or an accepted reference model.

Consequently, it is accurate to say "implemented and smoke-tested a SUE-Sim
transport adapter," but not "completed full end-to-end SUE-Sim integration."

## 10. Important Source Map

| Responsibility | Source |
|---|---|
| CLI and mode selection | `native/app/simai_scaleup.cpp` |
| Workload schema/parser | `native/include/simai/runtime/workload.hpp`, `native/src/workload_parser.cpp` |
| AICB import | `native/src/aicb_importer.cpp` |
| Schedule-provider interface | `native/include/simai/runtime/collective_planner.hpp` |
| Native planners | `native/src/allreduce_planner.cpp` |
| MSCCL++ parser/translator | `native/src/mscclpp_plan.cpp` |
| Simulation orchestration | `native/src/simulator.cpp` |
| GPU execution model | `native/src/gpu_execution.cpp` |
| PDU packetization | `native/src/sue_pdu.cpp` |
| Native SUE transport | `native/src/transport_backend.cpp`, `native/src/multi_rail_fabric.cpp` |
| SUE-Sim adapter | `native/src/sue_sim_transport_backend.cpp` |
| Reports | `native/src/report_writer.cpp` |
| Main regression suite | `native/tests/vertical_slice_test.cpp` |

## 11. Recommended Next Milestones

1. Add a topology-session owner that constructs and owns the SUE-Sim/ns-3
   objects for the duration of one simulation.
2. Add a CLI/backend selector such as `--backend native-sue|sue-sim`.
3. Run the same MSCCL++ plan through both backends and compare operation and byte
   conservation before comparing time.
4. Add bounded event-loop execution and deadlock diagnostics.
5. Export SUE-Sim queue/CBFC/retry counters into the shared report schema.
6. Add Chakra 3.0 as another workload import path without moving collective
   decomposition into the workload parser.
7. Validate against a published or measured reference and document expected
   modeling error.

## 12. Chakra End Goal

Chakra should be treated as a workload/trace input format, not as the CCL. A
future Chakra importer should map supported execution-trace nodes and
dependencies into the simulator's layer/collective request representation. The
selected schedule provider should still decide how a collective becomes
PUT/GET/SIGNAL/WAIT/REDUCE operations. This preserves the architecture:

```text
Chakra trace -> workload/dependencies -> MSCCL++ schedule -> micro-ops -> SUE
```

Any Chakra fields that already encode lower-level communication must be mapped
carefully to avoid decomposing the same collective twice.

## 13. Claims Checklist

Safe current claims:

- implemented an MSCCL++-style execution-plan parser and micro-op translator;
- modeled PUT, two-leg GET, semaphore SIGNAL/WAIT, and local REDUCE semantics;
- built deterministic transaction/PDU, multi-rail, switch, credit, retry, and
  reassembly paths;
- added a transport abstraction and an operation-tagged SUE-Sim/ns-3 adapter;
- verified a general four-XPU direct all-gather toy plan and an eight-endpoint
  bridge metadata smoke test.

Claims that require more work or external evidence:

- complete MSCCL++ compatibility;
- full SUE-Sim end-to-end CLI integration;
- hardware accuracy or a percentage performance improvement;
- production readiness;
- complete Chakra 3.0 support.
