#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "esphome/core/component.h"

namespace esphome::hubitat_local {

static constexpr size_t RESPONSE_BODY_CAPACITY = 32768;

enum HubitatCommand : uint8_t {
  ON,
  OFF,
  SET_LEVEL,
  SET_HEATING_SETPOINT,
  SET_COOLING_SETPOINT,
};

enum class HubitatResultCode : int16_t {
  OK = 0,
  INVALID_REQUEST = -1,
  BUSY = -2,
  TRANSPORT_ERROR = -3,
  HTTP_ERROR = -4,
  REDIRECT_REJECTED = -5,
  RESPONSE_TOO_LARGE = -6,
  RESPONSE_TIMEOUT = -7,
  INVALID_JSON = -8,
  QUEUE_ERROR = -9,
};

struct HubitatResult {
  static void *operator new(size_t size, const std::nothrow_t &) noexcept;
  static void operator delete(void *pointer) noexcept;
  uint32_t device_id{0};
  int http_status{0};
  int16_t result_code{static_cast<int16_t>(HubitatResultCode::TRANSPORT_ERROR)};
  bool is_command{false};
  bool command_dispatched{false};
  bool json_valid{false};
  uint16_t body_length{0};
  char body[RESPONSE_BODY_CAPACITY + 1]{};
  char date_header[40]{};
};

// Lightweight main-loop callback view. This keeps OOM/error handling off the
// stack without constructing the 32 KiB worker response object locally.
struct HubitatResultView {
  uint32_t device_id{0};
  int http_status{0};
  int16_t result_code{static_cast<int16_t>(HubitatResultCode::TRANSPORT_ERROR)};
  bool is_command{false};
  bool command_dispatched{false};
  bool json_valid{false};
  uint16_t body_length{0};
  const char *body{nullptr};
  char date_header[40]{};
};

class HubitatLocal : public esphome::Component {
 public:
  using ResultCallback = std::function<void(const HubitatResultView &)>;

  void set_host(const std::string &host) { this->host_ = host; }
  void set_app_id(const std::string &app_id) { this->app_id_ = app_id; }
  void set_access_token(const std::string &token) { this->access_token_ = token; }
  void set_temperature_scale(const std::string &scale) { this->temperature_scale_ = scale == "C" ? 'C' : 'F'; }
  bool add_device_id(uint32_t device_id);
  bool clear_devices_before_setup();
  // Read-only discovery: fixed Maker API paths, immutable credentials, own HTTP client.
  bool fetch_metadata(uint32_t id, std::string &body);
  bool add_allowed_command(uint32_t device_id, HubitatCommand command);

  void setup() override;
  void loop() override;
  void dump_config() override;

  // Queue a state read or allowlisted Maker API command. All result callbacks
  // run from loop(), never from the worker task. Callback references, including
  // the response body, are valid only until callback return; use the last_* getters
  // for values retained by this component.
  bool request_state(uint32_t device_id);
  bool request_command(uint32_t device_id, HubitatCommand command, float value = 0.0f);
  void add_result_callback(ResultCallback callback) { this->callbacks_.push_back(std::move(callback)); }

  uint32_t last_device_id() const { return this->last_device_id_; }
  int last_http_status() const { return this->last_http_status_; }
  int last_result_code() const { return this->last_result_code_; }
  bool last_json_valid() const { return this->last_json_valid_; }
  bool last_command_dispatched() const { return this->last_command_dispatched_; }
  uint32_t last_response_completed_ms() const { return this->last_response_completed_ms_; }
  uint32_t last_successful_state_ms() const { return this->last_successful_state_ms_; }
  const char *last_body() const { return this->last_body_; }
  bool request_pending() const { return this->request_pending_; }

 protected:
  struct Request {
    uint32_t device_id{0};
    HubitatCommand command{HubitatCommand::ON};
    float value{0.0f};
    bool is_command{false};
    bool metadata{false};
  };

  struct AllowedDevice {
    uint32_t id{0};
    uint32_t command_mask{0};
  };

  static void worker_entry(void *arg);
  void worker_loop();
  HubitatResult *perform_request(const Request &request);
  bool is_selected_(uint32_t device_id) const;
  uint32_t command_bit_(HubitatCommand command) const;

  std::string host_;
  std::string app_id_;
  std::string access_token_;
  char temperature_scale_{'F'};
  std::vector<AllowedDevice> devices_;
  std::vector<ResultCallback> callbacks_;

  void *request_queue_{nullptr};
  void *result_queue_{nullptr};
  void *worker_task_{nullptr};
  bool request_pending_{false};
  uint32_t pending_device_id_{0};
  bool pending_is_command_{false};

  uint32_t last_device_id_{0};
  int last_http_status_{0};
  int last_result_code_{static_cast<int16_t>(HubitatResultCode::TRANSPORT_ERROR)};
  bool last_json_valid_{false};
  bool last_command_dispatched_{false};
  uint32_t last_response_completed_ms_{0};
  uint32_t last_successful_state_ms_{0};
  char last_body_[RESPONSE_BODY_CAPACITY + 1]{};
};

}  // namespace esphome::hubitat_local
