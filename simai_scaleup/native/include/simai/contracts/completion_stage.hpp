#ifndef SIMAI_CONTRACTS_COMPLETION_STAGE_HPP
#define SIMAI_CONTRACTS_COMPLETION_STAGE_HPP

#include <cstdint>

namespace simai {
namespace contracts {

enum class CompletionStage : std::uint8_t {
  Issued = 0,
  LocalComplete = 1,
  RemoteArrived = 2,
  RemoteVisible = 3,
  Finalized = 4
};

inline std::uint8_t completion_stage_index(CompletionStage stage) {
  return static_cast<std::uint8_t>(stage);
}

}  // namespace contracts
}  // namespace simai

#endif
