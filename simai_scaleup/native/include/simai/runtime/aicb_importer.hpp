#ifndef SIMAI_RUNTIME_AICB_IMPORTER_HPP
#define SIMAI_RUNTIME_AICB_IMPORTER_HPP

#include <cstdint>
#include <istream>
#include <string>

#include "simai/runtime/workload.hpp"

namespace simai {
namespace runtime {

struct AicbImportOptions {
  std::uint64_t schema_version;
  std::uint64_t deterministic_seed;
  CollectiveAlgorithmPolicy collective_algorithm;
  LinkConfig link;
  GpuConfig gpu;
  std::uint64_t nanoseconds_per_tick;
};

WorkloadSpec import_aicb_workload(
    std::istream& input,
    const AicbImportOptions& options);
WorkloadSpec import_aicb_workload_file(
    const std::string& path,
    const AicbImportOptions& options);

}  // namespace runtime
}  // namespace simai

#endif
