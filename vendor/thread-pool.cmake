include(ExternalProject)

# Download thread-pool
ExternalProject_Add(
	thread-pool
    PREFIX "vendor/thread-pool"
    GIT_REPOSITORY "https://github.com/bshoshany/thread-pool.git"
    GIT_TAG 67fad04348b91cf93bdfad7495d298f54825602c
    TIMEOUT 10
    CMAKE_ARGS
        -DRAPIDJSON_BUILD_TESTS=OFF
        -DRAPIDJSON_BUILD_DOC=OFF
        -DRAPIDJSON_BUILD_EXAMPLES=OFF
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
)

# Prepare thread-pool
ExternalProject_Get_Property(thread-pool source_dir)
set(THREADPOOL_INCLUDE_DIR ${source_dir})
