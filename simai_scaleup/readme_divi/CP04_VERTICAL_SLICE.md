# CP-04 Minimal End-to-End Vertical Slice

## Status

Complete on 2026-10-05. The checkpoint passes a warning-as-error C++14 build,
contract tests, a deterministic golden end-to-end test, an independent CLI
golden comparison, dependency isolation, the forbidden-source scan, and the
legacy-subtree integrity check.

## Scope Implemented

The vertical slice accepts a strict `key=value` workload format with:

- schema version 1;
- exactly eight accelerator endpoints;
- one tensor-parallel group containing those endpoints in rank order;
- exactly two ordered layers with deterministic integer-nanosecond compute;
- exactly one AllReduce;
- one SUE link bandwidth and propagation-delay configuration.

The AllReduce planner uses an intentionally simple gather-reduce-broadcast
algorithm. Seven peers PUT their full contribution to endpoint 1, endpoint 1
performs seven ordered local reductions, and endpoint 1 PUTs the result to the
seven peers. This produces an explicit validated DAG of 21 operations: 14
native PUTs and 7 REDUCE operations. It is an internal CP-04 algorithm, not an
imported schedule and not a performance claim.

The executor releases operations only after every explicit predecessor has
finalized. Data-bearing PUT commands reserve one deterministic shared SUE link.
The link records issue, serialization start, local serialization completion,
remote arrival, remote visibility, and finalization. PDU structure, packet
overhead, rails, switches, finite queues, credits, retry, and GPU contention
remain absent, and their report counters remain exactly zero.

## Input Grammar

Scalar keys occur once. A layer row has four comma-separated fields:

```text
schema_version=1
deterministic_seed=20261005
endpoint_count=8
link_bandwidth_gbps=200
propagation_delay_ns=10
layer=0,100,allreduce,1000
layer=1,50,none,0
```

Unknown keys, duplicate scalar keys, unsupported collectives, malformed
integers, wrong layer order, any endpoint count other than eight, any layer
count other than two, and any collective count other than one are rejected.

## Analytical Timing Proof

At 200 Gb/s, the numeric bandwidth is 200 bits/ns. A 1,000-byte logical
transfer therefore serializes in:

```text
ceil(1000 bytes * 8 bits/byte / 200 bits/ns) = 40 ns
```

Each gather/broadcast phase contains seven serialized transfers followed by a
10 ns final propagation delay:

```text
phase time = 7 * 40 ns + 10 ns = 290 ns
AllReduce time = 2 * 290 ns = 580 ns
end-to-end = 100 ns compute + 580 ns communication + 50 ns compute = 730 ns
```

The simulator and golden report produce those exact values. The 14 transfers
also conserve exactly `14 * 1000 = 14000` logical payload bytes.

## Files Added

- `native/include/simai/runtime/workload.hpp`
- `native/include/simai/runtime/allreduce_planner.hpp`
- `native/include/simai/runtime/simple_sue_link.hpp`
- `native/include/simai/runtime/simulator.hpp`
- `native/include/simai/runtime/report_writer.hpp`
- matching implementations under `native/src/`
- `native/app/simai_scaleup.cpp`
- `native/tests/vertical_slice_test.cpp`
- `native/tests/data/two_layer_golden.workload`
- `native/tests/data/two_layer_golden.report`

`LayerTiming` was extended with exact compute duration, exposed communication,
and per-layer transferred-payload fields.

## Verified Results

```text
All native contract tests passed
All CP-04 vertical-slice tests passed
CLI golden comparison passed
Forbidden production-source scan: zero matches
Legacy ASTRA subtree diff: zero
Whitespace check: passed
```

CMake metadata includes the runtime, tests, and executable, but CMake is still
not installed in this environment. Verification used the equivalent direct
GCC 6.3 commands with `-Wall -Wextra -Wpedantic -Werror`.

## Next Checkpoint

CP-05 must replace the payload-only link reservation with explicit bounded SUE
transactions and PDU/wire serialization. It must preserve the CP-04 golden
path while clearly separating logical payload bytes from on-wire bytes and
must not invent an undocumented fixed 128-byte SUE flit.
