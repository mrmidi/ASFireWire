#include "MetricsSink.hpp"

#include <string>
#include <string_view>

namespace ASFW::Driver {

MetricsSink::MetricsSink() = default;

void MetricsSink::Increment(std::string_view key) {
    counters_[std::string(key)] += 1;
}

void MetricsSink::SetGauge(std::string_view key, uint64_t value) {
    counters_[std::string(key)] = value;
}

} // namespace ASFW::Driver

