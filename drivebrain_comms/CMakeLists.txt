find_package(spdlog REQUIRED)

add_library(drivebrain_comms src/FlowmeterComms.cpp src/FlowmeterProcessor.cpp src/FakeFlowmeterComms.cpp)
target_include_directories(drivebrain_comms PUBLIC include ${CMAKE_SOURCE_DIR}/drivebrain_core/include)
target_link_libraries(drivebrain_comms PUBLIC spdlog::spdlog)
