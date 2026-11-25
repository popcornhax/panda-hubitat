#include "device_catalog.h"
#include "layout_codec.h"
#include <cctype>
#include <unordered_set>
namespace esphome::dashboard_config {
namespace {
std::string normalized(std::string value) {
  std::string result;
  for (unsigned char c : value) if (std::isalnum(c)) result += static_cast<char>(std::tolower(c));
  return result;
}
bool named(JsonVariantConst values, const char *name) {
  if (!values.is<JsonArrayConst>()) return false;
  for (JsonVariantConst value : values.as<JsonArrayConst>()) {
    const char *text = value.is<const char *>() ? value.as<const char *>() :
                       value["name"].as<const char *>();
    if (text == nullptr && value.is<JsonObjectConst>()) text = value["command"].as<const char *>();
    if (text != nullptr && normalized(text) == normalized(name)) return true;
  }
  return false;
}
bool attribute(JsonObjectConst object, const char *name) {
  auto attrs = object["attributes"];
  if (attrs.is<JsonObjectConst>()) return !attrs[name].isNull();
  return named(attrs, name);
}
bool supported_kind(const std::string &kind) {
  return kind == "switch" || kind == "dimmer" || kind == "motion" || kind == "presence" ||
         kind == "water" || kind == "weather" || kind == "contact" || kind == "sensor";
}
}
uint32_t catalog_id(JsonVariantConst value) {
  if (value.is<bool>()) return 0;
  if (value.is<uint32_t>()) return value.as<uint32_t>();
  const char *text = value.as<const char *>();
  if (text == nullptr || !*text) return 0;
  uint64_t id = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return 0;
    id = id * 10 + (*text - '0');
    if (id > UINT32_MAX) return 0;
  }
  return static_cast<uint32_t>(id);
}
bool classify_device(JsonObjectConst detail, CompiledDevice *out, std::string *reason) {
  *out = CompiledDevice{};
  out->id = catalog_id(detail["id"]);
  if (out->id == 0) { *reason = "Invalid device ID"; return false; }
  const char *label = detail["label"].as<const char *>();
  if (label == nullptr || !*label) label = detail["name"].as<const char *>();
  if (label != nullptr) {
    bool gap = false;
    for (unsigned char c : std::string(label)) {
      if (out->label.size() >= 32) break;
      if (c >= 33 && c <= 126) { out->label += static_cast<char>(c); gap = false; }
      else if (!gap && !out->label.empty()) { out->label += ' '; gap = true; }
    }
    while (!out->label.empty() && out->label.back() == ' ') out->label.pop_back();
  }
  if (out->label.empty()) out->label = "Device " + std::to_string(out->id);
  const std::string type = normalized(detail["type"] | "");
  auto caps = detail["capabilities"], commands = detail["commands"];
  out->motion_capable = named(caps, "MotionSensor") || attribute(detail, "motion");
  for (const char *risky : {"lock", "garagedoorcontrol", "doorcontrol", "windowshade", "valve", "thermostat", "fancontrol",
                           "smokedetector", "carbonmonoxidedetector", "smokesensor", "carbonmonoxidesensor"}) {
    if (named(caps, risky) || type.find(risky) != std::string::npos) {
      *reason = "Unsupported device type"; return false;
    }
  }
  if (attribute(detail, "forecastHigh") || attribute(detail, "condition_text") || type.find("weather") != std::string::npos)
    out->kind = "weather";
  else if (named(caps, "PresenceSensor")) out->kind = "presence";
  else if (named(caps, "MotionSensor") || attribute(detail, "motion")) out->kind = type.find("presence") != std::string::npos ? "presence" : "motion";
  else if (named(caps, "ContactSensor")) out->kind = "contact";
  else if (named(caps, "WaterSensor") || named(caps, "LeakSensor")) out->kind = "water";
  else if (named(caps, "Switch") || named(caps, "SwitchLevel")) {
    const std::string identity = type + normalized(out->label);
    for (const char *risky : {"lock", "garage", "heater", "heating", "oven", "stove", "valve", "pump", "siren", "alarm", "blind", "shade"})
      if (identity.find(risky) != std::string::npos) { *reason = "Unsupported actuator"; return false; }
    for (const char *risky : {"lock", "unlock", "open", "close", "arm", "disarm", "setPosition", "setValvePosition"})
      if (named(commands, risky)) { *reason = "Unsupported actuator commands"; return false; }
    if (!named(commands, "on") || !named(commands, "off")) { *reason = "Missing on/off commands"; return false; }
    out->kind = named(caps, "SwitchLevel") ? "dimmer" : "switch";
    if (out->kind == "dimmer" && !named(commands, "setLevel")) { *reason = "Missing setLevel command"; return false; }
    out->command_mask = out->kind == "dimmer" ? 7 : 3;
  } else if (attribute(detail, "temperature") || attribute(detail, "humidity") || attribute(detail, "illuminance") ||
             attribute(detail, "power") || attribute(detail, "voltage")) out->kind = "sensor";
  else { *reason = "No supported readings or controls"; return false; }
  reason->clear(); return true;
}
bool parse_registry(JsonArrayConst array, std::vector<CompiledDevice> *out) {
  if (array.isNull() || array.size() < 1 || array.size() > 32) return false;
  std::vector<CompiledDevice> parsed; std::unordered_set<uint32_t> seen;
  for (JsonObjectConst item : array) {
    CompiledDevice device;
    device.id = catalog_id(item["id"]);
    if (!item["label"].is<const char *>() || !item["kind"].is<const char *>() ||
        !item["favorite"].is<bool>() || !item["command_mask"].is<uint32_t>() || item["command_mask"].is<bool>()) return false;
    device.label = item["label"].as<std::string>(); device.kind = item["kind"].as<std::string>();
    device.motion_capable = item["motion_capable"] | (device.kind == "motion");
    device.favorite = item["favorite"].as<bool>(); device.command_mask = item["command_mask"].as<uint32_t>();
    const uint32_t expected = device.kind == "switch" ? 3 : device.kind == "dimmer" ? 7 : 0;
    if (!device.id || !seen.insert(device.id).second || !supported_kind(device.kind) ||
        !valid_ascii_label(device.label.c_str(), 1, 32) || device.label.find('\0') != std::string::npos ||
        device.command_mask != expected) return false;
    parsed.push_back(std::move(device));
  }
  *out = std::move(parsed); return true;
}
void write_registry(JsonArray array, const std::vector<CompiledDevice> &devices) {
  for (const auto &device : devices) {
    auto item = array.add<JsonObject>(); item["id"] = device.id; item["label"] = device.label;
    item["kind"] = device.kind; item["favorite"] = device.favorite; item["command_mask"] = device.command_mask; item["motion_capable"] = device.motion_capable;
  }
}
}
