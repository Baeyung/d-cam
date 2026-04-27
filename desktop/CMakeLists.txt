cmake_minimum_required(VERSION 3.10)
project(DroidCamDesktopService)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread")

add_executable(desktop_service src/main.cpp)
