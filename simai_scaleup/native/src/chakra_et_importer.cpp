#include "simai/runtime/chakra_et_importer.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace simai {
namespace runtime {
namespace {

enum class ChakraNodeType : std::uint64_t {
  Invalid = 0,
  Metadata = 1,
  MemoryLoad = 2,
  MemoryStore = 3,
  Compute = 4,
  Send = 5,
  Receive = 6,
  Collective = 7
};

struct ChakraAttribute {
  std::string name;
  bool has_integer;
  std::uint64_t integer;
  bool integer_is_signed;
  std::string string;
  std::string protobuf_hex;
};

struct ChakraIo {
  std::string values;
  std::string shapes;
  std::string types;
};

struct ChakraNode {
  std::uint64_t id;
  std::string name;
  ChakraNodeType type;
  bool has_start_time;
  std::uint64_t start_time_micros;
  bool has_duration;
  std::uint64_t duration_micros;
  ChakraIo inputs;
  ChakraIo outputs;
  std::vector<std::uint64_t> control_dependencies;
  std::vector<std::uint64_t> dependencies;
  std::map<std::string, ChakraAttribute> attributes;
};

class WireReader {
 public:
  explicit WireReader(const std::vector<unsigned char>& bytes)
      : bytes_(bytes), position_(0) {}

  bool empty() const { return position_ == bytes_.size(); }

  std::uint64_t varint(const char* field) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 70; shift += 7) {
      if (position_ == bytes_.size()) {
        throw std::invalid_argument(std::string("truncated Chakra ") + field);
      }
      const unsigned char byte = bytes_[position_++];
      if (shift == 63 && (byte & 0xfeU) != 0) {
        throw std::invalid_argument(std::string("oversized Chakra ") + field);
      }
      value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
      if ((byte & 0x80U) == 0) return value;
    }
    throw std::invalid_argument(std::string("invalid Chakra ") + field);
  }

  std::vector<unsigned char> bytes(const char* field) {
    const std::uint64_t length = varint(field);
    if (length > bytes_.size() - position_) {
      throw std::invalid_argument(std::string("truncated Chakra ") + field);
    }
    const std::size_t end = position_ + static_cast<std::size_t>(length);
    std::vector<unsigned char> result(
        bytes_.begin() + position_, bytes_.begin() + end);
    position_ = end;
    return result;
  }

  std::string text(const char* field) {
    const std::vector<unsigned char> value = bytes(field);
    return std::string(value.begin(), value.end());
  }

  void skip(std::uint64_t wire_type, const char* field) {
    if (wire_type == 0) {
      static_cast<void>(varint(field));
      return;
    }
    if (wire_type == 1) {
      skip_fixed(8, field);
      return;
    }
    if (wire_type == 2) {
      static_cast<void>(bytes(field));
      return;
    }
    if (wire_type == 5) {
      skip_fixed(4, field);
      return;
    }
    throw std::invalid_argument("unsupported Chakra protobuf wire type");
  }

 private:
  void skip_fixed(std::size_t length, const char* field) {
    if (length > bytes_.size() - position_) {
      throw std::invalid_argument(std::string("truncated Chakra ") + field);
    }
    position_ += length;
  }

  const std::vector<unsigned char>& bytes_;
  std::size_t position_;
};

bool read_stream_varint(
    std::istream& input, std::uint64_t& value, bool allow_clean_eof) {
  value = 0;
  for (unsigned shift = 0; shift < 70; shift += 7) {
    const int next = input.get();
    if (next == std::char_traits<char>::eof()) {
      if (shift == 0 && allow_clean_eof) return false;
      throw std::invalid_argument("truncated Chakra message length");
    }
    const unsigned char byte = static_cast<unsigned char>(next);
    if (shift == 63 && (byte & 0xfeU) != 0) {
      throw std::invalid_argument("oversized Chakra message length");
    }
    value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0) return true;
  }
  throw std::invalid_argument("invalid Chakra message length");
}

