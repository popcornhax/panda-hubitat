#include "layout_codec.h"

#include <cstring>
#include <unordered_set>

#include <ArduinoJson.h>

namespace esphome::dashboard_config {
namespace {

void fail_(std::string *error, const char *message) {
  if (error != nullptr) *error = message;
}

bool only_keys_(JsonObjectConst object, const char *const *allowed, size_t count) {
  for (JsonPairConst pair : object) {
    bool found = false;
    for (size_t i = 0; i < count; ++i)
      if (pair.key() == allowed[i]) { found = true; break; }
    if (!found) return false;
  }
  return true;
}

const CompiledDevice *find_device_(const std::vector<CompiledDevice> &devices, uint32_t id) {
  for (const auto &device : devices) if (device.id == id) return &device;
  return nullptr;
}

bool color_(JsonVariantConst value, uint32_t *out) {
  if (!value.is<uint32_t>() || value.is<bool>()) return false;
  const uint32_t color = value.as<uint32_t>();
  if (color > 0xFFFFFF) return false;
  *out = color;
  return true;
}

bool string_value_(JsonVariantConst value, const char **out, size_t min_length, size_t max_length) {
  if (!value.is<JsonString>()) return false;
  const JsonString string = value.as<JsonString>();
  const char *text = string.c_str();
  if (text == nullptr || std::strlen(text) != string.size() || !valid_ascii_label(text, min_length, max_length))
    return false;
  *out = text;
  return true;
}

bool one_json_value_(const uint8_t *data, size_t length) {
  size_t pos = 0;
  while (pos < length && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r' || data[pos] == '\n')) ++pos;
  if (pos == length || data[pos] != '{') return false;
  int depth = 0;
  bool in_string = false, escaped = false, complete = false;
  for (; pos < length; ++pos) {
    const uint8_t ch = data[pos];
    if (in_string) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') in_string = false;
      continue;
    }
    if (ch == '"') in_string = true;
    else if (ch == '{' || ch == '[') ++depth;
    else if (ch == '}' || ch == ']') {
      if (--depth < 0) return false;
      if (depth == 0) { ++pos; complete = true; break; }
    }
  }
  if (!complete || in_string || depth != 0) return false;
  for (; pos < length; ++pos)
    if (data[pos] != ' ' && data[pos] != '\t' && data[pos] != '\r' && data[pos] != '\n') return false;
  return true;
}

}  // namespace

bool valid_ascii_label(const char *value, size_t min_length, size_t max_length) {
  if (value == nullptr) return false;
  const size_t length = std::strlen(value);
  if (length < min_length || length > max_length) return false;
  for (size_t i = 0; i < length; ++i)
    if (static_cast<unsigned char>(value[i]) < 0x20 || static_cast<unsigned char>(value[i]) > 0x7E)
      return false;
  return true;
}

bool parse_layout_json(const uint8_t *data, size_t length, const std::vector<CompiledDevice> &devices,
                       DashboardLayout *out, std::string *error) {
  if (data == nullptr || out == nullptr || length == 0 || length > MAX_LAYOUT_BYTES || !one_json_value_(data, length)) {
    fail_(error, "layout size is invalid"); return false;
  }
  JsonDocument doc;
  const auto parse_error = deserializeJson(doc, data, length, DeserializationOption::NestingLimit(6));
  if (parse_error || !doc.is<JsonObject>()) { fail_(error, "invalid JSON object"); return false; }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  static const char *const root_keys[] = {"version", "theme", "font", "pages", "labels", "sleep"};
  if (!only_keys_(root, root_keys, 6) || (root.size() != 5 && root.size() != 6) || !root["version"].is<uint32_t>() ||
      root["version"].is<bool>() || root["version"].as<uint32_t>() != 1 ||
      !root["theme"].is<JsonObjectConst>() || !root["pages"].is<JsonArrayConst>() ||
      !root["labels"].is<JsonArrayConst>()) { fail_(error, "unknown or missing top-level field"); return false; }

  DashboardLayout parsed;
  JsonObjectConst theme = root["theme"].as<JsonObjectConst>();
  static const char *const theme_keys[] = {"background", "panel", "text", "muted", "accent", "active", "updating", "warning", "info"};
  if (!(theme.size() == 5 || theme.size() == 9) || !only_keys_(theme, theme_keys, 9) ||
      !color_(theme["background"], &parsed.background) || !color_(theme["panel"], &parsed.panel) ||
      !color_(theme["text"], &parsed.text) || !color_(theme["muted"], &parsed.muted) ||
      !color_(theme["accent"], &parsed.accent)) { fail_(error, "theme colors must be 24-bit integers"); return false; }
  if (theme.size() == 9 && (!color_(theme["active"], &parsed.active) || !color_(theme["updating"], &parsed.updating) ||
      !color_(theme["warning"], &parsed.warning) || !color_(theme["info"], &parsed.info))) {
    fail_(error, "semantic theme colors must be 24-bit integers"); return false;
  }
  const char *font = nullptr;
  if (!string_value_(root["font"], &font, 1, 6) ||
      (strcmp(font, "normal") != 0 && strcmp(font, "large") != 0)) {
    fail_(error, "font must be normal or large"); return false;
  }
  parsed.font = strcmp(font, "large") == 0 ? DashboardFont::LARGE : DashboardFont::NORMAL;

  JsonArrayConst pages = root["pages"].as<JsonArrayConst>();
  if (pages.size() < 1 || pages.size() > 8) { fail_(error, "pages must contain 1 to 8 pages"); return false; }
  for (JsonVariantConst item : pages) {
    if (!item.is<JsonObjectConst>()) { fail_(error, "page must be an object"); return false; }
    JsonObjectConst page_obj = item.as<JsonObjectConst>();
    static const char *const page_keys[] = {"label", "title", "devices"};
    if (!only_keys_(page_obj, page_keys, 3) || page_obj.size() != 3 || !page_obj["devices"].is<JsonArrayConst>()) {
      fail_(error, "unknown or missing page field"); return false;
    }
    const char *label = nullptr;
    const char *title = nullptr;
    JsonArrayConst ids = page_obj["devices"].as<JsonArrayConst>();
    if (!string_value_(page_obj["label"], &label, 1, 10) ||
        !string_value_(page_obj["title"], &title, 1, 24) || ids.size() > 6) {
      fail_(error, "page label, title, or device count is invalid (maximum six)"); return false;
    }
    for (const auto &existing : parsed.pages)
      if (existing.label == label) { fail_(error, "page labels must be unique"); return false; }
    DashboardPage page; page.label = label; page.title = title;
    std::unordered_set<uint32_t> page_ids;
    for (JsonVariantConst id_value : ids) {
      if (!id_value.is<uint32_t>() || id_value.is<bool>()) { fail_(error, "device IDs must be positive integers"); return false; }
      const uint32_t id = id_value.as<uint32_t>();
      if (find_device_(devices, id) == nullptr || !page_ids.insert(id).second) {
        fail_(error, "device ID is unknown or duplicated on this page"); return false;
      }
      page.devices.push_back(id);
    }
    parsed.pages.push_back(std::move(page));
  }

  JsonArrayConst labels = root["labels"].as<JsonArrayConst>();
  if (labels.size() > 32) { fail_(error, "at most 32 label overrides are allowed"); return false; }
  std::unordered_set<uint32_t> labelled;
  for (JsonVariantConst item : labels) {
    if (!item.is<JsonObjectConst>()) { fail_(error, "label override must be an object"); return false; }
    JsonObjectConst entry = item.as<JsonObjectConst>();
    static const char *const label_keys[] = {"id", "label"};
    if (!only_keys_(entry, label_keys, 2) || entry.size() != 2 || !entry["id"].is<uint32_t>() || entry["id"].is<bool>()) {
      fail_(error, "unknown or missing label override field"); return false;
    }
    const uint32_t id = entry["id"].as<uint32_t>();
    const char *label = nullptr;
    if (find_device_(devices, id) == nullptr || !labelled.insert(id).second ||
        !string_value_(entry["label"], &label, 1, 32)) {
      fail_(error, "label override is invalid"); return false;
    }
    parsed.labels.push_back({id, label});
  }
  if (root.containsKey("sleep")) {
    JsonObjectConst sleep = root["sleep"].as<JsonObjectConst>();
    static const char *const keys[] = {"enabled", "motion_device_id", "timeout_seconds", "fade_ms"};
    if (sleep.isNull() || sleep.size() != 4 || !only_keys_(sleep, keys, 4) || !sleep["enabled"].is<bool>()) {
      fail_(error, "invalid sleep settings"); return false;
    }
    for (const char *key : {"motion_device_id", "timeout_seconds", "fade_ms"})
      if (!sleep[key].is<uint32_t>() || sleep[key].is<bool>()) { fail_(error, "sleep values must be integers"); return false; }
    parsed.sleep.enabled = sleep["enabled"].as<bool>();
    parsed.sleep.motion_device_id = sleep["motion_device_id"].as<uint32_t>();
    parsed.sleep.timeout_seconds = sleep["timeout_seconds"].as<uint32_t>();
    parsed.sleep.fade_ms = sleep["fade_ms"].as<uint32_t>();
    if (parsed.sleep.timeout_seconds < 30 || parsed.sleep.timeout_seconds > 3600 || parsed.sleep.fade_ms > 3000) {
      fail_(error, "sleep timeout must be 30-3600 seconds; fade 0-3000 ms"); return false;
    }
    const auto *motion = find_device_(devices, parsed.sleep.motion_device_id);
    if (parsed.sleep.enabled && (motion == nullptr || !motion->motion_capable || (motion->kind != "motion" && motion->kind != "presence"))) {
      fail_(error, "select an imported motion/presence sensor for wake"); return false;
    }
  }
  *out = std::move(parsed);
  if (error != nullptr) error->clear();
  return true;
}

std::string serialize_layout_json(const DashboardLayout &layout, const std::vector<CompiledDevice> &devices) {
  JsonDocument doc;
  doc["version"] = 1;
  JsonObject theme = doc["theme"].to<JsonObject>();
  theme["background"] = layout.background; theme["panel"] = layout.panel; theme["text"] = layout.text;
  theme["muted"] = layout.muted; theme["accent"] = layout.accent;
  theme["active"] = layout.active; theme["updating"] = layout.updating;
  theme["warning"] = layout.warning; theme["info"] = layout.info;
  doc["font"] = layout.font == DashboardFont::LARGE ? "large" : "normal";
  JsonArray pages = doc["pages"].to<JsonArray>();
  for (const auto &page : layout.pages) {
    JsonObject entry = pages.add<JsonObject>();
    entry["label"] = page.label; entry["title"] = page.title;
    JsonArray ids = entry["devices"].to<JsonArray>();
    for (uint32_t id : page.devices) if (find_device_(devices, id) != nullptr) ids.add(id);
  }
  JsonArray labels = doc["labels"].to<JsonArray>();
  for (const auto &label : layout.labels) if (find_device_(devices, label.device_id) != nullptr) {
    JsonObject item = labels.add<JsonObject>(); item["id"] = label.device_id; item["label"] = label.label;
  }
  JsonObject sleep = doc["sleep"].to<JsonObject>();
  sleep["enabled"] = layout.sleep.enabled;
  sleep["motion_device_id"] = layout.sleep.motion_device_id;
  sleep["timeout_seconds"] = layout.sleep.timeout_seconds;
  sleep["fade_ms"] = layout.sleep.fade_ms;
  std::string result; serializeJson(doc, result); return result;
}

}  // namespace esphome::dashboard_config
