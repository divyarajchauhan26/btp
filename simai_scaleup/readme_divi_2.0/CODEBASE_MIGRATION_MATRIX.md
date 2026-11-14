# Codebase Migration Matrix

Nothing is retained solely because it already exists. This matrix is the
starting classification and may change after implementation evidence.

## 1. Retain With Minimal Change

| Component | Current location | Reason |
|---|---|---|
| Typed IDs | `native/include/simai/contracts/ids.hpp` | Extend with new IDs; existing pattern is useful. |
| Validation style | `native/include/simai/contracts/validation.hpp` | Reuse for new static/dynamic contracts. |
| Deterministic event queue | `native/include/simai/contracts/event_queue.hpp` | Correct foundation; expand event kinds/payload ownership. |
| Report formatting pattern | `native/src/report_writer.cpp` | Retain structure, greatly expand schema. |
| Test harness | `native/tests/vertical_slice_test.cpp` | Keep existing regressions, then split new tests by subsystem. |

## 2. Retain but Extend or Wrap

| Component | Current location | Required change |
|---|---|---|
| Buffer references | `contracts/buffer.hpp` | Add typed memory spaces, alignment, access type, and remote/local validation. |
| MSCCL++ parser | `runtime/mscclpp_plan.*` | Retain JSON parsing; replace direct lowering with fine-grained kernel lowering in fine mode. |
| Schedule-provider boundary | `runtime/collective_planner.*` | Preserve for coarse mode; add kernel-plan registry/lowering boundary. |
| Completion stages | `contracts/completion_stage.hpp` | Extend semantics for issue, local memory, network acceptance, and remote visibility. |
| SUE operation metadata | `contracts/sue_transaction.hpp`, `runtime/sue_pdu.*` | Add instruction/request provenance; keep packetization transport-owned. |
| Transport backend abstraction | `runtime/transport_backend.*` | Introduce a semantic scale-up request layer above it. |
| SUE-Sim bridge | `runtime/sue_sim_transport_backend.*` and `SUE-Sim` bridge files | Retain packet identity work; redesign for persistent asynchronous sessions. |
| Reassembly | `runtime/reassembly.*` | Generalize from micro-op completion to request/instruction completion. |
| Native fabric | `runtime/multi_rail_fabric.*`, switch/link files | Keep as deterministic test backend, not final fidelity authority. |
| Workload parser | `runtime/workload_parser.cpp` | Retain legacy/coarse mode; split network/GPU ownership for 2.0. |

## 3. Replace Behind a Compatibility Mode

| Component | Current location | Replacement |
|---|---|---|
| Coarse GPU model | `runtime/gpu_execution.*` | New CU/workgroup/wavefront/instruction execution subsystem. Keep old model as `coarse` until migration gates pass. |
| Monolithic collective executor | `runtime/simulator.cpp` | Kernel-graph orchestrator plus independent GPU, memory, synchronization, and transport engines. |
| Direct MicroOp packetization | `runtime/simulator.cpp`, `runtime/sue_pdu.cpp` | GPU operation lowering → instructions → memory requests → transport packetization. |
| Combined `WorkloadSpec` ownership | `runtime/workload.hpp` | Separate workload graph, GPU config, memory config, and scale-up network config. |

## 4. Keep Only as Baseline/Test Utilities

| Component | Purpose |
|---|---|
| Native ring/gather/direct planners | Simple reference schedules and network tests, not the detailed research path. |
| Existing text workload fixtures | Small deterministic regressions and migration comparisons. |
| Existing native SUE backend | Fast analytical/deterministic comparison and unit testing. |

## 5. New Components

```text
gpu/instruction
gpu/operation
gpu/kernel_ir
gpu/execution_state
gpu/cu_scheduler
gpu/wavefront_scheduler
memory/address
memory/request
memory/local_hierarchy
synchronization/semaphore
synchronization/barrier
synchronization/wait_counter
workload/kernel_graph
workload/chakra_frontend
workload/compute_kernel_lowering
scaleup/request
scaleup/sue_adapter
plan/registry
diagnostics/deadlock
```

## 6. Potential Deletions

No deletion occurs during initial construction. Candidates become removable
only after replacement gates pass:

- direct `MicroOp -> PDU` execution path in fine mode;
- one-workgroup-per-micro-op lowering assumptions;
- coarse-only GPU timing fields from the main fine-mode configuration;
- mixed network fields in the pure workload representation;
- duplicate semaphore/dependency logic superseded by the new synchronization
  engine;
- synchronous per-PDU SUE-Sim event-loop execution;
- test executables and generated binaries accidentally stored in source trees.

## 7. Deletion Gate

A legacy component may be removed only when:

1. Its replacement has unit tests.
2. At least one end-to-end vertical slice uses the replacement.
3. Equivalent invariants pass against the legacy path where comparison is
   meaningful.
4. Documentation and callers have migrated.
5. `rg` confirms no supported caller remains.
6. The deletion is isolated in a reviewable patch.

## 8. Folder Strategy

Do not create a second independent simulator repository yet. Add clean 2.0
namespaces/subdirectories inside `simai_scaleup`, selectable through a mode or a
new executable during development.

Recommended transition:

```text
simai_scaleup             legacy/coarse executable remains stable
simai_scaleup_fine        temporary development executable
```

After the fine path is complete, consolidate the CLI while preserving an
explicit coarse mode if still valuable.

