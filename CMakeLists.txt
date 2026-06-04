cmake_minimum_required(VERSION 3.16)
project(sqlrecover LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
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
    src/database.cpp
    src/btree.cpp
    src/schema.cpp
    src/artifact.cpp
    src/timeline.cpp
    src/recover.cpp
    src/wal.cpp
    src/output.cpp
    src/image.cpp
)

target_include_directories(sqlrecover PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Optional: physical-image carving via The Sleuth Kit. Enable with -DUSE_TSK=ON.
# When on, the --image path can carve .db files out of a raw EXT4/F2FS image.
option(USE_TSK "Link libtsk for raw image carving" OFF)
if(USE_TSK)
    find_library(TSK_LIB tsk)
    if(TSK_LIB)
        target_compile_definitions(sqlrecover PRIVATE SQLRECOVER_USE_TSK=1)
        target_link_libraries(sqlrecover PRIVATE ${TSK_LIB})
    else()
        message(WARNING "USE_TSK requested but libtsk not found; building without it")
    endif()
endif()

install(TARGETS sqlrecover RUNTIME DESTINATION bin)
