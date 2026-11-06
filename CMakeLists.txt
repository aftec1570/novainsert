cmake_minimum_required(VERSION 3.25)
project(NovaInsert VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
endif()

# Own code is warning-clean under -Wall -Wextra -Werror (clang/gcc) or /W4 /WX (MSVC); third-party is SYSTEM.
set(NI_WARNINGS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W4;/WX,-Wall;-Wextra;-Werror>")

add_subdirectory(external/JUCE)

add_subdirectory(src)
enable_testing()
add_subdirectory(tests)
