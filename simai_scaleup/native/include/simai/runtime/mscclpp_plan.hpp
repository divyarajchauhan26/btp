#ifndef SIMAI_RUNTIME_MSCCLPP_PLAN_HPP
#define SIMAI_RUNTIME_MSCCLPP_PLAN_HPP

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "simai/contracts/fine_gpu.hpp"
#include "simai/runtime/collective_planner.hpp"

namespace simai {
namespace runtime {

struct MscclppBufferRef {
  bool remote;
  std::string type;
  std::uint64_t buffer_id;
  std::uint64_t index;
  std::uint64_t size;
};

struct MscclppOperation {
  std::string name;
  std::string channel_type;
  std::vector<std::uint64_t> channel_ids;
  std::vector<MscclppBufferRef> sources;
  std::vector<MscclppBufferRef> destinations;
  std::string reduce_op;
};

struct MscclppThreadblock {
  std::uint64_t id;
  std::vector<MscclppOperation> operations;
};

struct MscclppChannel {
  std::string type;
  std::vector<std::uint64_t> connected_to;
};

struct MscclppRemoteBuffer {
  std::uint64_t rank;
  std::string type;
  std::vector<std::string> access_channel_types;
};

struct MscclppGpu {
  std::uint64_t id;
  std::uint64_t input_chunks;
  std::uint64_t output_chunks;
  std::uint64_t scratch_chunks;
  std::vector<MscclppThreadblock> threadblocks;
  std::vector<MscclppChannel> channels;
  std::vector<MscclppRemoteBuffer> remote_buffers;
};

struct MscclppPlan {
  std::string name;
  std::string collective;
  std::string protocol;
  bool inplace;
  bool reuse_resources;
  std::vector<MscclppGpu> gpus;
};

MscclppPlan parse_mscclpp_plan(std::istream& input);
MscclppPlan parse_mscclpp_plan_file(const std::string& path);

struct MscclppFineLoweringConfig {
  std::uint32_t workgroup_residency_slots;
  std::uint64_t memory_alignment_bytes;
  std::uint64_t reduce_cycles;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
};

std::vector<contracts::FineKernelDefinition> lower_mscclpp_fine_kernels(
    const MscclppPlan& plan,
    std::uint64_t payload_bytes,
    const MscclppFineLoweringConfig& config);

struct MscclppPlanRegistryEntry {
  MscclppPlan plan;
  std::uint64_t minimum_payload_bytes;
  std::uint64_t maximum_payload_bytes;
  std::uint64_t payload_multiple_bytes;
};

class MscclppPlanRegistry {
 public:
  void add(const MscclppPlanRegistryEntry& entry);
  const MscclppPlan& select(
      contracts::CollectiveKind collective,
      std::uint32_t participant_count,
      const std::string& protocol,
      std::uint64_t payload_bytes) const;

  std::size_t size() const { return entries_.size(); }

 private:
  std::vector<MscclppPlanRegistryEntry> entries_;
};

class MscclppScheduleProvider final : public CollectiveScheduleProvider {
 public:
  explicit MscclppScheduleProvider(const MscclppPlan& plan);
  explicit MscclppScheduleProvider(const std::string& plan_path);

  PlannedCollective plan(
      const CollectiveRequest& request,
      const PlannerCapabilities& capabilities) const override;

 private:
  MscclppPlan plan_;
};

}  // namespace runtime
}  // namespace simai

#endif
