#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/hubitat_local/hubitat_local.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/components/dashboard_config/dashboard_config.h"
#include "lvgl.h"

namespace esphome::hubitat_dashboard {

enum DashboardDeviceKind : uint8_t { SWITCH, DIMMER, MOTION, WEATHER, PRESENCE, WATER, CONTACT, SENSOR };

class HubitatDashboard : public Component {
 public:
  void set_transport(hubitat_local::HubitatLocal *transport) { transport_ = transport; }
  void set_backlight(light::LightState *light) { backlight_ = light; }
  void set_wifi(wifi::WiFiComponent *wifi) { wifi_ = wifi; }
  void set_config(dashboard_config::DashboardConfig *config) { config_ = config; }
  bool add_device(uint32_t id, const std::string &label, DashboardDeviceKind kind, bool favorite);
  float get_setup_priority() const override { return setup_priority::LATE; }
  void setup() override;
  void loop() override;

 protected:
  struct Device {
    uint32_t id{0};
    std::string label;
    DashboardDeviceKind kind{SWITCH};
    bool favorite{false};
    bool state_known{false};
    bool on{false};
    int level{0};
    uint32_t last_state_ms{0};
    std::string sensor_summary;
    std::string weather_temperature;
    std::string weather_details;
    struct Forecast { std::string high, low, condition, precipitation; };
    std::array<Forecast, 3> forecast{};
    std::string weather_heading, weather_wind, weather_sun, weather_report;
    int battery{-1};
    bool sensor_offline{false};
    bool motion_known{false};
    bool motion_active{false};
    bool pending{false};
    bool command_queued{false};
    bool command_inflight{false};
    bool state_refresh_required{false};
    bool command_error{false};
    bool pending_on{false};
    int pending_level{-1};
    uint32_t pending_since{0};
    uint8_t failures{0};
    uint32_t next_poll_ms{0};
    struct View {
      lv_obj_t *state_label{nullptr};
      lv_obj_t *primary_label{nullptr};
      lv_obj_t *button{nullptr};
      lv_obj_t *level_label{nullptr};
      lv_obj_t *slider{nullptr};
      lv_obj_t *power_ring{nullptr};
      lv_obj_t *detail_target{nullptr};
    };
    std::array<View, 8> views{};
    uint8_t view_count{0};
  };
  struct EventContext { HubitatDashboard *self; uint8_t index; };
  static constexpr size_t MAX_DEVICES = 32;
  static constexpr size_t MAX_PAGES = 9;
  static void result_cb_(const hubitat_local::HubitatResult &result, void *context);
  static void nav_event_(lv_event_t *event);
  static void page_gesture_event_(lv_event_t *event);
  static void device_event_(lv_event_t *event);
  static void level_event_(lv_event_t *event);
  static void backlight_event_(lv_event_t *event);
  static void restart_event_(lv_event_t *event);
  static void weather_event_(lv_event_t *event);
  static void weather_close_event_(lv_event_t *event);
  static void sleep_touch_event_(lv_event_t *event);
  void update_sleep_();
  void wake_display_();
  void show_weather_details_(size_t index);
  void update_weather_details_();
  void build_ui_();
  void show_page_(size_t page);
  void create_device_card_(lv_obj_t *parent, size_t index, int x, int y, int w, int h);
  void update_device_ui_(size_t index);
  void handle_result_(const hubitat_local::HubitatResultView &result);
  void apply_state_(Device &device, const hubitat_local::HubitatResultView &result);
  void poll_next_();
  void refresh_status_();
  void set_page_(size_t page);
  void update_nav_();
  std::vector<Device> devices_;
  std::array<EventContext, MAX_DEVICES> event_contexts_{};
  size_t device_count_{0};
  hubitat_local::HubitatLocal *transport_{nullptr};
  light::LightState *backlight_{nullptr};
  wifi::WiFiComponent *wifi_{nullptr};
  dashboard_config::DashboardConfig *config_{nullptr};
  size_t page_{0};
  size_t page_count_{0};
  size_t nav_offset_{0};
  size_t nav_visible_pages_{0};
  int nav_button_width_{0};
  lv_obj_t *nav_left_{nullptr};
  lv_obj_t *nav_right_{nullptr};
  lv_obj_t *settings_tab_{nullptr};
  std::array<lv_obj_t *, MAX_PAGES> page_previous_{};
  std::array<lv_obj_t *, MAX_PAGES> page_next_{};
  lv_obj_t *pages_[MAX_PAGES]{};
  lv_obj_t *nav_buttons_[MAX_PAGES]{};
  lv_image_dsc_t background_desc_{};
  uint8_t *background_pixels_{nullptr};
  lv_obj_t *background_image_{nullptr};
  lv_obj_t *status_label_{nullptr};
  lv_obj_t *clock_label_{nullptr};
  lv_obj_t *greeting_label_{nullptr};
  lv_obj_t *weather_overlay_{nullptr};
  lv_obj_t *sleep_curtain_{nullptr};
  bool display_asleep_{false};
  float wake_brightness_{1.0f};
  uint32_t last_wake_activity_{0};
  int weather_index_{-1};
  lv_obj_t *weather_heading_label_{nullptr};
  lv_obj_t *weather_wind_label_{nullptr};
  lv_obj_t *weather_sun_label_{nullptr};
  lv_obj_t *weather_report_label_{nullptr};
  std::array<lv_obj_t *, 3> forecast_range_{};
  std::array<lv_obj_t *, 3> forecast_title_{};
  std::array<lv_obj_t *, 3> forecast_condition_{};
  std::array<lv_obj_t *, 3> forecast_precipitation_{};
  lv_obj_t *settings_status_{nullptr};
  lv_obj_t *backlight_slider_{nullptr};
  uint32_t last_poll_start_{0};
  uint32_t last_ui_refresh_{0};
  uint32_t page_index_{0};
  uint8_t retry_round_{0};
  bool hubitat_available_{false};
  uint32_t last_hubitat_ok_ms_{0};
  static HubitatDashboard *instance_;
};

}  // namespace esphome::hubitat_dashboard
