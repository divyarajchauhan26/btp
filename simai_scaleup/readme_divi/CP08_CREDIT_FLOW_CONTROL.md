# CP-08 Explicit Credit Flow Control

## Status

Complete on 2026-10-06.

## Model

- Credits are configured independently per switch egress.
- One admitted PDU consumes one credit.
- A credit remains outstanding until switch departure plus the configured
  return latency.
- A PDU arriving with no available credit waits for the earliest credit return.
- Queue-capacity blocking and credit starvation are recorded independently.
- Every state transition checks `initial = available + outstanding`.
- Finalization drains timed returns and requires the initial balance to be
  completely restored.

Credit flow control is optional. Zero configured credits preserves the CP-07
queue-only behavior and all earlier golden results.

## Analytical Test

With one credit, capacity two, 40 ns serialization, and 5 ns credit-return
latency:

```text
PDU 1: admit 0, depart 40, credit returns 45, destination arrival 50
PDU 2: arrive 0, admit 45, depart 85, credit returns 90, arrival 95
starvation events = 1
starvation time = 45 ns
credits consumed = returned = 2
initial credits = final available credits = 1
maximum outstanding credits = 1
```

The integrated two-rail workload consumes and returns all 14 credits, records
six root-egress credit-starvation events and 505 ns total starvation, and
finishes in the hand-calculated 740 ns.

## Verification

Contract/runtime warning-as-error builds, all CP-04 through CP-08 tests, CLI
golden comparison, forbidden-source scan, legacy ASTRA integrity, and
whitespace checks passed.

## Deferred

CP-09 adds deterministic error injection, bounded replay, duplicate rejection,
and exactly-once reassembly/completion. Credits currently protect switch
admission; protocol reliability remains absent until that checkpoint.
