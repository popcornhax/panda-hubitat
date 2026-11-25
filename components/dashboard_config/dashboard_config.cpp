#include "dashboard_config.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "layout_codec.h"
#include "device_catalog.h"
#include "esphome/components/hubitat_local/hubitat_local.h"
#include <unordered_set>
#include "web_ui.h"

namespace esphome::dashboard_config {
namespace {

constexpr size_t BACKGROUND_BYTES = 400U * 240U * 2U;
constexpr size_t SCREENSHOT_BYTES = 800U * 480U * 2U;
constexpr uint32_t REBOOT_DELAY_MS = 1500;
static const char *const TAG = "dashboard_config";

bool receive_body_(httpd_req_t *request, uint8_t *data, size_t capacity) {
  if (request->content_len > capacity) return false;
  const int64_t started = esp_timer_get_time();
  size_t received = 0;
  while (received < request->content_len) {
    const int amount = httpd_req_recv(request, reinterpret_cast<char *>(data + received),
                                      request->content_len - received);
    if (amount <= 0) return false;
    received += static_cast<size_t>(amount);
    if (esp_timer_get_time() - started > 10000000) return false;
  }
  return received == request->content_len;
}

bool header_(httpd_req_t *request, const char *name, char *value, size_t capacity) {
  if (httpd_req_get_hdr_value_len(request, name) == 0 || capacity < 2) return false;
  return httpd_req_get_hdr_value_str(request, name, value, capacity) == ESP_OK;
}

bool allowed_host_(const char *host) {
  if (host == nullptr) return false;
  std::string value(host);
  if (value == "panda-touch" || value == "panda-touch:80" || value == "panda-touch.local" ||
      value == "panda-touch.local:80") return true;
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t info{};
  if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
  char ip[24]{};
  snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
  return value == ip || value == std::string(ip) + ":80";
}

bool request_allowed_(httpd_req_t *request, bool write) {
  char host[96]{}, origin[128]{}, write_header[8]{};
  if (!header_(request, "Host", host, sizeof(host)) || !allowed_host_(host)) return false;
  if (!write) return true;
  if (!header_(request, "Origin", origin, sizeof(origin)) ||
      !header_(request, "X-Dashboard-Request", write_header, sizeof(write_header)) ||
      strcmp(write_header, "1") != 0) return false;
  char expected[112]{};
  snprintf(expected, sizeof(expected), "http://%s", host);
  return strcmp(origin, expected) == 0;
}

void put_u16_(uint8_t *out, uint16_t value) { out[0] = value & 0xFF; out[1] = value >> 8; }
void put_u32_(uint8_t *out, uint32_t value) {
  out[0] = value & 0xFF; out[1] = (value >> 8) & 0xFF;
  out[2] = (value >> 16) & 0xFF; out[3] = (value >> 24) & 0xFF;
}

esp_err_t forbidden_(httpd_req_t *request) {
  httpd_resp_set_status(request, "403 Forbidden");
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  return httpd_resp_sendstr(request, "Request rejected");
}

esp_err_t message_(httpd_req_t *request, const char *status, const char *message) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  return httpd_resp_sendstr(request, message);
}

}  // namespace

bool DashboardConfig::add_device(uint32_t id, const std::string &label, const std::string &kind, bool favorite) {
  if (id == 0 || label.empty() || label.size() > 32 || devices_.size() >= 32) return false;
  for (const auto &device : devices_) if (device.id == id) return false;
  devices_.push_back({id, label, kind, favorite, kind == "switch" ? 3U : kind == "dimmer" ? 7U : 0U, kind == "motion" || kind == "presence"});
  return true;
}

void DashboardConfig::make_default_() {
  layout_ = DashboardLayout{};
  DashboardPage home{"Home", "Good day", {}};
  DashboardPage lights{"Lights", "Lights", {}};
  DashboardPage sensors{"Sensors", "Climate & sensors", {}};
  size_t favorites = 0;
  for (const auto &device : devices_) {
    if (device.favorite && favorites < 6) { home.devices.push_back(device.id); ++favorites; }
    if (device.kind == "switch" || device.kind == "dimmer") lights.devices.push_back(device.id);
    else sensors.devices.push_back(device.id);
  }
  layout_.pages.push_back(std::move(home));
  layout_.pages.push_back(std::move(lights));
  layout_.pages.push_back(std::move(sensors));
}

