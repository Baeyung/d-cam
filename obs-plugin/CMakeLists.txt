cmake_minimum_required(VERSION 3.10)
project(DroidCamOBSPlugin)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)

# For a real OBS plugin, you'd find libobs. For boilerplate, we'll just mock it or compile a shared library.
add_library(droidcam_usb_source SHARED src/plugin-main.c)
# We don't have libobs headers here easily, so we will stub the OBS macros for compilation success.
target_compile_definitions(droidcam_usb_source PRIVATE MOCK_OBS=1)
