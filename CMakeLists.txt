cmake_minimum_required(VERSION 3.12.0)

project(NCPatcher VERSION 1.0)

# Set C++ 20 standard.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED true)

find_package(rapidjson REQUIRED)

# Add source to this project's executable.
file(GLOB_RECURSE SOURCES "${PROJECT_SOURCE_DIR}/source/*.cpp")
add_executable(${PROJECT_NAME} ${SOURCES})

if (NOT WIN32)
	target_link_libraries(${PROJECT_NAME} pthread)
endif()
