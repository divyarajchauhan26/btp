# CP-07 Finite Switch Queues

## Status

Complete on 2026-10-06.

## Model

- The switch is an optional explicit stage so prior direct-fabric regressions
  remain available.
- Each destination has one finite egress queue and independent egress service.
- Capacity counts resident PDUs, including the PDU currently serializing.
- A slot is released at actual switch departure, not when service begins.
- A full queue delays admission until the earliest resident departure; this
  delay is recorded as backpressure.
- Switch processing latency, egress serialization, and destination propagation
  are separate timestamps.
- Ingress arrivals are scheduled on the deterministic event queue, so PDUs
  from independent rails arbitrate by simulated arrival time and stable
  insertion order rather than host call order.

## Focused Analytical Test

Two 1000-byte PDUs arrive together at a capacity-one egress. With 5 ns switch
latency, 40 ns serialization, and 10 ns propagation:

```text
PDU 1: admit 0, service 5, depart 45, destination arrival 55
PDU 2: admit 45, service 50, depart 90, destination arrival 100
backpressure events = 1
total admission delay = 45 ns
maximum occupancy = 1
final occupancy = 0
busy time = 80 ns
```

The arbitration trace preserves PDU 1 then PDU 2 for the equal-time arrivals.

## Integrated Analytical Test

The two-rail AllReduce with a capacity-one switch queue produces:

```text
root egress accepted/departed PDUs = 7 / 7
root maximum/final occupancy = 1 / 0
root backpressure events = 6
root total admission delay = 400 ns
root busy time = 280 ns
communication = 560 ns
end-to-end = 710 ns
```

All eight egress reports together conserve the global 14 PDUs and 14000
payload/wire bytes.

## Verification

Warning-as-error contract/runtime/CLI builds, all focused and regression tests,
CLI golden comparison, dependency isolation, forbidden-source scan, ASTRA
integrity, and whitespace checks passed.

## Deferred

Queue capacity currently creates admission backpressure directly. CP-08 adds
explicit credit ownership, consumption, return timing, and starvation tests.
Retry, reassembly, GPU contention, more collectives, and external workload
import remain deferred.
