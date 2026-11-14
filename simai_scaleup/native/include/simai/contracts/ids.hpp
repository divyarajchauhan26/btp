#ifndef SIMAI_CONTRACTS_IDS_HPP
#define SIMAI_CONTRACTS_IDS_HPP

#include <cstdint>
#include <functional>

namespace simai {
namespace contracts {

using SimTimeNs = std::uint64_t;

template <typename Tag>
class StrongId {
 public:
  using ValueType = std::uint64_t;

  constexpr StrongId() : value_(0) {}
  explicit constexpr StrongId(ValueType value) : value_(value) {}

  constexpr ValueType value() const { return value_; }
  constexpr bool valid() const { return value_ != 0; }

 private:
  ValueType value_;
};

template <typename Tag>
constexpr bool operator==(StrongId<Tag> lhs, StrongId<Tag> rhs) {
  return lhs.value() == rhs.value();
}

template <typename Tag>
constexpr bool operator!=(StrongId<Tag> lhs, StrongId<Tag> rhs) {
  return !(lhs == rhs);
}

template <typename Tag>
constexpr bool operator<(StrongId<Tag> lhs, StrongId<Tag> rhs) {
  return lhs.value() < rhs.value();
}

struct DeviceIdTag {};
struct BufferIdTag {};
struct WorkloadEventIdTag {};
struct ParallelGroupIdTag {};
struct CollectivePlanIdTag {};
struct OperationIdTag {};
struct ThreadBlockIdTag {};
struct TransactionIdTag {};
struct PduIdTag {};
struct RailIdTag {};
struct SemaphoreIdTag {};
struct OrderingDomainIdTag {};
struct EventIdTag {};
struct KernelIdTag {};
struct WorkgroupIdTag {};
struct WavefrontIdTag {};
struct GpuInstructionIdTag {};
struct GpuOperationIdTag {};
struct MemoryRequestIdTag {};
struct ComputeUnitIdTag {};
struct BarrierIdTag {};
struct KernelInstanceIdTag {};
struct WorkgroupInstanceIdTag {};
struct WavefrontInstanceIdTag {};

using DeviceId = StrongId<DeviceIdTag>;
using BufferId = StrongId<BufferIdTag>;
using WorkloadEventId = StrongId<WorkloadEventIdTag>;
using ParallelGroupId = StrongId<ParallelGroupIdTag>;
using CollectivePlanId = StrongId<CollectivePlanIdTag>;
using OperationId = StrongId<OperationIdTag>;
using ThreadBlockId = StrongId<ThreadBlockIdTag>;
using TransactionId = StrongId<TransactionIdTag>;
using PduId = StrongId<PduIdTag>;
using RailId = StrongId<RailIdTag>;
using SemaphoreId = StrongId<SemaphoreIdTag>;
using OrderingDomainId = StrongId<OrderingDomainIdTag>;
using EventId = StrongId<EventIdTag>;
using KernelId = StrongId<KernelIdTag>;
using WorkgroupId = StrongId<WorkgroupIdTag>;
using WavefrontId = StrongId<WavefrontIdTag>;
using GpuInstructionId = StrongId<GpuInstructionIdTag>;
using GpuOperationId = StrongId<GpuOperationIdTag>;
using MemoryRequestId = StrongId<MemoryRequestIdTag>;
using ComputeUnitId = StrongId<ComputeUnitIdTag>;
using BarrierId = StrongId<BarrierIdTag>;
using KernelInstanceId = StrongId<KernelInstanceIdTag>;
using WorkgroupInstanceId = StrongId<WorkgroupInstanceIdTag>;
using WavefrontInstanceId = StrongId<WavefrontInstanceIdTag>;

}  // namespace contracts
}  // namespace simai

namespace std {

template <typename Tag>
struct hash<simai::contracts::StrongId<Tag>> {
  std::size_t operator()(simai::contracts::StrongId<Tag> id) const {
    return std::hash<std::uint64_t>()(id.value());
  }
};

}  // namespace std

#endif
