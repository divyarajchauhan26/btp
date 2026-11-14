# Synthetic Fine Trace Guide

## Purpose

The synthetic fine trace is the completed deterministic input path for
describing memory behavior inside one GPU kernel without requiring a hardware
profiler. Every memory operation explicitly provides:

1. an absolute timestamp in nanoseconds from kernel admission;
2. a `load` or `store` operation;
3. a synthetic buffer-relative memory address;
4. `local` or `remote` placement plus the owning GPU; and
5. the data size in bytes.

The trace also carries the minimum structural information needed by the
runtime: issuing device, kernel kind, workgroup, wavefront, buffer identity,
residency, and a register-buffer identity.

This path is synthetic and deterministic. It does not claim that its records
were extracted from a GPU binary or measured on H200 hardware.

## File Format

The format uses `key=value` lines, supports `#` comments, and accepts schema
versions 1 and 2. Version 1 remains executable without modification. Version 2
adds required provenance and timestamp-semantics metadata while retaining the
same nine-field record representation.

```text
schema_version=1
kernel_name=synthetic_gpu0_copy
device=1
kernel_kind=communication
timestamp_unit=ns
workgroup_residency_slots=1
register_buffer=7001

# timestamp|workgroup|wavefront|operation|address|locality|owner|buffer|size
record=10|1|1|load|0x0000|local|1|101|64
record=20|1|1|store|0x1000|remote|2|201|64
record=30|1|1|load|0x1040|remote|2|201|128
record=45|1|1|store|0x0040|local|1|101|128
```

The runnable example is
`native/tests/data/synthetic_two_gpu.trace`.

## Metadata Fields

| Field | Meaning |
| --- | --- |
| `schema_version` | Must be `1`. |
| `kernel_name` | Nonempty report and binding name. |
| `device` | One-based issuing GPU ID. |
| `kernel_kind` | `compute` or `communication`. |
| `timestamp_unit` | Must be `ns`. |
| `workgroup_residency_slots` | Finite CU residency consumed by each workgroup. |
| `register_buffer` | Nonzero synthetic register-file buffer identity. |

Schema version 2 additionally requires:

| Field | Meaning |
| --- | --- |
| `trace_producer` | Tool or workflow that generated the trace. |
| `launch_id` | Stable identity used to correlate this trace with a kernel launch. |
| `hardware_id` | Hardware/configuration identity under which the trace applies. |
| `timestamp_semantics` | Currently exactly `earliest_issue`. |

Version 2 also requires one or more repeatable buffer declarations:

```text
# buffer=buffer-id|owner-device|allocation-size-bytes
buffer=101|1|4096
buffer=201|2|8192
```

Every record buffer must be declared, its owner must match the declaration,
and `address + size` must remain within the declared allocation. Version-2
record addresses are therefore allocation-relative byte offsets, not unchecked
virtual addresses.

The runnable version-2 example is `native/tests/data/synthetic_v2.trace`.

## Record Fields

The nine pipe-separated fields are:

| Position | Field | Meaning |
| --- | --- | --- |
| 1 | timestamp | Earliest issue offset in nanoseconds from kernel admission. |
| 2 | workgroup | Nonzero workgroup/threadblock identity. |
| 3 | wavefront | Nonzero wavefront identity local to the workgroup. |
| 4 | operation | Exactly `load` or `store`. |
| 5 | address | Unsigned decimal or `0x` buffer-relative byte address. |
| 6 | locality | Exactly `local` or `remote`. |
| 7 | owner | One-based GPU that owns the referenced buffer. |
| 8 | buffer | Nonzero semantic buffer identity. |
| 9 | size | Nonzero number of bytes. |

Locality is validated, not trusted blindly:

```text
local  requires owner == issuing device
remote requires owner != issuing device
```

The address and size must form a non-overflowing 64-bit range. The hardware
workload must contain both the issuing device and every owner device.

## Timestamp Semantics

Each record is lowered to:

```text
TraceWaitUntil(timestamp_ns)
Load or Store(address, locality, owner, buffer, size)
```

`TraceWaitUntil` uses kernel launch/admission as time zero. Every workgroup in
the kernel retains that same origin even if finite CU residency makes the
workgroup wait before dispatch. The memory
operation cannot issue before the recorded timestamp. Resource contention,
issue width, earlier operations, memory backpressure, or transport delay may
make it issue later. The simulator never moves an event backward in time to
force exact replay.

