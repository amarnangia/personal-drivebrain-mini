cmake_minimum_required(VERSION 3.16)
set(CMAKE_CXX_STANDARD 17)
project(mini-drivebrain)

find_package(Protobuf REQUIRED CONFIG)
find_package(foxglove-websocket REQUIRED)
find_package(mcap REQUIRED)
find_package(spdlog REQUIRED)

add_subdirectory(drivebrain_comms)    # runs drivebrain_comms/CMakeLists.txt

# Add your source files here
add_executable(mini-drivebrain
    drivebrain_app/main.cpp
    drivebrain_core/src/FoxgloveServer.cpp
    drivebrain_core/src/MCAPLogger.cpp
    proto/drivebrain_msgs.proto
)

# proto/drivebrain_msgs.proto -> #include <drivebrain_msgs.pb.h>
protobuf_generate(TARGET mini-drivebrain IMPORT_DIRS proto)

target_include_directories(mini-drivebrain PRIVATE drivebrain_core/include ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(mini-drivebrain PRIVATE
    drivebrain_comms                  # links the flowmeter driver into the program
    foxglove-websocket::foxglove-websocket
    mcap::mcap
    spdlog::spdlog
    protobuf::libprotobuf
)

# static so the binary runs on the pi without installing anything
target_link_options(mini-drivebrain PRIVATE -static)
