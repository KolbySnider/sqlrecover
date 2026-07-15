cmake_minimum_required(VERSION 3.16)
project(sqlrecover LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(sqlrecover
    src/main.cpp
    src/output.cpp
    src/core/util.cpp
    src/core/varint.cpp
    src/core/serial.cpp
    src/format/page.cpp
    src/format/mapped_file.cpp
    src/format/database.cpp
    src/format/btree.cpp
    src/format/schema.cpp
    src/format/wal.cpp
    src/recovery/recover.cpp
    src/artifact/artifact.cpp
    src/artifact/timeline.cpp
    src/carve/image.cpp
    src/carve/filecarve.cpp
)

target_include_directories(sqlrecover PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Per-database recovery is parallelized across a thread pool.
find_package(Threads REQUIRED)
target_link_libraries(sqlrecover PRIVATE Threads::Threads)

install(TARGETS sqlrecover RUNTIME DESTINATION bin)
