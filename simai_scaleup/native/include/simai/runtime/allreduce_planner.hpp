#ifndef SIMAI_RUNTIME_ALLREDUCE_PLANNER_HPP
#define SIMAI_RUNTIME_ALLREDUCE_PLANNER_HPP

#include <cstdint>

#include "simai/contracts/collective.hpp"

namespace simai {
namespace runtime {

contracts::CollectivePlan make_gather_reduce_broadcast_allreduce(
    contracts::CollectivePlanId plan_id,
    const contracts::ParallelGroup& group,
    std::uint64_t payload_bytes);

}  // namespace runtime
}  // namespace simai

#endif
