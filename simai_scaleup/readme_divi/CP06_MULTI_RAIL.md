# CP-06 Deterministic Multi-Rail Fabric

## Status

Complete on 2026-10-06. The saved warning-as-error binaries and golden CLI
comparison were re-run after the session interruption and passed.

## Behavior Added

- One to four independent physical serialization rails are configurable.
- Every PDU is assigned to exactly one rail.
- Selection is a deterministic hash of source endpoint, destination endpoint,
  and ordering domain.
- All PDUs in the same ordered flow select the same rail; the implementation
  does not stripe one ordered flow opportunistically.
- Each rail owns independent serialization availability and exact PDU,
  payload-byte, wire-byte, and busy-time counters.
- The final report validates that the sum of all rail counters equals the
  global fabric counters.

The selection policy is a documented simulator policy, not a claim about a
particular device's proprietary load balancer.

## Analytical Evidence

The two-rail test uses the existing 14-PDU, 1000-byte-per-PDU AllReduce. After
CP-14 replaced the low-bit-colliding additive selector with the current mixed
flow hash, the deterministic mapping assigns five PDUs to rail 0 and nine to
rail 1. At 200 Gb/s each PDU occupies a rail for 40 ns:

```text
rail 0: 5 PDUs, 5000 bytes, 200 ns busy
rail 1: 9 PDUs, 9000 bytes, 360 ns busy
dependency-aware communication: 380 ns
end-to-end: 100 + 380 + 50 = 530 ns
```

The one-rail golden remains 580 ns communication and 730 ns end-to-end, with
14 PDUs, 14000 wire bytes, and 560 ns of rail busy time.

## Verification

```text
All native contract tests passed
All native runtime tests passed
CLI golden comparison passed
Native dependency isolation: passed
Forbidden production-source scan: zero matches
Legacy ASTRA subtree diff: zero
Whitespace check: passed
```

## Deferred

Rails currently connect endpoints directly. CP-07 adds the shared switch and
finite egress queues. Credits, retry, reassembly, GPU contention, additional
collectives, and external workload import remain deferred.
