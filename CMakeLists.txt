cmake_minimum_required(VERSION 3.20)
project(lapse VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
               Debug Release RelWithDebInfo MinSizeRel)
endif()

add_executable(lapse src/lapse.cpp src/sha256.cpp)
target_compile_features(lapse PRIVATE cxx_std_17)
target_compile_definitions(lapse PRIVATE LAPSE_VERSION="${PROJECT_VERSION}")
set_target_properties(lapse PROPERTIES CXX_EXTENSIONS OFF)

if(WIN32)
  target_link_libraries(lapse PRIVATE advapi32)
endif()

if(MSVC)
  target_compile_options(lapse PRIVATE /W4 /utf-8)
else()
  target_compile_options(lapse PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS lapse RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
  find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME lapse.integration
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py"
        "$<TARGET_FILE:lapse>"
    )
    set_tests_properties(lapse.integration PROPERTIES TIMEOUT 120)
  else()
    message(WARNING "Python 3.9+ was not found; integration tests are unavailable")
  endif()
endif()
