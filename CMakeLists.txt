cmake_minimum_required(VERSION 3.28)
project(soundspin VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)

set(CMAKE_C_FLAGS_RELEASE   "-O3 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)

FetchContent_Declare(
    sendspin-cpp
    GIT_REPOSITORY https://github.com/Sendspin/sendspin-cpp.git
    GIT_TAG v0.6.1
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(sendspin-cpp)
FetchContent_Declare(
    libshelbourne
    GIT_REPOSITORY https://github.com/davidfreina/libshelbourne.git
    GIT_TAG 59e66e72ba8c098c5c8dccf5083055a664b3b1f9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libshelbourne)

add_executable(soundspin
    src/main.cpp
    src/libshelbourne_sink.cpp
    src/config.cpp
    ${libshelbourne_SOURCE_DIR}/shelbourne.c
)
target_link_options(soundspin PRIVATE
    -static-libgcc
    -static-libstdc++
    -static
)
target_include_directories(soundspin PRIVATE
    src
)
target_include_directories(soundspin SYSTEM PRIVATE
    ${libshelbourne_SOURCE_DIR}
)
target_link_libraries(soundspin PRIVATE sendspin pthread m)
target_compile_features(soundspin PRIVATE cxx_std_20)