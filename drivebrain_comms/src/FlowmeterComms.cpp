#include <FlowmeterComms.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <spdlog/spdlog.h>

namespace comms {
    //this is how long poll() waits before returning to loop. WOrse case delay for stop()
    static constexpr int POLL_TIMEOUT_MS = 100;

    //max events taken out of the kernel every read(). Grab in batches means less syscalls and less chance of dropping events
    static constexpr int MAX_EVENTS_PER_READ = 16;

    //name shown by gpioinfo as the lines owner 
    static constexpr const char *CONSUMER_NAME = "mini-drivebrain-flowmeter";

    //only sotres the settings. opeening hardware happens in start() so that we can return an error code if the line cant be opened intead of throwing exception
    FlowmeterComms::FlowmeterComms(core::ThreadSafeDeque<FlowmeterPulse> &out_queue, const std::string &chip_path, uint32_t line_offset)
        : _out_queue(out_queue), _chip_path(chip_path), _line_offset(line_offset) {
    }

    //RAII, regardless of how object dies, the thread is joined, fd is cloded. 
    FlowmeterComms::~FlowmeterComms() {
        stop();
    }

    bool FlowmeterComms::start() {
        //already runing, so no re run otherewise we woudl assign a new thread 
        if (_running) {
            spdlog::warn("FlowmeterComms::start() called while already running");
            return true;
        }

        //if prev run ende don its own (error during read loop), we ahve finisehd its thread but havent joined it and its fd is sitll open. Stop() cleans that 
        stop();

        //get the line fd from kernel. we dont spawn kernel if nothign to read
        if (!_open_line()) {
            return false;
        }

        //set the flag before spwn thread so that thread doenst accieentally instantly exit 
        _running = true;

        //spawn reader thread. Lambda capptures "this" so that the thread can call memebr funcitons 
        _thread = std::thread([this]() { _read_loop(); });

        spdlog::info("FlowmeterComms started for chip {} line {}", _chip_path, _line_offset);
        return true;
    }

    void FlowmeterComms::stop() {
        //set the flag so that the thread exits its loop. Will notive within one POLL_TIMEOUT_MS, so worst case delay for stop() is that long.
        _running = false;

        //wait for thread to curally exit. joinable is false if we didnt start or already joined
        if (_thread.joinable()) {
            _thread.join();
            spdlog::info("FlowmeterComms stopped for chip {} line {}", _chip_path, _line_offset);
        }

        //cldoe the pd only afer the join, os that we dont use a dead fd in the thread.
        // The thread will exit on its own when it sees _running is false, so we dont need to force it to wake up.
        if (_line_fd >= 0) {
            close(_line_fd);
            _line_fd = -1; //note that -1 ,eans not open so if we call stop again it wont close it 
        }
    }

    bool FlowmeterComms::_open_line() {
        //open the chip and request the line as a rising edge input. We set _line_fd
        int chip_fd = open(_chip_path.c_str(), O_RDONLY | O_CLOEXEC); //we  dont leak o_cloexec's fd inot any program we might exec() later.
        // We dont need to write to the chip, so O_RDONLY is fine

        if (chip_fd < 0) {
            spdlog::error("Failed to open GPIO chip {}: {}", _chip_path, std::strerror(errno)); //errno says why
            return false;
        }

        //now we ned to describe the line we want. pasdding fields must be zero and also use 0 as default 
        gpio_v2_line_request req{}; //this {} defautls everyrhting to 0 in the structure 
        
        req.offsets[0] = _line_offset; //we only want one line, so we set the first offset and set num_lines to 1
        req.num_lines = 1;

        //owner name for gpioinof. size - 1 so that he last byte is always 0, which is required by the kernel.
        //  The kernel will not copy more than GPIO_MAX_NAME_SIZE bytes, so we are safe.
        std::strncpy(req.consumer, CONSUMER_NAME, sizeof(req.consumer) - 1);

        //input, read form the pin and never drive it. Rising edge, so we get an event when the pin goes from low to high.
        //edge rising. 
        //we do bias pull up since flowmeters ususalyl have open collector output, we only pull form the low line. Nee
        //NEED TO VERIFY THE ABOVE IN CLASS 
        req.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;

        //now we hand the request to the kernel. The kernel will return a fd for the line, which we can use to read events from it.
        int ret = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);

