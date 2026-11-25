#pragma once

#include <cstdint>
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esp_http_server.h"

namespace esphome::hubitat_local { class HubitatLocal; }

namespace esphome::dashboard_config {

enum class DashboardFont : uint8_t { NORMAL, LARGE };

struct DashboardPage {
  std::string label;
  std::string title;
  std::vector<uint32_t> devices;
};

struct DashboardLabel {
  uint32_t device_id{0};
  std::string label;
};

struct CompiledDevice {
  uint32_t id{0};
  std::string label;
  std::string kind;
  bool favorite{false};
  uint32_t command_mask{0};
  bool motion_capable{false};
};

struct SleepSettings {
  bool enabled{false};
  uint32_t motion_device_id{0};
  uint32_t timeout_seconds{120};
  uint32_t fade_ms{800};
};

struct DashboardLayout {
  uint32_t background{0x0B1220};
  uint32_t panel{0x141F30};
  uint32_t text{0xF1F5F9};
  uint32_t muted{0x9AABC0};
  uint32_t accent{0x49D7C8};
  uint32_t active{0xF2B866};
  uint32_t updating{0x25453F};
  uint32_t warning{0xF27878};
  uint32_t info{0x6BA8FF};
  DashboardFont font{DashboardFont::NORMAL};
  std::vector<DashboardPage> pages;
  std::vector<DashboardLabel> labels;
  SleepSettings sleep;
};

class DashboardConfig : public Component {
 public:
  void set_transport(hubitat_local::HubitatLocal *transport) { transport_ = transport; }
  const std::vector<CompiledDevice> &get_devices() const { return devices_; }
  void set_wake_callback(std::function<void()> callback) { wake_callback_ = std::move(callback); }
  void publish_display_status(bool asleep, bool ready, float level) {
    display_asleep_.store(asleep); motion_ready_.store(ready); backlight_level_.store(level);
  }
  bool add_device(uint32_t id, const std::string &label, const std::string &kind, bool favorite);
  void setup() override;
  void loop() override;
  void on_safe_shutdown() override;
  float get_setup_priority() const override { return setup_priority::DATA + 10; }
  const DashboardLayout &get_layout() const { return this->layout_; }
  const uint8_t *background_data() const { return this->background_data_; }
  void set_screenshot_callback(std::function<bool(uint8_t *, size_t, bool)> callback) {
    this->screenshot_callback_ = std::move(callback);
  }

 protected:
  static esp_err_t discovery_handler_(httpd_req_t *request);
  static esp_err_t provision_handler_(httpd_req_t *request);
  static esp_err_t display_status_handler_(httpd_req_t *request);
  static esp_err_t wake_handler_(httpd_req_t *request);
  static esp_err_t root_handler_(httpd_req_t *request);
  static esp_err_t config_get_handler_(httpd_req_t *request);
  static esp_err_t config_post_handler_(httpd_req_t *request);
  static esp_err_t devices_get_handler_(httpd_req_t *request);
  static esp_err_t background_get_handler_(httpd_req_t *request);
  static esp_err_t background_post_handler_(httpd_req_t *request);
  static esp_err_t background_delete_handler_(httpd_req_t *request);
  static esp_err_t screenshot_get_handler_(httpd_req_t *request);
  static esp_err_t reset_handler_(httpd_req_t *request);
  bool load_storage_();
  bool store_config_(const std::string &json);
  bool store_bundle_(const std::vector<CompiledDevice> &devices, const std::string &layout);
  bool store_background_(const uint8_t *data, size_t len);
  bool start_server_();
  void make_default_();
  bool set_boot_pending_(bool pending);
  hubitat_local::HubitatLocal *transport_{nullptr};
  std::function<void()> wake_callback_;
  std::atomic<bool> wake_requested_{false};
  std::atomic<bool> configuration_pending_{false};
  std::atomic<bool> display_asleep_{false}, motion_ready_{false};
  std::atomic<float> backlight_level_{1.0f};
  DashboardLayout layout_{};
  uint8_t *background_data_{nullptr};
  std::vector<CompiledDevice> devices_;
  void *server_{nullptr};
  std::atomic<bool> reboot_pending_{false};
  uint32_t last_server_retry_{0};
  uint32_t reboot_at_{0};
  uint32_t boot_started_at_{0};
  bool boot_guard_armed_{false};
  enum class ScreenshotState : uint8_t { IDLE, REQUESTED, REQUESTED_WEATHER, READY, FAILED };
  void capture_screenshot_();
  std::function<bool(uint8_t *, size_t, bool)> screenshot_callback_;
  uint8_t *screenshot_pixels_{nullptr};
  std::atomic<ScreenshotState> screenshot_state_{ScreenshotState::IDLE};
};

}  // namespace esphome::dashboard_config
