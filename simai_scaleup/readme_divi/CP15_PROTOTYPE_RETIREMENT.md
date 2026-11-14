# CP-15 Prototype Retirement

## Status

Complete on 2026-10-06 after an artifact-by-artifact review, a clean rebuild,
the full verification suite, and removal of `C:\Users\S15\btp\simai_fresh`.

## Why the Prototype Was Reviewed

`simai_fresh` was an experimental edit of the upstream SimAI tree. It mixed a
new SUE prototype with the old runtime and contained 259 uncommitted changes at
upstream commit `a57d765c5478f319c5f5ab6fdc53f9cc7f461743`. The useful ideas
had to be identified before deleting the folder, but copying the implementation
would have reintroduced the dependencies and semantics removed by CP-13.

## Artifact Disposition

| Prototype artifact | Finding | Final disposition |
|---|---|---|
| Six `readme_divi` planning/research files | Useful motivation and migration history, but claims were mixed with unfinished implementation assumptions. | Superseded by `MASTER_README.md`, `context.md`, CP-01 through CP-14 records, and this retirement record. |
| `SueTypes.hh` | Collected flit, link, credit, and statistics concepts, but used fixed asserted constants and retained two-sided/legacy terminology. | Concepts are represented by typed native contracts, workload configuration, report counters, and validation tests. No source copied. |
| `CreditManager.hh` | Per-link credits and FIFO stalls were useful concepts. The prototype had no end-to-end conservation invariant and counted requeues as new stalls. | Superseded by finite egress credits with exact consume/return/starvation counters and conservation checks. |
| `MultiRailSprayer.hh` | Contained fixed-size slicing and per-flit round-robin striping. | Superseded by bounded transactions, configurable PDUs, deterministic flow hashing, per-rail serialization, and CP-14 balance tests. |
| `LinkLevelRetry.hh` | Contained seeded random corruption and a replay deque, but silently evicted the oldest entry even if it was not acknowledged. | Superseded by deterministic fault injection, bounded retries, explicit exhaustion failure, physical-attempt accounting, and exactly-once completion tests. |
| `ReassemblyBuffer.hh` | Counted arrivals rather than unique PDU identities, so a duplicate could falsely complete a message. | Superseded by expected/received PDU sets with duplicate rejection and exact completion tests. |
| `SueSwitch.hh` | Serialized egress traffic, but never drained occupancy, never enforced its declared capacity, and therefore did not implement a finite queue. | Superseded by finite queues, capacity enforcement, deterministic arbitration, backpressure, credit control, and conservation tests. |
| `MscclParser.hh/.cc` | Despite its name, `load_kernel_json` ignored its path and generated a fixed two-threadblock schedule. Its AllReduce was one neighbor exchange rather than a full collective; ReduceScatter called the same builder; compute-delay parameters were unused. | Rejected. Native parameterized planners now generate validated AllReduce, ReduceScatter, AllGather, and AllToAll DAGs. A real external-plan adapter remains optional future work and must parse and validate an actual schema. |
| `SueNetworkApi.hh/.cc` | Preserved the old abstract network interface. Two-sided send/receive calls were relabeled as one-sided operations with synthesized addresses. GET injected only a response and omitted the request path. | Rejected. The native runtime carries explicit PUT/GET/SIGNAL/WAIT semantics in its contracts and has no legacy backend adapter. |
| `SueSession.hh/.cc` | Joined the prototype modules to the old event/API layer. It completed sends at receiver reassembly, matched receives only by tag/source/destination, and could not guarantee data-before-signal ordering across rails. | Rejected. The native deterministic event pipeline uses explicit operation dependencies, global identities, completion stages, and tested reassembly. |
| Prototype executable and build files | Locally generated, tied to the old tree, and not reproducible evidence for the clean runtime. | Rejected. CP-15 rebuilds every native target directly from source. |
| Copied MockNCCL headers | Retained the exact legacy collective dependency the clean architecture removes. | Rejected. Native planners own collective selection and lowering. |

## Knowledge Preserved

The review retained the useful architectural questions, not unsupported
answers:

- PDU sizing and overhead must be configurable rather than asserted as a
  universal SUE constant.
- Credit flow control needs finite capacity, delayed returns, starvation
  metrics, and a conservation invariant.
- Multi-rail traffic needs deterministic, measured balance; a convenient hash
  is not assumed to be balanced.
- Retry needs bounded state and explicit failure, while reassembly must count
  unique PDU identities.
- A SIGNAL may release a waiter only after all ordered data dependencies are
  complete.
- A GET model must include both request and response causality.
- An MSCCL++ adapter is useful only if it parses a documented plan format,
  maps ranks/buffers to workload groups, validates dependencies, and then
  lowers into the same native micro-operation contract.

## Recoverability

The original upstream repository remains reproducible from commit
`a57d765c5478f319c5f5ab6fdc53f9cc7f461743`. The experimental SUE files were
untracked and therefore were not recoverable from that commit after deletion.
They were intentionally not archived because their useful design information
is recorded above and their implementation failed the clean architecture and
semantic-correctness gates.

## Final Gate

Before retirement, CP-15 requires all of the following to pass from current
source:

- strict C++14 warning-as-error contract, runtime, and CLI builds;
- all contract and end-to-end tests;
- native and bounded-AICB CLI checks;
- CP-14 reproducible sweeps;
- source/reachability and binary symbol/linkage audit;
- dependency isolation and whitespace validation.

Only `simai_fresh` is removed. Other sibling repositories and research inputs
remain untouched.
