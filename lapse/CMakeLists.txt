cmake_minimum_required(VERSION 3.14)
project(lapse VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(lapse src/lapse.cpp src/sha256.cpp)

if(MSVC)
  target_compile_options(lapse PRIVATE /W4)
else()
  target_compile_options(lapse PRIVATE -Wall -Wextra)
endif()

install(TARGETS lapse RUNTIME DESTINATION bin)