Memory requests may remain in flight while later independent operations
advance. Explicit `Waitcnt` remains the mechanism for dependency stalls in
non-synthetic programs. The synthetic schema intentionally contains only the
five agreed operation fields and therefore does not invent synchronization.

## Data-Size Semantics

The existing instruction generator splits a record larger than the configured
cache-line size into multiple Load or Store instructions. With the current CLI
configuration, the cache line is 64 bytes:

```text
size 64  -> one memory instruction
size 128 -> two memory instructions
```

Local instructions enter the analytical L1/L2/HBM path. Remote instructions
become semantic remote requests and then SUE PDUs over the native deterministic
scale-up transport.

## Direct Run

From the repository build directory:

```text
build/native/simai_scaleup --gpu-model fine --synthetic-trace \
  native/tests/data/synthetic_two_gpu.trace \
  native/tests/data/fine_two_gpu.workload
```

## Inspect Without Simulation

Validate either trace version and print its provenance, structure, operation
counts, byte totals, and timestamp span without requiring a hardware workload:

```text
build/native/simai_scaleup --inspect-synthetic-trace \
  native/tests/data/synthetic_v2.trace
```

Inspection performs the same strict parsing and lowering validation as normal
execution. Unsupported timestamp semantics fail explicitly rather than being
silently interpreted as earliest-issue replay.

The checked direct-run example produces:

```text
issued_instructions=10
completed_instructions=10
trace_wait_instructions=4
local_memory_requests=3
local_memory_bytes=192
remote_requests=3
remote_memory_bytes=192
remote_command_pdus=5
remote_response_pdus=2
```

Ten instructions means four timestamp waits plus six cache-line Load/Store
instructions. The two 128-byte records each become two memory instructions.
The byte counters prove that the original record sizes are conserved separately
from SUE payload and wire overhead.

## Chakra-Driven Run

The versioned Chakra execution manifest supports:

```text
synthetic=name|zero-based-rank:source-node-id|trace-path
```

Example:

```text
synthetic=rank0_pre|0:0|synthetic_two_gpu.trace
```

The trace is parsed and lowered, then attached to exactly one executable Chakra
node. The binding rejects a trace whose issuing device conflicts with the
Chakra node device. Normal Chakra dependencies still control kernel release;
synthetic timestamps begin when that kernel is admitted.

See `native/tests/data/fine_chakra_manifest.example`.

## Standalone Multi-GPU Run

Synthetic scale-up studies do not require placeholder Chakra files. A separate
execution manifest can launch multiple trace kernels and express dependencies:

```text
schema_version=1

# kernel=name|trace-path|comma-separated-dependency-names-or--
kernel=gpu0|synthetic_two_gpu.trace|-
kernel=gpu1|synthetic_gpu1.trace|-
```

Run it with:

```text
build/native/simai_scaleup --gpu-model fine --synthetic-manifest \
  native/tests/data/synthetic_two_gpu.manifest \
  native/tests/data/fine_two_gpu.workload
```

Independent entries are concurrent graph roots. A dependency field such as
`gpu0` delays the dependent kernel until `gpu0` completes. Names, paths, and
dependencies are validated; cycles, missing names, duplicates, and
self-dependencies are rejected.

The checked two-GPU manifest reports six timestamp waits, four local requests,
four remote requests, 256 local bytes, and 256 remote bytes.

## Complete Execution Flow

```text
synthetic trace
  -> strict schema and locality validation
  -> records grouped by workgroup and wavefront
  -> timestamp ordering with file-order tie breaking
  -> TraceWaitUntil plus Load/Store operations
  -> validated FineKernelDefinition
  -> direct launch, standalone multi-GPU manifest, or Chakra-node binding
  -> finite CU scheduling and wavefront issue
  -> cache-line instruction expansion
  -> local analytical memory or remote request lowering
  -> native SUE packetization and scale-up transport
  -> completion, retirement, counters, and report
```

## Honest Boundary

The agreed synthetic-trace project target is implemented end to end. The
following are separate future research extensions, not hidden requirements of
this format:

- hardware-derived traces and correlation IDs;
- ALU, atomic, fence, barrier, semaphore, and `Waitcnt` records;
- cache-hit/miss observations and explicit GPU-internal NoC/HBM channels;
- calibrated H200 timing accuracy;
- persistent asynchronous SUE-Sim/ns-3 execution.

The current path is suitable for deterministic functional studies of
timestamped local and remote Load/Store traffic. It must not be described as
binary-accurate or hardware-calibrated.
