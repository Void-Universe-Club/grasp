# grasp: lightweight LLM-based agent framework (C++11 + RAII)
# cross-platform build: MSVC / MinGW / g++ (Linux/macOS)
# all OS-specific code lives in src/os.cpp behind the os:: API (see src/os.h)
cmake_minimum_required(VERSION 3.16)
project(grasp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(grasp
    src/main.cpp src/cli.cpp src/model.cpp src/store.cpp src/os.cpp
    src/session.cpp src/llm.cpp src/driver.cpp src/repl.cpp src/svg.cpp)

target_include_directories(grasp PRIVATE src third_party)

if(MSVC)
    # json.hpp is third-party: keep the warning level low; /utf-8 keeps Chinese messages sane
    target_compile_options(grasp PRIVATE /W3 /utf-8)
    target_compile_definitions(grasp PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(grasp PRIVATE -Wall -Wextra)
endif()

install(TARGETS grasp RUNTIME DESTINATION bin)
