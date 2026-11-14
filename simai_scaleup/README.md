# SimAI Scale-Up

SimAI Scale-Up is an experimental C++ simulator for collective communication
inside one accelerator scale-up domain. It accepts a layer-oriented workload,
turns each collective into a dependency graph of micro-operations, packetizes
communication into SUE transactions/PDUs, and produces deterministic timing and
traffic counters.

The fine MSCCL++ path additionally lowers GPU/threadblock programs into
workgroups, wavefronts, Load/Store instructions, Waitcnt, reductions, and
semaphore operations before generating semantic local or remote requests.

The runtime supports two schedule sources:

- native collective planners for ring/baseline algorithms; and
- an MSCCL++-style JSON interpreter for explicit `put`, `get`, `signal`, `wait`,
  and `reduce` operations.

## Quick Start

```text
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the verified four-XPU MSCCL++ toy plan:

```text
build/native/simai_scaleup --mscclpp \
  native/tests/data/mscclpp_four_gpu_allgather.json \
  native/tests/data/four_gpu_allgather.workload
```

Run the fine-grained two-XPU MSCCL++ vertical path:

```text
build/native/simai_scaleup --gpu-model fine --mscclpp \
  native/tests/data/mscclpp_two_gpu.json \
  native/tests/data/fine_two_gpu.workload
```

This command reports the analytical L1/L2/HBM assumptions it used, CU
dispatches, instruction conservation, remote request/PDU counts, rail traffic,
and end-to-end simulated time.

Run the built-in collective planner:

```text
build/native/simai_scaleup native/tests/data/ring_allreduce.workload
```

Run the bounded AICB importer:

```text
build/native/simai_scaleup --aicb \
  native/tests/data/aicb_scaleup_subset.txt \
  native/tests/data/aicb_base.workload 1
```

Import and validate an uncompressed Chakra ET protobuf trace for one device:

```text
build/native/simai_scaleup --chakra path/to/chakra.0.et 1
```

The final argument is the simulator's one-based device ID for that per-rank ET
file. This mode prints the validated neutral kernel graph, including original
Chakra IDs, node kinds, data dependencies, durations, and known communication
or tensor sizes. Missing trace attributes remain explicitly unknown.

Run a multi-rank Chakra graph through the fine GPU runtime:

```text
build/native/simai_scaleup --gpu-model fine --chakra-fine \
  path/to/execution.manifest \
  native/tests/data/fine_two_gpu.workload
```

The execution manifest lists one ET file per rank and binds nodes, identified
by `zero-based-rank:source-node-id`, to either an abstract compute description
or one grouped MSCCL++ collective plan. Paths are resolved relative to the
manifest. See
[`native/tests/data/fine_chakra_manifest.example`](native/tests/data/fine_chakra_manifest.example).

Run the completed synthetic fine-trace path directly:

```text
build/native/simai_scaleup --gpu-model fine --synthetic-trace \
  native/tests/data/synthetic_two_gpu.trace \
  native/tests/data/fine_two_gpu.workload
```

Each synthetic record specifies an absolute nanosecond timestamp from kernel
admission, workgroup/wavefront, Load or Store, buffer-relative address,
local/remote placement, owner GPU, buffer ID, and byte count. See
[`readme_divi_2.0/SYNTHETIC_TRACE_GUIDE.md`](readme_divi_2.0/SYNTHETIC_TRACE_GUIDE.md).

Run multiple synthetic GPU kernels concurrently without Chakra fixtures:

```text
build/native/simai_scaleup --gpu-model fine --synthetic-manifest \
  native/tests/data/synthetic_two_gpu.manifest \
  native/tests/data/fine_two_gpu.workload
```

## Documentation

The ASTRA-sim 3.0 comparison and complete fine-graining implementation roadmap
is [`readme_divi_2.0/ASTRA_3_FINE_GRAINING_ROADMAP.md`](readme_divi_2.0/ASTRA_3_FINE_GRAINING_ROADMAP.md).

The complete build, run, architecture, micro-operation, verification, and
roadmap guide is
[`readme_divi/END_TO_END_MSCCLPP_SUE_GUIDE.md`](readme_divi/END_TO_END_MSCCLPP_SUE_GUIDE.md).

The implementation checkpoint ledger is
[`readme_divi/MASTER_README.md`](readme_divi/MASTER_README.md), and the
production-integration plan is
[`readme_divi/PRODUCTION_MSCCLPP_SUE_IMPLEMENTATION_PLAN.md`](readme_divi/PRODUCTION_MSCCLPP_SUE_IMPLEMENTATION_PLAN.md).

## Current Boundary

The coarse and fine CLIs both use the native deterministic SUE fabric. The
fine MSCCL++ CLI supports one collective layer, while `--chakra-fine` supports
multi-rank ET import, explicit node binding, grouped MSCCL++ collective
lowering, dependency-aware kernel release, and synthetic Load/Store kernel
bindings. Duration-backed abstract Reduce remains available as an explicit
fallback when no detailed trace is supplied. The optional SUE-Sim/ns-3 adapter still
runs through a per-PDU synchronous adapter, and the CLI does not construct one
persistent asynchronous ns-3 session. Do not describe the current simulation
as binary-accurate, cache-validated, or a persistent ns-3 end-to-end run.

The native runtime also has a separate initial dynamic microtrace importer for the
observed block/thread/address TSV format. With an explicit instruction legend,
block geometry, access width, and allocation ownership map, it coalesces lane
records into warp memory spans and lowers them to local or remote fine
Load/Store operations. A tested C++ binding can attach the resulting kernel to
one Chakra executable node. Hardware-trace selection and correlation remain
future work; unlike that captured-trace path, the synthetic format is wired
through both direct and `--chakra-fine` execution and its nanosecond timestamps
are enforced as earliest-issue constraints.

## License

Apache License 2.0. See [`LICENSE`](LICENSE).
