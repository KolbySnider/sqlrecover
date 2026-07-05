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
    src/util.cpp
    src/varint.cpp
    src/serial.cpp
    src/page.cpp
    src/mapped_file.cpp
    src/database.cpp
    src/btree.cpp
    src/schema.cpp
    src/artifact.cpp
    src/timeline.cpp
    src/recover.cpp
    src/wal.cpp
    src/output.cpp
    src/image.cpp
    src/filecarve.cpp
)

target_include_directories(sqlrecover PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Per-database recovery is parallelized across a thread pool.
find_package(Threads REQUIRED)
target_link_libraries(sqlrecover PRIVATE Threads::Threads)

install(TARGETS sqlrecover RUNTIME DESTINATION bin)
