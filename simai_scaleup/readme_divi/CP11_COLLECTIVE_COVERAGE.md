# CP-11 Collective Coverage

## Status

Complete on 2026-10-06.

## Supported Native Collectives

All supported collectives now enter through `CollectiveRequest`, lower into
the same buffer-aware micro-operation DAG, pass through the optional GPU
resource executor, and use the same SUE fabric.

| Collective | Native algorithm | Required reduction |
|---|---|---|
| AllReduce | ring or gather-reduce-broadcast baseline | sum |
| ReduceScatter | ring | sum |
| AllGather | ring | none |
| AllToAll | direct exchange | none |

`auto` selects direct exchange for AllToAll, ring for divisible
ReduceScatter/AllGather, and ring for divisible AllReduce. An indivisible
AllReduce may use the baseline; other indivisible ring requests fail. Invalid
algorithm/collective combinations fail explicitly.

The workload parser accepts these four collective names and now supports a
single scale-up domain containing 2 through 1024 endpoints instead of fixing
the vertical slice at eight.

## Byte Semantics

`payload_bytes` is the logical tensor extent per rank. Ring algorithms split
it into one chunk per rank, so divisibility is currently required.

For eight endpoints and 1000 bytes:

```text
chunk = 1000 / 8 = 125 bytes
ReduceScatter network bytes = 8 * 7 * 125 = 7000
AllGather network bytes = 8 * 7 * 125 = 7000
ring AllReduce network bytes = 7000 + 7000 = 14000
direct AllToAll network bytes = 8 * 7 * 125 = 7000
```

AllToAll omits each rank's local self-copy from fabric traffic.

## Verification Matrix

- Tiny: two-endpoint planner counts for every collective and a full
  two-endpoint AllGather execution (172 ns, two operations, 256 wire bytes).
- Medium: eight-endpoint 1000-byte end-to-end executions with exact operation,
  PDU, byte, timing, and provenance checks.
- Contention: two rails, finite capacity-one switch queues, one credit per
  egress, and delayed credit return for ReduceScatter, AllGather, and AllToAll;
  every PDU is admitted/departed and actual backpressure is required. Existing
  AllReduce switch/credit tests remain active.
- Negative: unsupported collective, reduction, capability, algorithm pairing,
  and indivisible ring cases all terminate explicitly.

The eight-endpoint results without GPU-resource timing are:

| Collective | Operations | PDUs | Network bytes | End-to-end |
|---|---:|---:|---:|---:|
| ReduceScatter | 112 | 56 | 7000 | 440 ns |
| AllGather | 56 | 56 | 7000 | 440 ns |
| AllToAll | 56 | 56 | 7000 | 440 ns |

## Files

- `native/src/collective_planner.cpp`
- `native/include/simai/runtime/workload.hpp`
- `native/src/workload_parser.cpp`
- `native/src/simulator.cpp`
- `native/tests/vertical_slice_test.cpp`
- collective fixtures under `native/tests/data/`

## Full Gate

Strict builds, all contract/runtime tests, the original CLI golden comparison,
deterministic executions, dependency isolation, forbidden-source scan,
ASTRA-subtree integrity, and whitespace checks passed.

## Next

CP-12 replaces the deliberately narrow two-layer input restriction with a
general layer trace and adds explicit compute/communication overlap semantics.
Real external trace import must remain an adapter into the typed workload
events; it must not bypass the planner, GPU executor, or SUE fabric.
