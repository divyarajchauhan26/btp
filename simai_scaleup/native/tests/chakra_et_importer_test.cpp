#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/runtime/chakra_et_importer.hpp"

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

std::string attribute(
    const std::string& name, std::uint64_t value_field, std::uint64_t value) {
  std::string result;
  bytes_field(result, 1, name);
  integer_field(result, value_field, value);
  return result;
}

std::string version_metadata() {
  std::string result;
  bytes_field(result, 1, "0.0.4");
  return result;
}

std::string metadata() {
  std::string schema_attribute;
  bytes_field(schema_attribute, 1, "schema");
  bytes_field(schema_attribute, 29, "1.0.2-chakra.0.0.4");
  std::string result;
  bytes_field(result, 2, schema_attribute);
  return result;
}

std::string node(
    std::uint64_t id,
    const std::string& name,
    std::uint64_t type,
    const std::vector<std::uint64_t>& dependencies,
    std::uint64_t duration_micros,
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
  if (duration_micros != 0) integer_field(result, 7, duration_micros);
  for (const std::string& entry : attributes) bytes_field(result, 10, entry);
  return result;
}

std::string io(
    const std::string& values,
    const std::string& shapes,
    const std::string& types) {
  std::string result;
  bytes_field(result, 1, values);
  bytes_field(result, 2, shapes);
  bytes_field(result, 3, types);
  return result;
}

void delimited(std::string& output, const std::string& message) {
  varint(output, message.size());
  output += message;
}

std::string fixture() {
  std::string result;
  delimited(result, metadata());
  std::string forward = node(0, "forward", 4, {}, 5, {});
  integer_field(forward, 6, 11);
  bytes_field(forward, 8, io("input0", "[4,8]", "float32"));
  bytes_field(forward, 9, io("output0", "[4,8]", "float32"));
  delimited(result, forward);
  delimited(
      result,
      node(
          7,
          "allreduce",
          7,
          {0},
          0,
          {attribute("comm_type", 9, 0),
           attribute("comm_size", 9, 256)}));
  delimited(
      result,
      node(
          9,
          "load_weights",
          2,
          {7},
          0,
          {attribute("tensor_size", 13, 128)}));
  return result;
}

FineKernelGraph parse(const std::string& bytes) {
  std::istringstream input(bytes, std::ios::binary);
  return import_chakra_et(
      input, ChakraEtImportOptions{DeviceId(2), 1024 * 1024});
}

void test_official_wire_shape_imports() {
  const FineKernelGraph graph = parse(fixture());
  expect(graph.source_format == "chakra_et", "source format is Chakra ET");
  expect(
      graph.source_version == "1.0.2-chakra.0.0.4",
      "metadata schema attribute is preserved");
  expect(graph.nodes.size() == 3, "all Chakra nodes are imported");
  expect(
      graph.nodes[0].source_id == 0 &&
          graph.nodes[0].id == WorkloadEventId(1),
      "zero-based Chakra IDs map to valid internal IDs");
  expect(
      graph.nodes[0].kind == FineKernelGraphNodeKind::Compute &&
          graph.nodes[0].has_start_time &&
          graph.nodes[0].start_time_ns == 11000 &&
          graph.nodes[0].has_duration &&
          graph.nodes[0].duration_ns == 5000,
      "compute timing converts from microseconds to nanoseconds");
  expect(
      graph.nodes[0].inputs.values == "input0" &&
          graph.nodes[0].inputs.shapes == "[4,8]" &&
          graph.nodes[0].outputs.values == "output0",
      "Chakra input and output descriptions are preserved");
  expect(
      graph.nodes[1].kind == FineKernelGraphNodeKind::Collective &&
          graph.nodes[1].has_collective && graph.nodes[1].has_payload &&
          graph.nodes[1].collective == CollectiveKind::AllReduce &&
          graph.nodes[1].payload_bytes == 256,
      "collective semantics and payload are preserved");
  expect(
      graph.nodes[1].attributes.size() == 2 &&
          !graph.nodes[1].attributes[0].protobuf_hex.empty(),
      "typed and raw Chakra attributes are preserved");
  expect(
      graph.nodes[2].kind == FineKernelGraphNodeKind::MemoryLoad &&
          graph.nodes[2].has_payload &&
          graph.nodes[2].payload_bytes == 128,
      "memory-node tensor size is preserved");
  const std::vector<WorkloadEventId> order = graph.topological_order();
  expect(
      order.size() == 3 && order[0] == WorkloadEventId(1) &&
          order[1] == WorkloadEventId(2) &&
          order[2] == WorkloadEventId(3),
      "Chakra dependencies produce deterministic topology");
}

