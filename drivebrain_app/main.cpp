#include <FoxgloveServer.hpp>
#include <MCAPLogger.hpp>
#include <drivebrain_msgs.pb.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <fmt/chrono.h>
#include <spdlog/spdlog.h>
#include <cmath>
#include <memory>

std::atomic<bool> running{true};

void sig_handler(int signal) {
    if(signal == SIGINT || signal == SIGTERM) {
      spdlog::warn("Interrupted, stopping mini-drivebrain");
      running = false;
    }
}

int main() {
  std::signal(SIGINT, sig_handler);
  std::signal(SIGTERM, sig_handler);

  core::MCAPLogger::create(mcap::McapWriterOptions(""));
  core::FoxgloveServer::create();

  core::MCAPLogger::instance().open_new_mcap();
  core::MCAPLogger::instance().init_logging();

  spdlog::info("Constructed logging singletons");

  auto loop_time = 0.004;
  std::chrono::microseconds loop_time_ms((int) (loop_time * 1000000.0f));
  auto next_tick = std::chrono::steady_clock::now();

  // I ADDED: FAKE STATE UNTIL WE AHVE REAL DATA
  double fake_pulses = 0.0;
  uint64_t tick = 0;

  while(running) {

     // fake a flow that slowly swings between 20 and 80 Hz (about a 6 s period)
    double fake_freq_hz = 50.0 + 30.0 * std::sin(tick * loop_time);
    fake_pulses += fake_freq_hz * loop_time;
    tick++;

    auto msg = std::make_shared<drivebrain_msgs::FlowmeterData>();
    msg->set_total_pulses(static_cast<uint64_t>(fake_pulses));
    msg->set_last_pulse_timestamp_ns(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    msg->set_frequency_hz(fake_freq_hz);
    msg->set_missed_events(0);

    core::FoxgloveServer::instance().send_live_telem_msg(msg);
    core::MCAPLogger::instance().log_msg(msg);

    next_tick += loop_time_ms;

    auto now = std::chrono::steady_clock::now();
    if(now > next_tick) {
      spdlog::warn("Loop overrun by {}", now-next_tick);
      next_tick = now;
    }

    std::this_thread::sleep_until(next_tick);
  }

  core::MCAPLogger::instance().destroy();
  core::FoxgloveServer::instance().destroy();
  spdlog::info("Logging singletons safely destroyed");

  return 0;
}
