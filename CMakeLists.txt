cmake_minimum_required(VERSION 3.10)

project(InstyCompiler)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb -O0")

# PGO profile usage (if profile exists)
if(EXISTS "${CMAKE_SOURCE_DIR}/default.profdata")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-use=${CMAKE_SOURCE_DIR}/default.profdata")
    message(STATUS "Using PGO profile: ${CMAKE_SOURCE_DIR}/default.profdata")
endif()

# Prefer the bundled LLVM tree when present. The system LLVM package on some
# distributions ships only libLLVM.so plus a few static archives, which makes
# static component linking impossible and can mix versions with local archives.
set(INSTY_LLVM_ROOT "" CACHE PATH "Path to an LLVM install root")
set(INSTY_BUNDLED_LLVM_ROOT "/home/sky/Downloads/LLVM-22.1.7-Linux-X64")
if(NOT INSTY_LLVM_ROOT AND EXISTS "${INSTY_BUNDLED_LLVM_ROOT}/lib/cmake/llvm/LLVMConfig.cmake")
    set(INSTY_LLVM_ROOT "${INSTY_BUNDLED_LLVM_ROOT}" CACHE PATH "Path to an LLVM install root" FORCE)
endif()

if(INSTY_LLVM_ROOT AND EXISTS "${INSTY_LLVM_ROOT}/lib/cmake/llvm/LLVMConfig.cmake")
    set(LLVM_DIR "${INSTY_LLVM_ROOT}/lib/cmake/llvm" CACHE PATH "LLVM CMake package directory" FORCE)
endif()

# Find required dependencies for LLVM
find_package(zstd QUIET)
if(NOT zstd_FOUND)
    # If zstd package not found, try to find the library directly
    find_library(ZSTD_LIBRARY NAMES zstd)
    if(ZSTD_LIBRARY)
        add_library(zstd::libzstd_static UNKNOWN IMPORTED)
        set_target_properties(zstd::libzstd_static PROPERTIES
            IMPORTED_LOCATION "${ZSTD_LIBRARY}"
        )
        message(STATUS "Found zstd library: ${ZSTD_LIBRARY}")
    else()
        message(WARNING "zstd library not found - install libzstd-dev")
    endif()
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "LLVM library dir: ${LLVM_LIBRARY_DIRS}")

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

find_program(LLVM_CONFIG_EXECUTABLE NAMES llvm-config
    HINTS
        ${INSTY_LLVM_ROOT}/bin
        ${LLVM_TOOLS_BINARY_DIR}
    NO_DEFAULT_PATH
)
if(NOT LLVM_CONFIG_EXECUTABLE)
    find_program(LLVM_CONFIG_EXECUTABLE NAMES llvm-config)
endif()
if(NOT LLVM_CONFIG_EXECUTABLE)
    message(FATAL_ERROR "llvm-config not found; required for static LLVM linking")
endif()
message(STATUS "Using llvm-config: ${LLVM_CONFIG_EXECUTABLE}")

set(INSTY_LLVM_COMPONENTS
    support
    core
    irreader
    executionengine
    orcjit
    native
    x86asmparser
    x86codegen
    x86desc
    x86info
    aarch64asmparser
    aarch64codegen
    aarch64desc
    aarch64info
)

execute_process(
    COMMAND ${LLVM_CONFIG_EXECUTABLE} --libdir
    OUTPUT_VARIABLE LLVM_STATIC_LIBRARY_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)

# When cross-compiling (e.g. a Linux host building Windows binaries with a
# native llvm-config), the native llvm-config cannot enumerate the target's
# archives: both its `--libs` and `--libnames` modes validate that each archive
# exists in the *native* libdir and fail otherwise. Link the target install's
# archives directly instead (see below).
if(CMAKE_CROSSCOMPILING AND INSTY_LLVM_ROOT)
    set(LLVM_STATIC_LIBRARY_DIR "${INSTY_LLVM_ROOT}/lib")
    # Link every LLVM archive in the target install inside the --start-group/
    # --end-group below. The install was built with exactly the components this
    # compiler needs (X86 + AArch64 + core/JIT), so linking all of them both
    # satisfies the requested components and resolves their transitive
    # intra-LLVM references without having to reproduce llvm-config's dependency
    # closure (which the native llvm-config cannot compute for a cross target).
    file(GLOB LLVM_CROSS_ARCHIVES "${LLVM_STATIC_LIBRARY_DIR}/libLLVM*.a")
    set(LLVM_STATIC_LIBS_TEXT "")
    foreach(LLVM_CROSS_ARCHIVE IN LISTS LLVM_CROSS_ARCHIVES)
        get_filename_component(LLVM_CROSS_STEM "${LLVM_CROSS_ARCHIVE}" NAME_WE)
        string(REGEX REPLACE "^lib" "" LLVM_CROSS_STEM "${LLVM_CROSS_STEM}")
        set(LLVM_STATIC_LIBS_TEXT "${LLVM_STATIC_LIBS_TEXT} -l${LLVM_CROSS_STEM}")
    endforeach()
    # LLVM's Windows Support library depends on these system DLLs (ntdll for
    # RtlGetLastNtStatus, ws2_32 for sockets, etc.). The native llvm-config
    # cannot report the target's --system-libs, so list them explicitly.
    set(LLVM_STATIC_SYSTEM_LIBS_TEXT "-lntdll -lws2_32 -lpsapi -lshell32 -lole32 -luuid -ladvapi32")
