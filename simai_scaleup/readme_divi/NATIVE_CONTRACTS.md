# CP-03 Native Semantic Contracts

## Status and Scope

This document freezes the semantic boundary for the first native runtime.
CP-03 defines value types, validation rules, ownership, completion stages, and
deterministic event ordering. It does not implement a collective algorithm,
packet format, fabric queue, switch, flow control, retry, or workload parser.

The contracts are independent of upstream ASTRA, SimCCL, AICB, and every
legacy network frontend.

## Time and Identity

- Simulation time is an unsigned integer number of nanoseconds (`SimTimeNs`).
- Floating-point time is forbidden at contract boundaries.
- Device, buffer, workload-event, group, plan, operation, transaction,
  semaphore, ordering-domain, and event identifiers are distinct strong
  types; accidental comparison between identity domains must not compile.
- Identifier value zero is invalid. Construction is permitted for simple
  serialization, but every public validator rejects zero before execution.
- User-visible identifiers are stable. The event queue adds its own monotonic
  insertion sequence, which is not a model identity.

## Buffer and Address Ownership

A `BufferSlice` is a descriptor, not allocated storage. It contains:

- owning accelerator endpoint;
- buffer identity;
- byte offset;
- byte length;
- memory region (`DeviceLocal` for the first implementation).

The slice is valid only when its identifiers and length are nonzero and
`offset + length` does not overflow. Contract objects own copies of
descriptors; no raw pointers or borrowed buffer lifetimes cross an interface.

For a memory operation, “local” and “remote” are relative to the issuing
device:

- PUT: source is owned by the issuer; destination is owned by another member
  of the same parallel group.
- GET: source is owned by another member; destination is owned by the issuer.
- REDUCE: source and destination must be explicit; in-place aliasing is legal
  only when both descriptors are exactly equal.
- SIGNAL and WAIT use a typed semaphore identity and carry no buffer slice.

The simulator models addresses symbolically during CP-03/CP-04. Address
translation, protection keys, memory registration, and actual host pointers
are outside this contract.

## Workload Events

A `WorkloadEvent` is either:

- `Compute`: an endpoint-local nonzero duration; or
- `Collective`: a reference to one `CollectivePlan`.

Every workload event has an identity, layer index, and a set of predecessor
workload-event identities. Dependencies are unique and may not reference the
event itself. Whole-workload DAG validation is intentionally owned by the
future workload parser/executor rather than by a single event value.

## Parallel Groups

A `ParallelGroup` has a typed group identity and an ordered list of device
identities. The list must:

- contain at least two endpoints for a collective;
- contain no zero or duplicate endpoint;
- define the stable logical-rank order used by collective planners.

The first runtime accepts one scale-up domain only. A group is a set of
accelerator endpoints, not a host/node/cluster hierarchy.

## Collective Plans and Micro-Operations

A `CollectivePlan` binds one collective request to one parallel group and an
explicit dependency DAG of `MicroOp` values. The initial type system names
AllReduce, ReduceScatter, AllGather, AllToAll, and Broadcast, but CP-04 will
implement only AllReduce.

A `MicroOp` is one of PUT, GET, SIGNAL, WAIT, or REDUCE. Every operation has:

- operation identity;
- issuing endpoint;
- thread-block identity;
- monotonically increasing order within that `(issuer, thread block)`;
- explicit predecessor operation identities;
- memory ordering (`Relaxed` or `Ordered`);
- a required final-release stage.

Operand rules are validated by operation kind. Dependencies must be unique,
must exist in the same plan, and may not self-reference. Plan validation also
rejects duplicate operation identities, issuers outside the group,
non-increasing thread-block order, and dependency cycles.

Thread-block order and dependency edges are separate constraints. Order
captures program order; dependencies capture readiness. A planner must not
silently infer a cross-thread-block dependency from numeric order.

## Signal, Wait, and Visibility

SIGNAL carries `(semaphore, value)` and produces visibility at its target
endpoint. WAIT carries the same typed pair and becomes runnable only when that
visible value is sufficient. Neither is translated into a compatibility data
transfer.

`Relaxed` allows operations without dependency edges to overlap.
`Ordered` requires the execution layer to preserve program order for the same
issuer, thread block, and ordering domain. CP-03 validates representation but
does not implement the memory-order engine.

## Completion Semantics

The following stages are distinct and ordered:

1. `Issued`: accepted from the micro-operation scheduler.
2. `LocalComplete`: the issuer may release the local execution resource.
3. `RemoteArrived`: required bytes or control reached the destination SUE.
4. `RemoteVisible`: the destination memory/semaphore effect is observable.
5. `Finalized`: the operation's requested completion condition is satisfied
   and dependent workload work may be released.

A `CompletionTimeline` rejects duplicate, skipped, or time-reversing stages.
All five stages are recorded even when adjacent stages have equal timestamps.
Arrival never implies memory visibility. Finalization never precedes the
operation's configured release stage.

## SUE Transactions

A `SueTransaction` is a fabric-facing command or response produced from a
micro-operation. It contains:

- transaction and parent operation identity;
- source and destination endpoint;
- service (`Put`, `Get`, `Atomic`, or `Control`);
- message role (`Command` or `Response`);
- traffic class/virtual channel in the inclusive range 0 through 3;
- ordering-domain identity;
- logical byte length and whether the message carries data;
- source/destination slices when required;
- response status for response messages.

Contract validation enforces first-slice rules:

- PUT command carries data; its success response does not.
- GET command carries no data; its success response carries requested data.
- ATOMIC/CONTROL details remain representable but their detailed wire and
  execution rules are deferred.

Transaction byte length is logical service data, not on-wire size. PDU
packing, headers, checksums, packet sequence numbers, and acknowledgement
state are deliberately absent until CP-05/CP-09 so specification facts are
not confused with guessed wire formats.

## Deterministic Event Boundary

`DeterministicEventQueue` owns scheduled event values. Ordering is strictly:

1. lower simulation timestamp;
2. lower monotonic insertion sequence for equal timestamps.

Scheduling into the past is rejected. Popping advances `now`; equal-time
events do not advance it. The queue stores no callbacks or raw pointers, so
subsystem behavior remains outside the ordering primitive.

The random seed belongs to future `SimulationConfig` provenance. CP-03 has no
random behavior.

## Reports and Conservation Counters

`SimulationReport` is a value object containing per-layer timing records and
integer conservation counters. The counter schema includes issued and
finalized operations, logical transaction bytes, transaction count, and
reserved future fields for PDUs, packets, credits, and retries. Counters not
yet modeled must remain zero; they must not be estimated.

The report validator checks monotonic layer timestamps, nonnegative derived
durations by construction, and `finalized_operations <= issued_operations`.

## Error Policy

- Contract constructors/factories produce plain values.
- `validate()` returns a list of stable machine-readable error codes plus
  human-readable detail.
- Execution boundaries call `require_valid()` and receive
  `std::invalid_argument` on invalid input.
- Unsupported operations must fail explicitly. No fallback backend or timing
  approximation is permitted.

## CP-03 Non-Goals

- no workload-file schema or parser;
- no collective lowering implementation;
- no transaction execution or link serialization;
- no PDU/packet structure;
- no switch, rail, queue, credit, retry, or reassembly behavior;
- no GPU compute-resource model;
- no external workload or collective dependency;
- no command-line executable.

These remain later checkpoint responsibilities.
