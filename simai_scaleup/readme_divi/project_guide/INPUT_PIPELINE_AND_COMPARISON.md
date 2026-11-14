# Input, End-to-End Pipeline, and SimAI Comparison

## What This Simulator Accepts

The simulator has two input paths:

1. A native `key=value` workload file. This is the clearest format for new
   scale-up experiments.
2. A bounded AICB text trace plus a native base configuration. This path is
   intentionally limited to one TP group covering the entire scale-up domain.

The input describes workload phases and model parameters. It does not contain
real GPU instructions or packet traces. The native planners derive collective
micro-operations, and the SUE runtime derives transactions and PDUs.

## Source Versus Generated Directories

- `native/`, `tools/`, `readme_divi/`, the top-level CMake file, README, and
  license are durable project files.
- `build-final/` is generated and ignored by Git. It is retained only because
  it contains the latest verified executable and tests for immediate use.
- The obsolete `build-optimized/` checkpoint output has been removed.
- Generated `results/` output has also been removed. Running
  `tools/research_sweep.ps1` recreates it deterministically.

Deleting `build-final/` would not delete source code, but the project would
need to be rebuilt before it could be run again.

## Native Workload Format

Comments begin with `#`. Scalar fields use `key=value`. Each workload layer
uses a repeated `layer=` entry.

```text
schema_version=1
deterministic_seed=20261005
endpoint_count=8
collective_algorithm=ring

link_bandwidth_gbps=200
propagation_delay_ns=10
transaction_payload_bytes=256
pdu_payload_bytes=4096
pdu_overhead_bytes=0
rail_count=2

switch_enabled=1
switch_queue_capacity_pdus=4
switch_latency_ns=5
switch_credits_per_egress=2
credit_return_latency_ns=10

error_injection_pdu_id=0
error_injection_attempts=0
maximum_retries=2
replay_latency_ns=10

layer=0,100,allreduce,4096,after_previous_finish
layer=1,200,none,0,after_previous_compute
layer=2,50,alltoall,8192,after_previous_compute
```

Run it with:

```text
simai_scaleup <native-workload-file>
```

### Global Fields

| Field | Meaning and current boundary |
|---|---|
| `schema_version` | Must currently be `1`. |
| `deterministic_seed` | Reproducibility identity. Fault injection itself is currently selected by PDU identity and attempt, not random sampling. |
| `endpoint_count` | Number of accelerators in the single scale-up group; valid range is 2 through 1024. |
| `collective_algorithm` | `auto`, `ring`, `gather_reduce_broadcast`, or `direct`. `auto` selects a supported native algorithm. The baseline algorithm supports only AllReduce; `direct` supports only AllToAll. |

### SUE Fabric Fields

| Field | Meaning and current boundary |
|---|---|
| `link_bandwidth_gbps` | Serialization rate of one rail. |
| `propagation_delay_ns` | One configured propagation delay in nanoseconds. |
| `transaction_payload_bytes` | Maximum transaction payload; 1 through 256 bytes. |
| `pdu_payload_bytes` | PDU payload limit; at least one transaction and at most 4096 bytes. |
| `pdu_overhead_bytes` | Extra serialized bytes per PDU. |
| `rail_count` | Parallel rails; currently 1 through 4. |
| `switch_enabled` | `0` for direct fabric timing or `1` for the finite-switch model. |
| `switch_queue_capacity_pdus` | Maximum PDU entries per modeled egress when the switch is enabled. |
| `switch_latency_ns` | Fixed switch traversal latency. |
| `switch_credits_per_egress` | Available egress credits. Zero disables explicit credit limiting. It cannot exceed queue capacity. |
| `credit_return_latency_ns` | Delay before a consumed credit becomes available again. |
| `error_injection_pdu_id` | Exact PDU selected for deterministic failure; zero disables injection. |
| `error_injection_attempts` | Number of failed attempts for the selected PDU; it must be zero exactly when the PDU ID is zero. |
| `maximum_retries` | Maximum replay attempts before explicit simulation failure. |
| `replay_latency_ns` | Additional delay before a failed PDU is retried. |

These are model inputs, not hard-coded claims about a particular device.

### Layer Fields

```text
layer=index,compute_ns,collective,payload_bytes[,start_policy]
```

- `index` must be zero-based and contiguous.
- `compute_ns` is a duration supplied by the user or trace. The simulator does
  not derive it from GPU instructions.
- `collective` is `none`, `allreduce`, `reducescatter`, `allgather`, or
  `alltoall`.
- `payload_bytes` is the collective tensor payload. It must be nonzero for a
  collective and zero for `none`.
- `start_policy` defaults to `after_previous_finish`.
  `after_previous_compute` permits the next phase to begin once the previous
  computation finishes, allowing compute/communication overlap.

Native layer names currently become `layer_0`, `layer_1`, and so on. Named
phases imported from AICB retain names such as `attention.forward`.

### Optional GPU Resource Fields

Omitting these fields leaves the GPU resource model disabled. If
`gpu_enabled=1`, every other GPU field is required and nonzero.

```text
gpu_enabled=1
gpu_compute_units_per_device=8
gpu_workgroup_slots_per_compute_unit=4
gpu_wavefronts_per_workgroup=4
gpu_wavefront_issue_width=2
gpu_wavefront_issue_latency_ns=3
gpu_reduction_bytes_per_ns=100
```

This is an abstract contention model. Each ready collective micro-operation is
represented as one synthetic communication workgroup with a configured number
of wavefronts. The model accounts for finite CU slots, issue rounds, waiting,
and byte-dependent local reduction time. It does not read or execute a real
GPU kernel; the exact limitation is explained in
[`FUTURE_PLAN_AND_MODEL_LIMITS.md`](FUTURE_PLAN_AND_MODEL_LIMITS.md).

