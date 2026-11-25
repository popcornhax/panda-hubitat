#include "hubitat_dashboard.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/time.h>

#include "esphome/components/json/json_util.h"
#include "esphome/core/application.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "state_logic.h"

namespace esphome::hubitat_dashboard {

static constexpr uint32_t STALE_MS = 30000;
static constexpr uint32_t COMMAND_CONFIRM_MS = 15000;
static constexpr uint32_t POLL_SPACING_MS = 350;
static constexpr uint32_t RETRY_BASE_MS = 2500;
static constexpr uint32_t RETRY_MAX_MS = 30000;
static uint32_t NAVY = 0x0B1220;
static uint32_t PANEL = 0x141F30;
static uint32_t PANEL_2 = 0x1B2A3D;
static uint32_t PANEL_ON = 0x24534F;
static uint32_t PANEL_PRESSED = 0x294361;
static uint32_t TEXT = 0xF1F5F9;
static uint32_t MUTED = 0x9AABC0;
static uint32_t TEAL = 0x49D7C8;
static uint32_t ACTIVE = 0xF2B866;
static uint32_t PANEL_UPDATING = 0x25453F;
static uint32_t WARNING = 0xF27878;
static uint32_t INFO = 0x6BA8FF;
static const char *const TAG = "hubitat_dashboard";
HubitatDashboard *HubitatDashboard::instance_ = nullptr;

static lv_color_t color_(uint32_t color) { return lv_color_hex(color); }
static uint32_t blend_(uint32_t foreground, uint32_t background, unsigned amount) {
  uint32_t result = 0;
  for (unsigned shift : {0U, 8U, 16U}) {
    const unsigned channel = (((foreground >> shift) & 255U) * amount +
                              ((background >> shift) & 255U) * (255U - amount)) / 255U;
    result |= channel << shift;
  }
  return result;
}
static void set_label_text_if_changed_(lv_obj_t *label, const char *text) {
  if (label != nullptr && std::strcmp(lv_label_get_text(label), text) != 0)
    lv_label_set_text(label, text);
}
static void set_label_color_if_changed_(lv_obj_t *label, uint32_t color) {
  if (label != nullptr && !lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), color_(color)))
    lv_obj_set_style_text_color(label, color_(color), 0);
}
static lv_obj_t *label_(lv_obj_t *parent, const char *text, int x, int y, int w, int h, int font = 18,
                        uint32_t color = TEXT) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, w, h);
  const lv_font_t *typeface = font >= 28 ? &lv_font_montserrat_28
                              : font >= 22 ? &lv_font_montserrat_22
                              : font >= 18 ? &lv_font_montserrat_18
                                           : &lv_font_montserrat_14;
  lv_obj_set_style_text_font(label, typeface, 0);
  lv_obj_set_style_text_color(label, color_(color), 0);
  lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
  lv_obj_set_style_text_letter_space(label, font > 20 ? 1 : 0, 0);
  lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
  return label;
}
static lv_obj_t *panel_(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_bg_color(obj, color_(PANEL), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_radius(obj, 16, 0);
  lv_obj_set_style_pad_all(obj, 12, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
  return obj;
}

bool HubitatDashboard::add_device(uint32_t id, const std::string &label, DashboardDeviceKind kind, bool favorite) {
  if (id == 0 || label.empty() || this->device_count_ >= MAX_DEVICES)
    return false;
  for (size_t i = 0; i < this->device_count_; ++i)
    if (this->devices_[i].id == id)
      return false;
  this->devices_.emplace_back();
  ++this->device_count_;
  auto &device = this->devices_.back();
  device.id = id;
  device.label = label;
  device.kind = kind;
  device.favorite = favorite;
  return true;
}

void HubitatDashboard::setup() {
  if (config_ != nullptr) {
    devices_.clear(); device_count_ = 0;
    devices_.reserve(MAX_DEVICES);
    for (const auto &device : config_->get_devices()) {
      const auto kind = device.kind == "switch" ? SWITCH : device.kind == "dimmer" ? DIMMER :
          device.kind == "weather" ? WEATHER : device.kind == "motion" ? MOTION :
          device.kind == "presence" ? PRESENCE : device.kind == "water" ? WATER :
          device.kind == "contact" ? CONTACT : SENSOR;
      add_device(device.id, device.label, kind, device.favorite);
    }
  }
  if (this->transport_ == nullptr || this->device_count_ == 0 || this->backlight_ == nullptr ||
      this->wifi_ == nullptr || this->config_ == nullptr) {
    this->mark_failed();
    return;
  }
  // Hubitat's local HTTP Date header is the only time source. No SNTP or cloud
  // hostname is configured. Set the display's local-time rules independently.
  setenv("TZ", "CST6CDT,M3.2.0,M11.1.0", 1);
  tzset();
  instance_ = this;
  this->config_->set_screenshot_callback([this](uint8_t *pixels, size_t capacity, bool forecast) {
    if (pixels == nullptr || capacity < 800U * 480U * 2U) return false;
    const bool was_open = weather_overlay_ != nullptr && !lv_obj_has_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
    const int previous_index = weather_index_;
    if (forecast) {
      size_t index = 0;
      while (index < device_count_ && devices_[index].kind != WEATHER) ++index;
      if (index == device_count_) return false;
      if (devices_[index].view_count > 0 && devices_[index].views[0].detail_target != nullptr)
        lv_obj_send_event(devices_[index].views[0].detail_target, LV_EVENT_CLICKED, nullptr);
      else show_weather_details_(index);
    }
    lv_image_dsc_t image{};
    const auto result = lv_snapshot_take_to_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565,
                                               &image, pixels, capacity);
    if (forecast) {
      if (was_open && previous_index >= 0) show_weather_details_(previous_index);
      else lv_obj_add_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
    return result == LV_RESULT_OK && image.header.w == 800 && image.header.h == 480 &&
           image.header.stride == 1600 && image.data_size == 800U * 480U * 2U;
  });
  this->transport_->add_result_callback([this](const hubitat_local::HubitatResultView &r) { this->handle_result_(r); });
  this->build_ui_();
  last_wake_activity_ = millis();
  config_->set_wake_callback([this]() { wake_display_(); });
  ESP_LOGI(TAG, "Dashboard ready with %u selected devices", static_cast<unsigned>(this->device_count_));
}

void HubitatDashboard::build_ui_() {
  const auto &layout = this->config_->get_layout();
  NAVY = layout.background;
  PANEL = layout.panel;
  TEXT = layout.text;
  MUTED = layout.muted;
  TEAL = layout.accent;
  ACTIVE = layout.active; PANEL_UPDATING = layout.updating; WARNING = layout.warning; INFO = layout.info;
  PANEL_2 = blend_(TEXT, PANEL, 8);
  PANEL_ON = blend_(ACTIVE, PANEL, 36);
  PANEL_PRESSED = blend_(ACTIVE, PANEL, 76);
  const int name_font = layout.font == dashboard_config::DashboardFont::LARGE ? 28 : 22;
  const int state_font = layout.font == dashboard_config::DashboardFont::LARGE ? 22 : 18;
  for (const auto &override_label : layout.labels)
    for (size_t i = 0; i < device_count_; ++i)
      if (devices_[i].id == override_label.device_id) devices_[i].label = override_label.label;

  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, color_(NAVY), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
  if (const uint8_t *pixels = this->config_->background_data()) {
    // Expand once into PSRAM. LVGL's image transform path needs large temporary
    // rendering buffers; a native-sized bitmap avoids those allocations.
    background_pixels_ = static_cast<uint8_t *>(heap_caps_malloc(800U * 480U * 2U,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (background_pixels_ != nullptr) {
      for (size_t y = 0; y < 240; ++y) {
        uint8_t *row = background_pixels_ + y * 3200;
        for (size_t x = 0; x < 400; ++x) {
          const uint8_t *source = pixels + (y * 400 + x) * 2;
          row[x * 4] = row[x * 4 + 2] = source[0];
          row[x * 4 + 1] = row[x * 4 + 3] = source[1];
        }
        std::memcpy(row + 1600, row, 1600);
      }
      background_desc_.header.magic = LV_IMAGE_HEADER_MAGIC;
      background_desc_.header.cf = LV_COLOR_FORMAT_RGB565;
      background_desc_.header.w = 800;
      background_desc_.header.h = 480;
      background_desc_.header.stride = 1600;
      background_desc_.data_size = 800 * 480 * 2;
      background_desc_.data = background_pixels_;
      background_image_ = lv_image_create(screen);
      lv_image_set_src(background_image_, &background_desc_);
      lv_obj_set_pos(background_image_, 0, 0);
      lv_obj_clear_flag(background_image_, LV_OBJ_FLAG_CLICKABLE);
    } else {
      ESP_LOGW(TAG, "Not enough PSRAM for background; using theme color");
    }
  }

  this->page_count_ = std::min(layout.pages.size() + 1, MAX_PAGES);
  lv_obj_t *nav = lv_obj_create(screen);
  lv_obj_set_pos(nav, 0, 420); lv_obj_set_size(nav, 800, 60);
  lv_obj_set_style_bg_color(nav, color_(PANEL), 0); lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(nav, 0, 0); lv_obj_set_style_radius(nav, 0, 0);
  lv_obj_set_style_pad_all(nav, 0, 0); lv_obj_set_scrollbar_mode(nav, LV_SCROLLBAR_MODE_OFF);
  const size_t custom_pages = page_count_ - 1;
  const bool arrows = custom_pages > 4;
  const size_t visible_pages = arrows ? 4 : custom_pages;
  nav_visible_pages_ = visible_pages;
  const int nav_width = 165;
  nav_button_width_ = nav_width;
  lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
  for (size_t i = 0; i < custom_pages; ++i) {
    bool has_lights = false, has_sensors = false;
    for (uint32_t id : layout.pages[i].devices)
      for (size_t d = 0; d < device_count_; ++d)
        if (devices_[d].id == id) {
          if (devices_[d].kind == SWITCH || devices_[d].kind == DIMMER) has_lights = true;
          else has_sensors = true;
        }
    const char *symbol = has_lights && !has_sensors ? LV_SYMBOL_POWER :
        has_sensors && !has_lights ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_HOME;
    const std::string name = std::string(symbol) + "  " + layout.pages[i].label;
    lv_obj_t *button = lv_button_create(nav); nav_buttons_[i] = button;
    lv_obj_set_size(button, nav_width, 48);
    lv_obj_set_style_bg_color(button, color_(PANEL_2), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, color_(PANEL_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, color_(PANEL_ON), LV_STATE_CHECKED);
    lv_obj_set_style_border_width(button, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(button, color_(PANEL_2), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(button, color_(TEAL), LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(button, 0, 0); lv_obj_set_style_radius(button, 14, 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, name.c_str()); lv_obj_center(label); lv_obj_set_width(label, nav_width - 12);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT); lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(label, color_(TEXT), LV_STATE_DEFAULT);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(button, nav_event_, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
  }
  if (arrows) {
    for (int direction : {-1, 1}) {
      auto *button = lv_button_create(nav);
      lv_obj_set_pos(button, direction < 0 ? 12 : 696, 6);
      lv_obj_set_size(button, 92, 48);
      auto *label = lv_label_create(button); lv_label_set_text(label, direction < 0 ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT); lv_obj_center(label);
      if (direction < 0) nav_left_ = button; else nav_right_ = button;
      lv_obj_add_event_cb(button, nav_event_, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(page_count_ + (direction < 0 ? 10 : 11))));
    }
  }
  settings_tab_ = lv_button_create(nav);
  lv_obj_set_pos(settings_tab_, 704, 6); lv_obj_set_size(settings_tab_, 92, 48);
  lv_obj_set_style_bg_color(settings_tab_, color_(PANEL_2), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(settings_tab_, color_(PANEL_PRESSED), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(settings_tab_, color_(PANEL_ON), LV_STATE_CHECKED);
  lv_obj_set_style_border_width(settings_tab_, 1, LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(settings_tab_, color_(PANEL_2), LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(settings_tab_, color_(TEAL), LV_STATE_CHECKED);
  lv_obj_set_style_shadow_width(settings_tab_, 0, 0); lv_obj_set_style_radius(settings_tab_, 14, 0);
  auto *settings_tab_label = lv_label_create(settings_tab_);
  lv_label_set_text(settings_tab_label, LV_SYMBOL_SETTINGS); lv_obj_center(settings_tab_label);
  lv_obj_set_style_text_font(settings_tab_label, &lv_font_montserrat_18, 0);
  lv_obj_add_event_cb(settings_tab_, nav_event_, LV_EVENT_CLICKED,
      reinterpret_cast<void *>(static_cast<uintptr_t>(page_count_ - 1)));

  update_nav_();
  lv_obj_t *content = lv_obj_create(screen);
  lv_obj_set_pos(content, 0, 0); lv_obj_set_size(content, 800, 420);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(content, 0, 0); lv_obj_set_style_radius(content, 0, 0);
  lv_obj_set_style_pad_all(content, 0, 0); lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

  for (size_t p = 0; p < page_count_; ++p) {
    pages_[p] = lv_obj_create(content);
    lv_obj_set_pos(pages_[p], 0, 0); lv_obj_set_size(pages_[p], 800, 420);
    lv_obj_set_style_bg_opa(pages_[p], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pages_[p], 0, 0); lv_obj_set_style_radius(pages_[p], 0, 0);
    lv_obj_set_style_pad_all(pages_[p], 0, 0); lv_obj_set_scrollbar_mode(pages_[p], LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(pages_[p], LV_OBJ_FLAG_GESTURE_BUBBLE);
    const bool settings = p + 1 == page_count_;
    const std::string title = settings ? "Settings" : layout.pages[p].title;
    auto *title_label = label_(pages_[p], title.c_str(), 18, 6, 490, 38, 28, TEXT);
    if (!settings && p == 0 && title == "Good day") greeting_label_ = title_label;
    if (!settings) {
      auto *previous = lv_button_create(pages_[p]);
      lv_obj_set_pos(previous, 600, 6); lv_obj_set_size(previous, 80, 52);
      auto *previous_icon = lv_label_create(previous); lv_label_set_text(previous_icon, LV_SYMBOL_LEFT); lv_obj_center(previous_icon);
      page_previous_[p] = previous;
      lv_obj_add_event_cb(previous, nav_event_, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(page_count_ + 20)));
      auto *next = lv_button_create(pages_[p]);
      lv_obj_set_pos(next, 690, 6); lv_obj_set_size(next, 80, 52);
      auto *next_icon = lv_label_create(next); lv_label_set_text(next_icon, LV_SYMBOL_RIGHT); lv_obj_center(next_icon);
      page_next_[p] = next;
      lv_obj_add_event_cb(next, nav_event_, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(page_count_ + 21)));
      if (p == 0) {
        clock_label_ = label_(pages_[p], "Waiting for time", 18, 43, 260, 24, 18, MUTED);
        status_label_ = label_(pages_[p], "Connecting", 260, 43, 220, 24, 16, TEAL);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_LEFT, 0);
      }
      lv_obj_t *list = lv_obj_create(pages_[p]);
      const int list_y = 68;
      lv_obj_set_pos(list, 12, list_y); lv_obj_set_size(list, 776, 344);
      lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(list, 0, 0);
      lv_obj_set_style_pad_all(list, 0, 0); lv_obj_set_style_pad_row(list, 8, 0);
      lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
      lv_obj_add_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
      size_t card_index = 0;
      const bool large = layout.font == dashboard_config::DashboardFont::LARGE;
      const int columns = 3;
      const int card_width = 248;
      const int card_height = 160;
      const int card_step = card_height + 8;
      for (uint32_t id : layout.pages[p].devices) {
        for (size_t i = 0; i < device_count_; ++i) {
          if (devices_[i].id != id) continue;
          const int col = static_cast<int>(card_index % columns);
          const int row = static_cast<int>(card_index / columns);
          const int x = 4 + col * (card_width + (large ? 16 : 12));
          create_device_card_(list, i, x, 4 + row * card_step, card_width, card_height);
          ++card_index;
          break;
        }
      }
      if (card_index == 0) {
        lv_obj_t *empty = panel_(pages_[p], 22, list_y + 20, 756, 180);
        label_(empty, "No devices on this page", 18, 24, 570, 36, name_font, TEXT);
        label_(empty, "Choose devices in the uploaded dashboard layout.", 18, 78, 570, 42, state_font, MUTED);
      }
      continue;
    }

    const int settings_y = 62;
    lv_obj_t *network = panel_(pages_[p], 22, settings_y, 756, 120);
    label_(network, "NETWORK", 16, 12, 560, 24, 14, TEAL);
    settings_status_ = label_(network, "Wi-Fi connecting\nHubitat waiting", 16, 40, 700, 68, state_font, TEXT);
    lv_obj_t *light_panel = panel_(pages_[p], 22, 194, 756, 96);
    lv_obj_set_style_pad_all(light_panel, 0, 0);
    label_(light_panel, "DISPLAY BRIGHTNESS", 16, 8, 560, 20, 14, TEAL);
    backlight_slider_ = lv_slider_create(light_panel);
    lv_obj_set_pos(backlight_slider_, 16, 38); lv_obj_set_size(backlight_slider_, 700, 48);
    lv_slider_set_range(backlight_slider_, 10, 100);
    lv_obj_add_event_cb(backlight_slider_, backlight_event_, LV_EVENT_VALUE_CHANGED, this);
    const auto *light_state = this->backlight_;
    const int current = light_state->current_values.is_on()
        ? static_cast<int>(light_state->current_values.get_brightness() * 100.0f) : 10;
    lv_slider_set_value(backlight_slider_, std::max(10, current), LV_ANIM_OFF);
    lv_obj_t *system = panel_(pages_[p], 22, 304, 756, 94);
    label_(system, "SYSTEM", 16, 8, 560, 20, 14, TEAL);
    label_(system, "ESPHome 2026.10.2 | encrypted OTA", 16, 34, 480, 26, 16, TEXT);
    lv_obj_t *restart = lv_button_create(system);
    lv_obj_set_pos(restart, 560, 22); lv_obj_set_size(restart, 160, 52);
    lv_obj_set_style_bg_color(restart, color_(PANEL_2), 0); lv_obj_set_style_radius(restart, 12, 0);
    lv_obj_t *restart_label = lv_label_create(restart);
    lv_label_set_text(restart_label, "Restart device"); lv_obj_center(restart_label);
    lv_obj_add_event_cb(restart, restart_event_, LV_EVENT_CLICKED, this);
    label_(pages_[p], "Layout editor: http://panda-touch.local", 26, 394, 600, 20, 14, MUTED);
  }
  this->show_page_(0);
}

void HubitatDashboard::create_device_card_(lv_obj_t *parent, size_t index, int x, int y, int w, int h) {
  auto &d = devices_[index];
  if (d.view_count >= d.views.size()) return;
  auto &view = d.views[d.view_count++];
  const bool large = config_->get_layout().font == dashboard_config::DashboardFont::LARGE;
  const bool control = d.kind == SWITCH || d.kind == DIMMER;
  const int name_font = large ? 28 : 22;
  const uint32_t accent = control ? ACTIVE : (d.kind == WEATHER || d.kind == WATER ? INFO : TEAL);
  lv_obj_t *card = panel_(parent, x, y, w, h);
  lv_obj_set_style_pad_all(card, 0, 0);
  lv_obj_set_style_radius(card, 18, 0);
  lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_t *surface = card;
  if (control) {
    view.button = lv_button_create(card);
    surface = view.button;
    lv_obj_set_pos(surface, 0, 0); lv_obj_set_size(surface, w, h);
    lv_obj_clear_flag(surface, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(surface, 0, 0);
    lv_obj_set_style_radius(surface, 18, 0);
    lv_obj_set_style_shadow_width(surface, 0, 0);
    lv_obj_set_style_bg_color(surface, color_(PANEL), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(surface, color_(blend_(ACTIVE, PANEL, 28)), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(surface, color_(blend_(ACTIVE, PANEL, 60)), LV_STATE_PRESSED);
    const auto disabled_checked = static_cast<lv_style_selector_t>(LV_STATE_DISABLED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(surface, color_(PANEL_UPDATING), LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(surface, color_(PANEL_UPDATING), disabled_checked);
    lv_obj_set_style_bg_opa(surface, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(surface, LV_OPA_COVER, disabled_checked);
    lv_obj_set_style_opa(surface, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_opa(surface, LV_OPA_COVER, disabled_checked);
    lv_obj_set_style_color_filter_opa(surface, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(surface, 1, 0);
    lv_obj_set_style_border_color(surface, color_(blend_(TEXT, PANEL, 18)), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(surface, color_(blend_(ACTIVE, PANEL, 110)), LV_STATE_CHECKED);
    lv_obj_add_flag(surface, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CHECKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    event_contexts_[index] = {this, static_cast<uint8_t>(index)};
    lv_obj_add_event_cb(surface, device_event_, LV_EVENT_CLICKED, &event_contexts_[index]);
  } else {
    lv_obj_set_style_bg_color(card, color_(blend_(accent, PANEL, 10)), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, color_(blend_(accent, PANEL, 60)), 0);
  }
  // Every decoration is non-interactive: the full tile remains the light target.
  lv_obj_t *icon = label_(surface, control ? LV_SYMBOL_POWER : d.kind == WATER ? LV_SYMBOL_TINT :
                                      d.kind == WEATHER ? "" : d.kind == PRESENCE ? LV_SYMBOL_HOME : LV_SYMBOL_EYE_OPEN,
                         14, 12, 38, 38, 22, accent);
  lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(icon, color_(blend_(accent, PANEL, 30)), 0);
  lv_obj_set_style_radius(icon, 12, 0);
  lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(icon, 7, 0);
  lv_obj_clear_flag(icon, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  view.power_ring = icon;
  if (d.kind == WEATHER) {
    lv_obj_t *sun = panel_(icon, 19, 2, 12, 12);
    lv_obj_set_style_bg_color(sun, color_(ACTIVE), 0); lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *cloud = panel_(icon, 6, 13, 25, 10);
    lv_obj_set_style_bg_color(cloud, color_(TEXT), 0); lv_obj_set_style_radius(cloud, 6, 0);
    lv_obj_t *puff = panel_(icon, 11, 8, 14, 14);
    lv_obj_set_style_bg_color(puff, color_(TEXT), 0); lv_obj_set_style_radius(puff, LV_RADIUS_CIRCLE, 0);
    for (auto *part : {sun, cloud, puff}) lv_obj_clear_flag(part, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  }
  if (control) {
    view.state_label = label_(surface, "Waiting", w - 114, 19, 100, 25, 14, MUTED);
    lv_obj_set_style_text_align(view.state_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(view.state_label, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    auto *name = label_(surface, d.label.c_str(), 14, large ? 50 : 60, w - 28, large ? 64 : 52, name_font, TEXT);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(name, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    if (d.kind == DIMMER) {
      const int slider_x = large ? 88 : 76;
      view.level_label = label_(card, "--%", 14, h - 36, slider_x - 18, 28, 18, MUTED);
      lv_obj_clear_flag(view.level_label, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
      view.slider = lv_slider_create(card);
      lv_obj_set_pos(view.slider, slider_x, h - 24); lv_obj_set_size(view.slider, w - slider_x - 18, 6);
      lv_obj_set_ext_click_area(view.slider, 20);
      lv_obj_set_style_bg_color(view.slider, color_(blend_(ACTIVE, PANEL, 40)), LV_PART_MAIN);
      lv_obj_set_style_bg_color(view.slider, color_(ACTIVE), LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(view.slider, color_(TEXT), LV_PART_KNOB);
      lv_obj_set_style_pad_all(view.slider, 6, LV_PART_KNOB);
      lv_slider_set_range(view.slider, 1, 100);
      lv_obj_add_flag(view.slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
      lv_obj_add_event_cb(view.slider, level_event_, LV_EVENT_RELEASED, &event_contexts_[index]);
    }
  } else {
    auto *name = label_(card, d.label.c_str(), 62, 13, w - 76, 52, name_font, TEXT);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(name, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    if (d.kind == WEATHER) {
      lv_obj_set_width(name, w - 100);
      label_(card, ">", w - 30, 19, 18, 24, 18, INFO);
      event_contexts_[index] = {this, static_cast<uint8_t>(index)};
      lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(card, weather_event_, LV_EVENT_CLICKED, &event_contexts_[index]);
      view.detail_target = card;
      view.primary_label = label_(card, "--", 14, 59, w - 28, 36, 28, TEXT);
      view.state_label = label_(card, "Waiting for reading", 14, 99, w - 28, h - 105, large ? 22 : 18, MUTED);
    } else {
      view.state_label = label_(card, "Waiting for reading", 14, 73, w - 28, h - 79, large ? 22 : 18, TEXT);
    }
    lv_label_set_long_mode(view.state_label, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(view.state_label, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  }
  update_device_ui_(index);
}

void HubitatDashboard::sleep_touch_event_(lv_event_t *event) {
  auto *self = static_cast<HubitatDashboard *>(lv_event_get_user_data(event));
  if (!self) return;
  // Consume the waking touch so it cannot also toggle a light underneath.
  if (auto *indev = lv_indev_active()) lv_indev_wait_release(indev);
  self->wake_display_();
}

void HubitatDashboard::wake_display_() {
  last_wake_activity_ = millis();
  lv_display_trigger_activity(lv_display_get_default());
  if (!display_asleep_) return;
  display_asleep_ = false;
  if (sleep_curtain_) lv_obj_add_flag(sleep_curtain_, LV_OBJ_FLAG_HIDDEN);
  auto call = backlight_->turn_on();
  call.set_brightness(wake_brightness_).set_transition_length(config_->get_layout().sleep.fade_ms).set_save(false);
  call.perform();
}

void HubitatDashboard::update_sleep_() {
  const auto &sleep = config_->get_layout().sleep;
  const uint32_t now = millis();
  const Device *motion = nullptr;
  for (size_t i = 0; i < device_count_; ++i)
    if (devices_[i].id == sleep.motion_device_id) motion = &devices_[i];
  const bool ready = wifi_ && wifi_->is_connected() && motion && motion->motion_known && !motion->sensor_offline && !motion->failures &&
                     state_logic::fresh(motion->state_known, motion->last_state_ms, now, 15000);
  const bool active = ready && motion->motion_active;
  // Fail awake if the selected sensor/network is unavailable. Touch and the
  // local web wake action remain available even while the panel is black.
  if (!sleep.enabled || !ready || active) {
    last_wake_activity_ = now;
    if (display_asleep_) wake_display_();
  }
  const uint32_t idle = std::min(now - last_wake_activity_, lv_display_get_inactive_time(lv_display_get_default()));
  if (!display_asleep_ && state_logic::should_sleep(sleep.enabled, ready, active, idle, sleep.timeout_seconds * 1000)) {
    wake_brightness_ = std::clamp(backlight_->remote_values.get_brightness(), 0.1f, 1.0f);
    if (sleep_curtain_ == nullptr) {
      sleep_curtain_ = panel_(lv_screen_active(), 0, 0, 800, 480);
      lv_obj_set_style_bg_color(sleep_curtain_, color_(0), 0);
      lv_obj_set_style_radius(sleep_curtain_, 0, 0);
      lv_obj_add_flag(sleep_curtain_, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(sleep_curtain_, sleep_touch_event_, LV_EVENT_PRESSED, this);
    }
    lv_obj_clear_flag(sleep_curtain_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sleep_curtain_);
    display_asleep_ = true;
    auto call = backlight_->turn_off();
    call.set_transition_length(250).set_save(false); call.perform();
  }
  const float level = backlight_->current_values.get_state() * backlight_->current_values.get_brightness();
  config_->publish_display_status(display_asleep_, ready, level);
}

void HubitatDashboard::weather_event_(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *ctx = static_cast<EventContext *>(lv_event_get_user_data(event));
  if (ctx && ctx->self) ctx->self->show_weather_details_(ctx->index);
}

void HubitatDashboard::weather_close_event_(lv_event_t *event) {
  auto *self = static_cast<HubitatDashboard *>(lv_event_get_user_data(event));
  if (self && self->weather_overlay_) lv_obj_add_flag(self->weather_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void HubitatDashboard::show_weather_details_(size_t index) {
  if (index >= device_count_ || devices_[index].kind != WEATHER) return;
  weather_index_ = static_cast<int>(index);
  if (weather_overlay_ == nullptr) {
    weather_overlay_ = panel_(lv_screen_active(), 0, 0, 800, 480);
    lv_obj_set_style_pad_all(weather_overlay_, 0, 0);
    lv_obj_set_style_radius(weather_overlay_, 0, 0);
    lv_obj_set_style_bg_color(weather_overlay_, color_(NAVY), 0);
    lv_obj_add_flag(weather_overlay_, LV_OBJ_FLAG_CLICKABLE);
    label_(weather_overlay_, "3-day forecast", 20, 14, 640, 38, 28, TEXT);
    auto *back = lv_button_create(weather_overlay_);
    lv_obj_set_pos(back, 682, 14); lv_obj_set_size(back, 98, 44);
    lv_obj_set_style_bg_color(back, color_(PANEL_2), 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    auto *back_label = label_(back, "Back", 0, 0, 74, 24, 18, TEXT);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back, weather_close_event_, LV_EVENT_CLICKED, this);
    weather_heading_label_ = label_(weather_overlay_, "Waiting for weather", 20, 56, 760, 44, 18, MUTED);
    for (size_t day = 0; day < 3; ++day) {
      auto *card = panel_(weather_overlay_, 16 + static_cast<int>(day) * 260, 112, 248, 182);
      lv_obj_set_style_pad_all(card, 0, 0);
      lv_obj_set_style_bg_color(card, color_(blend_(INFO, PANEL, 12)), 0);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_border_color(card, color_(blend_(INFO, PANEL, 70)), 0);
      forecast_title_[day] = label_(card, "", 14, 10, 220, 28, 22, INFO);
      forecast_range_[day] = label_(card, "-- / --", 14, 43, 220, 38, 28, TEXT);
      label_(card, "High / Low (°F)", 14, 81, 220, 21, 14, MUTED);
      forecast_condition_[day] = label_(card, "No forecast provided", 14, 106, 220, 44, 18, TEXT);
      lv_label_set_long_mode(forecast_condition_[day], LV_LABEL_LONG_WRAP);
      forecast_precipitation_[day] = label_(card, "Precip --", 14, 154, 220, 24, 18, TEAL);
    }
    weather_wind_label_ = label_(weather_overlay_, "", 20, 316, 760, 44, 18, TEXT);
    weather_sun_label_ = label_(weather_overlay_, "", 20, 372, 760, 28, 18, TEXT);
    weather_report_label_ = label_(weather_overlay_, "", 20, 434, 760, 28, 14, MUTED);
  }
  lv_obj_clear_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
  update_weather_details_();
}

void HubitatDashboard::update_weather_details_() {
  if (weather_overlay_ == nullptr || lv_obj_has_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN) || weather_index_ < 0)
    return;
  const auto &device = devices_[weather_index_];
  const bool stale = !state_logic::fresh(device.state_known, device.last_state_ms, millis(), STALE_MS) ||
                     device.failures > 0;
  set_label_text_if_changed_(weather_heading_label_, device.weather_heading.empty() ?
                            "Waiting for weather readings" : device.weather_heading.c_str());
  char third_day[24] = "Day 3";
  const time_t now = std::time(nullptr);
  if (now >= 1700000000) {
    struct tm day{};
    localtime_r(&now, &day);
    day.tm_hour = 12; day.tm_mday += 2;
    std::mktime(&day);
    std::strftime(third_day, sizeof(third_day), "%A", &day);
  }
  for (size_t day = 0; day < 3; ++day) {
    const auto &forecast = device.forecast[day];
    set_label_text_if_changed_(forecast_title_[day], day == 0 ? "Today" : day == 1 ? "Tomorrow" : third_day);
    const std::string range = (forecast.high.empty() ? "--" : forecast.high + "°") + " / " +
                              (forecast.low.empty() ? "--" : forecast.low + "°");
    const std::string precipitation = forecast.precipitation.empty() ? "Precip --" :
                                       "Precip " + forecast.precipitation + "%";
    set_label_text_if_changed_(forecast_range_[day], range.c_str());
    set_label_text_if_changed_(forecast_condition_[day], forecast.condition.empty() ?
                              "No forecast provided" : forecast.condition.c_str());
    set_label_text_if_changed_(forecast_precipitation_[day], precipitation.c_str());
  }
  set_label_text_if_changed_(weather_wind_label_, device.weather_wind.empty() ?
                            "Wind reading unavailable" : device.weather_wind.c_str());
  set_label_text_if_changed_(weather_sun_label_, device.weather_sun.empty() ?
                            "Sunrise/sunset unavailable" : device.weather_sun.c_str());
  std::string report = stale ? "Last received data (stale) | " : "";
  report += device.weather_report.empty() ? "Forecast report time unavailable" :
              "Forecast report: " + device.weather_report;
  set_label_text_if_changed_(weather_report_label_, report.c_str());
  set_label_color_if_changed_(weather_report_label_, stale ? ACTIVE : MUTED);
}

void HubitatDashboard::show_page_(size_t page) {
  page_ = page;
  for (size_t i = 0; i < page_count_; ++i) {
    if (pages_[i] != nullptr) {
      if (i == page) lv_obj_clear_flag(pages_[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(pages_[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  const size_t custom_pages = page_count_ - 1;
  for (size_t i = 0; i < custom_pages; ++i) {
    if (page_previous_[i]) {
      if (page == 0) { lv_obj_add_state(page_previous_[i], LV_STATE_DISABLED); lv_obj_clear_flag(page_previous_[i], LV_OBJ_FLAG_CLICKABLE); }
      else { lv_obj_clear_state(page_previous_[i], LV_STATE_DISABLED); lv_obj_add_flag(page_previous_[i], LV_OBJ_FLAG_CLICKABLE); }
    }
    if (page_next_[i]) {
      if (page + 1 >= custom_pages) { lv_obj_add_state(page_next_[i], LV_STATE_DISABLED); lv_obj_clear_flag(page_next_[i], LV_OBJ_FLAG_CLICKABLE); }
      else { lv_obj_clear_state(page_next_[i], LV_STATE_DISABLED); lv_obj_add_flag(page_next_[i], LV_OBJ_FLAG_CLICKABLE); }
    }
  }
  if (page < custom_pages && (page < nav_offset_ || page >= nav_offset_ + nav_visible_pages_))
    nav_offset_ = (page / nav_visible_pages_) * nav_visible_pages_;
  update_nav_();
}
void HubitatDashboard::update_nav_() {
  const size_t custom_pages = page_count_ - 1;
  const bool two_strips = nav_left_ != nullptr;
  const bool second_strip = two_strips && nav_offset_ > 0;
  const size_t shown = std::min(custom_pages > nav_offset_ ? custom_pages - nav_offset_ : 0,
                                nav_visible_pages_);
  const int fixed_width = 92;
  const int gap = 6;
  const int tab_start = second_strip ? 12 + fixed_width + gap : 12;
  const int fixed_controls = second_strip ? fixed_width * 2 : fixed_width;
  const int page_gaps = static_cast<int>(shown + (second_strip ? 1 : 0)) * gap;
  const int available = 776 - fixed_controls - page_gaps;
  nav_button_width_ = shown ? available / static_cast<int>(shown) : 0;
  for (size_t i = 0; i < custom_pages; ++i) {
    auto *button = nav_buttons_[i]; if (!button) continue;
    const bool visible = i >= nav_offset_ && i < nav_offset_ + nav_visible_pages_;
    if (visible) {
      lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(button, tab_start + static_cast<int>(i - nav_offset_) * (nav_button_width_ + gap), 6);
      lv_obj_set_size(button, nav_button_width_, 48);
      if (auto *label = lv_obj_get_child(button, 0)) {
        lv_obj_set_width(label, nav_button_width_ - 12);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
      }
    } else lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    if (i == page_) lv_obj_add_state(button, LV_STATE_CHECKED);
    else lv_obj_clear_state(button, LV_STATE_CHECKED);
  }
  if (settings_tab_) {
    if (!two_strips || second_strip) {
      lv_obj_clear_flag(settings_tab_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(settings_tab_, tab_start + static_cast<int>(shown) * (nav_button_width_ + gap), 6);
      lv_obj_set_size(settings_tab_, fixed_width, 48);
    } else lv_obj_add_flag(settings_tab_, LV_OBJ_FLAG_HIDDEN);
    if (page_ == custom_pages) lv_obj_add_state(settings_tab_, LV_STATE_CHECKED);
    else lv_obj_clear_state(settings_tab_, LV_STATE_CHECKED);
  }
  if (nav_right_) {
    if (second_strip) lv_obj_add_flag(nav_right_, LV_OBJ_FLAG_HIDDEN);
    else {
      lv_obj_clear_flag(nav_right_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(nav_right_, tab_start + static_cast<int>(shown) * (nav_button_width_ + gap), 6);
    }
  }
  if (nav_left_) {
    if (second_strip) lv_obj_clear_flag(nav_left_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(nav_left_, LV_OBJ_FLAG_HIDDEN);
  }
}
void HubitatDashboard::page_gesture_event_(lv_event_t *) {
  // Page changes use explicit header buttons so swiping over a light control
  // can never race with LVGL's CLICKED event and trigger the device.
}
void HubitatDashboard::set_page_(size_t page) {
  if (page < page_count_) show_page_(page);
}
void HubitatDashboard::nav_event_(lv_event_t *event) {
  auto *self = instance_;
  if (self == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const auto value = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (value == static_cast<intptr_t>(self->page_count_ - 1)) { self->set_page_(self->page_count_ - 1); return; }
  if (value == static_cast<intptr_t>(self->page_count_ + 20)) { if (self->page_ > 0 && self->page_ < self->page_count_) self->set_page_(self->page_ - 1); return; }
  if (value == static_cast<intptr_t>(self->page_count_ + 21)) { if (self->page_ + 1 < self->page_count_ - 1) self->set_page_(self->page_ + 1); return; }
  if (value == static_cast<intptr_t>(self->page_count_ + 10)) {
    if (self->nav_offset_ > 0) { self->nav_offset_ = self->nav_offset_ >= self->nav_visible_pages_ ? self->nav_offset_ - self->nav_visible_pages_ : 0; self->update_nav_(); }
    return;
  }
  if (value == static_cast<intptr_t>(self->page_count_ + 11)) {
    if (self->nav_offset_ + self->nav_visible_pages_ < self->page_count_ - 1) { self->nav_offset_ += self->nav_visible_pages_; self->update_nav_(); }
    return;
  }
  self->set_page_(static_cast<size_t>(value));
}
void HubitatDashboard::device_event_(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *ctx = static_cast<EventContext *>(lv_event_get_user_data(event));
  if (!ctx || !ctx->self) return;
  auto &d = ctx->self->devices_[ctx->index];
  if (!state_logic::can_start_command(d.pending, d.command_inflight, d.state_refresh_required) || !d.state_known) return;
  const bool target = !d.on;
  const bool dispatched = ctx->self->transport_->request_command(
      d.id, target ? hubitat_local::HubitatCommand::ON : hubitat_local::HubitatCommand::OFF);
  state_logic::begin_command(d.pending, d.command_queued, d.command_inflight, dispatched);
  d.command_error = false; d.pending_on = target; d.pending_level = -1; d.pending_since = millis();
  d.next_poll_ms = millis();
  ctx->self->update_device_ui_(ctx->index);
}
void HubitatDashboard::level_event_(lv_event_t *event) {
  if (!state_logic::dimmer_release_event(lv_event_get_code(event) == LV_EVENT_RELEASED)) return;
  auto *ctx = static_cast<EventContext *>(lv_event_get_user_data(event));
  if (!ctx || !ctx->self) return;
  auto &d = ctx->self->devices_[ctx->index];
  if (!state_logic::can_start_command(d.pending, d.command_inflight, d.state_refresh_required) || !d.state_known || !d.on) return;
  const int target = lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(event)));
  if (std::abs(target - d.level) < 3) return;
  const bool dispatched = ctx->self->transport_->request_command(d.id, hubitat_local::HubitatCommand::SET_LEVEL, target);
  state_logic::begin_command(d.pending, d.command_queued, d.command_inflight, dispatched);
  d.command_error = false; d.pending_on = true; d.pending_level = target; d.pending_since = millis();
  d.next_poll_ms = millis();
  ctx->self->update_device_ui_(ctx->index);
}
void HubitatDashboard::backlight_event_(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
  auto *self = static_cast<HubitatDashboard *>(lv_event_get_user_data(event));
  if (!self || !self->backlight_) return;
  const float value = lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(event))) / 100.0f;
  auto call = self->backlight_->turn_on(); call.set_brightness(value); call.perform();
}
void HubitatDashboard::restart_event_(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *self = static_cast<HubitatDashboard *>(lv_event_get_user_data(event));
  static uint32_t armed_at = 0;
  const uint32_t now = millis();
  if (self == nullptr) return;
  if (armed_at != 0 && now - armed_at <= 5000) App.safe_reboot();
  armed_at = now;
  auto *label = lv_obj_get_child(lv_event_get_target_obj(event), 0);
  if (label != nullptr) {
    lv_label_set_text(label, "Tap again");
    self->set_timeout("restart_confirmation", 5000, [label]() { lv_label_set_text(label, "Restart device"); });
  }
}

static std::string attribute_(JsonVariantConst attributes, const char *wanted) {
  if (attributes.is<JsonArrayConst>()) {
    for (JsonVariantConst item : attributes.as<JsonArrayConst>())
      if (item["name"].as<const char *>() != nullptr && std::strcmp(item["name"].as<const char *>(), wanted) == 0)
        return item["currentValue"].isNull() ? std::string{} : item["currentValue"].as<std::string>();
  } else if (attributes.is<JsonObjectConst>()) {
    JsonVariantConst value = attributes[wanted];
    if (!value.isNull()) return value.as<std::string>();
  }
  return {};
}
static std::string clean_weather_text_(std::string value, size_t limit = 44) {
  std::string cleaned; bool in_tag = false;
  for (const char ch : value) {
    if (ch == '<') { in_tag = true; continue; }
    if (ch == '>') { in_tag = false; continue; }
    if (!in_tag && static_cast<unsigned char>(ch) >= 32) cleaned.push_back(ch);
  }
  while (!cleaned.empty() && cleaned.front() == ' ') cleaned.erase(cleaned.begin());
  while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
  if (cleaned.size() > limit) cleaned.resize(limit);
  return cleaned;
}

static bool set_http_time_(const char *header) {
  if (header == nullptr || header[0] == '\0') return false;
  char weekday[4]{}, month_name[4]{}, zone[4]{};
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  if (std::sscanf(header, "%3s, %d %3s %d %d:%d:%d %3s", weekday, &day, month_name, &year,
                  &hour, &minute, &second, zone) != 8 || std::strcmp(zone, "GMT") != 0 ||
      year < 2024 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
    return false;
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int month = 0;
  for (; month < 12 && std::strcmp(month_name, months[month]) != 0; ++month) {}
  if (month == 12) return false;
  // Gregorian days-from-civil conversion, independent of the display TZ.
  int adjusted_year = year - (month < 2 ? 1 : 0);
  const int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(adjusted_year - era * 400);
  const unsigned doy = (153U * static_cast<unsigned>(month + (month > 1 ? -2 : 10)) + 2U) / 5U +
                       static_cast<unsigned>(day - 1);
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
  const int64_t epoch = days * 86400 + hour * 3600 + minute * 60 + std::min(second, 59);
  timeval tv{}; tv.tv_sec = static_cast<time_t>(epoch); tv.tv_usec = 0;
  return settimeofday(&tv, nullptr) == 0;
}

void HubitatDashboard::handle_result_(const hubitat_local::HubitatResultView &result) {
  if (result.date_header[0] != '\0') set_http_time_(result.date_header);
  if (result.is_command) {
    for (size_t i = 0; i < device_count_; ++i) {
      auto &device = devices_[i];
      if (device.id != result.device_id) continue;
      if (!state_logic::finish_command(device.command_inflight, device.state_refresh_required)) return;
      if (result.http_status == 200 && result.command_dispatched) {
        device.command_queued = false;
        device.next_poll_ms = millis() + 350;
        return;  // Dispatch is not confirmation.
      }
      device.pending = false; device.pending_level = -1; device.command_error = true;
      device.command_queued = false;
      hubitat_available_ = false;
      device.next_poll_ms = millis(); update_device_ui_(i); return;
    }
    return;
  }
  for (size_t i = 0; i < device_count_; ++i) {
    auto &d = devices_[i];
    if (d.id != result.device_id) continue;
    if (result.result_code == static_cast<int16_t>(hubitat_local::HubitatResultCode::OK) && result.json_valid) {
      apply_state_(d, result);
      hubitat_available_ = true;
      last_hubitat_ok_ms_ = millis();
      d.failures = 0;
      d.state_known = state_logic::record_state_result(d.last_state_ms, millis(), true);
      d.next_poll_ms = d.id == config_->get_layout().sleep.motion_device_id && config_->get_layout().sleep.enabled ?
          d.last_state_ms + 1000 : state_logic::successful_poll_due(d.last_state_ms, d.kind != SWITCH && d.kind != DIMMER);
      if (d.pending) {
        const bool confirmed = state_logic::command_matches(d.pending_on, d.pending_level, d.on, d.level);
        if (state_logic::confirm_command(confirmed, d.pending, d.command_inflight, d.state_refresh_required, d.command_error))
          d.pending_level = -1;
        else d.next_poll_ms = millis() + 500;
      } else d.state_refresh_required = false;
    } else {
      hubitat_available_ = false;
      state_logic::record_state_result(d.last_state_ms, millis(), false);
      d.failures = std::min<uint8_t>(d.failures + 1, 5);
      d.next_poll_ms = millis() + state_logic::retry_delay(d.failures, RETRY_BASE_MS, RETRY_MAX_MS);
    }
    update_device_ui_(i);
    break;
  }
}
void HubitatDashboard::apply_state_(Device &device, const hubitat_local::HubitatResultView &result) {
  auto document = esphome::json::parse_json(std::string(result.body, result.body_length));
  JsonObject root = document.as<JsonObject>();
  if (root.isNull()) return;
  const std::string sw = attribute_(root["attributes"], "switch");
  if (device.kind == SWITCH || device.kind == DIMMER) {
    device.on = sw == "on";
    const std::string level = attribute_(root["attributes"], "level");
    if (!level.empty()) device.level = std::atoi(level.c_str());
    return;
  }
  const auto attrs = root["attributes"];
  const std::string battery = attribute_(attrs, "battery");
  device.battery = battery.empty() ? -1 : std::atoi(battery.c_str());
  const std::string motion_state = attribute_(attrs, "motion");
  device.motion_known = motion_state == "active" || motion_state == "inactive";
  device.motion_active = motion_state == "active";
  device.sensor_offline = state_logic::sensor_offline(attribute_(attrs, "healthStatus") == "offline", motion_state == "active");
  const auto unavailable = device.sensor_offline ? "Offline | last reported " : "";
  if (device.kind == WEATHER) {
    const std::string temp = attribute_(attrs, "temperature");
    const std::string humidity = attribute_(attrs, "humidity");
    const std::string feels = attribute_(attrs, "feelsLike");
    const std::string condition = clean_weather_text_(attribute_(attrs, "condition_text"));
    device.sensor_summary = unavailable;
    if (!temp.empty()) device.sensor_summary += temp + "°F";
    if (!feels.empty()) device.sensor_summary += " | feels " + feels + "°F";
    if (!humidity.empty()) device.sensor_summary += " | " + humidity + "% RH";
    if (!condition.empty()) device.sensor_summary += "\n" + condition;
    auto value = [&](const char *name) { return clean_weather_text_(attribute_(attrs, name), 120); };
    for (size_t day = 0; day < device.forecast.size(); ++day) {
      const std::string suffix = day == 0 ? "" : std::to_string(day);
      auto &forecast = device.forecast[day];
      forecast.high = value(("forecastHigh" + suffix).c_str());
      forecast.low = value(("forecastLow" + suffix).c_str());
      forecast.condition = value(("forecast_text" + suffix).c_str());
      forecast.precipitation = value(day == 0 ? "percentPrecip" : ("PoP" + suffix).c_str());
    }
    device.weather_heading = (temp.empty() ? "--" : temp + "°F") + "  |  " + condition;
    if (!feels.empty()) device.weather_heading += "  |  Feels " + feels + "°F";
    if (!humidity.empty()) device.weather_heading += "  |  " + humidity + "% RH";
    device.weather_wind = value("wind_string");
    const std::string sunrise = value("localSunrise"), sunset = value("localSunset");
    device.weather_sun = sunrise.empty() && sunset.empty() ? "" :
        "Sunrise " + (sunrise.empty() ? "--" : sunrise) + "   |   Sunset " + (sunset.empty() ? "--" : sunset);
    device.weather_report = value("last_observation_Forecast");
    device.weather_temperature = temp.empty() ? "--" : temp + "°F";
    device.weather_details = feels.empty() ? "" : "Feels " + feels + "°";
    if (!humidity.empty()) device.weather_details += (device.weather_details.empty() ? "" : "  |  ") + humidity + "% RH";
    if (!condition.empty()) device.weather_details += "\n" + (condition.size() > 22 ? condition.substr(0, 19) + "..." : condition);
  } else if (device.kind == MOTION || device.kind == PRESENCE) {
    const std::string temp = attribute_(attrs, "temperature");
    std::string presence_state = attribute_(attrs, "presence");
    if (presence_state.empty()) presence_state = motion_state;
    device.sensor_summary = unavailable;
    if (device.sensor_offline) device.sensor_summary = "Offline\nLast: ";
    else if (!temp.empty()) device.sensor_summary += temp + "°F\n";
    if (device.kind == PRESENCE)
      device.sensor_summary += presence_state.empty() ? "No presence reading" : (device.sensor_offline ? "" : "Presence ") + presence_state;
    else
      device.sensor_summary += motion_state.empty() ? "No motion reading" : (device.sensor_offline ? "" : "Motion ") + motion_state;
  } else if (device.kind == CONTACT) {
    const std::string contact = attribute_(attrs, "contact");
    device.sensor_summary = (device.sensor_offline ? "Offline\nLast: " : "") + (contact.empty() ? "No contact reading" : "Contact " + contact);
  } else if (device.kind == SENSOR) {
    device.sensor_summary = device.sensor_offline ? "Offline\nLast: " : "";
    unsigned lines = device.sensor_offline ? 1 : 0;
    const unsigned maximum = device.battery >= 0 && device.battery <= 10 ? 2 : 3;
    for (const auto &field : {std::pair<const char *, const char *>{"temperature", "°F"},
                             {"humidity", "% RH"}, {"illuminance", " lux"}, {"power", " W"}, {"voltage", " V"}}) {
      const std::string value = attribute_(attrs, field.first);
      if (!value.empty() && lines < maximum) {
        if (!device.sensor_summary.empty() && device.sensor_summary.back() != ' ' && device.sensor_summary.back() != '\n') device.sensor_summary += "\n";
        device.sensor_summary += value + field.second; ++lines;
      }
    }
  } else if (device.kind == WATER) {
    const std::string water = attribute_(attrs, "water");
    device.sensor_summary = device.sensor_offline ? "Offline\nLast: " : "";
    device.sensor_summary += water.empty() ? "No water reading" : "Water " + water;
  }
  if (device.battery >= 0 && device.battery <= 10) device.sensor_summary += "\nLow battery | " + std::to_string(device.battery) + "%";
  if (device.sensor_summary.empty()) device.sensor_summary = "No supported reading";
}
void HubitatDashboard::update_device_ui_(size_t index) {
  auto &d = devices_[index];
  std::string text;
  const uint32_t now = millis();
  const bool stale = !state_logic::fresh(d.state_known, d.last_state_ms, now, STALE_MS) || d.failures > 0;
  if (d.pending) text = "Updating...";
  else if (d.command_error) text = "Not confirmed";
  else if (d.kind != SWITCH && d.kind != DIMMER) {
    if (stale) text = d.sensor_offline ? "Offline | last report stale" : "Reading stale";
    else text = d.sensor_summary;
  }
  else if (stale && d.failures > 0) text = "Stale";
  else if (stale) text = "Waiting";
  else if (d.kind == DIMMER) text = d.on ? "On" : "Off";
  else text = d.on ? "On" : "Off";
  for (uint8_t view_index = 0; view_index < d.view_count; ++view_index) {
    auto &view = d.views[view_index];
    if (view.state_label) {
      const bool weather = d.kind == WEATHER && !stale;
      set_label_text_if_changed_(view.state_label, weather ? d.weather_details.c_str() : text.c_str());
      set_label_color_if_changed_(view.state_label, d.pending || d.command_error || d.sensor_offline || (d.battery >= 0 && d.battery <= 10) ? ACTIVE : (stale ? MUTED : (d.on ? ACTIVE : MUTED)));
    }
    if (view.primary_label) set_label_text_if_changed_(view.primary_label, stale ? "--" : d.weather_temperature.c_str());
    if (view.power_ring) set_label_color_if_changed_(view.power_ring, d.kind == SWITCH || d.kind == DIMMER ? (d.on && !stale ? ACTIVE : MUTED) : (d.sensor_offline ? ACTIVE : TEAL));
    if (view.level_label != nullptr) {
      char value[16]; std::snprintf(value, sizeof(value), "%d%%", d.level);
      set_label_text_if_changed_(view.level_label, value);
    }
    const bool enabled = state_logic::controls_enabled(wifi_ && wifi_->is_connected(), hubitat_available_,
                                                        d.state_known && d.failures == 0, d.last_state_ms,
                                                        now, STALE_MS,
                                                        d.pending || d.command_inflight || d.state_refresh_required);
    if (view.button) {
      if (enabled) lv_obj_clear_state(view.button, LV_STATE_DISABLED);
      else lv_obj_add_state(view.button, LV_STATE_DISABLED);
      if (d.on) lv_obj_add_state(view.button, LV_STATE_CHECKED);
      else lv_obj_clear_state(view.button, LV_STATE_CHECKED);
    }
    if (view.slider) {
      if (enabled && d.on) lv_obj_clear_state(view.slider, LV_STATE_DISABLED);
      else lv_obj_add_state(view.slider, LV_STATE_DISABLED);
      if (!d.pending && !lv_slider_is_dragged(view.slider))
        lv_slider_set_value(view.slider, std::max(1, d.level), LV_ANIM_OFF);
    }
  }
}
void HubitatDashboard::poll_next_() {
  const uint32_t now = millis();
  if (transport_->request_pending() || now - last_poll_start_ < POLL_SPACING_MS) return;
  for (size_t i = 0; i < device_count_; ++i) {
    auto &d = devices_[i];
    if (!d.pending || !d.command_queued || !state_logic::start_queued_command(false, d.command_queued)) continue;
    const bool accepted = d.pending_level >= 0
        ? transport_->request_command(d.id, hubitat_local::HubitatCommand::SET_LEVEL, d.pending_level)
        : transport_->request_command(d.id, d.pending_on ? hubitat_local::HubitatCommand::ON : hubitat_local::HubitatCommand::OFF);
    if (!accepted) d.command_queued = true;
    else { d.command_inflight = true; d.pending_since = now; }
    last_poll_start_ = now;
    return;
  }
  const auto &sleep = config_->get_layout().sleep;
  if (sleep.enabled) {
    for (size_t i = 0; i < device_count_; ++i) {
      auto &d = devices_[i];
      if (d.id == sleep.motion_device_id && static_cast<int32_t>(now - d.next_poll_ms) >= 0 &&
          transport_->request_state(d.id)) { last_poll_start_ = now; return; }
    }
  }
  for (size_t offset = 0; offset < device_count_; ++offset) {
    const size_t i = (page_index_ + offset) % device_count_;
    auto &d = devices_[i];
    if (static_cast<int32_t>(now - d.next_poll_ms) < 0) continue;
    if (transport_->request_state(d.id)) {
      page_index_ = (i + 1) % device_count_;
      last_poll_start_ = now;
      return;
    }
  }
}
void HubitatDashboard::refresh_status_() {
  const bool connected = wifi_->is_connected();
  const std::string net = connected ? "Wi-Fi | connected" : "Wi-Fi | disconnected";
  const bool hub_ok = hubitat_available_ && millis() - last_hubitat_ok_ms_ <= 60000;
  const std::string hub = !connected ? "Hubitat | waiting for Wi-Fi"
      : transport_->request_pending() ? "Hubitat | updating"
      : hub_ok ? "Hubitat | responding" : "Hubitat | unavailable";
  if (settings_status_) {
    std::string text = net;
    if (connected) {
      char address[esphome::network::IP_ADDRESS_BUFFER_SIZE]; wifi_->wifi_sta_ip_addresses()[0].str_to(address);
      text += "  " + std::string(address) + "  " + std::to_string(wifi_->wifi_rssi()) + " dBm";
    }
    text += "\n" + hub;
    set_label_text_if_changed_(settings_status_, text.c_str());
  }
  if (status_label_) {
    const bool available = connected && hub_ok;
    set_label_text_if_changed_(status_label_, !connected ? "Wi-Fi disconnected"
        : hub_ok ? LV_SYMBOL_WIFI "  Connected" : "Hubitat unavailable");
    set_label_color_if_changed_(status_label_, available ? TEAL : ACTIVE);
  }
  if (clock_label_) {
    const time_t now = std::time(nullptr);
    if (now < 1700000000) set_label_text_if_changed_(clock_label_, "Waiting for Hubitat time");
    else {
      struct tm local{};
      localtime_r(&now, &local);
      char text[40];
      std::strftime(text, sizeof(text), "%a  %b %d | %I:%M %p", &local);
      set_label_text_if_changed_(clock_label_, text);
      set_label_text_if_changed_(greeting_label_, state_logic::greeting_for_hour(local.tm_hour));
    }
  }
}
void HubitatDashboard::loop() {
  const uint32_t now = millis();
  update_sleep_();
  if (wifi_ && wifi_->is_connected()) poll_next_();
  if (now - last_ui_refresh_ >= 500) {
    last_ui_refresh_ = now;
    refresh_status_();
    update_weather_details_();
    for (size_t i = 0; i < device_count_; ++i) {
      auto &d = devices_[i];
      if (d.pending && now - d.pending_since >= COMMAND_CONFIRM_MS) {
        state_logic::timeout_command(d.pending, d.command_queued, d.command_error,
                                     d.command_inflight, d.state_refresh_required);
        d.pending_level = -1; d.next_poll_ms = now;
      }
      update_device_ui_(i);
    }
  }
}

}  // namespace esphome::hubitat_dashboard
