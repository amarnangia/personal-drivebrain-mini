#include <FoxgloveServer.hpp>
#include <MCAPLogger.hpp>
#include <drivebrain_msgs.pb.h>
#include <FlowmeterComms.hpp>
#include <FakeFlowmeterComms.hpp>
#include <FlowmeterProcessor.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <fmt/chrono.h>
#include <spdlog/spdlog.h>
#include <memory>
#include <string>

std::atomic<bool> running{true};

//runs when we get ctrl-c (SIGINT) or systemd stops us (SIGTERM). Only sets the flag,
//almost nothing else is safe to do inside a signal handler. The main loop sees it and starts shutdown
void sig_handler(int signal) {
    if(signal == SIGINT || signal == SIGTERM) {
      spdlog::warn("Interrupted, stopping mini-drivebrain");
      running = false;
    }
}

int main(int argc, char **argv) {
  //`mini-drivebrain --fake` uses the fake flowmeter so we can test without a pi
  //no flag = real GPIO. The systemd service runs it with no flag
  //this has to be asked for on purpose, we never fall back to fake on our own, otherwise a broken driver on the pi would look like it works
  bool use_fake = argc > 1 && std::string(argv[1]) == "--fake";

  std::signal(SIGINT, sig_handler);
  std::signal(SIGTERM, sig_handler);

  core::MCAPLogger::create(mcap::McapWriterOptions(""));
  core::FoxgloveServer::create();

  core::MCAPLogger::instance().open_new_mcap();
  core::MCAPLogger::instance().init_logging();

  //main own the queu, sot he driver pushes into it
  //consumer thread will eventually pop form it. Must outlive both threads, so we put it here
  core::ThreadSafeDeque<comms::FlowmeterPulse> pulse_queue;

  //consumer: sleeps in cv.wait on pulse_queue, turns pulses into total/frequency/missed
  //we start it before the producer so it is already waiting when the first pulse shows up
  comms::FlowmeterProcessor processor(pulse_queue);
  processor.start();

  //TO DO: REPLACE WITH THE REAL CHIP AND LINE
  comms::FlowmeterComms flowmeter(pulse_queue, "/dev/gpiochip0", 4);

  //fake producer: 50 Hz, and fakes one kernel drop every 100 pulses so we can test missed_events
  //we create both producers but only start one. The one we dont start never spawns a thread, so it costs nothing
  comms::FakeFlowmeterComms fake_flowmeter(pulse_queue, 50.0, 100);

  if (use_fake) {
    fake_flowmeter.start();
  } else if (!flowmeter.start()) {
    //keep running so logging still works, but say loudly that there is no real data. Everything will just read 0
    spdlog::error("Failed to start flowmeter driver. Run with --fake to test without hardware");
  }

  spdlog::info("Constructed logging singletons");

  auto loop_time = 0.004;
  std::chrono::microseconds loop_time_ms((int) (loop_time * 1000000.0f));
  auto next_tick = std::chrono::steady_clock::now();

  while(running) {

    //grab the consumer's latest snapshot. get_latest locks its mutex just long enough to copy the struct,
    //so this never waits on the pulse queue and never blocks our 4 ms loop
    //it also sets frequency to 0 if there has been no pulse for 500 ms
    comms::FlowmeterState state = processor.get_latest();

    //same message as phase 2, just with real values instead of the fake sine wave
    auto msg = std::make_shared<drivebrain_msgs::FlowmeterData>();
    msg->set_total_pulses(state.total_pulses);
    msg->set_last_pulse_timestamp_ns(state.last_pulse_timestamp);
    msg->set_frequency_hz(state.frequency_hz);
    msg->set_missed_events(state.missed_events);

    //foxglove sends it right now over the websocket, mcap pushes it into its own queue and its writer thread saves it
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

  //this stops and join our thread before the loggers are destroyed, destructor only does thait at the end of main after destroy
  //order matters here:
  //1. producers first, so nothing gets pushed into the queue after the consumer is gone
  //   calling stop on the one we never started is fine, its thread isnt joinable so it does nothing
  flowmeter.stop();
  fake_flowmeter.stop();

  //2. then the consumer. It wakes up, drains whatever pulses are still in the queue, then exits and gets joined
  //   it logs "N pulses processed, D missed", which should match the producer's "pushed N, faked D dropped" line
  processor.stop();

  //3. loggers last, now no thread can still be using them. destroy flushes and closes the mcap file
  core::MCAPLogger::instance().destroy();
  core::FoxgloveServer::instance().destroy();
  spdlog::info("Logging singletons safely destroyed");

  return 0;
}
