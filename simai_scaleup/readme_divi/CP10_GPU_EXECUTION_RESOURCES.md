# CP-10 Finite GPU Execution Resources

## Status

Complete on 2026-10-06 for the deliberately scoped first GPU model.

## Implemented Model

Ready collective micro-operations no longer have to enter SUE immediately.
When enabled, each micro-operation is lowered into one communication kernel,
one workgroup, and a configurable number of wavefronts. A deterministic GPU
executor then models:

- a finite number of compute units per endpoint;
- finite resident workgroup slots per compute unit;
- wavefront issue width and issue latency;
- FIFO waiting when every resident slot is occupied;
- slot retention until the micro-operation's configured completion;
- byte-dependent local reduction execution time;
- per-endpoint dispatch, wavefront, residency, stall, issue, and reduction
  counters.

This is a resource model, not a complete GPU ISA simulator. One native
micro-operation currently becomes one synthetic workgroup. Real kernel shape,
HBM/NoC traffic, instruction mixes, and profiler/trace import remain future
work. The disabled default preserves the earlier network-only baselines.

## Analytical Unit Case

Configuration:

```text
1 CU, 1 resident slot
4 wavefronts per workgroup
issue width 2
3 ns per issue round
100 reduction bytes/ns
```

The first workgroup takes two issue rounds, or 6 ns. A second workgroup ready
at 0 ns waits until the first releases its only slot at 20 ns, producing one
20 ns resource stall. A 1000-byte reduction takes another 10 ns after its
6 ns issue stage. Exact counters verify 1 maximum resident workgroup, 8 issued
wavefronts for the two contending workgroups, and 12 ns aggregate issue work.

## End-to-End Evidence

The same configuration applied to the baseline eight-endpoint AllReduce
produces:

```text
end-to-end time = 854 ns
collective exposed time = 704 ns
root workgroups = 14
root wavefronts = 56
root maximum resident workgroups = 1
root resource stalls = 6
root aggregate resource stall time = 1176 ns
root issue work = 84 ns
root reduction work = 70 ns
issued = finalized = completions = 21
```

The aggregate stall time is intentionally a sum across waiting workgroups and
may exceed wall-clock time.

## Files

- `native/include/simai/runtime/gpu_execution.hpp`
- `native/src/gpu_execution.cpp`
- `native/include/simai/runtime/workload.hpp`
- `native/src/workload_parser.cpp`
- `native/include/simai/contracts/report.hpp`
- `native/src/simulator.cpp`
- `native/src/report_writer.cpp`
- `native/tests/data/gpu_contended.workload`
- `native/tests/vertical_slice_test.cpp`

## Verification

Strict warning-as-error builds, exact synthetic scheduling tests, integrated
contention tests, deterministic replay, CP-04 through CP-09A regressions, CLI
golden comparison, dependency isolation, forbidden-source scan, ASTRA-subtree
integrity, and whitespace checks passed.

## Known Boundary

The executor currently holds a workgroup slot until the micro-operation's
release-stage completion. This is a conservative and explicit assumption for
the current one-operation workgroups. Multi-operation workgroups and an
independent outstanding-memory-request pool must be introduced before claiming
hardware-level occupancy fidelity.

## Next

CP-11 expands the common planner and native IR to ReduceScatter, AllGather,
and AllToAll while retaining explicit failure for unsupported operations.
