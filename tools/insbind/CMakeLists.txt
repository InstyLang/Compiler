# insbind: C/C++ header -> Insty binding generator.
# Dependency-free (C++ std only) and statically linked like the compiler, so
# the shipped executable needs nothing installed on the user's machine.
#
# Builds two ways:
#   - as part of the compiler project (add_subdirectory from the root), or
#   - standalone:  cmake -S tools/insbind -B tools/insbind/build-win -G Ninja
# Standalone mode is what CI uses to exercise the tool on its own.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.15)
    project(insbind CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release)
    endif()
    # Match the compiler: static CRT, no VC++ redistributable required.
    if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    enable_testing()
    set(INSBIND_STANDALONE ON)
endif()

add_executable(insbind
    src/main.cpp
    src/constexpr.cpp
    src/emit.cpp
    src/layout.cpp
    src/lexer.cpp
    src/macro.cpp
    src/model.cpp
    src/parse.cpp
    src/ppeval.cpp
    src/preproc.cpp
    src/selftest.cpp
    src/constexpr.h
    src/emit.h
    src/layout.h
    src/lexer.h
    src/macro.h
    src/model.h
    src/parse.h
    src/ppeval.h
    src/preproc.h
    src/selftest.h
    src/token.h
)

# One CTest entry runs the whole golden suite; the harness reports per-file
# results. Update goldens with: insbind self-test tools/insbind/tests --bless
if(INSTY_BUILD_TESTS OR INSBIND_STANDALONE)
    add_test(NAME insbind_self_test
        COMMAND insbind self-test ${CMAKE_CURRENT_SOURCE_DIR}/tests)
endif()