        //save errno, close below is another system call adn coould override 
        int ioctl_errno = errno;

        //the chips fd job is done. Keep line fd valid on its own 
        close(chip_fd);

        if (ret < 0) {
            //common: ebusy means another process has the line open. EINVAL means the line doesnt exist on this chip. EACCES means we dont have permission to open it.
            spdlog::error("Failed to request GPIO line {} on chip {}: {}", _line_offset, _chip_path, std::strerror(ioctl_errno));
            return false;
        }

        _line_fd = req.fd; //the kernel gave us a fd for the line, which we can use to read events from it. We will close it in stop() or destructor.
        spdlog::info("Requested {} line {} as rising-edge input", _chip_path, _line_offset);
        return true;
    }

    void FlowmeterComms::_read_loop() {
        //poll() takes an array of pds to wats. We will jstu watch one, our line fd
        //pollin says wake me up when ther ei ssometihgn to read 
        pollfd pfd{};
        pfd.fd = _line_fd;
        pfd.events = POLLIN;

        //buffer for read() declare once outside the loop 
        gpio_v2_line_event events[MAX_EVENTS_PER_READ];

        while (_running) {
            //wait for an edge, but not longer that poll timeout 
            int ready = poll(&pfd, 1, POLL_TIMEOUT_MS);

            if (ready == 0) {
                //we timedout with no edges, need ot re loop 
                continue;
            }

            if (ready < 0) {
                //EINRT is a signal liek ctrl-c, we will just re loop and check _running. Other errors are fatal
                if (errno == EINTR) {
                    continue;
                } else {
                    spdlog::error("Poll failed on GPIO line {}: {}", _line_offset, std::strerror(errno));
                    break;
                }
            }

            //the data is ready, so read() coppies as many queued avents as we ahve room for in the buffer. 
            // We will read up to MAX_EVENTS_PER_READ events at a time. The kernel will drop events if we dont read them fast enough, so we want to read as many as possible each time.
            ssize_t bytes = read(_line_fd, events, sizeof(events));
            if (bytes < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue; //signal, re loop
                } else {
                    spdlog::error("Read failed on GPIO line {}: {}", _line_offset, std::strerror(errno));
                    break;
                }
            }

            //read() returns BYTES not events, so the kernal only hands out whoel events 
            size_t num_events = static_cast<size_t>(bytes) / sizeof(gpio_v2_line_event);
            if (num_events == 0) {
                spdlog::warn("Read 0 bytes from GPIO line {}, this should not happen", _line_offset);
                continue;
            }

            //hadn teh pulses to the consuper 
            {
                //;pcl tje deque mutex so that we can push to the deque. 
                // We will push all events in one lock/unlock cycle to minimize contention with the consumer.
                std::lock_guard lock(_out_queue.mtx);
            
                for (size_t i = 0; i < num_events; ++i) { 
                    //keep only what we neeed later. We need when it happens and sequenc number
                    //sequence number sisa. jobp > 1 between pulses means the kernel dropped events. We can use that to detect if we are missing pulses. 
                    _out_queue.deque.push_back(FlowmeterPulse{events[i].timestamp_ns, events[i].line_seqno});
                }
            } //mutex is released here 

            //wake the customer sinc eit i ssleeping in sv.wait 
            _out_queue.cv.notify_one();
        }
        //we get here form stop or an error break. This nees a cleared flag so taht start knows this run is over
        _running = false;
        spdlog::info("FlowmeterComms read loop exited for chip {}", _chip_path );
    }
}