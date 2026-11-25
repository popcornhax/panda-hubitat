#include "hubitat_local.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "esphome/components/json/json_util.h"
#include "esphome/core/log.h"

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace esphome::hubitat_local {

// State polling and browser discovery may each own a bounded 32 KiB result.
// Keep these buffers in PSRAM; the internal heap can be fragmented despite
// having ample total free memory. IDF's nothrow new can abort on allocation
// failure with exceptions disabled, so use a genuinely nullable allocator.
void *HubitatResult::operator new(size_t size, const std::nothrow_t &) noexcept {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void HubitatResult::operator delete(void *pointer) noexcept { heap_caps_free(pointer); }

static const char *const TAG = "hubitat_local";
static constexpr uint32_t HTTP_IO_TIMEOUT_MS = 1000;
// Checked between IDF client calls; the IDF header/body readers can perform
// several socket reads internally, so this is not a hard slow-drip deadline.
static constexpr uint32_t REQUEST_DEADLINE_MS = 5000;
static constexpr size_t HTTP_BUFFER_SIZE = 512;
static constexpr uint32_t WORKER_STACK_WORDS = 8192;

static std::string url_encode_(const std::string &input) {
  static constexpr char HEX[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(input.size() * 3);
  for (const unsigned char ch : input) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      encoded.push_back(static_cast<char>(ch));
    } else {
      encoded.push_back('%');
      encoded.push_back(HEX[ch >> 4]);
      encoded.push_back(HEX[ch & 0x0F]);
    }
  }
  return encoded;
}

static void redact_token_(HubitatResult *result, const std::string &token) {
  if (token.empty() || result->body_length == 0)
    return;
  std::string body(result->body, result->body_length);
  for (const std::string needle : {token, url_encode_(token)}) {
    if (needle.empty())
      continue;
    size_t position = 0;
    while ((position = body.find(needle, position)) != std::string::npos) {
      body.erase(position, needle.size());
    }
  }
  if (body.size() > RESPONSE_BODY_CAPACITY)
    body.resize(RESPONSE_BODY_CAPACITY);
  result->body_length = static_cast<uint16_t>(body.size());
  memcpy(result->body, body.data(), body.size());
  result->body[result->body_length] = '\0';
}

static uint32_t elapsed_ms_(int64_t started_us) {
  const int64_t elapsed = (esp_timer_get_time() - started_us) / 1000;
  return elapsed < 0 ? 0 : static_cast<uint32_t>(elapsed);
}

static bool json_device_id_matches_(JsonVariantConst value, uint32_t expected) {
  if (value.is<bool>())
    return false;
  if (value.is<const char *>()) {
    const char *text = value.as<const char *>();
    if (text == nullptr || text[0] == '\0')
      return false;
    uint64_t parsed = 0;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
      if (*cursor < '0' || *cursor > '9')
        return false;
      parsed = parsed * 10 + static_cast<uint64_t>(*cursor - '0');
      if (parsed > 0xFFFFFFFFULL)
        return false;
    }
    return parsed == expected;
  }
  if (value.is<uint32_t>())
    return value.as<uint32_t>() == expected;
  return false;
}

static void clear_body_(HubitatResult *result) {
  result->body_length = 0;
  result->body[0] = '\0';
}

static esp_err_t http_event_handler_(esp_http_client_event_t *event) {
  if (event != nullptr && event->event_id == HTTP_EVENT_ON_HEADER && event->user_data != nullptr &&
      event->header_key != nullptr && event->header_value != nullptr && strcasecmp(event->header_key, "Date") == 0) {
    auto *result = static_cast<HubitatResult *>(event->user_data);
    std::snprintf(result->date_header, sizeof(result->date_header), "%s", event->header_value);
  }
  return ESP_OK;
}

bool HubitatLocal::add_device_id(uint32_t device_id) {
  if (device_id == 0 || this->devices_.size() >= 32 || this->is_selected_(device_id))
    return false;
  this->devices_.push_back({device_id, 0});
  return true;
}

bool HubitatLocal::clear_devices_before_setup() {
  if (worker_task_ != nullptr) return false;
  devices_.clear();
  return true;
}