bool DashboardConfig::load_storage_() {
  nvs_handle_t handle;
  if (nvs_open("dashcfg", NVS_READWRITE, &handle) != ESP_OK) return false;
  size_t length = 0;
  bool bundle_loaded = false;
  if (nvs_get_blob(handle, "bundle", nullptr, &length) == ESP_OK && length > 0 && length <= 32768) {
    std::string data(length, '\0');
    if (nvs_get_blob(handle, "bundle", data.data(), &length) == ESP_OK) {
      JsonDocument doc;
      std::vector<CompiledDevice> registry;
      DashboardLayout layout;
      std::string json, error;
      if (!deserializeJson(doc, data, DeserializationOption::NestingLimit(9)) && doc["version"].as<int>() == 1 &&
          parse_registry(doc["devices"].as<JsonArrayConst>(), &registry)) {
        serializeJson(doc["layout"], json);
        if (parse_layout_json(reinterpret_cast<const uint8_t *>(json.data()), json.size(), registry, &layout, &error)) {
          devices_ = std::move(registry); layout_ = std::move(layout); bundle_loaded = true;
        }
      }
      if (!bundle_loaded) ESP_LOGW(TAG, "Saved device/layout bundle invalid; using seed fallback");
    }
  }
  length = 0;
  esp_err_t err = nvs_get_blob(handle, "layout", nullptr, &length);
  if (!bundle_loaded && err == ESP_OK && length > 0 && length <= MAX_LAYOUT_BYTES) {
    auto *data = new (std::nothrow) uint8_t[length];
    if (data != nullptr && nvs_get_blob(handle, "layout", data, &length) == ESP_OK) {
      DashboardLayout parsed;
      std::string error;
      if (parse_layout_json(data, length, devices_, &parsed, &error)) layout_ = std::move(parsed);
      else ESP_LOGW(TAG, "Stored layout ignored: %s", error.c_str());
    }
    delete[] data;
  }
  length = 0;
  err = nvs_get_blob(handle, "wall", nullptr, &length);
  if (err == ESP_OK && length == BACKGROUND_BYTES) {
    background_data_ = static_cast<uint8_t *>(heap_caps_malloc(BACKGROUND_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (background_data_ == nullptr || nvs_get_blob(handle, "wall", background_data_, &length) != ESP_OK) {
      if (background_data_ != nullptr) heap_caps_free(background_data_);
      background_data_ = nullptr;
    }
  }
  nvs_close(handle);
  return true;
}

bool DashboardConfig::store_config_(const std::string &json) {
  return store_bundle_(devices_, json);
}

bool DashboardConfig::store_bundle_(const std::vector<CompiledDevice> &devices, const std::string &layout) {
  JsonDocument doc, parsed;
  if (deserializeJson(parsed, layout, DeserializationOption::NestingLimit(8))) return false;
  doc["version"] = 1;
  write_registry(doc["devices"].to<JsonArray>(), devices);
  doc["layout"].set(parsed.as<JsonVariantConst>());
  std::string bundle; serializeJson(doc, bundle);
  if (doc.overflowed() || bundle.size() > 32768) return false;
  nvs_handle_t handle;
  if (nvs_open("dashcfg", NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t saved = nvs_set_blob(handle, "bundle", bundle.data(), bundle.size());
  if (saved == ESP_OK) saved = nvs_commit(handle);
  nvs_close(handle);
  return saved == ESP_OK;
}

bool DashboardConfig::store_background_(const uint8_t *data, size_t len) {
  nvs_handle_t handle;
  if (nvs_open("dashcfg", NVS_READWRITE, &handle) != ESP_OK) return false;
  const esp_err_t saved = nvs_set_blob(handle, "wall", data, len);
  const esp_err_t committed = saved == ESP_OK ? nvs_commit(handle) : saved;
  nvs_close(handle);
  return committed == ESP_OK;
}

bool DashboardConfig::start_server_() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.max_uri_handlers = 14;
  config.max_open_sockets = 2;
  config.recv_wait_timeout = 3;
  config.send_wait_timeout = 3;
  config.lru_purge_enable = true;
  httpd_handle_t handle = nullptr;
  if (httpd_start(&handle, &config) != ESP_OK) return false;
  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = root_handler_, .user_ctx = this},
      {.uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler_, .user_ctx = this},
      {.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler_, .user_ctx = this},
      {.uri = "/api/devices", .method = HTTP_GET, .handler = devices_get_handler_, .user_ctx = this},
      {.uri = "/api/discovery", .method = HTTP_GET, .handler = discovery_handler_, .user_ctx = this},
      {.uri = "/api/provision", .method = HTTP_POST, .handler = provision_handler_, .user_ctx = this},
      {.uri = "/api/display", .method = HTTP_GET, .handler = display_status_handler_, .user_ctx = this},
      {.uri = "/api/display/wake", .method = HTTP_POST, .handler = wake_handler_, .user_ctx = this},
      {.uri = "/api/screen.bmp", .method = HTTP_GET, .handler = screenshot_get_handler_, .user_ctx = this},
      {.uri = "/api/background", .method = HTTP_GET, .handler = background_get_handler_, .user_ctx = this},
      {.uri = "/api/background", .method = HTTP_POST, .handler = background_post_handler_, .user_ctx = this},
      {.uri = "/api/background", .method = HTTP_DELETE, .handler = background_delete_handler_, .user_ctx = this},
      {.uri = "/api/reset", .method = HTTP_POST, .handler = reset_handler_, .user_ctx = this},
  };
  for (const auto &route : routes) {
    if (httpd_register_uri_handler(handle, &route) != ESP_OK) { httpd_stop(handle); return false; }
  }
  server_ = handle;
  return true;
}

void DashboardConfig::setup() {

  // A clean restart clears this marker in on_safe_shutdown(). If startup
  // crashes while drawing wallpaper, preserve the layout and recover with its
  // theme color. Power cycles do not invalidate an otherwise working image.
  nvs_handle_t handle;
  if (nvs_open("dashcfg", NVS_READWRITE, &handle) == ESP_OK) {
    uint8_t pending = 0;
    const auto reason = esp_reset_reason();
    const bool crashed = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
                         reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
    size_t wall_size = 0;
    if (crashed && nvs_get_u8(handle, "boot_pending", &pending) == ESP_OK && pending != 0 &&
        nvs_get_blob(handle, "wall", nullptr, &wall_size) == ESP_OK) {
      nvs_erase_key(handle, "wall");
      nvs_commit(handle);
      ESP_LOGE(TAG, "Previous wallpaper startup failed; using theme color");
    }
    nvs_close(handle);
  }
  boot_guard_armed_ = set_boot_pending_(true);
  boot_started_at_ = millis();
  make_default_();
  if (!load_storage_()) ESP_LOGW(TAG, "NVS unavailable; using compiled default layout");
  if (transport_ == nullptr || !transport_->clear_devices_before_setup()) {
    ESP_LOGE(TAG, "Device registry must load before transport worker setup"); mark_failed(); return;
  }
  for (const auto &device : devices_) {
    transport_->add_device_id(device.id);
    if (device.command_mask & 1) transport_->add_allowed_command(device.id, hubitat_local::ON);
    if (device.command_mask & 2) transport_->add_allowed_command(device.id, hubitat_local::OFF);
    if (device.command_mask & 4) transport_->add_allowed_command(device.id, hubitat_local::SET_LEVEL);
  }
}

void DashboardConfig::loop() {
  if (wake_requested_.exchange(false) && wake_callback_) wake_callback_();
  this->capture_screenshot_();
  if (boot_guard_armed_ && millis() - boot_started_at_ >= 30000 && set_boot_pending_(false))
    boot_guard_armed_ = false;
  if (server_ == nullptr && wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected() &&
      (last_server_retry_ == 0 || millis() - last_server_retry_ >= 10000)) {
    last_server_retry_ = millis();
    if (!start_server_()) ESP_LOGW(TAG, "Local layout editor start failed; retrying");
  }
  if (reboot_pending_.exchange(false, std::memory_order_acq_rel)) reboot_at_ = millis() + REBOOT_DELAY_MS;
  if (reboot_at_ != 0 && static_cast<int32_t>(millis() - reboot_at_) >= 0) {
    reboot_at_ = 0;
    App.safe_reboot();
  }
}

void DashboardConfig::capture_screenshot_() {
  const auto requested = this->screenshot_state_.load(std::memory_order_acquire);
  if (requested != ScreenshotState::REQUESTED && requested != ScreenshotState::REQUESTED_WEATHER) return;
  if (this->screenshot_pixels_ == nullptr) {
    this->screenshot_pixels_ = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        32, SCREENSHOT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (this->screenshot_pixels_ == nullptr || !this->screenshot_callback_ ||
      !this->screenshot_callback_(this->screenshot_pixels_, SCREENSHOT_BYTES, requested == ScreenshotState::REQUESTED_WEATHER)) {
    this->screenshot_state_.store(ScreenshotState::FAILED, std::memory_order_release);
    return;
  }
  this->screenshot_state_.store(ScreenshotState::READY, std::memory_order_release);
}

bool DashboardConfig::set_boot_pending_(bool pending) {
  nvs_handle_t handle;
  if (nvs_open("dashcfg", NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t result = nvs_set_u8(handle, "boot_pending", pending ? 1 : 0);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK;
}

void DashboardConfig::on_safe_shutdown() {
  if (boot_guard_armed_) set_boot_pending_(false);
}

esp_err_t DashboardConfig::root_handler_(httpd_req_t *request) {
  if (!request_allowed_(request, false)) return forbidden_(request);
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, WEB_UI, HTTPD_RESP_USE_STRLEN);
}

esp_err_t DashboardConfig::config_get_handler_(httpd_req_t *request) {
  if (!request_allowed_(request, false)) return forbidden_(request);
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  const std::string json = serialize_layout_json(self->layout_, self->devices_);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, json.c_str(), json.size());
}

esp_err_t DashboardConfig::devices_get_handler_(httpd_req_t *request) {
  if (!request_allowed_(request, false)) return forbidden_(request);
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  JsonDocument doc; JsonArray devices = doc.to<JsonArray>();
  for (const auto &device : self->devices_) {
    JsonObject item = devices.add<JsonObject>(); item["id"] = device.id; item["label"] = device.label;
    item["kind"] = device.kind; item["favorite"] = device.favorite; item["motion_capable"] = device.motion_capable;
  }
  std::string body; serializeJson(doc, body);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t DashboardConfig::discovery_handler_(httpd_req_t *request) {
  if (!request_allowed_(request, false)) return forbidden_(request);
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  std::string raw;
  if (!self->transport_ || !self->transport_->fetch_metadata(0, raw))
    return message_(request, "502 Bad Gateway", "Maker API discovery failed; existing configuration unchanged");
  JsonDocument listing, output;
  if (deserializeJson(listing, raw) || !listing.is<JsonArray>() || listing.size() > 64)
    return message_(request, "502 Bad Gateway", "Expected a Maker API list of at most 64 devices");
  auto array = output.to<JsonArray>();
  const int64_t started = esp_timer_get_time();
  std::unordered_set<uint32_t> ids;
  for (JsonObjectConst item : listing.as<JsonArrayConst>()) {
    const uint32_t id = catalog_id(item["id"]);
    if (!id || !ids.insert(id).second) return message_(request, "502 Bad Gateway", "Invalid Maker API device list");
    if (esp_timer_get_time() - started > 45000000)
      return message_(request, "504 Gateway Timeout", "Discovery timed out; retry when Hubitat is available");
    std::string detail; JsonDocument doc; CompiledDevice device; std::string reason;
    if (!self->transport_->fetch_metadata(id, detail) || deserializeJson(doc, detail) || catalog_id(doc["id"]) != id)
      return message_(request, "502 Bad Gateway", "Could not inspect all Maker API devices; retry");
    const bool supported = classify_device(doc.as<JsonObjectConst>(), &device, &reason);
    bool selected = false;
    for (const auto &existing : self->devices_) if (existing.id == id) selected = true;
    auto row = array.add<JsonObject>(); row["id"] = id; row["label"] = device.label;
    row["kind"] = supported ? device.kind : "unsupported"; row["supported"] = supported;
    row["reason"] = reason; row["selected"] = selected; row["motion_capable"] = device.motion_capable;
    if (output.overflowed()) return message_(request, "503 Service Unavailable", "Not enough memory for discovery");
  }
  std::string body; serializeJson(output, body);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t DashboardConfig::provision_handler_(httpd_req_t *request) {
  if (!request_allowed_(request, true)) return forbidden_(request);
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  if (self->configuration_pending_.load()) return message_(request, "409 Conflict", "Restart in progress; reload after restart");
  if (!request->content_len || request->content_len > MAX_LAYOUT_BYTES + 4096)
    return message_(request, "413 Payload Too Large", "Selection body too large");
  std::string body(request->content_len, '\0');
  if (!receive_body_(request, reinterpret_cast<uint8_t *>(body.data()), body.size()))
    return message_(request, "400 Bad Request", "Incomplete selection body");
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::NestingLimit(9)) || !doc.is<JsonObject>() || doc.size() != 2 ||
      !doc["device_ids"].is<JsonArray>() || !doc["layout"].is<JsonObject>())
    return message_(request, "400 Bad Request", "Expected device_ids and layout");
  auto ids = doc["device_ids"].as<JsonArrayConst>();
  if (ids.size() < 1 || ids.size() > 32) return message_(request, "400 Bad Request", "Select 1-32 devices");
  std::vector<CompiledDevice> registry; std::unordered_set<uint32_t> seen;
  const int64_t started = esp_timer_get_time();
  for (JsonVariantConst id_value : ids) {
    const uint32_t id = catalog_id(id_value);
    if (!id || !seen.insert(id).second) return message_(request, "400 Bad Request", "Device IDs must be unique positive integers");
    if (esp_timer_get_time() - started > 45000000)
      return message_(request, "504 Gateway Timeout", "Selection verification timed out; existing setup preserved");
    std::string detail, reason; JsonDocument metadata; CompiledDevice device;
    if (!self->transport_ || !self->transport_->fetch_metadata(id, detail) || deserializeJson(metadata, detail) ||
        catalog_id(metadata["id"]) != id || !classify_device(metadata.as<JsonObjectConst>(), &device, &reason))
      return message_(request, "400 Bad Request", "A selected device is unavailable or unsupported; existing setup preserved");
    // Only server-verified capabilities produce command permissions. Never accept
    // a kind or command list from the browser.
    for (const auto &old : self->devices_) if (old.id == id) device.favorite = old.favorite;
    registry.push_back(std::move(device));
  }
  std::string json, error; serializeJson(doc["layout"], json); DashboardLayout layout;
  if (!parse_layout_json(reinterpret_cast<const uint8_t *>(json.data()), json.size(), registry, &layout, &error))
    return message_(request, "400 Bad Request", error.c_str());
  json = serialize_layout_json(layout, registry);
  if (!self->store_bundle_(registry, json)) return message_(request, "507 Insufficient Storage", "Selection was not saved");
  self->configuration_pending_.store(true);
  self->reboot_pending_.store(true, std::memory_order_release);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, "{\"saved\":true,\"restarting\":true}");
}

esp_err_t DashboardConfig::display_status_handler_(httpd_req_t *request) {
  if (!request_allowed_(request, false)) return forbidden_(request);
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  JsonDocument doc;
  doc["restarting"] = self->configuration_pending_.load();
  doc["enabled"] = self->layout_.sleep.enabled;
  doc["motion_device_id"] = self->layout_.sleep.motion_device_id;
  doc["sleeping"] = self->display_asleep_.load();
  doc["wake_sensor_ready"] = self->motion_ready_.load();
  doc["backlight_level"] = self->backlight_level_.load();
  doc["free_internal_bytes"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  doc["free_psram_bytes"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  std::string body; serializeJson(doc, body);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t DashboardConfig::wake_handler_(httpd_req_t *request) {
  if (!request_allowed_(request, true)) return forbidden_(request);
  uint8_t body[64];
  if (request->content_len > sizeof(body) || !receive_body_(request, body, sizeof(body)))
    return message_(request, "400 Bad Request", "Invalid wake request");
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  self->wake_requested_.store(true);
  return message_(request, "202 Accepted", "Wake requested");
}

esp_err_t DashboardConfig::screenshot_get_handler_(httpd_req_t *request) {
  if (!request_allowed_(request, false)) return forbidden_(request);
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  const char *query = std::strchr(request->uri, '?');
  const bool weather = query != nullptr && std::strcmp(query, "?view=weather") == 0;
  if (query != nullptr && !weather && std::strcmp(query, "?view=current") != 0)
    return message_(request, "400 Bad Request", "Unknown capture view");
  const auto requested = weather ? ScreenshotState::REQUESTED_WEATHER : ScreenshotState::REQUESTED;
  auto expected = ScreenshotState::IDLE;
  if (!self->screenshot_state_.compare_exchange_strong(expected, requested,
                                                        std::memory_order_acq_rel)) {
    if (expected == ScreenshotState::READY || expected == ScreenshotState::FAILED) {
      // A prior HTTP waiter timed out; discard its completed result and retry.
      expected = self->screenshot_state_.load(std::memory_order_acquire);
      if (expected != ScreenshotState::READY && expected != ScreenshotState::FAILED)
        return message_(request, "503 Service Unavailable", "Capture busy");
      if (!self->screenshot_state_.compare_exchange_strong(expected, ScreenshotState::IDLE,
                                                            std::memory_order_acq_rel))
        return message_(request, "503 Service Unavailable", "Capture busy");
      expected = ScreenshotState::IDLE;
      if (!self->screenshot_state_.compare_exchange_strong(expected, requested,
                                                            std::memory_order_acq_rel))
        return message_(request, "503 Service Unavailable", "Capture busy");
    } else {
      return message_(request, "503 Service Unavailable", "Capture busy");
    }
  }
  const int64_t deadline = esp_timer_get_time() + 5000000;
  auto state = self->screenshot_state_.load(std::memory_order_acquire);
  while ((state == ScreenshotState::REQUESTED || state == ScreenshotState::REQUESTED_WEATHER) && esp_timer_get_time() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(10));
    state = self->screenshot_state_.load(std::memory_order_acquire);
  }
  if (state == ScreenshotState::REQUESTED || state == ScreenshotState::REQUESTED_WEATHER)
    return message_(request, "503 Service Unavailable", "Capture timed out");
  if (state == ScreenshotState::FAILED) {
    self->screenshot_state_.store(ScreenshotState::IDLE, std::memory_order_release);
    return message_(request, "503 Service Unavailable", "Capture failed");
  }

  uint8_t header[66]{};
  header[0] = 'B'; header[1] = 'M';
  put_u32_(header + 2, SCREENSHOT_BYTES + sizeof(header));
  put_u32_(header + 10, sizeof(header));
  put_u32_(header + 14, 40); put_u32_(header + 18, 800);
  put_u32_(header + 22, static_cast<uint32_t>(-480));
  put_u16_(header + 26, 1); put_u16_(header + 28, 16);
  put_u32_(header + 30, 3); put_u32_(header + 34, SCREENSHOT_BYTES);
  put_u32_(header + 54, 0xF800); put_u32_(header + 58, 0x07E0); put_u32_(header + 62, 0x001F);
  httpd_resp_set_type(request, "image/bmp");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  esp_err_t result = httpd_resp_send_chunk(request, reinterpret_cast<const char *>(header), sizeof(header));
  constexpr size_t CHUNK_BYTES = 16U * 1024U;
  for (size_t offset = 0; result == ESP_OK && offset < SCREENSHOT_BYTES; offset += CHUNK_BYTES) {
    const size_t amount = std::min(CHUNK_BYTES, SCREENSHOT_BYTES - offset);
    result = httpd_resp_send_chunk(request,
        reinterpret_cast<const char *>(self->screenshot_pixels_ + offset), amount);
  }
  if (result == ESP_OK) result = httpd_resp_send_chunk(request, nullptr, 0);
  self->screenshot_state_.store(ScreenshotState::IDLE, std::memory_order_release);
  return result;
}

esp_err_t DashboardConfig::config_post_handler_(httpd_req_t *request) {
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  if (self->configuration_pending_.load()) return message_(request, "409 Conflict", "Restart in progress; reload after restart");
  if (!request_allowed_(request, true)) return forbidden_(request);
  char content_type[48]{};
  if (!header_(request, "Content-Type", content_type, sizeof(content_type)) ||
      strncmp(content_type, "application/json", 16) != 0 || request->content_len == 0 ||
      request->content_len > MAX_LAYOUT_BYTES) return message_(request, "413 Payload Too Large", "Invalid JSON body size or type");
  auto *body = new (std::nothrow) uint8_t[request->content_len];
  if (body == nullptr) return message_(request, "503 Service Unavailable", "Not enough memory");
  if (!receive_body_(request, body, MAX_LAYOUT_BYTES)) { delete[] body; return message_(request, "400 Bad Request", "Incomplete body"); }
  DashboardLayout parsed; std::string error;
  const bool valid = parse_layout_json(body, request->content_len, self->devices_, &parsed, &error);
  delete[] body;
  if (!valid) return message_(request, "400 Bad Request", error.c_str());
  const std::string canonical = serialize_layout_json(parsed, self->devices_);
  if (!self->store_config_(canonical)) return message_(request, "507 Insufficient Storage", "Could not persist layout");
  self->configuration_pending_.store(true);
  self->reboot_pending_.store(true, std::memory_order_release);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, "{\"saved\":true,\"restarting\":true}");
}

esp_err_t DashboardConfig::background_get_handler_(httpd_req_t *request) {
  if (!request_allowed_(request, false)) return forbidden_(request);
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  if (self->background_data_ == nullptr) return message_(request, "404 Not Found", "No background image");
  httpd_resp_set_type(request, "application/octet-stream");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, reinterpret_cast<const char *>(self->background_data_), BACKGROUND_BYTES);
}

esp_err_t DashboardConfig::background_post_handler_(httpd_req_t *request) {
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  if (self->configuration_pending_.load()) return message_(request, "409 Conflict", "Restart in progress; reload after restart");
  if (!request_allowed_(request, true)) return forbidden_(request);
  char content_type[48]{};
  if (!header_(request, "Content-Type", content_type, sizeof(content_type)) ||
      strcmp(content_type, "application/octet-stream") != 0 || request->content_len != BACKGROUND_BYTES)
    return message_(request, "400 Bad Request", "Expected exactly 192000 RGB565 bytes");
  auto *data = static_cast<uint8_t *>(heap_caps_malloc(BACKGROUND_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (data == nullptr) return message_(request, "503 Service Unavailable", "Not enough memory");
  if (!receive_body_(request, data, BACKGROUND_BYTES)) { heap_caps_free(data); return message_(request, "400 Bad Request", "Incomplete image body"); }
  if (!self->store_background_(data, BACKGROUND_BYTES)) { heap_caps_free(data); return message_(request, "507 Insufficient Storage", "Could not persist image"); }
  // Keep the boot-time bitmap immutable while LVGL may still reference it.
  // The new NVS blob becomes active after the delayed reboot.
  heap_caps_free(data);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, "{\"saved\":true,\"restarting\":true}");
}

esp_err_t DashboardConfig::background_delete_handler_(httpd_req_t *request) {
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  if (self->configuration_pending_.load()) return message_(request, "409 Conflict", "Restart in progress; reload after restart");
  if (!request_allowed_(request, true)) return forbidden_(request);
  nvs_handle_t handle;
  if (nvs_open("dashcfg", NVS_READWRITE, &handle) != ESP_OK) return message_(request, "503 Service Unavailable", "Storage unavailable");
  esp_err_t err = nvs_erase_key(handle, "wall");
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) return message_(request, "507 Insufficient Storage", "Could not remove image");
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, "{\"removed\":true,\"restarting\":true}");
}

