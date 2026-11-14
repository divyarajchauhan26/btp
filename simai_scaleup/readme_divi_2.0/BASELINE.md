# Phase 0 Baseline

## Purpose

This document freezes the legacy/coarse behavior that must remain available
while the fine-grained GPU model is built.

## Supported baseline paths

```text
simai_scaleup <workload>
simai_scaleup --aicb <trace> <base-workload> <nanoseconds-per-tick>
simai_scaleup --mscclpp <plan> <workload>
```

These commands implicitly select the coarse model. The equivalent explicit
form is:

```text
simai_scaleup --gpu-model coarse ...
```

`--gpu-model fine` deliberately returns an error until the first complete fine
vertical slice exists. It must never silently execute the coarse path.

## Frozen four-GPU MSCCL++ result

Fixture:

```text
native/tests/data/four_gpu_allgather.workload
native/tests/data/mscclpp_four_gpu_allgather.json
```

Golden report:

```text
native/tests/data/four_gpu_mscclpp_golden.report
```

Expected conserved values:

```text
issued operations:      12
finalized operations:   12
completions:             12
logical payload bytes:  12288
PDUs:                    12
end-to-end model time:   602 ns
```

The timing is a coarse-model regression value, not a hardware measurement.

## Generated artifact inventory

Generated executables and logs were found in source directories, including:

```text
simai_scaleup/contracts_test.exe
simai_scaleup/simai_scaleup.exe
simai_scaleup/vertical_slice_test.exe
clang_probe.exe
msccl_instructions.log
sue_execution.log
```

They are now ignored by `.gitignore`. Existing files are not deleted as part
of Phase 0 because cleanup should be isolated from behavioral changes.

## Phase 0 regression tests

- Contract tests.
- Existing vertical-slice tests.
- Four-GPU MSCCL++ CLI run.
- Exact comparison of implicit and explicit coarse outputs.
- Exact comparison against the frozen four-GPU report.
- A negative test proving fine mode is unavailable rather than silently
  falling back.
