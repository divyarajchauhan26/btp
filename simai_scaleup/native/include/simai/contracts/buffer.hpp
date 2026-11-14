#ifndef SIMAI_CONTRACTS_BUFFER_HPP
#define SIMAI_CONTRACTS_BUFFER_HPP

#include <cstdint>
#include <limits>

#include "simai/contracts/ids.hpp"
#include "simai/contracts/validation.hpp"

namespace simai {
namespace contracts {

enum class MemoryRegion { DeviceLocal };

struct BufferSlice {
  DeviceId owner;
  BufferId buffer;
  std::uint64_t offset_bytes;
  std::uint64_t length_bytes;
  MemoryRegion region;

  ValidationErrors validate() const {
    ValidationErrors errors;
    if (!owner.valid() || !buffer.valid()) {
      append_error(
          errors,
          ValidationCode::InvalidIdentity,
          "buffer owner and buffer identity must be nonzero");
    }
    if (length_bytes == 0) {
      append_error(
          errors,
          ValidationCode::InvalidBuffer,
          "buffer length must be nonzero");
    }
    if (offset_bytes >
        std::numeric_limits<std::uint64_t>::max() - length_bytes) {
      append_error(
          errors,
          ValidationCode::InvalidBuffer,
          "buffer byte range overflows");
    }
    return errors;
  }

  void require_valid() const { require_no_errors(validate(), "BufferSlice"); }
};

inline bool operator==(const BufferSlice& lhs, const BufferSlice& rhs) {
  return lhs.owner == rhs.owner && lhs.buffer == rhs.buffer &&
      lhs.offset_bytes == rhs.offset_bytes &&
      lhs.length_bytes == rhs.length_bytes && lhs.region == rhs.region;
}

inline bool operator!=(const BufferSlice& lhs, const BufferSlice& rhs) {
  return !(lhs == rhs);
}

}  // namespace contracts
}  // namespace simai

#endif
