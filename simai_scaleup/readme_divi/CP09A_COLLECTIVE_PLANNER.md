# CP-09A General Collective Planner

## Status

Complete on 2026-10-06.

## Architecture

The runtime now creates a `CollectiveRequest` and sends it through
`NativeCollectivePlanner`. The request states the collective kind, ordered
parallel-group membership, payload, reduction, and algorithm policy. Planner
capabilities are checked before lowering. The result contains both a validated
native `CollectivePlan` and stable schedule provenance.

This is the default path; neither the legacy system runtime nor its collective
flow executor is in the native build. A future schedule importer must produce
the same native plan instead of bypassing this interface.

## Implemented Policies

- `gather_reduce_broadcast`: retained as the known 21-operation baseline.
- `ring`: reduce-scatter followed by all-gather, represented as explicit PUT
  and REDUCE operations with buffer slices and dependencies.
- `auto`: currently selects ring only when the payload divides evenly across
  the group; otherwise it selects the baseline. This is an explicit temporary
  policy, not a performance claim.

Only sum AllReduce is supported at this checkpoint. Other collective kinds,
reduction types, unavailable backend capabilities, and indivisible forced-ring
payloads fail explicitly.

## Analytical Evidence

For eight endpoints and a 1000-byte payload, the ring has seven
reduce-scatter steps and seven all-gather steps:

```text
reduce-scatter PUTs = 8 * 7 = 56
local REDUCE operations = 8 * 7 = 56
all-gather PUTs = 8 * 7 = 56
total micro-operations = 168
network PUTs = 112
bytes per PUT = 1000 / 8 = 125
network payload bytes = 112 * 125 = 14000
```

The deterministic one-rail execution reports 168 exactly-once completions,
112 PDUs, 14000 logical bytes, a ring provenance record, and 720 ns
end-to-end time. The original baseline remains byte-identical to its 730 ns
golden report.

## Files

- `native/include/simai/runtime/collective_planner.hpp`
- `native/src/collective_planner.cpp`
- `native/include/simai/runtime/workload.hpp`
- `native/src/workload_parser.cpp`
- `native/src/simulator.cpp`
- `native/include/simai/contracts/report.hpp`
- `native/src/report_writer.cpp`
- `native/tests/vertical_slice_test.cpp`
- `native/tests/data/ring_allreduce.workload`

## Verification

Warning-as-error contract, runtime, and CLI builds passed. Planner selection,
provenance, ring operation/byte counts, invalid requests, capability rejection,
ring end-to-end execution, deterministic replay, CP-04 through CP-09
regressions, CLI golden comparison, dependency isolation, forbidden-source
scan, ASTRA-subtree integrity, and whitespace checks all passed.

## Next

CP-10 inserts a deliberately small GPU execution-resource layer between ready
micro-operations and SUE issue. Its first gate will prove finite resource
contention with synthetic kernels before any real trace importer is added.
