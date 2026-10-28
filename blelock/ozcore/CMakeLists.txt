# ozcore — portable OZKIE v2 core (ozkey-49). Host build: unit tests on macOS/Linux.
#   cmake -S blelock/ozcore -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
# The AK1037 build compiles the same src/ files with the Anyka toolchain plus port/ak1037.
cmake_minimum_required(VERSION 3.16)
project(ozcore C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(ozcore STATIC
    src/oz_cbor.c
    src/oz_edhoc.c
    src/oz_coap.c
    src/oz_oscore.c
    src/oz_invite.c
    src/oz_lock.c
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

add_executable(test_cbor tests/test_cbor.c)
target_link_libraries(test_cbor ozport_host)
add_test(NAME cbor COMMAND test_cbor)

add_executable(test_edhoc tests/test_edhoc.c)
target_link_libraries(test_edhoc ozport_host)
add_test(NAME edhoc COMMAND test_edhoc)

add_executable(test_oscore tests/test_oscore.c)
target_link_libraries(test_oscore ozport_host)
add_test(NAME oscore COMMAND test_oscore)

add_executable(test_invite tests/test_invite.c)
target_link_libraries(test_invite ozport_host)
add_test(NAME invite COMMAND test_invite)

add_executable(test_lock tests/test_lock.c)
target_link_libraries(test_lock ozport_host)
add_test(NAME lock COMMAND test_lock)

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
        # Independent Python app (RFC-written EDHOC/OSCORE) against the host lock.
        add_test(NAME interop_python
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/ozapp.py
                         interop $<TARGET_FILE:ozhostlock>)
    else()
        message(STATUS "python cryptography not found: skipping ccm_differential")
    endif()
endif()

# Host test lock for interop (ozkey-49 P2): tools/hostlock.c, driven by tools/ozapp.py.
add_executable(ozhostlock tools/hostlock.c)
target_link_libraries(ozhostlock ozport_host)
