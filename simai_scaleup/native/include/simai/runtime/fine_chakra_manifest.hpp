#ifndef SIMAI_RUNTIME_FINE_CHAKRA_MANIFEST_HPP
#define SIMAI_RUNTIME_FINE_CHAKRA_MANIFEST_HPP

#include <cstdint>
#include <string>

#include "simai/runtime/chakra_et_importer.hpp"
#include "simai/runtime/fine_graph_execution.hpp"

namespace simai {
namespace runtime {

struct FineChakraExecutionProgram {
  FineKernelGraph graph;
  FineGraphExecutionPlan execution;
};

FineChakraExecutionProgram load_fine_chakra_execution_manifest(
    const std::string& path,
    std::uint64_t maximum_message_bytes);

}  // namespace runtime
}  // namespace simai

#endif
