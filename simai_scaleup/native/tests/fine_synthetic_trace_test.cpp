#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "simai/runtime/fine_synthetic_trace.hpp"

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

std::string valid_trace() {
  return
      "schema_version=1\n"
      "kernel_name=copy\n"
      "device=1\n"
      "kernel_kind=communication\n"
      "timestamp_unit=ns\n"
      "workgroup_residency_slots=1\n"
      "register_buffer=99\n"
      "record=20|1|1|store|0x100|remote|2|12|64\n"
      "record=10|1|1|load|0x0|local|1|11|64\n";
}

std::string valid_v2_trace() {
  return
      "schema_version=2\n"
      "kernel_name=copy_v2\n"
      "trace_producer=simai.synthetic\n"
      "launch_id=iteration-7.copy\n"
      "hardware_id=generic-scaleup-v1\n"
      "timestamp_semantics=earliest_issue\n"
      "device=1\n"
      "kernel_kind=communication\n"
      "timestamp_unit=ns\n"
      "workgroup_residency_slots=1\n"
      "register_buffer=99\n"
      "buffer=11|1|4096\n"
      "buffer=12|2|4096\n"
      "record=5|1|1|load|0x0|local|1|11|64\n"
      "record=15|1|1|store|0x100|remote|2|12|128\n";
}

void test_trace_lowers_and_orders_records() {
  std::istringstream input(valid_trace());
  const FineSyntheticTrace trace = parse_fine_synthetic_trace(input);
  expect(
      trace.records.size() == 2 && trace.kernel.workgroups.size() == 1,
      "synthetic records lower into one kernel");
  const std::vector<FineGpuOperation>& operations =
      trace.kernel.workgroups[0].wavefronts[0].operations;
  expect(
      operations.size() == 4 &&
          operations[0].kind == FineGpuOperationKind::TraceWaitUntil &&
          operations[0].trace_timestamp_ns == 10 &&
          operations[1].kind == FineGpuOperationKind::Load &&
          operations[2].trace_timestamp_ns == 20 &&
          operations[3].kind == FineGpuOperationKind::Store,
      "records are ordered by timestamp and guarded by replay waits");
  expect(
      operations[1].source.space == FineMemorySpace::LocalHbm &&
          operations[1].source.offset_bytes == 0 &&
          operations[1].source.length_bytes == 64,
      "local Load preserves address and size");
  expect(
      operations[3].destination.space == FineMemorySpace::RemoteHbm &&
          operations[3].destination.owner == DeviceId(2) &&
          operations[3].destination.offset_bytes == 0x100,
      "remote Store preserves ownership and address");
}

void test_invalid_locality_and_operation_fail() {
  std::string mismatch = valid_trace();
  const std::size_t remote = mismatch.find("remote|2");
  mismatch.replace(remote, 8, "local|2");
  std::istringstream first(mismatch);
  expect_invalid(
      [&first]() { parse_fine_synthetic_trace(first); },
      "locality must agree with owner");

  std::string unsupported = valid_trace();
  const std::size_t load = unsupported.find("load|");
  unsupported.replace(load, 5, "reduce|");
  std::istringstream second(unsupported);
  expect_invalid(
      [&second]() { parse_fine_synthetic_trace(second); },
      "only Load and Store are accepted");
}

void test_v2_metadata_and_summary() {
  std::istringstream input(valid_v2_trace());
  const FineSyntheticTrace trace = parse_fine_synthetic_trace(input);
  expect(
      trace.schema_version == 2 &&
          trace.trace_producer == "simai.synthetic" &&
          trace.launch_id == "iteration-7.copy" &&
          trace.hardware_id == "generic-scaleup-v1" &&
          trace.timestamp_semantics ==
              FineSyntheticTimestampSemantics::EarliestIssue,
      "version 2 preserves trace provenance and timestamp semantics");
  const std::string summary = format_fine_synthetic_trace_summary(trace);
  expect(
      summary.find("records=2\n") != std::string::npos &&
          summary.find("loads=1\n") != std::string::npos &&
          summary.find("stores=1\n") != std::string::npos &&
          summary.find("declared_buffers=2\n") != std::string::npos &&
          summary.find("declared_buffer_bytes=8192\n") != std::string::npos &&
          summary.find("local_bytes=64\n") != std::string::npos &&
          summary.find("remote_bytes=128\n") != std::string::npos &&
          summary.find("last_timestamp_ns=15\n") != std::string::npos,
      "synthetic trace summary reports structure, bytes, and timeline");
}

void test_v2_requires_complete_metadata() {
  std::string incomplete = valid_v2_trace();
  const std::string field = "hardware_id=generic-scaleup-v1\n";
  incomplete.erase(incomplete.find(field), field.size());
  std::istringstream first(incomplete);
  expect_invalid(
      [&first]() { parse_fine_synthetic_trace(first); },
      "version 2 requires complete provenance metadata");

  std::string wrong_semantics = valid_v2_trace();
  const std::size_t semantics = wrong_semantics.find("earliest_issue");
  wrong_semantics.replace(semantics, 14, "observed_issue");
  std::istringstream second(wrong_semantics);
  expect_invalid(
      [&second]() { parse_fine_synthetic_trace(second); },
      "unsupported timestamp semantics fail explicitly");
}

void test_v2_validates_buffer_declarations() {
  std::string undeclared = valid_v2_trace();
  const std::string declaration = "buffer=12|2|4096\n";
  undeclared.erase(undeclared.find(declaration), declaration.size());
  std::istringstream first(undeclared);
  expect_invalid(
      [&first]() { parse_fine_synthetic_trace(first); },
      "version 2 rejects undeclared record buffers");

  std::string wrong_owner = valid_v2_trace();
  const std::size_t owner = wrong_owner.find("buffer=12|2|4096");
  wrong_owner.replace(owner, 16, "buffer=12|1|4096");
  std::istringstream second(wrong_owner);
  expect_invalid(
      [&second]() { parse_fine_synthetic_trace(second); },
      "version 2 validates buffer ownership");

  std::string out_of_bounds = valid_v2_trace();
  const std::size_t address = out_of_bounds.find("store|0x100");
  out_of_bounds.replace(address, 11, "store|0x1000");
  std::istringstream third(out_of_bounds);
  expect_invalid(
      [&third]() { parse_fine_synthetic_trace(third); },
      "version 2 validates allocation-relative address bounds");
}

}  // namespace

int main() {
  test_trace_lowers_and_orders_records();
  test_invalid_locality_and_operation_fail();
  test_v2_metadata_and_summary();
  test_v2_requires_complete_metadata();
  test_v2_validates_buffer_declarations();
  if (failures != 0) {
    std::cerr << failures << " synthetic trace test(s) failed\n";
    return 1;
  }
  std::cout << "All fine synthetic trace tests passed\n";
  return 0;
}
