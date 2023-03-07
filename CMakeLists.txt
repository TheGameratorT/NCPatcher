cmake_minimum_required(VERSION 3.20)

# Set the project name and version
project(ncpatcher VERSION 1.0)

if (WIN32)
add_compile_definitions(_CRT_SECURE_NO_DEPRECATE)
endif()

# Get RapidJson
include("${CMAKE_SOURCE_DIR}/vendor/rapidjson.cmake")
include_directories(${RAPIDJSON_INCLUDE_DIR})
# Get thread-pool
include("${CMAKE_SOURCE_DIR}/vendor/thread-pool.cmake")
include_directories(${THREADPOOL_INCLUDE_DIR})

# Find the source files
file(GLOB_RECURSE SOURCES "source/*.cpp")

# Add the executable
add_executable(${PROJECT_NAME} ${SOURCES} $<IF:$<BOOL:${WIN32}>,win_resources.rc,>)
add_dependencies(${PROJECT_NAME} rapidjson thread-pool)

# Specify the C++ standard
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)

# Copy ncp.h and ncp_h.s to the executable output directory
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/ncp.h" $<TARGET_FILE_DIR:${PROJECT_NAME}>
)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/ncp_asm.h" $<TARGET_FILE_DIR:${PROJECT_NAME}>
)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/ncp_ide.h" $<TARGET_FILE_DIR:${PROJECT_NAME}>
)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/ncprt.c" $<TARGET_FILE_DIR:${PROJECT_NAME}>
)
