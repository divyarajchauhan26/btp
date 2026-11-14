# CP-14 Reproducible Research Validation

## Status

Complete on 2026-10-06 for analytical microbenchmarks and explicitly labeled
synthetic workload-shape proxies.

## Reproduction

Run:

```text
powershell -ExecutionPolicy Bypass -File tools/research_sweep.ps1 \
  -Binary <path-to-simai_scaleup.exe>
```

The script writes generated inputs, `summary.csv`, per-trace reports, and
`trace_summary.csv` below ignored `results/cp14/`. It verifies formulas and
trends while running; a violated invariant fails the script.

## Metric Definitions

- `end_to_end_ns`: maximum finish timestamp across all workload layers.
- `exposed_communication_ns`: layer finish minus that layer's compute-complete
  timestamp; it includes ordered-communication waiting and communication.
- `logical_operation_bytes`: sum of native PUT source lengths, counted once
  regardless of retry.
- `transmitted_wire_bytes`: bytes physically serialized across all attempts,
  including retry attempts and configured PDU overhead.
- `ideal_serialization_ns`: ceiling of transmitted bits divided by aggregate
  configured rail bandwidth. This is a lower bound, not a prediction, because
  dependencies and flow-to-rail imbalance can increase time.
- `maximum_rail_busy_ns`: largest accumulated serialization time on any one
  rail.
- `backpressure_events`: PDU admissions delayed by finite queue/credit state.
- `credit_starvation_events`: admissions specifically delayed by unavailable
  credits.
- `retries`: replay attempts beyond the first physical transmission.

Every run checks the ring formula
`logical bytes = 2 * (endpoints - 1) * payload` and requires exposed
communication time to meet both the aggregate ideal-serialization and busiest
rail lower bounds.

## Sweep Results

Representative exact results at 200 Gb/s per rail:

| Sweep | Values | End-to-end result |
|---|---|---|
| Payload bytes, 8 endpoints, 1 rail | 256 / 1024 / 4096 | 384 / 832 / 2512 ns |
| Endpoints, 4096 bytes, 1 rail | 2 / 4 / 8 / 16 | 488 / 1144 / 2512 / 5440 ns |
| Rails, 8 endpoints, 4096 bytes | 1 / 2 / 4 | 2512 / 1630 / 1042 ns |
| Queue capacity, baseline algorithm | 1 / 4 | 2322 / 2322 ns |
| Credit return latency | 0 / 10 / 100 ns | 2322 / 2382 / 2922 ns |
| Replay latency | 0 / 10 / 100 ns | 2543 / 2553 / 2643 ns |

Observed mechanisms:

- payload and endpoint scaling are monotonic and follow exact ring byte counts;
- added rails reduce the critical serialization path after flow hashing;
- capacity one records six backpressure events, while capacity four absorbs
  the burst with zero events; completion time is unchanged because the same
  root-egress service remains the bottleneck;
- every credit case exercises six starvation events, and longer return latency
  increases end-to-end time;
- one injected replay adds exactly one retry and one PDU's physical wire bytes;
  replay latency maps one-for-one into end-to-end latency in this dependency
  path.

## Flow-Hash Finding and Correction

The first sweep found that the original additive hash mapped every ring
neighbor flow to rail 0 with two rails. Four rails used only rails 0 and 2.
That result was a parity collision, not a fabric conclusion. CP-14 replaced it
with a deterministic 64-bit mixed hash over source, destination, and ordering
domain. The 1000-byte ring regression now uses both rails (42/70 PDUs) and
drops from 720 ns to 510 ns. All earlier exact multi-rail/switch/credit
expectations were recalculated and reverified.

## Workload-Shape Proxies

Two source-controlled synthetic traces exercise combinations common to AI
training, but they are not presented as measured model traces:

| Trace | Purpose | End-to-end | Network bytes | PDUs |
|---|---|---:|---:|---:|
| `llm_tp_proxy` | AllReduce + AllGather + ReduceScatter with overlap | 2786 ns | 114688 | 224 |
| `moe_ep_proxy` | AllToAll exchange + AllReduce with overlap | 1804 ns | 86016 | 168 |

The MoE name describes its communication shape only. The current single-group
workload cannot claim real expert-group mapping. Real model conclusions require
measured compute inputs and the future TP/EP/DP group extension documented by
CP-12's support boundary.

## Verification

The sweep assertions, proxy byte checks, strict test suite, original golden,
zero-contamination source/reachability audit, dependency isolation, binary
symbol/linkage scan, and whitespace check all pass.

## Completion

CP-15 reviewed every unique prototype artifact, preserved the required design
knowledge, passed the final verification gate, and retired `simai_fresh`.