else()
    execute_process(
        COMMAND ${LLVM_CONFIG_EXECUTABLE} --libs --link-static ${INSTY_LLVM_COMPONENTS}
        OUTPUT_VARIABLE LLVM_STATIC_LIBS_TEXT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    execute_process(
        COMMAND ${LLVM_CONFIG_EXECUTABLE} --system-libs --link-static
        OUTPUT_VARIABLE LLVM_STATIC_SYSTEM_LIBS_TEXT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

separate_arguments(LLVM_STATIC_LIBS_RAW UNIX_COMMAND "${LLVM_STATIC_LIBS_TEXT}")
separate_arguments(LLVM_STATIC_SYSTEM_LIBS UNIX_COMMAND "${LLVM_STATIC_SYSTEM_LIBS_TEXT}")

set(LLVM_STATIC_ARCHIVES)
foreach(LLVM_LIB_ARG IN LISTS LLVM_STATIC_LIBS_RAW)
    if(LLVM_LIB_ARG MATCHES "^-l(LLVM.*)$")
        set(LLVM_LIB_NAME "${CMAKE_MATCH_1}")
        find_library(LLVM_${LLVM_LIB_NAME}_ARCHIVE
            NAMES ${LLVM_LIB_NAME}
            PATHS ${LLVM_STATIC_LIBRARY_DIR}
            NO_DEFAULT_PATH
        )
        if(NOT LLVM_${LLVM_LIB_NAME}_ARCHIVE)
            message(FATAL_ERROR "Static LLVM archive ${LLVM_LIB_NAME} not found in ${LLVM_STATIC_LIBRARY_DIR}")
        endif()
        list(APPEND LLVM_STATIC_ARCHIVES ${LLVM_${LLVM_LIB_NAME}_ARCHIVE})
    else()
        list(APPEND LLVM_STATIC_ARCHIVES ${LLVM_LIB_ARG})
    endif()
endforeach()

set(LLVM_STATIC_SYSTEM_LIBS_RESOLVED)
foreach(LLVM_SYSTEM_LIB_ARG IN LISTS LLVM_STATIC_SYSTEM_LIBS)
    if(IS_ABSOLUTE "${LLVM_SYSTEM_LIB_ARG}" AND NOT EXISTS "${LLVM_SYSTEM_LIB_ARG}")
        get_filename_component(LLVM_SYSTEM_LIB_FILE "${LLVM_SYSTEM_LIB_ARG}" NAME)
        get_filename_component(LLVM_SYSTEM_LIB_STEM "${LLVM_SYSTEM_LIB_ARG}" NAME_WE)
        string(REGEX REPLACE "^lib" "" LLVM_SYSTEM_LIB_NAME "${LLVM_SYSTEM_LIB_STEM}")
        find_library(LLVM_SYSTEM_${LLVM_SYSTEM_LIB_NAME}_LIBRARY
            NAMES ${LLVM_SYSTEM_LIB_FILE} ${LLVM_SYSTEM_LIB_NAME}
            PATHS /usr/local/lib /usr/lib /lib
            NO_DEFAULT_PATH
        )
        if(NOT LLVM_SYSTEM_${LLVM_SYSTEM_LIB_NAME}_LIBRARY)
            message(FATAL_ERROR "LLVM system library ${LLVM_SYSTEM_LIB_ARG} was reported by llvm-config but does not exist")
        endif()
        list(APPEND LLVM_STATIC_SYSTEM_LIBS_RESOLVED ${LLVM_SYSTEM_${LLVM_SYSTEM_LIB_NAME}_LIBRARY})
    else()
        list(APPEND LLVM_STATIC_SYSTEM_LIBS_RESOLVED ${LLVM_SYSTEM_LIB_ARG})
    endif()
endforeach()

set(LLVM_LIBS -Wl,--start-group ${LLVM_STATIC_ARCHIVES} -Wl,--end-group ${LLVM_STATIC_SYSTEM_LIBS_RESOLVED})
message(STATUS "Using static LLVM archives from: ${LLVM_STATIC_LIBRARY_DIR}")

# Produce a self-contained Windows executable: statically link the C++ runtime
# (libc++), the unwinder and the compiler runtime so the .exe does not depend on
# libc++.dll / libunwind.dll being present alongside it.
if(CMAKE_CROSSCOMPILING AND WIN32)
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()

include_directories(include)

add_library(InstyLex STATIC
    src/lexer/lexer.cpp
    src/lexer/stringify.cpp

    include/lexer/lexer.hpp
)

add_library(InstyParse STATIC
    src/parser/parser.cpp
    src/parser/parserUtils.cpp
    src/parser/symbolTable.cpp
    src/parser/expressions/add.cpp
    src/parser/expressions/args.cpp
    src/parser/expressions/array.cpp
    src/parser/expressions/assign.cpp
    src/parser/expressions/call.cpp
    src/parser/expressions/dsn.cpp
    src/parser/expressions/expr.cpp
    src/parser/expressions/logical.cpp
    src/parser/expressions/member.cpp
    src/parser/expressions/memberCall.cpp
    src/parser/expressions/mul.cpp
    src/parser/expressions/object.cpp
    src/parser/expressions/primary.cpp
    src/parser/expressions/shift.cpp
    src/parser/expressions/unary.cpp
    src/parser/expressions/literals.cpp
    src/parser/expressions/operators.cpp

    src/parser/statements/fn.cpp
    src/parser/statements/if.cpp
    src/parser/statements/stmt.cpp
    src/parser/statements/struct.cpp
    src/parser/statements/class.cpp
    src/parser/statements/enum.cpp
    src/parser/statements/compileTimeIf.cpp
    src/parser/statements/vars-fn.cpp
    src/parser/statements/when.cpp
    src/parser/statements/switch.cpp
    src/parser/statements/while.cpp
    src/parser/statements/loop.cpp
    src/parser/statements/declarations.cpp
    src/parser/statements/control_flow.cpp

    include/parser/parser.hpp
)

add_library(InstyOptimize STATIC
    src/optimizer/optimizer.cpp

    include/optimizer/optimizer.hpp
)

add_library(InstyUtilities STATIC
    src/utilities/config.cpp
    src/utilities/utils.cpp
    src/utilities/values.cpp
    src/utilities/errors.cpp
    src/utilities/linker.cpp
    src/utilities/target.cpp
    src/utilities/type_system.cpp
    src/utilities/logger.cpp
    src/extra/builtins.cpp

    include/utilities/env.hpp
    include/utilities/toml.hpp
    include/utilities/utils.hpp
    include/utilities/errors.hpp
    include/utilities/linker.hpp
    include/utilities/logger.hpp
    include/extra/type_system.hpp
    include/extra/builtins.hpp
)

target_link_libraries(InstyUtilities PRIVATE ${LLVM_LIBS})

add_library(InstySema STATIC
    src/sema/analyzer.cpp
    src/sema/declarations.cpp
    src/sema/typecheck.cpp
    src/sema/diagnostics.cpp

    include/sema/sema.hpp
    include/sema/checker.hpp
)
target_link_libraries(InstySema PRIVATE InstyUtilities)

add_library(InstyCodegen STATIC
    src/codegen/core.cpp
    src/codegen/expressions.cpp
    src/codegen/statements.cpp
    src/codegen/functions.cpp
    src/codegen/types.cpp
    src/codegen/class.cpp
    src/codegen/generics.cpp
    src/codegen/interp.cpp
    src/codegen/strings.cpp
    src/codegen/runtime.cpp
    src/codegen/runtime_core.cpp
    src/codegen/runtime_alloc.cpp
    src/codegen/runtime_fmt.cpp
    src/codegen/runtime_std.cpp
    src/codegen/runtime_platform.cpp
    src/codegen/imports.cpp
    include/codegen/codegen.hpp
    include/extra/builtins.hpp
)

target_link_libraries(InstyCodegen PRIVATE ${LLVM_LIBS})

# Compiler driver layer (CLI-independent pipeline orchestration).
add_library(InstyDriver STATIC
    src/compiler/driver.cpp
    src/compiler/module_resolver.cpp

    include/compiler/driver.hpp
    include/compiler/module_resolver.hpp
)
target_link_libraries(InstyDriver PRIVATE ${LLVM_LIBS})

# Plugin system
add_library(InstyPlugin STATIC
    src/plugin/plugin_manager.cpp
    include/plugin/plugin_interface.hpp
    include/plugin/plugin_manager.hpp
)

target_link_libraries(InstyPlugin PRIVATE ${CMAKE_DL_LIBS})

add_executable(insty src/main.cpp)
target_link_libraries(insty PRIVATE InstyDriver InstyParse InstyLex InstyOptimize InstySema InstyCodegen InstyUtilities InstyPlugin ${LLVM_LIBS})

# Post-build: Copy the Insty standard library next to the compiler so that
# `import std::io` etc. resolve via <insty dir>/libs/std/<module>.ins (see
# ModuleResolver: `::` is the directory separator). copy_directory (rather than
# a configure-time GLOB) so newly added std modules are picked up without
# re-running cmake.
add_custom_command(TARGET insty POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/std $<TARGET_FILE_DIR:insty>/libs/std
    COMMENT "Copying Insty standard library to $<TARGET_FILE_DIR:insty>/libs/std"
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(INSTY_BUILD_TESTS "Build the compiler test suite" ON)

if(INSTY_BUILD_TESTS)
    enable_testing()

    # Unit tests exercise the frontend + sema directly (no LLVM linkage needed
    # beyond what the libraries already pull in).
    add_executable(insty_unit_tests
        tests/unit_tests.cpp
    )
    target_link_libraries(insty_unit_tests PRIVATE
        InstyParse InstyLex InstySema InstyUtilities ${LLVM_LIBS})
    add_test(NAME insty_unit_tests COMMAND insty_unit_tests)

    # Codegen tests build a tiny program and verify the emitted IR.
    add_executable(insty_codegen_tests
        tests/codegen_tests.cpp
    )
    target_link_libraries(insty_codegen_tests PRIVATE
        InstyDriver InstyParse InstyLex InstySema InstyCodegen
        InstyOptimize InstyUtilities ${LLVM_LIBS})
    add_test(NAME insty_codegen_tests COMMAND insty_codegen_tests)

    # End-to-end: run insty against fixture .ins files.
    add_test(
        NAME insty_e2e_emit_llvm
        COMMAND insty --emit-llvm -O0 --no-std
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/arithmetic.ins
                -o ${CMAKE_CURRENT_BINARY_DIR}/arithmetic.ll
    )
    add_test(
        NAME insty_e2e_invalid_type
        COMMAND insty --check --no-std
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bad_type.ins
    )
    set_tests_properties(insty_e2e_invalid_type PROPERTIES WILL_FAIL TRUE)

    # Classes + operator overloading build to an executable that returns 33.
    add_test(
        NAME insty_e2e_classes
        COMMAND insty --no-std
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/classes.ins
                -o ${CMAKE_CURRENT_BINARY_DIR}/classes_test
    )

    # Generic function monomorphization.
    add_test(
        NAME insty_e2e_generics
        COMMAND insty --no-std
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generics.ins
                -o ${CMAKE_CURRENT_BINARY_DIR}/generics_test
    )

    # Generic class monomorphization (Box<i32> / Box<i64> distinct layouts).
    add_test(
        NAME insty_e2e_generic_classes
        COMMAND insty --no-std
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generic_classes.ins
                -o ${CMAKE_CURRENT_BINARY_DIR}/generic_classes_test
    )

    # String interpolation: int/bool/char/text/float/struct/class formatting,
    # built into a stack buffer (no allocator, freestanding-safe).
    add_test(
        NAME insty_e2e_interpolation
        COMMAND insty --no-std
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/interpolation.ins
                -o ${CMAKE_CURRENT_BINARY_DIR}/interpolation_test
    )

    # Indexing a `text` yields its u8 bytes (regression: void-element load).
    add_test(
        NAME insty_e2e_text_index
        COMMAND insty --no-std
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/text_index.ins
                -o ${CMAKE_CURRENT_BINARY_DIR}/text_index_test
    )
endif()
