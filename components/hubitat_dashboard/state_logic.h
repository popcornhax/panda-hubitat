#pragma once
#include <cstdint>
#include <cstdlib>
#include <algorithm>

namespace esphome::hubitat_dashboard::state_logic {
inline bool fresh(bool known, uint32_t last_ms, uint32_t now_ms, uint32_t stale_ms) {
  return known && static_cast<uint32_t>(now_ms - last_ms) <= stale_ms;
}
inline bool command_matches(bool desired_on, int desired_level, bool actual_on, int actual_level) {
  return desired_on == actual_on && (desired_level < 0 || std::abs(desired_level - actual_level) <= 2);
}
inline uint32_t retry_delay(uint8_t failures, uint32_t base_ms, uint32_t max_ms) {
  if (failures == 0) return 0;
  uint32_t delay = base_ms;
  for (uint8_t i = 1; i < failures && delay < max_ms; ++i)
    delay = std::min(max_ms, delay * 2);
  return std::min(delay, max_ms);
}
inline uint32_t successful_poll_due(uint32_t now_ms, bool sensor) { return now_ms + (sensor ? 30000U : 5000U); }
inline bool record_state_result(uint32_t &last_state_ms, uint32_t completed_ms, bool valid_state) {
  if (!valid_state) return false;
  last_state_ms = completed_ms;
  return true;
}
inline void queue_command(bool &queued) { queued = true; }
inline bool can_start_command(bool pending, bool inflight, bool refresh_required = false) {
  return !pending && !inflight && !refresh_required;
}
inline void begin_command(bool &pending, bool &queued, bool &inflight, bool transport_accepted) {
  pending = true;
  queued = !transport_accepted;
  inflight = transport_accepted;
}
inline void timeout_command(bool &pending, bool &queued, bool &error, bool inflight, bool &refresh_required) {
  pending = false;
  queued = false;  // An intent not sent before timeout must never be actuated later.
  error = true;
  refresh_required = refresh_required || inflight;
}
inline bool finish_command(bool &inflight, bool &refresh_required) {
  if (!inflight) return false;
  inflight = false;
  refresh_required = true;
  return true;
}
inline bool confirm_command(bool matches, bool &pending, bool &inflight, bool &refresh_required, bool &error) {
  if (!pending || !matches) return false;
  pending = false;
  inflight = false;
  refresh_required = false;
  error = false;
  return true;
}
inline bool dimmer_release_event(bool released) { return released; }
inline bool start_queued_command(bool transport_busy, bool &queued) {
  if (transport_busy || !queued) return false;
  queued = false;
  return true;
}
inline bool controls_enabled(bool wifi_connected, bool hubitat_available, bool state_known,
                             uint32_t last_state_ms, uint32_t now_ms, uint32_t stale_ms, bool pending) {
  return wifi_connected && hubitat_available && fresh(state_known, last_state_ms, now_ms, stale_ms) && !pending;
}
inline const char *greeting_for_hour(int hour) {
  if (hour < 0 || hour > 23) return "Good day";
  if (hour < 5 || hour >= 22) return "Good night";
  if (hour < 12) return "Good morning";
  if (hour < 17) return "Good day";
  return "Good evening";
}
inline bool should_sleep(bool enabled, bool sensor_ready, bool motion_active,
                         uint32_t idle_ms, uint32_t timeout_ms) {
  return enabled && sensor_ready && !motion_active && idle_ms >= timeout_ms;
}
inline bool sensor_offline(bool health_offline, bool reported_active) {
  (void) reported_active;
  return health_offline;
}
}  // namespace esphome::hubitat_dashboard::state_logic
