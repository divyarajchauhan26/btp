# CP-05 Bounded Transactions, PDUs, and Wire Serialization

## Status

Complete on 2026-10-05.

## Behavior Added

- Every SUE service transaction is limited to at most 256 bytes.
- Compatible PUT transactions are packed into a PDU carrying at most 4096
  bytes of service payload.
- A PDU maps one-to-one to the packet counter.
- Configured per-PDU wire overhead is counted and serialized separately from
  service payload. It is an experiment assumption, not a claimed fixed SUE
  header size.
- Buffer offsets are preserved across transaction segmentation.
- Operation bytes, transaction bytes, PDU payload bytes, and wire bytes have
  separate conservation counters.
- The link serializes PDU wire bytes, not the unsplit micro-operation size.

No fixed 128-byte SUE flit was introduced. GPU request size, service
transaction size, PDU payload, and on-wire size remain distinct concepts.

## Analytical Tests

For a 1000-byte PUT with 256-byte transactions, 4096-byte PDU capacity, and
32 bytes of configured wire overhead:

```text
transactions = ceil(1000 / 256) = 4
PDUs = 1
wire bytes = 1000 + 32 = 1032
serialization = ceil(1032 * 8 / 200) = 42 ns
arrival = 42 + 10 ns propagation = 52 ns
```

For a 9000-byte PUT under the same packetization:

```text
transactions = ceil(9000 / 256) = 36
PDUs = ceil(9000 / 4096) = 3
payload bytes = 9000
wire bytes = 9000 + 3 * 32 = 9096
```

Both equalities are asserted by tests, along with every transaction and PDU
boundary.

## Golden Regression

The golden workload uses zero configured PDU overhead so its CP-04 timing
remains unchanged at 580 ns communication and 730 ns end-to-end. Its more
accurate CP-05 counters are:

```text
logical PUT operations = 14
service transactions = 56
PDUs = packets = 14
logical operation bytes = 14000
logical transaction bytes = 14000
PDU payload bytes = 14000
wire bytes = 14000
```

## Verification

```text
All native contract tests passed
All native vertical-slice and PDU tests passed
CLI golden comparison passed
Native dependency isolation: passed
Forbidden production-source scan: zero matches
Legacy ASTRA subtree diff: zero
Whitespace check: passed
```

The build used GCC 6.3, C++14, and warnings as errors because CMake remains
unavailable in the environment.

## Deferred

CP-05 still uses one physical link resource. Rail selection, switch resources,
finite queues, credits, retry, reassembly, and GPU contention remain deferred.