bool read_message(
    std::istream& input,
    std::uint64_t maximum_message_bytes,
    std::vector<unsigned char>& message,
    bool allow_clean_eof) {
  std::uint64_t length = 0;
  if (!read_stream_varint(input, length, allow_clean_eof)) return false;
  if (length > maximum_message_bytes ||
      length > static_cast<std::uint64_t>(
                   std::numeric_limits<std::size_t>::max())) {
    throw std::invalid_argument("Chakra message exceeds configured limit");
  }
  message.assign(static_cast<std::size_t>(length), 0);
  if (length != 0) {
    input.read(
        reinterpret_cast<char*>(message.data()),
        static_cast<std::streamsize>(length));
    if (input.gcount() != static_cast<std::streamsize>(length)) {
      throw std::invalid_argument("truncated Chakra message body");
    }
  }
  return true;
}

ChakraAttribute parse_attribute(const std::vector<unsigned char>& message);

std::string hexadecimal(const std::vector<unsigned char>& bytes) {
  static const char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 0x0f]);
  }
  return result;
}

std::string parse_metadata(const std::vector<unsigned char>& message) {
  WireReader reader(message);
  std::string version;
  while (!reader.empty()) {
    const std::uint64_t key = reader.varint("metadata key");
    const std::uint64_t field = key >> 3;
    const std::uint64_t wire = key & 7;
    if (field == 1 && wire == 2) {
      version = reader.text("metadata version");
    } else if (field == 2 && wire == 2) {
      const ChakraAttribute attribute =
          parse_attribute(reader.bytes("metadata attribute"));
      if (attribute.name == "schema" && !attribute.string.empty()) {
        version = attribute.string;
      }
    } else {
      reader.skip(wire, "metadata field");
    }
  }
  if (version.empty()) {
    throw std::invalid_argument("Chakra ET global metadata has no version");
  }
  return version;
}

std::int64_t zigzag(std::uint64_t value) {
  return static_cast<std::int64_t>(
      (value >> 1) ^ (0U - (value & 1U)));
}

ChakraAttribute parse_attribute(const std::vector<unsigned char>& message) {
  WireReader reader(message);
  ChakraAttribute attribute{"", false, 0, false, "", hexadecimal(message)};
  while (!reader.empty()) {
    const std::uint64_t key = reader.varint("attribute key");
    const std::uint64_t field = key >> 3;
    const std::uint64_t wire = key & 7;
    if (field == 1 && wire == 2) {
      attribute.name = reader.text("attribute name");
    } else if ((field == 7 || field == 9 || field == 11 || field == 13 ||
                field == 15 || field == 17 || field == 27) &&
               wire == 0) {
      const std::uint64_t raw = reader.varint("attribute integer");
      attribute.has_integer = true;
      attribute.integer_is_signed = field == 7 || field == 9 || field == 15 ||
          field == 17;
      if (field == 15 || field == 17) {
        attribute.integer = static_cast<std::uint64_t>(zigzag(raw));
      } else {
        attribute.integer = raw;
      }
    } else if ((field == 29 || field == 31) && wire == 2) {
      attribute.string = reader.text("attribute string");
    } else {
      reader.skip(wire, "attribute field");
    }
  }
  if (attribute.name.empty()) {
    throw std::invalid_argument("Chakra node has an unnamed attribute");
  }
  return attribute;
}

ChakraIo parse_io(const std::vector<unsigned char>& message) {
  WireReader reader(message);
  ChakraIo io{};
  while (!reader.empty()) {
    const std::uint64_t key = reader.varint("I/O key");
    const std::uint64_t field = key >> 3;
    const std::uint64_t wire = key & 7;
    if (field == 1 && wire == 2) {
      io.values = reader.text("I/O values");
    } else if (field == 2 && wire == 2) {
      io.shapes = reader.text("I/O shapes");
    } else if (field == 3 && wire == 2) {
      io.types = reader.text("I/O types");
    } else {
      reader.skip(wire, "I/O field");
    }
  }
  return io;
}

void append_packed_dependencies(
    const std::vector<unsigned char>& bytes,
    std::vector<std::uint64_t>& dependencies) {
  WireReader packed(bytes);
  while (!packed.empty()) {
    dependencies.push_back(packed.varint("packed dependency"));
  }
}

