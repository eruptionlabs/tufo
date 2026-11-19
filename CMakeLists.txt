cmake_minimum_required(VERSION 3.16)
project(tufo VERSION 0.1.0 LANGUAGES CXX
        DESCRIPTION "Tufo: the .tuf compacted virtual-folder archive")

# Tufo builds on its own and also as a subdirectory of another project (the
# Eruption Engine ships it as tools/tufo). It exposes the target tufo::tufo.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(TUFO_IS_TOP_LEVEL ON)
else()
    set(TUFO_IS_TOP_LEVEL OFF)
endif()

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_tufo_cli_default ON)
if(ANDROID OR EMSCRIPTEN)
    set(_tufo_cli_default OFF)
endif()
option(TUFO_BUILD_CLI "Build the tufo command line tool" ${_tufo_cli_default})

# zstd. In order of preference:
#   1. TUFO_ZSTD_INCLUDE + TUFO_ZSTD_LIB (full zstd: compress + decompress + dictionaries)
#   2. TUFO_ZSTD_DECODER_TARGET: a CMake target that only decompresses (for
#      platforms without libzstd); archives can be read, not written
#   3. libzstd found on the system (find_path / find_library, or pkg-config)
set(TUFO_ZSTD_INCLUDE "" CACHE PATH "Directory with zstd.h and zdict.h")
set(TUFO_ZSTD_LIB "" CACHE FILEPATH "libzstd to link")
set(TUFO_ZSTD_DECODER_TARGET "" CACHE STRING "Existing target that provides only the zstd decoder")

set(TUFO_HAVE_COMPRESSOR OFF)
if(TUFO_ZSTD_INCLUDE AND TUFO_ZSTD_LIB)
    set(TUFO_HAVE_COMPRESSOR ON)
elseif(TUFO_ZSTD_DECODER_TARGET AND TARGET ${TUFO_ZSTD_DECODER_TARGET})
    message(STATUS "tufo: zstd decoder only (${TUFO_ZSTD_DECODER_TARGET}): archives are read-only")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(_tufo_zstd QUIET libzstd)
    endif()
    find_path(_tufo_zstd_inc zstd.h HINTS ${_tufo_zstd_INCLUDE_DIRS})
    find_library(_tufo_zstd_lib zstd HINTS ${_tufo_zstd_LIBRARY_DIRS})
    if(_tufo_zstd_inc AND _tufo_zstd_lib)
        set(TUFO_ZSTD_INCLUDE "${_tufo_zstd_inc}")
        set(TUFO_ZSTD_LIB "${_tufo_zstd_lib}")
        set(TUFO_HAVE_COMPRESSOR ON)
    else()
        message(FATAL_ERROR "tufo: zstd not found. Install libzstd-dev, or pass -DTUFO_ZSTD_INCLUDE=<dir> "
                            "-DTUFO_ZSTD_LIB=<libzstd>, or -DTUFO_ZSTD_DECODER_TARGET=<target>.")
    endif()
endif()

find_package(Threads REQUIRED)

add_library(tufo_lib STATIC src/Archive.cpp src/Ops.cpp src/Cli.cpp)
add_library(tufo::tufo ALIAS tufo_lib)
set_target_properties(tufo_lib PROPERTIES OUTPUT_NAME tufo POSITION_INDEPENDENT_CODE ON)
target_include_directories(tufo_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(tufo_lib PRIVATE Threads::Threads)
if(TUFO_HAVE_COMPRESSOR)
    target_compile_definitions(tufo_lib PRIVATE TUFO_HAVE_ZSTD=1)
    target_include_directories(tufo_lib SYSTEM PRIVATE "${TUFO_ZSTD_INCLUDE}")
    target_link_libraries(tufo_lib PRIVATE "${TUFO_ZSTD_LIB}")
else()
    target_link_libraries(tufo_lib PRIVATE ${TUFO_ZSTD_DECODER_TARGET})
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tufo_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(TUFO_BUILD_CLI)
    add_executable(tufo src/tufo_main.cpp)
    target_link_libraries(tufo PRIVATE tufo::tufo)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(tufo PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    install(TARGETS tufo RUNTIME DESTINATION bin)
endif()

if(TUFO_IS_TOP_LEVEL)
    install(TARGETS tufo_lib ARCHIVE DESTINATION lib)
    install(DIRECTORY include/ DESTINATION include)
endif()
