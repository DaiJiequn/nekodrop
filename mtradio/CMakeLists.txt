cmake_minimum_required(VERSION 3.20)
project(mtradio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W4 /permissive- /utf-8)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)

add_library(mtcore STATIC src/mt7902.cpp src/mt7902_radio.cpp src/mt7902_sta.cpp src/proxy.cpp src/dma.cpp)
target_include_directories(mtcore PUBLIC src)
target_link_libraries(mtcore PUBLIC setupapi cfgmgr32)

add_executable(mtprobe src/probe_main.cpp)
target_link_libraries(mtprobe PRIVATE mtcore winmm)

add_executable(nekodrop-radio-mt src/radio_mt_main.cpp)
target_link_libraries(nekodrop-radio-mt PRIVATE mtcore winmm advapi32)