ChakraNode parse_node(const std::vector<unsigned char>& message) {
  WireReader reader(message);
  ChakraNode node{};
  node.type = ChakraNodeType::Invalid;
  while (!reader.empty()) {
    const std::uint64_t key = reader.varint("node key");
    const std::uint64_t field = key >> 3;
    const std::uint64_t wire = key & 7;
    if (field == 1 && wire == 0) {
      node.id = reader.varint("node id");
    } else if (field == 2 && wire == 2) {
      node.name = reader.text("node name");
    } else if (field == 3 && wire == 0) {
      node.type = static_cast<ChakraNodeType>(reader.varint("node type"));
    } else if (field == 4 && wire == 0) {
      node.control_dependencies.push_back(
          reader.varint("node control dependency"));
    } else if (field == 4 && wire == 2) {
      append_packed_dependencies(
          reader.bytes("packed node control dependencies"),
          node.control_dependencies);
    } else if (field == 5 && wire == 0) {
      node.dependencies.push_back(reader.varint("node dependency"));
    } else if (field == 5 && wire == 2) {
      append_packed_dependencies(
          reader.bytes("packed node dependencies"), node.dependencies);
    } else if (field == 6 && wire == 0) {
      node.has_start_time = true;
      node.start_time_micros = reader.varint("node start time");
    } else if (field == 7 && wire == 0) {
      node.has_duration = true;
      node.duration_micros = reader.varint("node duration");
    } else if (field == 8 && wire == 2) {
      node.inputs = parse_io(reader.bytes("node inputs"));
    } else if (field == 9 && wire == 2) {
      node.outputs = parse_io(reader.bytes("node outputs"));
    } else if (field == 10 && wire == 2) {
      ChakraAttribute attribute =
          parse_attribute(reader.bytes("node attribute"));
      if (!node.attributes.emplace(attribute.name, attribute).second) {
        throw std::invalid_argument("Chakra node has duplicate attributes");
      }
    } else {
      reader.skip(wire, "node field");
    }
  }
  if (node.name.empty() || node.type == ChakraNodeType::Invalid) {
    throw std::invalid_argument("Chakra node identity or type is missing");
  }
  std::sort(node.dependencies.begin(), node.dependencies.end());
  node.dependencies.erase(
      std::unique(node.dependencies.begin(), node.dependencies.end()),
      node.dependencies.end());
  std::sort(
      node.control_dependencies.begin(), node.control_dependencies.end());
  node.control_dependencies.erase(
      std::unique(
          node.control_dependencies.begin(),
          node.control_dependencies.end()),
      node.control_dependencies.end());
  return node;
}

const ChakraAttribute* find_attribute(
    const ChakraNode& node, const char* name) {
  const auto found = node.attributes.find(name);
  return found == node.attributes.end() ? nullptr : &found->second;
}

std::uint64_t unsigned_attribute(
    const ChakraNode& node, const char* name, bool required) {
  const ChakraAttribute* attribute = find_attribute(node, name);
  if (attribute == nullptr) {
    if (required) {
      throw std::invalid_argument(
          std::string("Chakra node is missing required attribute: ") + name);
    }
    return 0;
  }
  if (!attribute->has_integer ||
      (attribute->integer_is_signed &&
       static_cast<std::int64_t>(attribute->integer) < 0)) {
    throw std::invalid_argument(
        std::string("Chakra attribute must be a nonnegative integer: ") +
        name);
  }
  return attribute->integer;
}

contracts::CollectiveKind collective_kind(std::uint64_t value) {
  switch (value) {
    case 0:
      return contracts::CollectiveKind::AllReduce;
    case 2:
      return contracts::CollectiveKind::AllGather;
    case 5:
      return contracts::CollectiveKind::Broadcast;
    case 6:
      return contracts::CollectiveKind::AllToAll;
    case 7:
    case 8:
      return contracts::CollectiveKind::ReduceScatter;
    default:
      throw std::invalid_argument(
          "Chakra collective kind is unsupported by SimAI Scale-Up");
  }
}

