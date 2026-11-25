#pragma once
#include <ArduinoJson.h>
#include "dashboard_config.h"
namespace esphome::dashboard_config {
uint32_t catalog_id(JsonVariantConst value);
bool classify_device(JsonObjectConst detail, CompiledDevice *out, std::string *reason);
bool parse_registry(JsonArrayConst array, std::vector<CompiledDevice> *out);
void write_registry(JsonArray array, const std::vector<CompiledDevice> &devices);
}