void test_invalid_traces_fail_explicitly() {
  expect_invalid(
      []() { parse(""); },
      "empty Chakra ET is rejected");

  std::string missing_dependency;
  delimited(missing_dependency, metadata());
  delimited(
      missing_dependency,
      node(1, "compute", 4, {99}, 1, {}));
  expect_invalid(
      [&missing_dependency]() { parse(missing_dependency); },
      "absent Chakra dependency is rejected");

  std::string unsupported_collective;
  delimited(unsupported_collective, metadata());
  delimited(
      unsupported_collective,
      node(
          1,
          "barrier",
          7,
          {},
          0,
          {attribute("comm_type", 9, 9),
           attribute("comm_size", 9, 1)}));
  expect_invalid(
      [&unsupported_collective]() { parse(unsupported_collective); },
      "unsupported Chakra collective is rejected");
}

void test_direct_metadata_version_imports() {
  std::string bytes;
  delimited(bytes, version_metadata());
  delimited(bytes, node(0, "compute", 4, {}, 1, {}));
  expect(
      parse(bytes).source_version == "0.0.4",
      "direct metadata version is supported");
}

void test_unresolved_collective_metadata_is_preserved() {
  std::string unresolved;
  integer_field(unresolved, 1, 4);
  bytes_field(unresolved, 2, "captured_collective");
  integer_field(unresolved, 3, 7);
  integer_field(unresolved, 4, 999);
  std::string bytes;
  delimited(bytes, metadata());
  delimited(bytes, unresolved);
  const FineKernelGraph graph = parse(bytes);
  expect(
      graph.nodes.size() == 1 &&
          graph.nodes[0].kind == FineKernelGraphNodeKind::Collective &&
          !graph.nodes[0].has_collective &&
          !graph.nodes[0].has_payload &&
          graph.nodes[0].dependencies.empty(),
      "missing collective attributes stay unknown and ctrl deps stay non-executional");
  expect(
      graph.nodes[0].control_dependencies.size() == 1 &&
          graph.nodes[0].control_dependencies[0] == 999,
      "Chakra control dependencies are preserved separately");
}

void test_rank_graphs_merge_without_id_collisions() {
  std::istringstream first(fixture(), std::ios::binary);
  std::istringstream second(fixture(), std::ios::binary);
  const FineKernelGraph merged = merge_fine_kernel_graphs({
      import_chakra_et(
          first, ChakraEtImportOptions{DeviceId(1), 1024 * 1024}),
      import_chakra_et(
          second, ChakraEtImportOptions{DeviceId(2), 1024 * 1024})});
  expect(merged.nodes.size() == 6, "two rank graphs retain every node");
  expect(
      merged.nodes[0].source_rank == 0 &&
          merged.nodes[3].source_rank == 1 &&
          merged.nodes[0].source_id == merged.nodes[3].source_id,
      "composite rank/source IDs allow repeated Chakra node IDs");
  expect(
      merged.nodes[4].dependencies.size() == 1 &&
          merged.nodes[4].dependencies[0] == WorkloadEventId(4),
      "rank-local dependencies are remapped to global graph identities");
}

int write_fixture(const char* path) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  const std::string bytes = fixture();
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    std::cerr << "could not write Chakra fixture\n";
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--write-fixture") {
    return write_fixture(argv[2]);
  }
  if (argc != 1) {
    std::cerr << "unexpected Chakra importer test arguments\n";
    return 2;
  }
  test_official_wire_shape_imports();
  test_direct_metadata_version_imports();
  test_unresolved_collective_metadata_is_preserved();
  test_rank_graphs_merge_without_id_collisions();
  test_invalid_traces_fail_explicitly();
  if (failures != 0) {
    std::cerr << failures << " Chakra importer test(s) failed\n";
    return 1;
  }
  std::cout << "All Chakra ET importer tests passed\n";
  return 0;
}