bool HubitatLocal::fetch_metadata(uint32_t id, std::string &body) {
  Request request;
  request.device_id = id;
  request.metadata = true;
  HubitatResult *result = perform_request(request);
  if (result == nullptr) return false;
  const bool ok = result->http_status == 200 &&
      result->result_code == static_cast<int16_t>(HubitatResultCode::OK);
  if (ok) body.assign(result->body, result->body_length);
  delete result;
  return ok;
}

uint32_t HubitatLocal::command_bit_(HubitatCommand command) const {
  const auto value = static_cast<uint8_t>(command);
  if (value > static_cast<uint8_t>(HubitatCommand::SET_COOLING_SETPOINT))
    return 0;
  return 1UL << value;
}

bool HubitatLocal::add_allowed_command(uint32_t device_id, HubitatCommand command) {
  const uint32_t bit = this->command_bit_(command);
  if (bit == 0)
    return false;
  for (auto &device : this->devices_) {
    if (device.id == device_id) {
      device.command_mask |= bit;
      return true;
    }
  }
  return false;
}

bool HubitatLocal::is_selected_(uint32_t device_id) const {
  for (const auto &device : this->devices_) {
    if (device.id == device_id)
      return true;
  }
  return false;
}

void HubitatLocal::setup() {
  // ESP-IDF's client logs the full URL at debug level. It contains the token,
  // so suppress that component's log tag before any client is created.
  esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
  QueueHandle_t request_queue = xQueueCreate(1, sizeof(Request));
  QueueHandle_t result_queue = xQueueCreate(1, sizeof(HubitatResult *));
  if (request_queue == nullptr || result_queue == nullptr) {
    if (request_queue != nullptr)
      vQueueDelete(request_queue);
    if (result_queue != nullptr)
      vQueueDelete(result_queue);
    this->mark_failed();
    return;
  }
  this->request_queue_ = request_queue;
  this->result_queue_ = result_queue;
  TaskHandle_t task = nullptr;
  const BaseType_t created = xTaskCreate(&HubitatLocal::worker_entry, "hubitat_http", WORKER_STACK_WORDS, this,
                                          tskIDLE_PRIORITY + 1, &task);
  if (created != pdPASS) {
    vQueueDelete(request_queue);
    vQueueDelete(result_queue);
    this->request_queue_ = nullptr;
    this->result_queue_ = nullptr;
    this->mark_failed();
    return;
  }
  this->worker_task_ = task;
}

void HubitatLocal::dump_config() {
  ESP_LOGCONFIG(TAG, "Hubitat Maker API local transport:");
  ESP_LOGCONFIG(TAG, "  Selected device count: %u", static_cast<unsigned>(this->devices_.size()));
  ESP_LOGCONFIG(TAG, "  Thermostat scale: %c", this->temperature_scale_);
  ESP_LOGCONFIG(TAG, "  Access token: configured (redacted)");
}

void HubitatLocal::worker_entry(void *arg) {
  static_cast<HubitatLocal *>(arg)->worker_loop();
}

bool HubitatLocal::request_state(uint32_t device_id) {
  if (this->request_pending_ || this->request_queue_ == nullptr || this->result_queue_ == nullptr ||
      !this->is_selected_(device_id))
    return false;
  Request request;
  request.device_id = device_id;
  request.is_command = false;
  if (xQueueSend(static_cast<QueueHandle_t>(this->request_queue_), &request, 0) != pdPASS)
    return false;
  this->pending_device_id_ = device_id;
  this->pending_is_command_ = false;
  this->request_pending_ = true;
  return true;
}

