# Possible values:
# - `address` (ASan)
# - `memory` (MSan)
# - `thread` (TSan)
# - `undefined` (UBSan)
# - "" (no sanitizing)
option (SANITIZE "Enable one of the code sanitizers" "")

set (SAN_FLAGS "${SAN_FLAGS} -g -fno-omit-frame-pointer -DSANITIZER")

if (SANITIZE)
    if (SANITIZE STREQUAL "address")
        # https://github.com/llvm/llvm-project/issues/59007
        set (ASAN_FLAGS "-fsanitize=address -fsanitize-address-use-after-scope -lresolv")
        if (COMPILER_CLANG)
            if (${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER_EQUAL 15 AND ${CMAKE_CXX_COMPILER_VERSION} VERSION_LESS 16)
                # LLVM-15 has a bug in Address Sanitizer, preventing the usage
                # of 'sanitize-address-use-after-scope', see [1].
                #
                #   [1]: https://github.com/llvm/llvm-project/issues/58633
                set (ASAN_FLAGS "${ASAN_FLAGS} -fno-sanitize-address-use-after-scope")
            endif()
        endif()
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SAN_FLAGS} ${ASAN_FLAGS}")
        set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SAN_FLAGS} ${ASAN_FLAGS}")

        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${ASAN_FLAGS}")
        endif()
        if (USE_STATIC_LIBRARIES AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libasan")
        endif ()

    elseif (SANITIZE STREQUAL "memory")
        # MemorySanitizer flags are set according to the official documentation:
        # https://clang.llvm.org/docs/MemorySanitizer.html#usage
        #
        # For now, it compiles with `cmake -DSANITIZE=memory -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS_ADD="-O1" -DCMAKE_C_FLAGS_ADD="-O1"`
        # Compiling with -DCMAKE_BUILD_TYPE=Debug leads to ld.lld failures because
        # of large files (was not tested with ld.gold). This is why we compile with
        # RelWithDebInfo, and downgrade optimizations to -O1 but not to -Og, to
        # keep the binary size down.
        # TODO: try compiling with -Og and with ld.gold.
        set (MSAN_FLAGS "-fsanitize=memory -fsanitize-memory-use-after-dtor -fsanitize-memory-track-origins -fno-optimize-sibling-calls -fsanitize-blacklist=${PROJECT_SOURCE_DIR}/tests/msan_suppressions.txt")

        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SAN_FLAGS} ${MSAN_FLAGS}")
        set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SAN_FLAGS} ${MSAN_FLAGS}")

        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=memory")
        endif()
        if (USE_STATIC_LIBRARIES AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libmsan")
        endif ()

    elseif (SANITIZE STREQUAL "thread")
        set (TSAN_FLAGS "-fsanitize=thread")
        if (COMPILER_CLANG)
            set (TSAN_FLAGS "${TSAN_FLAGS} -fsanitize-blacklist=${PROJECT_SOURCE_DIR}/tests/tsan_suppressions.txt")
        endif()


        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SAN_FLAGS} ${TSAN_FLAGS}")
        set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SAN_FLAGS} ${TSAN_FLAGS}")
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
        endif()
        if (USE_STATIC_LIBRARIES AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libtsan")
        endif ()

    elseif (SANITIZE STREQUAL "undefined")
        set (UBSAN_FLAGS "-fsanitize=undefined -fno-sanitize-recover=all -fno-sanitize=float-divide-by-zero")
        if (ENABLE_FUZZING)
            # Unsigned integer overflow is well defined behaviour from a perspective of C++ standard,
            # compilers or CPU. We use in hash functions like SipHash and many other places in our codebase.
            # This flag is needed only because fuzzers are run inside oss-fuzz infrastructure
            # and they have a bunch of flags not halt the program if UIO happend and even to silence that warnings.
            # But for unknown reason that flags don't work with ClickHouse or we don't understand how to properly use them,
            # that's why we often receive reports about UIO. The simplest way to avoid this is just  set this flag here.
            set(UBSAN_FLAGS "${UBSAN_FLAGS} -fno-sanitize=unsigned-integer-overflow")
        endif()
        if (COMPILER_CLANG)
            set (UBSAN_FLAGS "${UBSAN_FLAGS} -fsanitize-blacklist=${PROJECT_SOURCE_DIR}/tests/ubsan_suppressions.txt")
        endif()

        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SAN_FLAGS} ${UBSAN_FLAGS}")
        set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SAN_FLAGS} ${UBSAN_FLAGS}")
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=undefined")
        endif()
        if (USE_STATIC_LIBRARIES AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libubsan")
        endif ()

        # llvm-tblgen, that is used during LLVM build, doesn't work with UBSan.
        set (ENABLE_EMBEDDED_COMPILER 0 CACHE BOOL "")

    else ()
        message (FATAL_ERROR "Unknown sanitizer type: ${SANITIZE}")
    endif ()
endif()
