# SimAI Scale-Up Project Context and Handoff

## Active Status

The user explicitly resumed implementation on 2026-10-05 and requested
continuous checkpoint-by-checkpoint progress. CP-14 is complete. CP-15
prototype retirement is active.

This file is the quickest re-entry point. The authoritative detailed ledger
is [`MASTER_README.md`](MASTER_README.md); the roadmap is
[`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md).

## Objective

Create a fresh, self-contained, end-to-end AI workload simulator for research
inside one accelerator scale-up domain. It must model AI layer execution,
compute/communication dependencies, TP/EP/DP groups, collective planning,
native memory-oriented operations, GPU resources, and a native Scale-Up
Ethernet (SUE) fabric.

SUE is the only target communication backend. Compatibility with upstream
SimAI is not a requirement, and changing upstream core behavior is allowed.

## Non-Negotiable Boundary

The final runtime, build, configuration, examples, and product documentation
must not retain cluster networking, alternative network simulators,
two-sided/matching transport machinery, physical transport libraries,
multi-backend selection, or empirical link-ratio timing. Native PUT and GET
must never be implemented as a facade over a legacy communication call.

Historical research and migration documents may name rejected technologies so
that decisions remain auditable. Production source must remain clean.

## Workspace and Repository Roles

Workspace root: `C:\Users\S15\btp`

| Path | Role |
|---|---|
| `simai_scaleup/` | Fresh official clone and active clean implementation. |
| `simai_fresh/` | Earlier prototype; reference only and must not yet be deleted. |
| `SimAI/` | Older experimental implementation; reference only. |
| `SUE-Sim/` | Standalone research/reference simulator. |
| `astra-sim-sue/` | Earlier ASTRA/SUE experiment; reference only. |

`simai_fresh` may be removed only at CP-15, after the replacement passes clean
builds, end-to-end tests, SUE correctness tests, the zero-contamination audit,
and a unique-artifact review.

## Clean Baseline

- Official source: `https://github.com/aliyun/SimAI.git`
- Clone directory: `C:\Users\S15\btp\simai_scaleup`
- Baseline branch: `master`
- Baseline commit: `a57d765c5478f319c5f5ab6fdc53f9cc7f461743`
- The upstream ASTRA subtree remains unchanged.
- No upstream submodule has been initialized.

Recorded gitlinks:

| Gitlink | Commit | Policy |
|---|---|---|
| `SimAI-CLEM` | `4a91f8b655224c0b1d1c300aa09c7058f4da2707` | Do not initialize; remove from final product. |
| `SimCCL` | `fd7cd57d16f9bd42e3ccb70911c977e18ec294b9` | No runtime dependency; a future optional importer only if justified. |
| `aicb` | `23eec3c48ca2d2d93dd888a4c7b22ab4421e782f` | Deferred optional workload importer. |
| `ns-3-alibabacloud` | `3e0c7c1bfbbe9f77890ddcf5e5b9c79fc6dd7437` | Do not initialize or build. |

## Important Git State

At the pause point, the fresh clone has not been committed. Git reports these
new project paths as untracked:

- `CMakeLists.txt`
- `native/`
- `readme_divi/`

This is expected. Build artifacts live under the ignored
`build-optimized/` directory. A future agent must not assume that untracked
means disposable: these files contain all completed clean-room work.

## Completed Checkpoints

### CP-00: Official Baseline

- Created `simai_scaleup` from the official repository.
- Recorded commit and submodule state.
- Kept the earlier prototype separate and untouched.

### CP-01: Research and Architecture Preflight

Reviewed:

- the user-supplied ASTRA-sim 3.0 paper;
- Broadcom Scale-Up Ethernet Framework RM104;
- Broadcom public Tomahawk Ultra material;
- the MSCCL++ paper, repository documentation, DSL, primitive semantics, and
  execution-plan responsibilities;
- public UALink overview material without accepting its evaluation license.

Important conclusions:

- The authoritative SUE framework uses generic commands/responses and exposes
  PUT, GET, and atomic services.
- SUE transactions have up to 256 bytes of service data; compatible
  transactions may be packed into a PDU up to 4096 bytes.
- One SUE PDU maps to one Ethernet packet.
- The public framework permits up to four virtual channels, weighted
  round-robin selection, strict or unordered operation, multiple
  ports/instances, acknowledgements, negative acknowledgements, CRC, and
  Go-Back-N reliability.
- Link-level retry and congestion/backpressure are distinct mechanisms.
- Arrival acknowledgement is not proof of final accelerator-memory placement.
- The initial project profile is full SUE, one switch hop, initially one
  instance/port, strict ordering, and credit-based backpressure. Detailed
  reliability and collective offload remain later work.
- Earlier prototype constants such as a guessed flit size, header size, rail
  formula, credit formula, retry latency, and very small switch latency are
  not authoritative and must not be copied.
- Modern NCCL releases expose limited one-sided host operations, but the
  baseline SimAI/SimCCL model does not provide the native semantic IR required
  here.
- MSCCL++ is useful semantic reference material, especially for operation
  issue, local completion, remote visibility, signal/wait ordering, buffer
  chunks, ownership, dependency DAGs, and fusion. It is not a mandatory
  runtime dependency.

The detailed sources, facts, assumptions, and citations are recorded in
[`MASTER_README.md`](MASTER_README.md).

The authoritative explanation of the default native collective planner,
MockNCCL reuse boundary, optional MSCCL++ importer, replacement of `Sys.cc`,
and future fine-grained GPU/kernel pipeline is
[`COLLECTIVE_PLANNER_AND_GPU_PIPELINE.md`](COLLECTIVE_PLANNER_AND_GPU_PIPELINE.md).

### CP-02: Exhaustive Dependency Audit

Deliverable: [`FILE_AUDIT.md`](FILE_AUDIT.md)

- Audited all 138 files under the upstream ASTRA frontend, system, topology,
  collective, and workload source directories.
- Every file has an individual disposition and reason.
- Result: 21 `REWRITE`, 45 `REFERENCE`, 72 `REMOVE`, and zero production
  source files approved to compile unchanged.
- Audited adjacent build/input files and repository-level integration points.
- Proved that the old network boundary propagates through `Sys`, streams,
  collectives, workload execution, memory timing, and reports.
- Decided to build a new native source tree in the fresh official repository,
  using upstream algorithms only as reviewed reference material.

### CP-03: Native Semantic Contracts

Deliverable: [`NATIVE_CONTRACTS.md`](NATIVE_CONTRACTS.md)

Implemented under `native/`:

- integer-nanosecond time and strong typed identities;
- owned buffer descriptors with endpoint, buffer, byte range, and region;
- workload events and stable ordered parallel groups;
- collective plans containing explicit dependency DAGs;
- PUT, GET, SIGNAL, WAIT, and REDUCE micro-operations;
- separate thread-block order and dependency readiness;
- issue, local completion, remote arrival, remote visibility, and finalization
  stages;
- SUE command/response transaction values with endpoint, service, role,
  virtual channel, ordering domain, byte count, data presence, operands, and
  response status;
- a deterministic event queue ordered by integer time then insertion order;
- report timing and conservation-counter values;
- validation for identities, ownership, DAGs, program order, stages,
  timestamps, transaction semantics, and report bounds.

### CP-04: Minimal End-to-End Vertical Slice

Deliverable: [`CP04_VERTICAL_SLICE.md`](CP04_VERTICAL_SLICE.md)

- parses one strict schema-version-1 two-layer workload;
- creates exactly eight endpoints and one tensor-parallel group;
- lowers one AllReduce to 14 PUT and 7 REDUCE operations;
- executes the dependency DAG through the deterministic event queue and one
  shared SUE link;
- produces exact per-layer timing, exposed communication, transferred bytes,
  end-to-end timing, and conservation counters;
- includes a CLI, golden workload, golden report, serialization equality
  test, deterministic replay test, and invalid-input test;
- proves 580 ns communication and 730 ns end-to-end for the golden input.

### CP-05: Bounded Transactions, PDUs, and Wire Serialization

Deliverable: [`CP05_PDU_SERIALIZATION.md`](CP05_PDU_SERIALIZATION.md)

- enforces at most 256 service-data bytes per transaction;
- packs compatible transaction segments into PDUs carrying at most 4096 bytes;
- models configurable per-PDU wire overhead as an explicit assumption;
- serializes wire bytes and retains distinct operation, transaction, PDU
  payload, and wire byte counters;
- maps one PDU to one packet counter;
- passes single-PDU and multi-PDU analytical equality tests while preserving
  the 730 ns golden timing when configured overhead is zero.

### CP-06: Deterministic Multi-Rail Fabric

Deliverable: [`CP06_MULTI_RAIL.md`](CP06_MULTI_RAIL.md)

- adds one to four independent rail serialization resources;
- maps each ordered source/destination/domain flow deterministically to one
  rail;
- records and validates per-rail PDU, payload, wire-byte, and busy-time totals;
- passes a two-rail analytical test with a 5/9 PDU split, 200/360 ns busy
  times, 380 ns communication, and 530 ns end-to-end after the CP-14 flow-hash
  correction;
- preserves the one-rail 730 ns golden regression.

### CP-07: Finite Switch Queues

Deliverable: [`CP07_FINITE_SWITCH_QUEUES.md`](CP07_FINITE_SWITCH_QUEUES.md)

- adds optional finite per-destination egress queues;
- arbitrates PDUs at explicit simulated ingress-arrival events;
- tracks admission, processing, service, departure, delivery, occupancy,
  backpressure, delay, busy time, and byte conservation;
- passes capacity-one and equal-time arbitration tests;
- passes the integrated 710 ns analytical case with six blocked admissions and
  400 ns total root-egress admission delay.

### CP-08: Explicit Credit Flow Control

Deliverable: [`CP08_CREDIT_FLOW_CONTROL.md`](CP08_CREDIT_FLOW_CONTROL.md)

- adds optional finite credits per switch egress and timed returns;
- checks available-plus-outstanding conservation at every transition;
- records exact consumption, return, maximum outstanding depth, starvation
  events, and starvation delay;
- passes the one-credit 45 ns starvation proof and integrated 740 ns case;
- preserves all credit-disabled regressions.

### CP-09: Bounded Retry and Exactly-Once Reassembly

Deliverable: [`CP09_RETRY_REASSEMBLY.md`](CP09_RETRY_REASSEMBLY.md)

- assigns deterministic packet sequence identity;
- adds bounded deterministic error injection and replay latency;
- separates logical PDU/wire counters from physical attempts/transmitted bytes;
- rejects duplicate PDUs at the switch and during destination reassembly;
- proves one replay, bounded exhaustion, and exactly-once completion.

Explicitly not implemented yet:

- workload parsing;
- collective lowering;
- event callback execution;
- link serialization or transaction execution;
- PDU/packet layout;
- rails, switch queues, arbitration, credits, retry, or reassembly;
- GPU execution resources;
- CLI or end-to-end simulator.

## CP-03 Verification Evidence

Environment:

- GCC 6.3 is available.
- CMake is not installed in the current environment.
- The native code is portable C++14 and includes normal CMake metadata.

The verified direct build is equivalent to:

```powershell
g++ -std=c++14 -Wall -Wextra -Wpedantic -Werror `
  -Isimai_scaleup/native/include `
  simai_scaleup/native/src/collective.cpp `
  simai_scaleup/native/src/completion.cpp `
  simai_scaleup/native/src/event_queue.cpp `
  simai_scaleup/native/src/sue_transaction.cpp `
  simai_scaleup/native/tests/contracts_test.cpp `
  -o simai_scaleup/build-optimized/cp03-verification/simai_contracts_test.exe
```

Last verified result:

```text
All native contract tests passed
```

Additional evidence:

- warning-as-error build succeeded from a new output directory;
- compiler dependency output contained only standard and native contract
  headers;
- the native forbidden-source scan returned zero matches;
- whitespace scan returned zero matches;
- `git diff --quiet -- astra-sim-alibabacloud` returned exit code zero;
- the verified binary currently exists under the ignored build directory.

## Current Architecture Boundary

```text
future workload parser
  -> WorkloadEvent / ParallelGroup
  -> future workload executor
  -> CollectivePlan
  -> future collective planner
  -> MicroOp DAG
  -> future SUE backend
  -> SueTransaction / CompletionTimeline
  -> DeterministicEventQueue
  -> SimulationReport
```

Dependencies must point downward only. Workload execution must not directly
create fabric packets, collective planning must not manipulate switch queues,
and SUE execution must not depend on an external collective or network
runtime.

## Completion Point: CP-15

CP-00 through CP-15 passed their complete gates. The unique artifacts in
`simai_fresh` were reviewed, their useful lessons and rejection reasons were
recorded in
[`CP15_PROTOTYPE_RETIREMENT.md`](CP15_PROTOTYPE_RETIREMENT.md), and the final
strict build, regression, CLI, sweep, dependency, whitespace, source, symbol,
and linkage gates passed. The prototype directory was then removed on
2026-10-06.

## Separate MockNCCL One-Sided Investigation

While implementation remains paused, a read-only architecture investigation
was completed for adding one-sided PUT/GET to the existing SimAI MockNCCL.
The durable result is
[`MOCKNCCL_ONE_SIDED_RESEARCH.md`](MOCKNCCL_ONE_SIDED_RESEARCH.md).
The beginner-first explanation is
[`MOCKNCCL_ONE_SIDED_BEGINNER_GUIDE.md`](MOCKNCCL_ONE_SIDED_BEGINNER_GUIDE.md).

Its central finding is that the existing `SingleFlow`/`NcclFlowModel` path is
not expressive enough for correct one-sided execution. A future implementation
would require a native operation IR, registered memory ranges, distinct
PUT/GET state machines, typed completion dependencies, semaphore generations,
memory ordering, barriers/fences, resource contention, and typed backend RMA
events. It recommends native generators plus an optional MSCCL++ importer;
classic MSCCL should not define the core architecture.

This investigation did not modify native simulator code and did not begin
CP-04.

## Continuation Rule

Treat CP-15 as the completed baseline. Do not restore the retired prototype or
repeat the migration. New functionality must start from the native contracts
as a newly documented checkpoint with focused tests, the end-to-end
regression, zero-contamination audit, and master-ledger entry.