bool HubitatLocal::request_command(uint32_t device_id, HubitatCommand command, float value) {
  if (this->request_pending_ || this->request_queue_ == nullptr || this->result_queue_ == nullptr)
    return false;
  const uint32_t bit = this->command_bit_(command);
  if (bit == 0)
    return false;
  bool allowed = false;
  for (const auto &device : this->devices_) {
    if (device.id == device_id) {
      allowed = (device.command_mask & bit) != 0;
      break;
    }
  }
  if (!allowed)
    return false;
  if (command == HubitatCommand::SET_LEVEL && (!std::isfinite(value) || value < 0.0f || value > 100.0f))
    return false;
  if (command == HubitatCommand::SET_HEATING_SETPOINT || command == HubitatCommand::SET_COOLING_SETPOINT) {
    const float lower = this->temperature_scale_ == 'C' ? 5.0f : 41.0f;
    const float upper = this->temperature_scale_ == 'C' ? 35.0f : 95.0f;
    if (!std::isfinite(value) || value < lower || value > upper)
      return false;
  }

  Request request;
  request.device_id = device_id;
  request.command = command;
  request.value = value;
  request.is_command = true;
  if (xQueueSend(static_cast<QueueHandle_t>(this->request_queue_), &request, 0) != pdPASS)
    return false;
  this->pending_device_id_ = device_id;
  this->pending_is_command_ = true;
  this->request_pending_ = true;
  return true;
}

void HubitatLocal::worker_loop() {
  auto request_queue = static_cast<QueueHandle_t>(this->request_queue_);
  auto result_queue = static_cast<QueueHandle_t>(this->result_queue_);
  Request request;
  while (true) {
    if (xQueueReceive(request_queue, &request, portMAX_DELAY) != pdPASS)
      continue;
    HubitatResult *result = this->perform_request(request);
    // A null pointer is an allocation-free OOM sentinel; loop() turns it into
    // a result code using the pending request metadata.
    if (xQueueSend(result_queue, &result, 0) != pdPASS && result != nullptr)
      delete result;
  }
}

