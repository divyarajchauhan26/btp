#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/runtime/fine_chakra_manifest.hpp"

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

void varint(std::string& output, std::uint64_t value) {
  while (value >= 0x80) {
    output.push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  output.push_back(static_cast<char>(value));
}

void key(std::string& output, std::uint64_t field, std::uint64_t wire) {
  varint(output, (field << 3) | wire);
}

void integer_field(
    std::string& output, std::uint64_t field, std::uint64_t value) {
  key(output, field, 0);
  varint(output, value);
}

void bytes_field(
    std::string& output, std::uint64_t field, const std::string& value) {
  key(output, field, 2);
  varint(output, value.size());
  output += value;
}

std::string attribute(const std::string& name, std::uint64_t value) {
  std::string result;
  bytes_field(result, 1, name);
  integer_field(result, 9, value);
  return result;
}

std::string metadata() {
  std::string result;
  bytes_field(result, 1, "0.0.4");
  return result;
}

std::string node(
    std::uint64_t id,
    const std::string& name,
    std::uint64_t type,
    const std::vector<std::uint64_t>& dependencies,
    std::uint64_t duration,
    const std::vector<std::string>& attributes) {
  std::string result;
  if (id != 0) integer_field(result, 1, id);
  bytes_field(result, 2, name);
  integer_field(result, 3, type);
  if (!dependencies.empty()) {
    std::string packed;
    for (std::uint64_t dependency : dependencies) varint(packed, dependency);
    bytes_field(result, 5, packed);
  }
  if (duration != 0) integer_field(result, 7, duration);
  for (const std::string& entry : attributes) bytes_field(result, 10, entry);
  return result;
}

void delimited(std::string& output, const std::string& message) {
  varint(output, message.size());
  output += message;
}

std::string trace() {
  std::string result;
  delimited(result, metadata());
  delimited(result, node(0, "pre", 4, {}, 5, {}));
  delimited(
      result,
      node(
          7,
          "allreduce",
          7,
          {0},
          0,
          {attribute("comm_type", 0), attribute("comm_size", 256)}));
  delimited(result, node(9, "post", 4, {7}, 2, {}));
  return result;
}

void write_binary(const std::string& path, const std::string& bytes) {
  std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("could not write manifest test trace");
}

std::string data_file(const std::string& name) {
  return std::string(SIMAI_TEST_DATA_DIR) + "/" + name;
}

void write_program_files(
    const std::string& manifest,
    const std::string& trace0,
    const std::string& trace1) {
  write_binary(trace0, trace());
  write_binary(trace1, trace());
  std::ofstream output(manifest.c_str(), std::ios::trunc);
  output
      << "schema_version=1\n"
      << "trace=1|" << trace0 << "\n"
      << "trace=2|" << trace1 << "\n"
      << "synthetic=rank0_pre|0:0|"
      << data_file("synthetic_two_gpu.trace") << "\n"
      << "compute=rank1_pre|1:0|1|1|1|duration\n"
      << "collective=allreduce|0:7,1:7|"
      << data_file("mscclpp_two_gpu.json")
      << "|trace|1|64|4\n"
      << "compute=rank0_post|0:9|1|1|1|duration\n"
      << "compute=rank1_post|1:9|1|1|1|duration\n";
  if (!output) throw std::runtime_error("could not write manifest test file");
}

void test_file_manifest_loads_end_to_end() {
  const std::string trace0 = "fine_manifest_rank0.et";
  const std::string trace1 = "fine_manifest_rank1.et";
  const std::string manifest = "fine_manifest_test.txt";
  write_program_files(manifest, trace0, trace1);

  const FineChakraExecutionProgram program =
      load_fine_chakra_execution_manifest(manifest, 1024 * 1024);
  expect(program.graph.nodes.size() == 6, "manifest imports both rank traces");
  expect(
      program.execution.nodes.size() == 5,
      "manifest groups two rank collective nodes into one execution node");
  expect(
      program.execution.nodes[1].kernels[0]
              .workgroups[0]
              .wavefronts[0]
              .operations[0]
              .reduce_cycles == 5000,
      "remaining duration binding uses Chakra nanoseconds as abstract cycles");
  expect(
      program.execution.nodes[0].kernels[0]
              .workgroups[0]
              .wavefronts[0]
              .operations[0]
              .kind == FineGpuOperationKind::TraceWaitUntil,
      "synthetic binding lowers timestamped memory operations");
  expect(
      program.execution.nodes[2].kernels.size() == 2,
      "manifest collective loads and lowers the referenced MSCCL++ plan");

  std::remove(manifest.c_str());
  std::remove(trace0.c_str());
  std::remove(trace1.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 5 && std::string(argv[1]) == "--write-fixture") {
    write_program_files(argv[2], argv[3], argv[4]);
    return 0;
  }
  if (argc != 1) {
    std::cerr << "unexpected fine Chakra manifest test arguments\n";
    return 2;
  }
  test_file_manifest_loads_end_to_end();
  if (failures != 0) {
    std::cerr << failures << " fine Chakra manifest test(s) failed\n";
    return 1;
  }
  std::cout << "All fine Chakra manifest tests passed\n";
  return 0;
}
