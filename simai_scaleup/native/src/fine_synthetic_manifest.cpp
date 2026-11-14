#include "simai/runtime/fine_synthetic_manifest.hpp"

#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace simai {
namespace runtime {
namespace {

struct KernelSpec {
  std::string name;
  std::string trace_path;
  std::vector<std::string> dependencies;
};

std::string trim(const std::string& value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> fields;
  std::stringstream stream(value);
  std::string field;
  while (std::getline(stream, field, delimiter)) {
    fields.push_back(trim(field));
  }
  return fields;
}

std::string parent_directory(const std::string& path) {
  const std::size_t separator = path.find_last_of("/\\");
  return separator == std::string::npos ? std::string()
                                        : path.substr(0, separator + 1);
}

bool absolute_path(const std::string& path) {
  return !path.empty() &&
      (path[0] == '/' || path[0] == '\\' ||
       (path.size() > 1 && path[1] == ':'));
}

std::string resolve_path(
    const std::string& directory, const std::string& path) {
  return absolute_path(path) ? path : directory + path;
}

std::uint64_t version(const std::string& value) {
  if (value.empty() || value[0] == '-') {
    throw std::invalid_argument(
        "synthetic manifest schema version must be unsigned");
  }
  std::size_t consumed = 0;
  const std::uint64_t result = std::stoull(value, &consumed, 10);
  if (consumed != value.size()) {
    throw std::invalid_argument(
        "synthetic manifest schema version must be unsigned");
  }
  return result;
}

}  // namespace

FineSyntheticExecutionProgram load_fine_synthetic_execution_manifest(
    const std::string& path) {
  std::ifstream input(path.c_str());
  if (!input) {
    throw std::invalid_argument(
        "could not open synthetic execution manifest: " + path);
  }
  const std::string directory = parent_directory(path);
  std::uint64_t schema_version = 0;
  bool has_schema = false;
  std::vector<KernelSpec> specs;
  std::set<std::string> names;
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    line = trim(line);
    if (line.empty()) continue;
    if (line.find("schema_version=") == 0) {
      if (has_schema) {
        throw std::invalid_argument(
            "synthetic manifest contains duplicate schema version");
      }
      schema_version = version(trim(line.substr(15)));
      has_schema = true;
      continue;
    }
    if (line.find("kernel=") == 0) {
      const std::vector<std::string> fields = split(line.substr(7), '|');
      if (fields.size() != 3 || fields[0].empty() || fields[1].empty() ||
          !names.insert(fields[0]).second) {
        throw std::invalid_argument(
            "synthetic manifest kernel requires unique name, path, and dependencies");
      }
      std::vector<std::string> dependencies;
      if (fields[2] != "-") {
        dependencies = split(fields[2], ',');
        if (dependencies.empty()) {
          throw std::invalid_argument(
              "synthetic manifest dependency list is empty");
        }
      }
      specs.push_back(KernelSpec{
          fields[0], resolve_path(directory, fields[1]), dependencies});
      continue;
    }
    throw std::invalid_argument("unsupported synthetic manifest field");
  }
  if (!has_schema || schema_version != 1 || specs.empty()) {
    throw std::invalid_argument(
        "synthetic execution manifest requires schema version 1 and kernels");
  }

  FineSyntheticExecutionProgram program;
  std::map<std::string, contracts::WorkloadEventId> node_ids;
  for (std::size_t index = 0; index < specs.size(); ++index) {
    node_ids.emplace(
        specs[index].name, contracts::WorkloadEventId(index + 1));
    program.traces.push_back(
        parse_fine_synthetic_trace_file(specs[index].trace_path));
  }
  for (std::size_t index = 0; index < specs.size(); ++index) {
    std::vector<contracts::WorkloadEventId> dependencies;
    std::set<contracts::WorkloadEventId> unique_dependencies;
    for (const std::string& dependency : specs[index].dependencies) {
      const auto found = node_ids.find(dependency);
      if (found == node_ids.end() || found->second.value() == index + 1 ||
          !unique_dependencies.insert(found->second).second) {
        throw std::invalid_argument(
            "synthetic manifest dependency is missing, duplicate, or self-referential");
      }
      dependencies.push_back(found->second);
    }
    contracts::FineKernelDefinition kernel = program.traces[index].kernel;
    kernel.id = contracts::KernelId(index + 1);
    program.execution.nodes.push_back(FineGraphExecutionNode{
        contracts::WorkloadEventId(index + 1),
        specs[index].name,
        {contracts::WorkloadEventId(index + 1)},
        dependencies,
        {kernel}});
  }
  program.execution.require_valid();
  return program;
}

}  // namespace runtime
}  // namespace simai