FineKernelGraphNodeKind graph_kind(ChakraNodeType type) {
  switch (type) {
    case ChakraNodeType::Metadata:
      return FineKernelGraphNodeKind::Metadata;
    case ChakraNodeType::MemoryLoad:
      return FineKernelGraphNodeKind::MemoryLoad;
    case ChakraNodeType::MemoryStore:
      return FineKernelGraphNodeKind::MemoryStore;
    case ChakraNodeType::Compute:
      return FineKernelGraphNodeKind::Compute;
    case ChakraNodeType::Send:
      return FineKernelGraphNodeKind::Send;
    case ChakraNodeType::Receive:
      return FineKernelGraphNodeKind::Receive;
    case ChakraNodeType::Collective:
      return FineKernelGraphNodeKind::Collective;
    case ChakraNodeType::Invalid:
      break;
  }
  throw std::invalid_argument("unknown Chakra node type");
}

const char* descriptor(ChakraNodeType type) {
  switch (type) {
    case ChakraNodeType::Metadata:
      return "chakra.metadata";
    case ChakraNodeType::MemoryLoad:
      return "chakra.memory.load";
    case ChakraNodeType::MemoryStore:
      return "chakra.memory.store";
    case ChakraNodeType::Compute:
      return "chakra.compute";
    case ChakraNodeType::Send:
      return "chakra.communication.send";
    case ChakraNodeType::Receive:
      return "chakra.communication.receive";
    case ChakraNodeType::Collective:
      return "chakra.communication.collective";
    case ChakraNodeType::Invalid:
      break;
  }
  throw std::invalid_argument("unknown Chakra node type");
}

const char* payload_attribute_name(const ChakraNode& node) {
  if (node.type == ChakraNodeType::MemoryLoad ||
      node.type == ChakraNodeType::MemoryStore) {
    return "tensor_size";
  }
  if (node.type == ChakraNodeType::Collective) {
    return "comm_size";
  }
  if (node.type == ChakraNodeType::Send ||
      node.type == ChakraNodeType::Receive) {
    return find_attribute(node, "comm_size") == nullptr
        ? "tensor_size"
        : "comm_size";
  }
  return nullptr;
}

std::uint64_t checked_nanoseconds(std::uint64_t micros) {
  if (micros > std::numeric_limits<std::uint64_t>::max() / 1000) {
    throw std::invalid_argument("Chakra node duration overflows nanoseconds");
  }
  return micros * 1000;
}

}  // namespace

