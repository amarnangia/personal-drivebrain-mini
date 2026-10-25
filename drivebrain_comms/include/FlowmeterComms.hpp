#pragma once
#include <linux/gpio.h>
#include <DriverBus.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace comms {
    //this is one rising edge seen on the flowmeters GPIO line 
    //no prootbuf so that the driver doenst depend on the definition of the message
    struct FlowmeterPulse {
        uint64_t timestamp; //timestamp in nanoseconds
        uint32_t line_seqno; // the per line sequence number. If we have a gap, the kernel is dorpping events. This is a good way to detect that.
    };

    class FlowmeterComms {
        public:
            /**
            @param out_queue this is where pulses get pushed. Owned by main so it outlives this driver
            @param chip_path thigns like /dev/gpiochip0
            @param line_offset the offset of the line on the chip to watch for pulses
            */
            FlowmeterComms(core::ThreadSafeDeque<FlowmeterPulse> &out_queue, const std::string &chip_path, uint32_t line_offset);

            //stop[ and join the reader thread
            ~FlowmeterComms();

            /**
            * opens the GPIO line and spawn a thread to read pulses 
            * @return false if this line catn be opened, which means the thread is not started
            */
            bool start();

            // stops and joins the reader thread 
            void stop();

            //own a thread and file descriptor, so we dont need to copy this class
            FlowmeterComms(const FlowmeterComms &) = delete;
            FlowmeterComms &operator=(const FlowmeterComms &) = delete;
        

        private:
            /**
             * opens the chip and requests the line as a rising edge input. We set _line_fd
             */
             bool _open_line();

             // runs within _thread and waits for edges dn pusehs them to the out queue unti they stop 
             void _read_loop();

             core::ThreadSafeDeque<FlowmeterPulse> &_out_queue;
             std::string _chip_path;
             uint32_t _line_offset;

             int _line_fd = -1; // the file descriptor for the line, set by _open_line
             std::thread _thread;
             std::atomic<bool> _running{false};
            
    };
}
