# CP-13 Zero-Contamination and Reachability Audit

## Status

Complete on 2026-10-06.

## Structural Result

The new repository now contains only:

```text
CMakeLists.txt
LICENSE
README.md
native/
readme_divi/
tools/
```

Ignored build output may exist locally but is not a source or runtime
dependency.

The audit removed 388 tracked legacy files and gitlinks (47938 lines),
including the old multi-backend runtime, analytical and physical frontends,
old workload engine, legacy collective executor, unrelated inference package,
old examples/configuration, old product documentation, build wrappers, and
dormant submodule entries. These removals are recoverable from the recorded
upstream commit `a57d765c5478f319c5f5ab6fdc53f9cc7f461743`.

`simai_fresh` was outside this repository and remained preserved until its
CP-15 unique-artifact review and final gate completed; it has now been removed.

## Production Surface

The top-level README now documents only the native scale-up simulator. The
top-level CMake file adds only `native/`. Every one of the 16 native `.cpp`
files appears in `native/CMakeLists.txt`. There is no submodule manifest and
no runtime backend selector.

Historical research and migration records remain under `readme_divi/`; they
are not build inputs or product configuration and may name the systems that
were evaluated and removed.

## Repeatable Audit

[`tools/audit.ps1`](../tools/audit.ps1) verifies:

- all known legacy product paths are absent;
- forbidden backend vocabulary is absent from production code, CMake, and the
  top-level README;
- every native implementation source is reachable from CMake;
- optional binary symbol and linkage scans contain no forbidden backend.

The compiled dependency scan contains only standard-library and native
headers. Binary linkage is limited to the normal MinGW runtime libraries:

```text
KERNEL32.dll
msvcrt.dll
libgcc_s_dw2-1.dll
libstdc++-6.dll
```

## Verification

- strict warning-as-error contract/runtime/CLI builds passed;
- all CP-03 through CP-12 tests passed;
- native CLI golden and AICB CLI integration passed;
- production vocabulary scan passed;
- build reachability check passed;
- compiler dependency isolation passed;
- binary symbol and linkage scans passed;
- whitespace check passed.

## Completion

CP-14 added reproducible research-validation sweeps and exact metric
definitions. CP-15 subsequently compared all unique prototype artifacts,
repeated the final gates, and retired `simai_fresh`.
