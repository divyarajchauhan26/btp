#ifndef SIMAI_RUNTIME_REPORT_WRITER_HPP
#define SIMAI_RUNTIME_REPORT_WRITER_HPP

#include <string>

#include "simai/contracts/report.hpp"

namespace simai {
namespace runtime {

std::string format_report(const contracts::SimulationReport& report);

}  // namespace runtime
}  // namespace simai

#endif
