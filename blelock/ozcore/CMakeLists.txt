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
    src/oz_frame.c
    src/oz_ble.c
    src/oz_adv.c
    src/oz_crypto.c
    src/sha256_sw.c
    src/aes128_sw.c)
target_include_directories(ozcore PUBLIC include)
target_compile_options(ozcore PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

# Monocypher 4.0.2 (BSD-2 / CC0), X25519 until Anyka exposes the PKE curve API.
add_library(monocypher STATIC third_party/monocypher/monocypher.c)
target_include_directories(monocypher PUBLIC third_party/monocypher)

# littlefs 2.4.1 (BSD-3), upstream: the version the AK1037 SDK ships (cosmetic edits only).
# Host-only here; the AK1037 build uses the SDK's copy.
add_library(littlefs STATIC third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c)
target_include_directories(littlefs SYSTEM PUBLIC third_party/littlefs)
target_compile_definitions(littlefs PUBLIC LFS_NO_DEBUG LFS_NO_WARN)

# kv on littlefs (ozkey-49 P4), shared by the AK1037 port and the host test.
add_library(ozkv_lfs STATIC port/common/oz_kv_lfs.c)
target_include_directories(ozkv_lfs PUBLIC port/common)
target_link_libraries(ozkv_lfs PUBLIC ozcore littlefs)
target_compile_options(ozkv_lfs PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

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

add_executable(test_frame tests/test_frame.c)
target_link_libraries(test_frame ozport_host)
add_test(NAME frame COMMAND test_frame)

add_executable(test_kv_lfs tests/test_kv_lfs.c)
target_link_libraries(test_kv_lfs ozkv_lfs ozport_host)
add_test(NAME kv_lfs COMMAND test_kv_lfs)

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

add_executable(test_adv tests/test_adv.c)
target_link_libraries(test_adv ozport_host)
add_test(NAME adv COMMAND test_adv)
