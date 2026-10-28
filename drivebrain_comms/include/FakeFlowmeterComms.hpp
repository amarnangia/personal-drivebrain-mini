#pragma once
#include <DriverBus.hpp>
#include <FlowmeterComms.hpp> //for FlowmeterPulse, so the fake pushes the exact same struct the real driver does

#include <atomic>
#include <cstdint>
#include <thread>

namespace comms {
    //this is a test stand in for FlowmeterComms. It pushes fake pulses into the same queue at a rate we know,
    //so we can test the processor, main loop, foxglove, and mcap without a pi or a flowmeter.
    //pattern: flowing for 3 s, then stopped for 2 s, then repeat. The stopped part tests that frequency drops to 0 Hz
    //it has the same shape as the real driver (constructor takes the queue, start(), stop()) so main treats them the same
    class FakeFlowmeterComms {
        public:
            /**
            @param out_queue same queue the real driver pushes into. Owned by main so it outlives this
            @param rate_hz how many pulses per second while we are "flowing"
            @param drop_every skip one line_seqno every N pulses to pretend the kernel dropped an event (0 = never drop)
            */
            FakeFlowmeterComms(core::ThreadSafeDeque<FlowmeterPulse> &out_queue, double rate_hz, uint32_t drop_every = 0);

            //RAII, stop and join the thread no matter how this object dies
            ~FakeFlowmeterComms();

            //spawn the thread that pushes pulses. Returns bool just to match the real driver, this one cant fail
            bool start();

            //tell the thread to exit and join it. Safe to call twice, or if we never started
            void stop();

            //we own a thread, so copying makes no sense (two copies would both try to join it)
            FakeFlowmeterComms(const FakeFlowmeterComms &) = delete;
            FakeFlowmeterComms &operator=(const FakeFlowmeterComms &) = delete;

        private:
            //runs inside _thread. Sleeps one period, pushes a pulse, repeat until _running is false
            void _run();

            core::ThreadSafeDeque<FlowmeterPulse> &_out_queue;
            double _rate_hz;
            uint32_t _drop_every;

            std::thread _thread;
            //atomic because stop() writes it from main while _run reads it from our thread
            std::atomic<bool> _running{false};

            //only our thread touches these while it is running. stop() reads them after the join, so no lock needed
            //we log these at the end and compare against the processor's counts to prove nothing got lost
            uint64_t _pushed = 0;
            uint64_t _dropped = 0;
    };
}
