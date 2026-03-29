cmake_minimum_required(VERSION 3.16)
project(BoWWServer_x86)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. System Packages ---
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(nlohmann_json REQUIRED)

# Server-specific requirements
find_package(ALSA REQUIRED)
find_package(yaml-cpp REQUIRED)

# --- 2. Fetch & Build TensorFlow Lite ---
message(STATUS "Fetching TensorFlow Lite (x86_64 Optimized)...")
include(FetchContent)
FetchContent_Declare(
  tensorflow-lite
  GIT_REPOSITORY https://github.com/tensorflow/tensorflow.git
  GIT_TAG        v2.16.1      
  GIT_SHALLOW    TRUE         
  SOURCE_SUBDIR  tensorflow/lite
)

# Force x86 CPU optimizations
set(TFLITE_ENABLE_XNNPACK ON CACHE BOOL "Enable XNNPACK")
set(TFLITE_ENABLE_RUY ON CACHE BOOL "Enable RUY matrix multiplication")
set(BUILD_TESTING OFF CACHE BOOL "Disable TF tests")
FetchContent_MakeAvailable(tensorflow-lite)

# --- 3. Sources ---
set(SOURCES
    src/main.cpp
    src/BoWWServer.cpp
    src/ClientSession.cpp
    src/GroupController.cpp
    src/ConfigManager.cpp
    src/AlsaSinkManager.cpp
    src/WavWriter.cpp
    src/feature_extract.cpp
    src/tflite_runner.cpp
    src/pffft.c
    src/MDNSService.cpp
)

add_executable(boww_server ${SOURCES})

# Include local headers
target_include_directories(boww_server PRIVATE include)

# --- 4. Linking ---
target_link_libraries(boww_server PRIVATE
    Threads::Threads
    Boost::system
    Boost::thread
    nlohmann_json::nlohmann_json
    yaml-cpp
    ALSA::ALSA
    avahi-client
    avahi-common
    tensorflow-lite
)
