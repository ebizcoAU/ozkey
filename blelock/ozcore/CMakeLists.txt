# ozcore — portable OZKIE v2 core (ozkey-49). Host build: unit tests on macOS/Linux.
#   cmake -S blelock/ozcore -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
# The AK1037 build compiles the same src/ files with the Anyka toolchain plus port/ak1037.
cmake_minimum_required(VERSION 3.16)
project(ozcore C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(ozcore STATIC
    src/oz_crypto.c
    src/sha256_sw.c
    src/aes128_sw.c)
target_include_directories(ozcore PUBLIC include)
target_compile_options(ozcore PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

# Monocypher 4.0.2 (BSD-2 / CC0), X25519 until Anyka exposes the PKE curve API.
add_library(monocypher STATIC third_party/monocypher/monocypher.c)
target_include_directories(monocypher PUBLIC third_party/monocypher)

add_library(ozport_host STATIC port/host/oz_port_host.c)
target_include_directories(ozport_host PUBLIC port/host)
target_compile_definitions(ozport_host PRIVATE _DEFAULT_SOURCE)
target_link_libraries(ozport_host PUBLIC ozcore monocypher)
target_compile_options(ozport_host PRIVATE -Wall -Wextra -Werror)

enable_testing()

add_executable(test_crypto tests/test_crypto.c)
target_link_libraries(test_crypto ozport_host)
add_test(NAME crypto_kat COMMAND test_crypto)

add_executable(ccm_vec tests/ccm_vec.c)
target_link_libraries(ccm_vec ozport_host)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -I -c "import cryptography"
                    RESULT_VARIABLE _no_crypto OUTPUT_QUIET ERROR_QUIET)
    if(_no_crypto EQUAL 0)
        add_test(NAME ccm_differential
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/diff_ccm.py
                         $<TARGET_FILE:ccm_vec> 3000)
    else()
        message(STATUS "python cryptography not found: skipping ccm_differential")
    endif()
endif()
