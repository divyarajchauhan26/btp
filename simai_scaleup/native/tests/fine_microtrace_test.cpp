#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "simai/runtime/fine_microtrace.hpp"

namespace {

using namespace simai::contracts;
using namespace simai::runtime;

int failures = 0;

void expect(bool condition, const std::string& name) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}

void expect_invalid(
    const std::function<void()>& action, const std::string& name) {
  try {
    action();
    expect(false, name);
  } catch (const std::invalid_argument&) {
    expect(true, name);
  }
}

std::string sample_trace() {
  std::ostringstream output;
  output << "probe\tblock_x\tblock_y\tblock_z\tthread_x\tthread_y\t"
            "thread_z\tclock_delta\taddress\tinstr_type\n";
  for (std::uint64_t lane = 0; lane < 32; ++lane) {
    output << "0\t0\t0\t0\t" << lane
           << "\t0\t0\t977\t" << 0x10000000ULL + lane * 4
           << "\t2\n";
    output << "0\t0\t0\t0\t" << lane
           << "\t0\t0\t1315\t" << 0x10000200ULL + lane * 4
           << "\t1\n";
  }
  return output.str();
}

FineMicrotraceImportConfig config(DeviceId allocation_owner) {
  return FineMicrotraceImportConfig{
      KernelId(7),
      DeviceId(1),
      allocation_owner == DeviceId(1)
          ? FineKernelKind::Compute
          : FineKernelKind::Communication,
      32,
      1,
      1,
      32,
      1,
      4,
      BufferId(99),
      FineMicrotraceTimestampSemantics::KernelElapsedCycles,
      {{1, FineMicrotraceInstructionKind::Store},
       {2, FineMicrotraceInstructionKind::Load}},
      {{0x10000000ULL,
        0x1000,
        allocation_owner,
        BufferId(5),
        4}}};
}

void test_lane_records_coalesce_into_warp_accesses() {
  std::istringstream input(sample_trace());
  const FineMicrotraceImportResult result =
      import_fine_microtrace_tsv(input, config(DeviceId(1)));
  expect(result.records.size() == 64, "all lane records are preserved");
  expect(result.accesses.size() == 2, "lane records coalesce into two accesses");
  expect(
      result.timestamp_semantics ==
          FineMicrotraceTimestampSemantics::KernelElapsedCycles,
      "timestamp semantics are preserved with the imported trace");
  expect(
      result.accesses[0].clock == 977 &&
          result.accesses[0].active_lanes == 32 &&
          result.accesses[0].memory.offset_bytes == 0 &&
          result.accesses[0].memory.length_bytes == 128,
      "first warp access preserves time and 128-byte span");
  expect(
      result.accesses[1].clock == 1315 &&
          result.accesses[1].memory.offset_bytes == 0x200 &&
          result.accesses[1].memory.length_bytes == 128,
      "second warp access preserves time and offset");
  expect(
      result.kernel.kind == FineKernelKind::Compute &&
          result.kernel.workgroups.size() == 1 &&
          result.kernel.workgroups[0].wavefronts.size() == 1,
      "trace lowers to one compute workgroup and wavefront");
  const std::vector<FineGpuOperation>& operations =
      result.kernel.workgroups[0].wavefronts[0].operations;
  expect(
      operations.size() == 2 &&
          operations[0].kind == FineGpuOperationKind::Load &&
          operations[1].kind == FineGpuOperationKind::Store,
      "explicit type legend controls operation lowering");
}

void test_allocation_owner_classifies_remote_access() {
  std::istringstream input(sample_trace());
  const FineMicrotraceImportResult result =
      import_fine_microtrace_tsv(input, config(DeviceId(2)));
  expect(
      result.kernel.kind == FineKernelKind::Communication,
      "caller explicitly classifies the kernel");
  expect(
      result.accesses[0].memory.owner == DeviceId(2) &&
          result.accesses[0].memory.space == FineMemorySpace::RemoteHbm,
      "allocation ownership classifies remote HBM");
}

void test_sparse_lanes_form_separate_memory_spans() {
  std::ostringstream trace;
  trace << "probe\tblock_x\tblock_y\tblock_z\tthread_x\tthread_y\t"
           "thread_z\tclock_delta\taddress\tinstr_type\n"
        << "3\t0\t0\t0\t0\t0\t0\t50\t268435456\t2\n"
        << "3\t0\t0\t0\t1\t0\t0\t50\t268435460\t2\n"
        << "3\t0\t0\t0\t2\t0\t0\t50\t268435472\t2\n"
        << "3\t0\t0\t0\t3\t0\t0\t50\t268435476\t2\n";
  std::istringstream input(trace.str());
  const FineMicrotraceImportResult result =
      import_fine_microtrace_tsv(input, config(DeviceId(1)));
  expect(result.accesses.size() == 2, "sparse lanes produce separate spans");
  expect(
      result.accesses[0].active_lanes == 2 &&
          result.accesses[1].active_lanes == 2,
      "each coalesced span reports only its contributing lanes");
}

void test_unknown_semantics_are_rejected() {
  FineMicrotraceImportConfig unknown_type = config(DeviceId(1));
  unknown_type.instruction_types.pop_back();
  std::istringstream first(sample_trace());
  expect_invalid(
      [&first, &unknown_type]() {
        import_fine_microtrace_tsv(first, unknown_type);
      },
      "unmapped instruction types fail instead of being guessed");

  FineMicrotraceImportConfig unmapped_address = config(DeviceId(1));
  unmapped_address.allocations[0].size_bytes = 64;
  std::istringstream second(sample_trace());
  expect_invalid(
      [&second, &unmapped_address]() {
        import_fine_microtrace_tsv(second, unmapped_address);
      },
      "unmapped addresses fail instead of being guessed");
}

}  // namespace

int main() {
  test_lane_records_coalesce_into_warp_accesses();
  test_allocation_owner_classifies_remote_access();
  test_sparse_lanes_form_separate_memory_spans();
  test_unknown_semantics_are_rejected();
  if (failures != 0) {
    std::cerr << failures << " fine microtrace test(s) failed\n";
    return 1;
  }
  std::cout << "All fine microtrace tests passed\n";
  return 0;
}