esp_err_t DashboardConfig::reset_handler_(httpd_req_t *request) {
  auto *self = static_cast<DashboardConfig *>(request->user_ctx);
  if (self->configuration_pending_.load()) return message_(request, "409 Conflict", "Restart in progress; reload after restart");
  if (!request_allowed_(request, true)) return forbidden_(request);
  uint8_t body[64];
  if (request->content_len > sizeof(body) || !receive_body_(request, body, sizeof(body)))
    return message_(request, "400 Bad Request", "Invalid reset request");
  DashboardLayout defaults;
  DashboardPage home{"Home", "Good day", {}}, lights{"Lights", "Lights", {}}, sensors{"Sensors", "Climate & sensors", {}};
  for (const auto &device : self->devices_) {
    if (device.favorite && home.devices.size() < 6) home.devices.push_back(device.id);
    if (device.kind == "switch" || device.kind == "dimmer") lights.devices.push_back(device.id);
    else sensors.devices.push_back(device.id);
  }
  defaults.pages = {home, lights, sensors};
  if (!self->store_bundle_(self->devices_, serialize_layout_json(defaults, self->devices_)))
    return message_(request, "507 Insufficient Storage", "Could not restore layout defaults");
  nvs_handle_t handle;
  if (nvs_open("dashcfg", NVS_READWRITE, &handle) != ESP_OK)
    return message_(request, "503 Service Unavailable", "Storage unavailable");
  esp_err_t err = nvs_erase_key(handle, "wall");
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) return message_(request, "507 Insufficient Storage", "Could not remove background");
  self->configuration_pending_.store(true);
  self->reboot_pending_.store(true, std::memory_order_release);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, "{\"reset\":true,\"restarting\":true}");
}

}  // namespace esphome::dashboard_config
