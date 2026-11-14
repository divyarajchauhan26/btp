#ifndef SIMAI_RUNTIME_FINE_SYNTHETIC_MANIFEST_HPP
#define SIMAI_RUNTIME_FINE_SYNTHETIC_MANIFEST_HPP

#include <string>
#include <vector>

#include "simai/runtime/fine_graph_execution.hpp"
#include "simai/runtime/fine_synthetic_trace.hpp"

namespace simai {
namespace runtime {

struct FineSyntheticExecutionProgram {
  std::vector<FineSyntheticTrace> traces;
  FineGraphExecutionPlan execution;
};

FineSyntheticExecutionProgram load_fine_synthetic_execution_manifest(
    const std::string& path);

}  // namespace runtime
}  // namespace simai

#endif
