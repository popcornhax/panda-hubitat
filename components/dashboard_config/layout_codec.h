#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "dashboard_config.h"

namespace esphome::dashboard_config {

constexpr size_t MAX_LAYOUT_BYTES = 16384;
bool parse_layout_json(const uint8_t *data, size_t length, const std::vector<CompiledDevice> &devices,
                       DashboardLayout *out, std::string *error);
std::string serialize_layout_json(const DashboardLayout &layout, const std::vector<CompiledDevice> &devices);
bool valid_ascii_label(const char *value, size_t min_length, size_t max_length);

}  // namespace esphome::dashboard_config
