cmake_minimum_required(VERSION 3.20)

# set the project name and version
project(NCPatcher VERSION 1.0)

# specify the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED true)

# find the required packages
find_package(RapidJSON)

# find the source files
file(GLOB SOURCES "source/*.cpp")

# add the executable
add_executable(${PROJECT_NAME} ${SOURCES})
