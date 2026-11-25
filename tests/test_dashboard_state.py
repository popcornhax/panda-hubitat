"""Compile and exercise the same C++ state helpers used by the LVGL dashboard."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DashboardStateNativeTests(unittest.TestCase):
    def test_state_logic_cpp(self):
        compiler = shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("a native C++ compiler is required")
        source = r'''#include "components/hubitat_dashboard/state_logic.h"
#include <cassert>
#include <cstring>
using namespace esphome::hubitat_dashboard::state_logic;
int main() {
  assert(std::strcmp(greeting_for_hour(4), "Good night") == 0);
  assert(std::strcmp(greeting_for_hour(5), "Good morning") == 0);
  assert(std::strcmp(greeting_for_hour(11), "Good morning") == 0);
  assert(std::strcmp(greeting_for_hour(12), "Good day") == 0);
  assert(std::strcmp(greeting_for_hour(16), "Good day") == 0);
  assert(std::strcmp(greeting_for_hour(17), "Good evening") == 0);
  assert(std::strcmp(greeting_for_hour(21), "Good evening") == 0);
  assert(std::strcmp(greeting_for_hour(22), "Good night") == 0);
  assert(std::strcmp(greeting_for_hour(-1), "Good day") == 0);
  assert(!should_sleep(false, true, false, 30000, 30000));
  assert(!should_sleep(true, false, false, 30000, 30000));
  assert(!should_sleep(true, true, true, 30000, 30000));
  assert(!should_sleep(true, true, false, 29999, 30000));
  assert(should_sleep(true, true, false, 30000, 30000));
  uint32_t last = 100;
  assert(fresh(true, last, 200, 500));
  assert(!fresh(true, last, 601, 500));
  // A failed GET records its completion elsewhere but leaves the successful-state age unchanged.
  assert(!record_state_result(last, 900, false));
  assert(last == 100);
  assert(record_state_result(last, 950, true));
  assert(last == 950);
  assert(!command_matches(true, -1, false, 0));
  assert(!command_matches(true, 50, true, 40));
  assert(command_matches(true, 50, true, 52));
  assert(dimmer_release_event(true));
  assert(!dimmer_release_event(false));
  assert(retry_delay(1, 2500, 30000) == 2500);
  assert(retry_delay(4, 2500, 30000) == 20000);
  assert(retry_delay(8, 2500, 30000) == 30000);
  assert(successful_poll_due(1000, false) == 6000);
  assert(successful_poll_due(1000, true) == 31000);
  bool queued = false;
  queue_command(queued);
  assert(queued && !start_queued_command(true, queued) && queued);
  assert(start_queued_command(false, queued) && !queued);
  // An unsent intent is cancelled when the user's confirmation window expires.
  bool pending = false, inflight = false, error = false, refresh = false;
  begin_command(pending, queued, inflight, false);  // transport busy: queue the intent
  assert(pending && queued && !inflight);
  timeout_command(pending, queued, error, inflight, refresh);
  assert(!pending && !queued && error && !refresh);
  assert(!start_queued_command(false, queued));  // timeout cannot later actuate it

  // A dispatched command that times out keeps its transport gate until its late ACK.
  begin_command(pending, queued, inflight, true);
  assert(pending && !queued && inflight);
  timeout_command(pending, queued, error, inflight, refresh);
  assert(!pending && error && inflight && refresh);
  assert(!can_start_command(pending, inflight, refresh));
  assert(finish_command(inflight, refresh));  // late ACK ends the request only
  assert(!pending && !inflight && error && refresh);
  assert(!can_start_command(pending, inflight, refresh));
  refresh = false;  // a fresh GET is required before another control
  assert(error && !pending && !inflight);  // stale failure remains visible until a new intent
  error = false;
  assert(can_start_command(pending, inflight, refresh));

  // State confirmation clears pending/error only when the fresh state matches.
  begin_command(pending, queued, inflight, true);
  error = true;
  assert(!confirm_command(false, pending, inflight, refresh, error));
  assert(pending && inflight && error);
  assert(confirm_command(true, pending, inflight, refresh, error));
  assert(!pending && !inflight && !refresh && !error);
  assert(!controls_enabled(false, true, true, 100, 200, 500, false));
  assert(!controls_enabled(true, false, true, 100, 200, 500, false));
  assert(!controls_enabled(true, true, true, 100, 700, 500, false));
  assert(!controls_enabled(true, true, true, 100, 200, 500, true));
  assert(controls_enabled(true, true, true, 100, 200, 500, false));
  // Offline presence overrides a previously reported active motion state.
  assert(sensor_offline(true, true));
  assert(!sensor_offline(false, true));
}
'''
        with tempfile.TemporaryDirectory() as temp:
            src = Path(temp) / "state_test.cpp"
            exe = Path(temp) / "state_test"
            src.write_text(source, encoding="utf-8")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Werror", "-I", str(ROOT), str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
