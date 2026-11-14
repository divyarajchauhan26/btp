#include "simai/contracts/event_queue.hpp"

#include <stdexcept>

namespace simai {
namespace contracts {

DeterministicEventQueue::DeterministicEventQueue()
    : now_ns_(0), next_sequence_(0), events_() {}

bool DeterministicEventQueue::Later::operator()(
    const ScheduledEvent& lhs,
    const ScheduledEvent& rhs) const {
  if (lhs.time_ns != rhs.time_ns) {
    return lhs.time_ns > rhs.time_ns;
  }
  return lhs.insertion_sequence > rhs.insertion_sequence;
}

void DeterministicEventQueue::schedule(
    EventId id,
    SimTimeNs time_ns,
    EventKind kind) {
  if (!id.valid()) {
    throw std::invalid_argument("scheduled event identity must be nonzero");
  }
  if (time_ns < now_ns_) {
    throw std::invalid_argument("event may not be scheduled into the past");
  }
  events_.push(ScheduledEvent{id, time_ns, next_sequence_, kind});
  ++next_sequence_;
}

ScheduledEvent DeterministicEventQueue::pop() {
  if (events_.empty()) {
    throw std::underflow_error("cannot pop an empty event queue");
  }
  const ScheduledEvent event = events_.top();
  events_.pop();
  now_ns_ = event.time_ns;
  return event;
}

}  // namespace contracts
}  // namespace simai
