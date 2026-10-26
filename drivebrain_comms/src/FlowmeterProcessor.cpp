#include <FlowmeterProcessor.hpp>

#include <chrono>
#include <spdlog/spdlog.h>
#include <deque>

namespace comms {
    // if we dont get pulses for this longk, that means the flow stopped and report 0 Hz instead of freezing 
    static constexpr uint64_t STALE_NS =  500'000'000; // 500 ms

    //requence is average over FREQ_WINDOW recent pulses 
    static constexpr size_t FREQ_WINDOW = 10;

    static uint64_t monotonic_now_ns() {
        //stead clock is CLOCK_MONOTONIC, which is not affected by system time changes
        //same clock GPIO event timestamps are based on, so we can use it to calculate the time between pulses
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    FlowmeterProcessor::FlowmeterProcessor(core::ThreadSafeDeque<FlowmeterPulse> &in_queue) : _in_queue(in_queue) {
    }

    FlowmeterProcessor::~FlowmeterProcessor() {
        stop();
    }

    void FlowmeterProcessor::start() {
        if(_thread.joinable()) {
            spdlog::warn("FlowmeterProcessor already started");
            return;
        }
        {
            std::lock_guard lock(_in_queue.mtx);
            _stopping = false;
        }
        _thread = std::thread([this]() { _process_loop(); });
        spdlog::info("FlowmeterProcessor started");
    }

    void FlowmeterProcessor::stop() {
        //set the flag under the queeu mutex so that the waiter doesnt miss 
        {
            std::lock_guard lock(_in_queue.mtx);
            _stopping = true;
        }
        //wake up consumer even though we dont ahve pulses yet 
        _in_queue.cv.notify_all();

        if (_thread.joinable()) {
            _thread.join();
            FlowmeterState s = get_latest();
            spdlog::info("Flowmeter processor stopped: {} pulses processed, {} missed", s.total_pulses, s.missed_events);
        }
    }

    FlowmeterState FlowmeterProcessor::get_latest() const{
        FlowmeterState s; 
        {
            //hold the lock only long enogh for us to copy the strucy
            std::lock_guard lock(_state_mutex);
            s = _state;
        }
        //we do a feshness check. if pulses stoppe,d the last frequenc is stale 
        uint64_t now_ns = monotonic_now_ns();
        if (s.total_pulses == 0 || (now_ns > s.last_pulse_timestamp && now_ns - s.last_pulse_timestamp > STALE_NS)) {
            s.frequency_hz = 0.0;
        }

        return s;
    }

    void FlowmeterProcessor::_process_loop() {
        //local buffer we move our pulses into so that slwo work happens without hte lock 
        std::deque<FlowmeterPulse> batch;

        while (true) {
            {
                std::unique_lock lock(_in_queue.mtx); 

                //sleeps which uses 0% cpu unitl we get a pulse or stop is called 
                //gaurds agaisnt spurious wakeups and agasitn a notify that happens before we start watiing 
                _in_queue.cv.wait(lock, [this]() {
                    return !_in_queue.deque.empty() || _stopping;
                });

                //exit once queue is drained 
                if (_stopping && _in_queue.deque.empty()) {
                    break;
                }

                //take everying in o(1). Producer gers the empry deque back instant 
                batch = std::move(_in_queue.deque);
                _in_queue.deque.clear(); //moved form is valid but unpecified, so make it empty
            } //release lock here at end of breackets

            for (const auto &pulse : batch) {
                _handle_pulse(pulse);
            }
            batch.clear();

            _publish();
        }

    }

    void FlowmeterProcessor::_handle_pulse(const FlowmeterPulse &pulse) {
        //misse devents. We shoudl have line seqno go up by one per pusel 
        if (_working.total_pulses > 0 && pulse.line_seqno > _last_seqno + 1) {
            _working.missed_events += pulse.line_seqno - _last_seqno - 1;
        }
        _last_seqno = pulse.line_seqno;

        //a long gap means that the flow stopepd nad restarted. It snot a pusle interval 
        if (!_recent_ts.empty() && pulse.timestamp > _recent_ts.back() + STALE_NS) {
            _recent_ts.clear();
        }
        _recent_ts.push_back(pulse.timestamp);
        if (_recent_ts.size() > FREQ_WINDOW) {
            _recent_ts.pop_front();
        }
        _working.total_pulses++;
        _working.last_pulse_timestamp = pulse.timestamp;
    }

    void FlowmeterProcessor:: _publish() {
        //frequency = (N - 1) intervals / time form first ot last pulse 
        _working.frequency_hz = 0.0;
        if (_recent_ts.size() >= 2) {
            uint64_t span_ns = _recent_ts.back() - _recent_ts.front();
            if (span_ns > 0) {
                _working.frequency_hz = (_recent_ts.size() - 1) * 1e9 / static_cast<double>(span_ns);
            }
        }
        std::lock_guard lock(_state_mutex);
        _state = _working;
    }
    
}