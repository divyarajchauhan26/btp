#ifndef SIMAI_CONTRACTS_EVENT_QUEUE_HPP
#define SIMAI_CONTRACTS_EVENT_QUEUE_HPP

#include <cstddef>
#include <cstdint>
#include <queue>
#include <vector>

#include "simai/contracts/ids.hpp"

namespace simai {
namespace contracts {

enum class EventKind {
  WorkloadReady,
  MicroOpReady,
  GpuInstructionReady,
  FineInstructionIssue,
  FineInstructionCompletion,
  SueTransactionReady,
  PduArrivalReady,
  CompletionReady
};

struct ScheduledEvent {
  EventId id;
  SimTimeNs time_ns;
  std::uint64_t insertion_sequence;
  EventKind kind;
};

class DeterministicEventQueue {
 public:
  DeterministicEventQueue();

  void schedule(EventId id, SimTimeNs time_ns, EventKind kind);
  ScheduledEvent pop();

  bool empty() const { return events_.empty(); }
  std::size_t size() const { return events_.size(); }
  SimTimeNs now() const { return now_ns_; }

 private:
  struct Later {
    bool operator()(const ScheduledEvent& lhs, const ScheduledEvent& rhs) const;
  };

  SimTimeNs now_ns_;
  std::uint64_t next_sequence_;
  std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, Later>
      events_;
};

}  // namespace contracts
}  // namespace simai

#endif