HubitatResult *HubitatLocal::perform_request(const Request &request) {
  auto *result = new (std::nothrow) HubitatResult();
  if (result == nullptr)
    return nullptr;
  result->device_id = request.device_id;
  result->is_command = request.is_command;

  char path[160];
  int path_len = 0;
  if (request.metadata && request.device_id == 0) {
    path_len = snprintf(path, sizeof(path), "/apps/api/%s/devices", this->app_id_.c_str());
  } else if (!request.is_command) {
    path_len = snprintf(path, sizeof(path), "/apps/api/%s/devices/%lu", this->app_id_.c_str(),
                        static_cast<unsigned long>(request.device_id));
  } else {
    const char *command_path = nullptr;
    switch (request.command) {
      case HubitatCommand::ON:
        command_path = "on";
        break;
      case HubitatCommand::OFF:
        command_path = "off";
        break;
      case HubitatCommand::SET_LEVEL:
        command_path = "setLevel";
        break;
      case HubitatCommand::SET_HEATING_SETPOINT:
        command_path = "setHeatingSetpoint";
        break;
      case HubitatCommand::SET_COOLING_SETPOINT:
        command_path = "setCoolingSetpoint";
        break;
    }
    if (command_path == nullptr) {
      result->result_code = static_cast<int16_t>(HubitatResultCode::INVALID_REQUEST);
      return result;
    }
    if (request.command == HubitatCommand::ON || request.command == HubitatCommand::OFF) {
      path_len = snprintf(path, sizeof(path), "/apps/api/%s/devices/%lu/%s", this->app_id_.c_str(),
                          static_cast<unsigned long>(request.device_id), command_path);
    } else if (request.command == HubitatCommand::SET_LEVEL) {
      path_len = snprintf(path, sizeof(path), "/apps/api/%s/devices/%lu/%s/%d", this->app_id_.c_str(),
                          static_cast<unsigned long>(request.device_id), command_path,
                          static_cast<int>(lroundf(request.value)));
    } else {
      char value[16];
      snprintf(value, sizeof(value), "%.1f", static_cast<double>(request.value));
      path_len = snprintf(path, sizeof(path), "/apps/api/%s/devices/%lu/%s/%s", this->app_id_.c_str(),
                          static_cast<unsigned long>(request.device_id), command_path, value);
    }
  }
  if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
    result->result_code = static_cast<int16_t>(HubitatResultCode::INVALID_REQUEST);
    return result;
  }

  std::string url = "http://" + this->host_ + path + "?access_token=" + url_encode_(this->access_token_);
  const int64_t started_us = esp_timer_get_time();
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = HTTP_IO_TIMEOUT_MS;
  config.disable_auto_redirect = true;
  config.buffer_size = HTTP_BUFFER_SIZE;
  config.buffer_size_tx = HTTP_BUFFER_SIZE;
  config.user_agent = "PandaTouch-Hubitat/1.0";
  config.event_handler = http_event_handler_;
  config.user_data = result;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    result->result_code = static_cast<int16_t>(HubitatResultCode::TRANSPORT_ERROR);
    return result;
  }
  esp_http_client_set_method(client, HTTP_METHOD_GET);
  esp_http_client_set_header(client, "Accept", "application/json");
  const esp_err_t opened = esp_http_client_open(client, 0);
  if (opened != ESP_OK) {
    result->result_code = elapsed_ms_(started_us) >= REQUEST_DEADLINE_MS
                              ? static_cast<int16_t>(HubitatResultCode::RESPONSE_TIMEOUT)
                              : static_cast<int16_t>(HubitatResultCode::TRANSPORT_ERROR);
    esp_http_client_cleanup(client);
    return result;
  }
  uint32_t elapsed = elapsed_ms_(started_us);
  if (elapsed >= REQUEST_DEADLINE_MS) {
    result->result_code = static_cast<int16_t>(HubitatResultCode::RESPONSE_TIMEOUT);
    esp_http_client_cleanup(client);
    return result;
  }
  esp_http_client_set_timeout_ms(client, std::min(HTTP_IO_TIMEOUT_MS, REQUEST_DEADLINE_MS - elapsed));
  const int64_t content_length = esp_http_client_fetch_headers(client);
  result->http_status = esp_http_client_get_status_code(client);
  if (elapsed_ms_(started_us) >= REQUEST_DEADLINE_MS) {
    result->result_code = static_cast<int16_t>(HubitatResultCode::RESPONSE_TIMEOUT);
    esp_http_client_cleanup(client);
    return result;
  }
  if (result->http_status >= 300 && result->http_status < 400) {
    result->result_code = static_cast<int16_t>(HubitatResultCode::REDIRECT_REJECTED);
    esp_http_client_cleanup(client);
    return result;
  }
  if (content_length > static_cast<int64_t>(RESPONSE_BODY_CAPACITY)) {
    result->result_code = static_cast<int16_t>(HubitatResultCode::RESPONSE_TOO_LARGE);
    clear_body_(result);
    esp_http_client_cleanup(client);
    return result;
  }
  if (content_length < 0) {
    result->result_code = elapsed_ms_(started_us) >= REQUEST_DEADLINE_MS
                              ? static_cast<int16_t>(HubitatResultCode::RESPONSE_TIMEOUT)
                              : static_cast<int16_t>(HubitatResultCode::TRANSPORT_ERROR);
    esp_http_client_cleanup(client);
    return result;
  }

  char buffer[HTTP_BUFFER_SIZE];
  while (result->body_length < RESPONSE_BODY_CAPACITY) {
    elapsed = elapsed_ms_(started_us);
    if (elapsed >= REQUEST_DEADLINE_MS) {
      result->result_code = static_cast<int16_t>(HubitatResultCode::RESPONSE_TIMEOUT);
      clear_body_(result);
      esp_http_client_cleanup(client);
      return result;
    }
    esp_http_client_set_timeout_ms(client, std::min(HTTP_IO_TIMEOUT_MS, REQUEST_DEADLINE_MS - elapsed));
    const size_t remaining = RESPONSE_BODY_CAPACITY - result->body_length;
    const int read_len = esp_http_client_read(client, buffer,
                                              static_cast<int>(std::min(sizeof(buffer), remaining)));
    if (read_len < 0) {
      result->result_code = elapsed_ms_(started_us) >= REQUEST_DEADLINE_MS
                                ? static_cast<int16_t>(HubitatResultCode::RESPONSE_TIMEOUT)
                                : static_cast<int16_t>(HubitatResultCode::TRANSPORT_ERROR);
      clear_body_(result);
      esp_http_client_cleanup(client);
      return result;
    }
    if (read_len == 0)
      break;
    memcpy(result->body + result->body_length, buffer, static_cast<size_t>(read_len));
    result->body_length = static_cast<uint16_t>(result->body_length + read_len);
  }
  if (!esp_http_client_is_complete_data_received(client)) {
    result->result_code = elapsed_ms_(started_us) >= REQUEST_DEADLINE_MS
                              ? static_cast<int16_t>(HubitatResultCode::RESPONSE_TIMEOUT)
                              : static_cast<int16_t>(HubitatResultCode::TRANSPORT_ERROR);
    clear_body_(result);
    esp_http_client_cleanup(client);
    return result;
  }
  if (result->body_length == RESPONSE_BODY_CAPACITY && esp_http_client_is_chunked_response(client)) {
    result->result_code = static_cast<int16_t>(HubitatResultCode::RESPONSE_TOO_LARGE);
    clear_body_(result);
    esp_http_client_cleanup(client);
    return result;
  }
  result->body[result->body_length] = '\0';
  redact_token_(result, this->access_token_);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (result->http_status != 200) {
    result->result_code = static_cast<int16_t>(HubitatResultCode::HTTP_ERROR);
    return result;
  }
  if (request.is_command) {
    result->command_dispatched = true;
    result->result_code = static_cast<int16_t>(HubitatResultCode::OK);
    return result;
  }
  result->result_code = static_cast<int16_t>(HubitatResultCode::OK);
  return result;
}