## Bounded AICB Input

```text
simai_scaleup --aicb <aicb-file> <native-base-config> <nanoseconds-per-tick>
```

The base config supplies the fabric, GPU, seed, endpoint count, and algorithm
policy. The importer expands each AICB row into forward, input-gradient, and
weight-gradient phases and converts trace ticks using the explicit final
argument.

The current adapter accepts only one TP group containing all endpoints, with
`ep=1`, `pp=1`, dependency `-1`, and unsuffixed supported collective names.
It rejects DP/EP, PP, composite, and multi-domain layouts rather than silently
running them on the wrong devices.

## Complete Runtime Flow

```text
native workload file                 bounded AICB trace + base config
          |                                      |
          v                                      v
  workload_parser.cpp                    aicb_importer.cpp
          +----------------------+---------------+
                                 v
                    validated WorkloadSpec
          layers + one TP group + fabric/GPU configuration
                                 |
                                 v
                       ScaleUpSimulator::run
                                 |
              +------------------+------------------+
              |                                     |
              v                                     v
       layer compute timeline              CollectiveRequest
                                                    |
                                                    v
                                     NativeCollectivePlanner
                                  algorithm selection + provenance
                                                    |
                                                    v
                                      validated micro-op DAG
                                        (currently PUT/REDUCE)
                                                    |
                                                    v
                              optional finite GPU resource admission
                                                    |
                                                    v
                                  SUE transaction and PDU creation
                                                    |
                                                    v
                          deterministic rail serialization and retry
                                                    |
                                  +-----------------+----------------+
                                  |                                  |
                                  v                                  v
                         optional finite switch          direct destination
                         queues and credits                   arrival
                                  +-----------------+----------------+
                                                    v
                                      unique-PDU reassembly
                                                    |
                                                    v
                           dependency release and next ready micro-op
                                                    |
                                                    v
                 per-layer timing + provenance + GPU/fabric counters
```

Important separation rules:

- Workload parsing does not create packets.
- The collective planner does not operate switch queues.
- The GPU model delays issue but does not invent collective algorithms.
- SUE executes the plan and does not decide TP/DP/PP/EP membership.
- Reports distinguish logical bytes from physical transmitted bytes, so
  retries do not corrupt logical-work accounting.

## What the Output Contains

The CLI report includes:

- end-to-end time;
- per-layer start, compute completion, communication issue/completion, exposed
  communication, finish time, and transferred bytes;
- selected collective algorithm and schedule provenance;
- per-rail PDUs, bytes, attempts, retries, and busy time;
- switch/credit and GPU-resource counters when enabled;
- global issued/finalized operation, transaction, PDU, byte, retry, and
  completion conservation counters.

## Audited SimAI Versus SimAI Scale-Up

This comparison refers to the upstream SimAI tree audited at commit
`a57d765c5478f319c5f5ab6fdc53f9cc7f461743`, not to every ASTRA-sim or SimAI
version ever released.

| Concern | Audited upstream SimAI | New SimAI Scale-Up |
|---|---|---|
| Primary purpose | Broad distributed-AI simulation with several workload and network integrations. | One self-contained scale-up research domain. |
| Main path | Workload/System layer, SimCCL/MockNCCL flow generation, then a selected network frontend/backend. | Workload parser/importer, native planner, optional GPU resource model, then the single native SUE runtime. |
| Central orchestration | Large legacy System path, including `Sys.cc`-style integration and backend abstractions. | Small `ScaleUpSimulator::run` orchestration; no `Sys.cc`. |
| Collective representation | Collective algorithms ultimately lowered toward send/receive-style flows. | Validated dependency DAG of memory-oriented micro-operations. Built-in execution currently uses PUT and local REDUCE. |
| Network choice | Multiple analytical, packet, and physical backend paths existed in the audited tree. | One backend only; there is no runtime backend selector. |
| Parallel groups | Existing workload machinery contained broader DP/TP/PP-oriented concepts. | Current executable creates one group containing every endpoint. Full TP/DP/PP/EP membership is future work. |
| GPU detail | Primarily supplied compute delays in the audited path; later research motivates higher fidelity. | Optional finite CU/workgroup/wavefront contention, but still synthetic rather than trace-driven. |
| One-sided semantics | Existing flow path did not provide complete PUT/GET/SIGNAL/WAIT memory semantics. | Typed contracts exist for all five micro-op kinds, but the current production executor and native planners intentionally support PUT and REDUCE only. |
| Fabric resources | Behavior depended on the selected external backend. | Native deterministic rails, serialization, finite switch queues, credits, retry, and unique-PDU reassembly. |
| Failure behavior | Broad integration could accept configurations whose exact semantics depended on backend behavior. | Unsupported algorithms and AICB group layouts fail explicitly. |
| Reproducibility | Depended on a much larger dependency graph. | Strict C++14 codebase, exact regressions, deterministic sweeps, and zero-contamination audit. |

The new project deliberately trades upstream breadth and compatibility for a
smaller semantic surface that can be tested exactly. It should not yet be
described as a replacement for every upstream SimAI feature.

## Useful Files

- Native format example: `native/tests/data/multi_layer_overlap.workload`
- GPU example: `native/tests/data/gpu_contended.workload`
- AICB example: `native/tests/data/aicb_scaleup_subset.txt`
- Workload parsing: `native/src/workload_parser.cpp`
- Collective planning: `native/src/collective_planner.cpp`
- End-to-end execution: `native/src/simulator.cpp`
- Reporting: `native/src/report_writer.cpp`
