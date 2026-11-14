# CP-12 Multi-Layer and Bounded AICB Integration

## Status

Complete on 2026-10-06 for the explicitly supported single-domain AICB
subset described below.

## General Native Trace

The native workload is no longer restricted to two layers or one collective.
It accepts one to one million named, ordered layers and any number of the four
supported collectives. Each layer names how its compute starts:

- `after_previous_finish`: wait for the previous layer's own completion;
- `after_previous_compute`: begin when the previous layer's compute ends,
  allowing compute to overlap prior communication.

Collective launches use an explicit global communication fence. Therefore the
current model permits compute/communication overlap but deliberately does not
interleave two collective schedules. This keeps ordering reproducible until a
future global multi-kernel event scheduler is introduced.

The four-layer analytical test is:

```text
ring AllReduce -> overlapping compute -> overlapping compute + AllGather
-> serial compute
```

Its serial form ends at 1240 ns. Its overlap form ends at 990 ns, hiding
exactly 250 ns. It executes 224 micro-operations and moves 21000 bytes.

## Multi-Collective Identity Fix

Transaction and PDU identities previously restarted inside every collective.
That was safe only for the original one-collective trace. They now come from a
simulation-wide allocator. A persistent switch test executes multiple
collectives and proves that every PDU is accepted with zero duplicate IDs.

## AICB Adapter

`aicb_importer.cpp` reads the upstream whitespace-separated format:

```text
header
row count
name dependency
forward-compute forward-collective forward-bytes
input-gradient-compute input-gradient-collective input-gradient-bytes
weight-gradient-compute weight-gradient-collective weight-gradient-bytes
weight-update-compute
```

Each row expands into named forward, input-gradient, and weight-gradient
native phases. Weight-update time is added to the weight-gradient compute
phase. A required `nanoseconds-per-tick` parameter prevents an undocumented
time-unit assumption.

The CLI path is:

```text
simai_scaleup --aicb <aicb-file> <native-base-config> <nanoseconds-per-tick>
```

The base config supplies SUE, GPU, seed, and algorithm policy values. The
imported operations still pass through the native planner, GPU executor, and
SUE backend.

## Honest Support Boundary

The adapter accepts transformer traces only when:

- `all_gpus` is 2 through 1024;
- `model_parallel_NPU_group == all_gpus`;
- `ep == 1` and `pp == 1`;
- row dependency is `-1`;
- collective tokens are unsuffixed `ALLREDUCE`, `REDUCESCATTER`, `ALLGATHER`,
  `ALLTOALL`, or `NONE`.

It rejects EP, DP/EP, PP, composite, and multi-domain traces because the
current `WorkloadSpec` has one scale-up group. Silently mapping those operations
onto the TP group would produce plausible-looking but false results. This is a
deliberate correctness boundary, not a statement that full AICB is solved.

## Evidence

The six-phase AICB fixture executes from CLI to report in 1682 ns with four
schedule records, 392 micro-operations, and 35000 network bytes. Layer names
survive into the report. Unit tests cover phase expansion, update-time folding,
end-to-end execution, deterministic output, and unsupported-group rejection.

Strict builds, all prior regressions, native CLI golden comparison, AICB CLI
execution, dependency isolation, forbidden-source scan, ASTRA-subtree
integrity, and whitespace checks passed.

## Next

CP-13 performs the repository-wide zero-contamination and reachability audit.
Broader TP/EP/DP group construction is a separately visible extension and must
not be hidden inside the completed single-group adapter.
