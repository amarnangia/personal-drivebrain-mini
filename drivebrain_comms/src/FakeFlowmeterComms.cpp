#include <FakeFlowmeterComms.hpp>

#include <chrono>
#include <spdlog/spdlog.h>

namespace comms {
    //only stores the settings. The thread doesnt start until start(), same as the real driver
    FakeFlowmeterComms::FakeFlowmeterComms(core::ThreadSafeDeque<FlowmeterPulse> &out_queue, double rate_hz, uint32_t drop_every)
        : _out_queue(out_queue), _rate_hz(rate_hz), _drop_every(drop_every) {
    }

    //RAII, if main forgets to call stop() the thread still gets joined here
    FakeFlowmeterComms::~FakeFlowmeterComms() {
        stop();
    }

    bool FakeFlowmeterComms::start() {
        //already running, dont spawn a second thread on top of the first one
        if (_running) {
            return true;
        }

        //clean up a previous run that is finished but not joined yet, same as the real driver
        stop();

        //set the flag before spawning so the thread doesnt see false and instantly exit
        _running = true;

        //lambda captures "this" so the thread can call our member function
        _thread = std::thread([this]() { _run(); });

        //warn, not info, so its obvious in the logs that this data is fake
        spdlog::warn("Using FAKE flowmeter: {} Hz, 3 s on / 2 s off, dropping 1 every {} pulses", _rate_hz, _drop_every);
        return true;
    }

    void FakeFlowmeterComms::stop() {
        //tell the thread to exit. It checks this every loop, so worst case it notices within one period (20 ms at 50 Hz)
        _running = false;

        //joinable is false if we never started or already joined, so calling stop twice is fine
        if (_thread.joinable()) {
            _thread.join();
            //safe to read these without a lock now, the thread that wrote them has exited
            //compare these numbers to the processor's "processed" and "missed" line. They should match exactly
            spdlog::info("Fake flowmeter stopped: pushed {} pulses, faked {} dropped", _pushed, _dropped);
        }
    }

    void FakeFlowmeterComms::_run() {
        using clock = std::chrono::steady_clock;

        //time between pulses. 1 second / rate, so 50 Hz -> 20 ms
        const auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e9 / _rate_hz));

        //remember when we started so we know where we are in the 3 s on / 2 s off cycle
        const auto cycle_start = clock::now();
        auto next = cycle_start;

        //the kernel starts line_seqno at 1, so starting at 0 means the first ++ gives 1
        uint32_t seqno = 0;

        while (_running) {
            //sleep_until an absolute time instead of sleep_for, so time spent pushing doesnt add up and make the rate drift
            //same idea as the sleep_until in main's loop
            next += period;
            std::this_thread::sleep_until(next);

            //where are we in the 5 s cycle? 0-3 s we are flowing, 3-5 s we push nothing
            //% gives us the time since the start of the current cycle
            auto in_cycle = (clock::now() - cycle_start) % std::chrono::seconds(5);
            if (in_cycle >= std::chrono::seconds(3)) {
                continue;
            }

            seqno++;
            if (_drop_every != 0 && seqno % _drop_every == 0) {
                //pretend the kernel lost this event. Skip its seqno so the consumer sees a gap of 1
                //this is the only way to test missed_events, the real kernel basically never drops
                seqno++;
                _dropped++;
            }

            //steady_clock is CLOCK_MONOTONIC on linux, the same clock real GPIO event timestamps use
            //so the processor's stale check works exactly the same on fake and real pulses
            uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();

            //hand the pulse to the consumer, exactly how the real driver does it: lock, push, unlock, notify
            {
                std::lock_guard lock(_out_queue.mtx);
                _out_queue.deque.push_back(FlowmeterPulse{now_ns, seqno});
            } //mutex is released here

            //wake the consumer since it is sleeping in cv.wait. Notify after unlocking so it doesnt wake up and block on our lock
            _out_queue.cv.notify_one();
            _pushed++;
        }
    }
}