FineKernelGraph import_chakra_et(
    std::istream& input, const ChakraEtImportOptions& options) {
  if (!options.device.valid() || options.maximum_message_bytes == 0) {
    throw std::invalid_argument("invalid Chakra ET import options");
  }
  const int first = input.get();
  if (first == std::char_traits<char>::eof()) {
    throw std::invalid_argument("Chakra ET is empty");
  }
  const int second = input.peek();
  input.putback(static_cast<char>(first));
  if (!input) {
    throw std::invalid_argument("could not inspect Chakra ET stream");
  }
  if (first == 0x1f && second == 0x8b) {
    throw std::invalid_argument(
        "gzip-compressed Chakra ET is not supported; provide an uncompressed ET");
  }
  std::vector<unsigned char> message;
  if (!read_message(input, options.maximum_message_bytes, message, false)) {
    throw std::invalid_argument("Chakra ET is empty");
  }
  const std::string version = parse_metadata(message);

  std::vector<ChakraNode> chakra_nodes;
  while (read_message(
      input, options.maximum_message_bytes, message, true)) {
    chakra_nodes.push_back(parse_node(message));
  }
  if (chakra_nodes.empty()) {
    throw std::invalid_argument("Chakra ET contains no nodes");
  }

  std::map<std::uint64_t, contracts::WorkloadEventId> identities;
  std::vector<std::uint64_t> source_ids;
  source_ids.reserve(chakra_nodes.size());
  for (const ChakraNode& node : chakra_nodes) source_ids.push_back(node.id);
  std::sort(source_ids.begin(), source_ids.end());
  if (std::adjacent_find(source_ids.begin(), source_ids.end()) !=
      source_ids.end()) {
    throw std::invalid_argument("Chakra ET contains duplicate node IDs");
  }
  for (std::size_t index = 0; index < source_ids.size(); ++index) {
    identities.emplace(
        source_ids[index], contracts::WorkloadEventId(index + 1));
  }

  FineKernelGraph graph{1, "chakra_et", version, {}};
  for (const ChakraNode& source : chakra_nodes) {
    FineKernelGraphNode node{};
    node.id = identities.at(source.id);
    node.source_rank = options.device.value() - 1;
    node.source_id = source.id;
    node.name = source.name;
    node.kind = graph_kind(source.type);
    node.device = options.device;
    node.descriptor = descriptor(source.type);
    node.has_start_time = source.has_start_time;
    node.start_time_ns = checked_nanoseconds(source.start_time_micros);
    node.has_duration = source.has_duration;
    node.duration_ns = checked_nanoseconds(source.duration_micros);
    node.has_collective = source.type == ChakraNodeType::Collective &&
        find_attribute(source, "comm_type") != nullptr;
    node.collective = node.has_collective
        ? collective_kind(unsigned_attribute(source, "comm_type", true))
        : contracts::CollectiveKind::AllReduce;
    const char* payload_name = payload_attribute_name(source);
    node.has_payload = payload_name != nullptr &&
        find_attribute(source, payload_name) != nullptr;
    node.payload_bytes = node.has_payload
        ? unsigned_attribute(source, payload_name, true)
        : 0;
    node.has_peer = false;
    node.peer_rank = 0;
    if (source.type == ChakraNodeType::Send) {
      node.has_peer = find_attribute(source, "comm_dst") != nullptr;
      node.peer_rank = unsigned_attribute(source, "comm_dst", false);
    } else if (source.type == ChakraNodeType::Receive) {
      node.has_peer = find_attribute(source, "comm_src") != nullptr;
      node.peer_rank = unsigned_attribute(source, "comm_src", false);
    }
    node.inputs = FineKernelGraphIo{
        source.inputs.values, source.inputs.shapes, source.inputs.types};
    node.outputs = FineKernelGraphIo{
        source.outputs.values, source.outputs.shapes, source.outputs.types};
    for (const auto& entry : source.attributes) {
      const ChakraAttribute& attribute = entry.second;
      node.attributes.push_back(FineKernelGraphAttribute{
          attribute.name,
          attribute.has_integer,
          attribute.integer,
          attribute.integer_is_signed,
          attribute.string,
          attribute.protobuf_hex});
    }
    node.control_dependencies = source.control_dependencies;
    for (std::uint64_t dependency : source.dependencies) {
      const auto found = identities.find(dependency);
      if (found == identities.end()) {
        throw std::invalid_argument(
            "Chakra ET dependency references an absent node");
      }
      node.dependencies.push_back(found->second);
    }
    graph.nodes.push_back(node);
  }
  std::sort(
      graph.nodes.begin(), graph.nodes.end(),
      [](const FineKernelGraphNode& lhs, const FineKernelGraphNode& rhs) {
        return lhs.id < rhs.id;
      });
  graph.require_valid();
  return graph;
}

FineKernelGraph import_chakra_et_file(
    const std::string& path, const ChakraEtImportOptions& options) {
  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) {
    throw std::invalid_argument("could not open Chakra ET file: " + path);
  }
  return import_chakra_et(input, options);
}

FineKernelGraph import_chakra_et_files(
    const std::vector<ChakraEtRankTrace>& traces,
    std::uint64_t maximum_message_bytes) {
  if (traces.empty() || maximum_message_bytes == 0) {
    throw std::invalid_argument("multi-rank Chakra import requires traces");
  }
  std::vector<FineKernelGraph> graphs;
  graphs.reserve(traces.size());
  std::set<contracts::DeviceId> devices;
  for (const ChakraEtRankTrace& trace : traces) {
    if (trace.path.empty() || !trace.device.valid() ||
        !devices.insert(trace.device).second) {
      throw std::invalid_argument(
          "multi-rank Chakra trace paths and devices must be unique");
    }
    graphs.push_back(import_chakra_et_file(
        trace.path,
        ChakraEtImportOptions{trace.device, maximum_message_bytes}));
  }
  return merge_fine_kernel_graphs(graphs);
}

}  // namespace runtime
}  // namespace simai
