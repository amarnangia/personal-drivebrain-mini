#pragma once
#include <DriverBus.hpp>
#include <FlowmeterComms.hpp>

#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace comms {
    //this is a snapshot of everyhitng our main loop needs to know about the flowmeter. 
    // It is a snapshot of the state at a point in time, so that we can pass it to the consumer without worrying about it changing while we are using it.
    struct FlowmeterState {
        uint64_t total_pulses = 0;
        uint64_t last_pulse_timestamp = 0; //timestamp in nanoseconds, this CLOCK_MONOTONIC, so it is not wall clock time. It is only useful for measuring time between pulses.
        double frequency_hz = 0.0; // is calculated by the consumer thread, so it is not updated in real time. It is only updated when the consumer thread pops a pulse from the queue.
        uint64_t missed_events = 0; // is calculated by the consumer thread
    };


    class FlowmeterProcessor {
        public:
            // @param in_queue is the same queue flowmerCOmms pushes onto. Rememebr, this is owned by main
            explicit FlowmeterProcessor(core::ThreadSafeDeque<FlowmeterPulse> &in_queue);

            //stop and join consumer thread 
            ~FlowmeterProcessor();

            //spawn consumer thread 
            void start();

            //wake thead and drain what is left in the queue before we join 
            void stop();

            //safe from any thread. we apply no flow timeout so frequency falls to 0 once puless stop 
            FlowmeterState get_latest() const; 

            FlowmeterProcessor(const FlowmeterProcessor &) = delete;
            FlowmeterProcessor &operator=(const FlowmeterProcessor &) = delete;

        private:
            //runs in _thread. sleeps in cv.waitn, grab bratch, process, adn publish 
            void _process_loop();

            //update the _working with one pulse 
            void _handle_pulse(const FlowmeterPulse &pulse);

            //copy _working into _state under _state_mutex
            void _publish();

            core::ThreadSafeDeque<FlowmeterPulse> &_in_queue;
            std::thread _thread;

            //protected by _in_queue.mtx, not atomic: the cv.wait predicate reads it while holding that mutex
            bool _stopping = false;

            //only the consumer thread touches this, so no lock needed 
            FlowmeterState _working;
            //the last sequence number seen, so we can detect if the kernel dropped events. This is only used by the consumer thread, so no lock needed
            uint32_t _last_seqno = 0;
            //last few pulse timestamps, for frequency. Only used by the consumer thread
            std::deque<uint64_t> _recent_ts;

            //waht get_latest() will read. Mutable  because we want const getter to lock in 
            mutable std::mutex _state_mutex;
            FlowmeterState _state;
    };
}