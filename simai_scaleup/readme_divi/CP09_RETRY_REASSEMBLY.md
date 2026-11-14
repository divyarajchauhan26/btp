# CP-09 Bounded Retry and Exactly-Once Reassembly

## Status

Complete on 2026-10-06.

## Model

- Every PDU has a nonzero deterministic packet sequence identity.
- Configuration can corrupt a chosen PDU for a chosen number of attempts.
- Corrupted attempts occupy the selected rail and count as transmitted bytes,
  but do not reach the switch or destination.
- Replay begins after error detection at arrival plus explicit replay latency.
- Retry count is bounded; exhaustion throws a deterministic runtime error.
- Logical PDU/wire bytes and physical packet-attempt/transmitted bytes are
  reported separately.
- Switch egress queues reject repeated PDU identities.
- Destination reassembly accepts only expected unique PDU identities and
  releases one operation completion after the full unique set arrives.

## Analytical Evidence

The error test corrupts PDU 1 once with 7 ns replay latency. It produces:

```text
logical PDUs = 14
physical packet attempts = 15
retries = 1
logical wire bytes = 14000
physically transmitted wire bytes = 15000
rail busy time = 600 ns
finalized operations = completions = 21
end-to-end = 787 ns
```

A two-failure case with only one allowed retry terminates explicitly. Separate
tests prove switch duplicate rejection and reassembly duplicate rejection.

## Verification

All warning-as-error builds, CP-04 through CP-09 tests, CLI golden comparison,
dependency isolation, forbidden-source scan, ASTRA integrity, and whitespace
checks passed.

## Next

CP-09A replaces the vertical-slice planner entry point with a general
`CollectiveRequest`/planner interface, explicit provenance, and the first
non-baseline ring AllReduce generator.
