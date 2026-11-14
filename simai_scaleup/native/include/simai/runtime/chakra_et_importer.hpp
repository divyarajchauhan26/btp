#ifndef SIMAI_RUNTIME_CHAKRA_ET_IMPORTER_HPP
#define SIMAI_RUNTIME_CHAKRA_ET_IMPORTER_HPP

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "simai/runtime/fine_kernel_graph.hpp"

namespace simai {
namespace runtime {

struct ChakraEtImportOptions {
  contracts::DeviceId device;
  std::uint64_t maximum_message_bytes;
};

FineKernelGraph import_chakra_et(
    std::istream& input, const ChakraEtImportOptions& options);
FineKernelGraph import_chakra_et_file(
    const std::string& path, const ChakraEtImportOptions& options);

struct ChakraEtRankTrace {
  std::string path;
  contracts::DeviceId device;
};

FineKernelGraph import_chakra_et_files(
    const std::vector<ChakraEtRankTrace>& traces,
    std::uint64_t maximum_message_bytes);

}  // namespace runtime
}  // namespace simai

#endif