void HubitatLocal::loop() {
  if (this->result_queue_ == nullptr)
    return;
  HubitatResult *result = nullptr;
  if (xQueueReceive(static_cast<QueueHandle_t>(this->result_queue_), &result, 0) != pdPASS)
    return;
  HubitatResultView view{};
  if (result == nullptr) {
    this->last_device_id_ = this->pending_device_id_;
    this->last_http_status_ = 0;
    this->last_result_code_ = static_cast<int16_t>(HubitatResultCode::QUEUE_ERROR);
    this->last_json_valid_ = false;
    this->last_command_dispatched_ = false;
    this->last_response_completed_ms_ = millis();
    this->last_body_[0] = '\0';
    view.device_id = this->pending_device_id_;
    view.result_code = static_cast<int16_t>(HubitatResultCode::QUEUE_ERROR);
    view.is_command = this->pending_is_command_;
    view.body = nullptr;
    this->request_pending_ = false;
  } else {
    this->last_device_id_ = result->device_id;
    this->last_http_status_ = result->http_status;
    this->last_result_code_ = result->result_code;
    this->last_json_valid_ = result->json_valid;
    this->last_command_dispatched_ = result->command_dispatched;
    if (result->http_status == 200 && result->is_command && result->body_length > 0) {
    // Maker API command acknowledgements need not contain a device-state body.
      result->json_valid = esphome::json::parse_json(
          std::string(result->body, result->body_length), [](JsonObject root) { return !root.isNull(); });
    } else if (result->http_status == 200 && !result->is_command) {
      result->json_valid = result->body_length > 0 && esphome::json::parse_json(
          std::string(result->body, result->body_length), [expected = result->device_id](JsonObject root) {
            if (!json_device_id_matches_(root["id"], expected))
              return false;
            const JsonVariantConst attributes = root["attributes"];
            return attributes.is<JsonArrayConst>() || attributes.is<JsonObjectConst>();
          });
      if (!result->json_valid)
        result->result_code = static_cast<int16_t>(HubitatResultCode::INVALID_JSON);
    }
    this->last_result_code_ = result->result_code;
    this->last_json_valid_ = result->json_valid;
    this->last_response_completed_ms_ = millis();
    if (!result->is_command && result->result_code == static_cast<int16_t>(HubitatResultCode::OK))
      this->last_successful_state_ms_ = this->last_response_completed_ms_;
    memcpy(this->last_body_, result->body, result->body_length + 1);
    view.device_id = result->device_id;
    view.http_status = result->http_status;
    view.result_code = result->result_code;
    view.is_command = result->is_command;
    view.command_dispatched = result->command_dispatched;
    view.json_valid = result->json_valid;
    view.body_length = result->body_length;
    view.body = result->body;
    std::snprintf(view.date_header, sizeof(view.date_header), "%s", result->date_header);
    // Clear before callbacks so a callback may enqueue a follow-up state read.
    this->request_pending_ = false;
  }
  for (const auto &callback : this->callbacks_)
    callback(view);
  delete result;
}

}  // namespace esphome::hubitat_local
