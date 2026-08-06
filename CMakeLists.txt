# 3.15 is required for CMAKE_MSVC_RUNTIME_LIBRARY (policy CMP0091), used by the
# native Windows (MSVC / clang-cl) build to select a static CRT.
cmake_minimum_required(VERSION 3.15)

project(InstyCompiler)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Most modern distributions link executables as PIE by default. The static
# libraries (notably third_party/fadec, which emits absolute relocations) must
# therefore be position independent, or linking `insty` fails with
#   relocation R_X86_64_32S cannot be used against local symbol
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Detect whether the active compiler speaks the MSVC command-line dialect. This
# covers both Visual Studio's cl.exe and clang-cl (CMAKE_CXX_COMPILER_FRONTEND_
# VARIANT == "MSVC"). GNU-style flags (-fno-rtti, -Wl,...) are rejected by these
# drivers, so the build flags are selected per dialect below.
if(MSVC OR (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    set(INSTY_MSVC_DIALECT TRUE)
else()
    set(INSTY_MSVC_DIALECT FALSE)
endif()

# Release builds are optimized. The two compiler dialects need entirely
# different flag spellings:
if(INSTY_MSVC_DIALECT)
    # MSVC / clang-cl dialect:
    #  /Gy + /OPT:REF /OPT:ICF : function-level linking + dead-strip + identical
    #                  COMDAT folding (the equivalent of --gc-sections/--icf).
    #  /EHsc         : standard C++ exception model.
    add_compile_options(/Gy /EHsc)
    add_link_options(/OPT:REF /OPT:ICF)
else()
    #  -ffunction/-fdata-sections + --gc-sections : drop unreferenced code/data.
    #  -fuse-ld=lld --icf=all : fold identical functions.
    #  -s                 : strip symbols.
    # (Set -DCMAKE_BUILD_TYPE=Debug for a debuggable build.)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld -Wl,--gc-sections -Wl,--icf=all -s")
endif()

# PGO profile usage (if profile exists). The -fprofile-use spelling is a
# Clang/GCC driver flag; clang-cl accepts it via -Xclang-less /clang: pass-through
# but plain MSVC does not, so only enable it for the GNU dialect.
if(EXISTS "${CMAKE_SOURCE_DIR}/default.profdata" AND NOT INSTY_MSVC_DIALECT)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-use=${CMAKE_SOURCE_DIR}/default.profdata")
    message(STATUS "Using PGO profile: ${CMAKE_SOURCE_DIR}/default.profdata")
endif()

# Produce a self-contained Windows executable: statically link the C++ runtime
# (libc++), the unwinder and the compiler runtime so the .exe does not depend on
# libc++.dll / libunwind.dll being present alongside it. This is a GNU/llvm-mingw
# concept; with MSVC the CRT model is selected via CMAKE_MSVC_RUNTIME_LIBRARY
# instead (see below).
if(CMAKE_CROSSCOMPILING AND WIN32 AND NOT INSTY_MSVC_DIALECT)
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()

# For native MSVC/clang-cl builds, statically link the C runtime so the produced
# insty.exe does not require the VC++ redistributable to be installed.
if(INSTY_MSVC_DIALECT AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
    src/parser/statements/for.cpp
    src/parser/statements/loop.cpp
    src/parser/statements/declarations.cpp
    src/parser/statements/control_flow.cpp

    include/parser/parser.hpp
)

add_library(InstyUtilities STATIC
    src/utilities/config.cpp
    src/utilities/utils.cpp
    src/utilities/values.cpp
    src/utilities/errors.cpp
    src/utilities/linker.cpp
    src/utilities/target.cpp
    src/utilities/type_system.cpp
    src/utilities/ast_clone.cpp
    src/utilities/logger.cpp
    src/extra/builtins.cpp

    include/utilities/env.hpp
    include/utilities/toml.hpp
    include/utilities/utils.hpp
    include/utilities/errors.hpp
    include/utilities/linker.hpp
    include/utilities/logger.hpp
    include/extra/type_system.hpp
    include/extra/ast_clone.hpp
    include/extra/builtins.hpp
)

add_library(InstySema STATIC
    src/sema/analyzer.cpp
    src/sema/declarations.cpp
    src/sema/typecheck.cpp
    src/sema/diagnostics.cpp

    include/sema/sema.hpp
    include/sema/checker.hpp
)
target_link_libraries(InstySema PRIVATE InstyUtilities)

# Compiler driver layer (CLI-independent pipeline orchestration).
add_library(InstyDriver STATIC
    src/compiler/driver.cpp
    src/compiler/comptime.cpp
    src/compiler/module_resolver.cpp
    src/compiler/runtime_core.cpp

    include/compiler/driver.hpp
    include/compiler/module_resolver.hpp
    include/compiler/runtime_core.hpp
)
target_link_libraries(InstyDriver PRIVATE InstyBackend InstyParse InstyLex InstySema InstyUtilities)

# Plugin system
add_library(InstyPlugin STATIC
    src/plugin/plugin_manager.cpp
    include/plugin/plugin_interface.hpp
    include/plugin/plugin_manager.hpp
)

target_link_libraries(InstyPlugin PRIVATE ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------------------
# Custom code-generation backend.
#
# Emits machine code via the vendored Fadec x86-64 encoder and writes object
# files directly. The vendored Fadec C source ships pre-generated instruction
# tables (third_party/fadec), so no Python is needed at build time.
# ---------------------------------------------------------------------------
option(INSTY_BUILD_BACKEND "Build the custom codegen backend" ON)

if(INSTY_BUILD_BACKEND)
    add_library(InstyBackend STATIC
        third_party/fadec/encode2.c
        src/backend/machine_code.cpp
        src/backend/encoder.cpp
        src/backend/coff_writer.cpp
        src/backend/elf_writer.cpp
        src/backend/pe_writer.cpp
        src/backend/macho_writer.cpp
    src/backend/const_eval.cpp
    src/backend/asm_block.cpp
    src/backend/wasm_writer.cpp
        src/backend/wasm_emit.cpp
        src/backend/abi.cpp
        src/backend/regalloc.cpp
        src/backend/lower.cpp
        src/backend/isel.cpp
        src/backend/module_emit.cpp
        src/backend/mir_opt.cpp

        include/backend/machine_code.hpp
        include/backend/encoder.hpp
        include/backend/coff_writer.hpp
        include/backend/elf_writer.hpp
        include/backend/pe_writer.hpp
        include/backend/macho_writer.hpp
        include/backend/const_eval.hpp
        include/backend/wasm_writer.hpp
        include/backend/wasm_emit.hpp
        include/backend/reg.hpp
        include/backend/abi.hpp
        include/backend/machine_ir.hpp
        include/backend/regalloc.hpp
        include/backend/lower.hpp
        include/backend/isel.hpp
        include/backend/module_emit.hpp
        include/backend/mir_opt.hpp
    )
    # Fadec's headers and the generated *.inc tables are included as
    # <fadec-enc2.h> / <fadec-encode2-*.inc>, so put third_party/fadec on the
    # include path for this target (and its consumers).
    target_include_directories(InstyBackend PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fadec)
    # The instruction selector consumes the AST / sema / type-system headers.
    # These are LLVM-free, so InstyBackend stays free of any LLVM dependency.
    target_include_directories(InstyBackend PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    # No LLVM, no system libs: the backend is self-contained.
endif()

set(INSTY_BACKEND_LINK_LIBS)
if(TARGET InstyBackend)
    set(INSTY_BACKEND_LINK_LIBS InstyBackend)
endif()

add_executable(insty src/main.cpp)
target_link_libraries(insty PRIVATE InstyDriver InstyParse InstyLex InstySema InstyUtilities InstyPlugin ${INSTY_BACKEND_LINK_LIBS})

# Post-build: Copy the Insty standard library tree next to the compiler so that
# `import std::io`, `import core::mem`, `import windows::io` etc. resolve via
# <insty dir>/libs/<dir>/<module>.ins (see ModuleResolver: `::` is the directory
# separator and <insty dir>/libs is the std search root). copy_directory (rather
# than a configure-time GLOB) so newly added modules are picked up without
# re-running cmake.
add_custom_command(TARGET insty POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/libs $<TARGET_FILE_DIR:insty>/libs
    COMMENT "Copying Insty standard library to $<TARGET_FILE_DIR:insty>/libs"
)
add_custom_target(insty_stdlib_copy ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/libs $<TARGET_FILE_DIR:insty>/libs
    DEPENDS insty
    COMMENT "Refreshing Insty standard library in $<TARGET_FILE_DIR:insty>/libs"
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(INSTY_BUILD_TESTS "Build the compiler test suite" ON)

if(INSTY_BUILD_TESTS)
    enable_testing()

    # Unit tests exercise the frontend + sema directly.
    add_executable(insty_unit_tests
        tests/unit_tests.cpp
    )
    target_link_libraries(insty_unit_tests PRIVATE
        InstyParse InstyLex InstySema InstyUtilities)
    add_test(NAME insty_unit_tests COMMAND insty_unit_tests)

    # Backend: encode a tiny `main` (return 42), write COFF + ELF64 objects,
    # and validate each container.
    if(INSTY_BUILD_BACKEND)
        add_executable(insty_backend_tests tests/backend_tests.cpp)
        target_link_libraries(insty_backend_tests PRIVATE InstyBackend)
        # The test writes <base>.obj (COFF) and <base>.o (ELF64).
        set(INSTY_BACKEND_BASE ${CMAKE_CURRENT_BINARY_DIR}/backend_main)
        add_test(
            NAME insty_backend_objects
            COMMAND insty_backend_tests ${INSTY_BACKEND_BASE}
        )

        # ABI + register allocation + lowering scaffold unit tests.
        add_executable(insty_backend_scaffold_tests tests/backend_scaffold_tests.cpp)
        target_link_libraries(insty_backend_scaffold_tests PRIVATE InstyBackend)
        add_test(
            NAME insty_backend_scaffold
            COMMAND insty_backend_scaffold_tests
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )

        # Instruction-selector unit tests (AST + SemaResult -> Machine IR -> object).
        add_executable(insty_backend_isel_tests tests/backend_isel_tests.cpp)
        target_link_libraries(insty_backend_isel_tests PRIVATE InstyBackend InstyUtilities)
        add_test(
            NAME insty_backend_isel
            COMMAND insty_backend_isel_tests
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )

        # Machine-IR optimization-pass unit tests (peephole / DCE / branch).
        add_executable(insty_backend_opt_tests tests/backend_opt_tests.cpp)
        target_link_libraries(insty_backend_opt_tests PRIVATE InstyBackend)
        add_test(
            NAME insty_backend_opt
            COMMAND insty_backend_opt_tests
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )

        # WebAssembly binary writer: LEB128, section framing/ordering, type
        # interning, index spaces, and instruction encodings, checked against
        # golden bytes. Also emits two .wasm artifacts for the optional
        # runtime-validation test below.
        add_executable(insty_wasm_writer_tests tests/wasm_writer_tests.cpp)
        target_link_libraries(insty_wasm_writer_tests PRIVATE InstyBackend)
        set(INSTY_WASM_BASE ${CMAKE_CURRENT_BINARY_DIR}/wasm_writer_test)
        add_test(
            NAME insty_wasm_writer
            COMMAND insty_wasm_writer_tests ${INSTY_WASM_BASE}
        )

        # Validate (and run) the emitted modules with a real WebAssembly
        # toolchain. Neither tool is a build dependency: when they are absent
        # the test reports "not found" and passes, so the encoder's golden-byte
        # tests above remain the hard guarantee. Install wasm-tools and/or
        # wasmtime to turn this into a real check.
        add_test(
            NAME insty_wasm_validate
            COMMAND ${CMAKE_COMMAND}
                -DADD_WASM=${INSTY_WASM_BASE}_add.wasm
                -DSTART_WASM=${INSTY_WASM_BASE}_start.wasm
                -DALL_WASM=${INSTY_WASM_BASE}_all.wasm
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_validate.cmake
        )
        set_tests_properties(insty_wasm_validate PROPERTIES DEPENDS insty_wasm_writer)


        # End-to-end (COFF, Windows): link the emitted .obj and run it; it must
        # exit 42. lld-link wants an entry point and no CRT (body is just mov/ret).
        find_program(INSTY_LLD_LINK NAMES lld-link)
        if(INSTY_LLD_LINK AND WIN32)
            add_test(
                NAME insty_backend_link_run_coff
                COMMAND ${CMAKE_COMMAND}
                    -DLLD=${INSTY_LLD_LINK}
                    -DOBJ=${INSTY_BACKEND_BASE}.obj
                    -DEXE=${INSTY_BACKEND_BASE}.exe
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_link_run.cmake
            )
            set_tests_properties(insty_backend_link_run_coff PROPERTIES
                DEPENDS insty_backend_objects)

            # Separately-compiled objects sharing a generic instantiation must
            # link. This is the only test that links independently emitted
            # objects, so it is the only one that can catch a definition emitted
            # by several objects without being marked foldable. Whole-program
            # mode dedupes by name before emitting and cannot expose it.
            add_test(
                NAME insty_backend_comdat_link
                COMMAND ${CMAKE_COMMAND}
                    -DINSTY=$<TARGET_FILE:insty>
                    -DLLD=${INSTY_LLD_LINK}
                    -DOBJDIR=${CMAKE_CURRENT_BINARY_DIR}/comdat_objs
                    -DSRC_A=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/comdat_a.ins
                    -DSRC_B=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/comdat_b.ins
                    -DSRC_MAIN=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/comdat_main.ins
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_comdat_link.cmake
            )

            # COFF weak symbols: emit a weak definition + a strong override,
            # link both with lld-link, and verify the weak fallback (exit 7) and
            # the strong override (exit 99).
            add_test(
                NAME insty_backend_weak_coff
                COMMAND ${CMAKE_COMMAND}
                    -DINSTY=$<TARGET_FILE:insty>
                    -DLLD=${INSTY_LLD_LINK}
                    -DOBJDIR=${CMAKE_CURRENT_BINARY_DIR}
                    -DWEAK_SRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/weak_def.ins
                    -DSTRONG_SRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/weak_strong.ins
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_weak_coff.cmake
            )
        endif()

        # End-to-end (ELF, non-Windows): link the emitted .o with the system C
        # compiler driver and run it; it must exit 42. We link with -nostartfiles
        # and -e main since there is no _start/CRT in this minimal object.
        find_program(INSTY_CC_DRIVER NAMES cc clang gcc)
        if(INSTY_CC_DRIVER AND NOT WIN32)
            add_test(
                NAME insty_backend_link_run_elf
                COMMAND ${CMAKE_COMMAND}
                    -DCC=${INSTY_CC_DRIVER}
                    -DOBJ=${INSTY_BACKEND_BASE}.o
                    -DEXE=${INSTY_BACKEND_BASE}.elf.out
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_link_run_elf.cmake
            )
            set_tests_properties(insty_backend_link_run_elf PROPERTIES
                DEPENDS insty_backend_objects)
        endif()
    endif()

    # End-to-end: run insty against fixture .ins files.
    add_test(
        NAME insty_e2e_invalid_type
        COMMAND insty --check --no-std
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bad_type.ins
    )
    set_tests_properties(insty_e2e_invalid_type PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME insty_e2e_duplicate_cross_module_global
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dup_global_main.ins
            -DEXE=${CMAKE_CURRENT_BINARY_DIR}/dup_global_test.exe
            -DNAME=duplicate_cross_module_global
            -DMATCH=duplicate global 'shared' across modules
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_expect_fail.cmake
    )

    # Classes + operator overloading build to an executable that returns 33.
    add_test(
        NAME insty_e2e_classes
        COMMAND insty --no-std --target x86_64_windows -c
                --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/classes.ins
    )

    # RAII destructors: block-scoped class locals run their destructor once each
    # at scope exit (regression: a parser desync once made `destructor()` and any
    # RAII class uncompilable). Returns 12 (7 + 5).
    if(WIN32)
        add_test(
            NAME insty_e2e_raii_destructor
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/raii_destructor.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/raii_destructor_test.exe
                -DEXPECT=12
                -DNAME=raii_destructor
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_raii_destructor
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/raii_destructor.ins
        )
    endif()

    # Whole-struct/class copy-initialization (`T x = <init>`): copies from an
    # lvalue/call/literal produce independent values, and copy-init of a class
    # from a constructor call is construct-in-place so its destructor runs once
    # (regression: the constructor temporary used to be destroyed twice).
    # Returns 41.
    if(WIN32)
        add_test(
            NAME insty_e2e_copy_init
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/copy_init.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/copy_init_test.exe
                -DEXPECT=41
                -DNAME=copy_init
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_copy_init
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/copy_init.ins
        )
    endif()

    # Return-by-value of a class with a destructor: RVO for prvalue returns, NRVO
    # for named-local returns, and per-path move-out on conditional returns, so
    # the object is destroyed exactly once. Such classes are returned in memory
    # regardless of size (ABI). Returns 36.
    if(WIN32)
        add_test(
            NAME insty_e2e_return_value
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/return_value.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/return_value_test.exe
                -DEXPECT=36
                -DNAME=return_value
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_return_value
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/return_value.ins
        )
    endif()

    # By-value aggregate arguments are true copies: a memory-ABI aggregate (large
    # struct or class-with-destructor) is copied into a caller temporary so the
    # callee cannot mutate the caller's object, and a class argument is destroyed
    # exactly once at statement end (prvalue-temporary args are not re-copied).
    # Returns 68.
    if(WIN32)
        add_test(
            NAME insty_e2e_arg_byvalue
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/arg_byvalue.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/arg_byvalue_test.exe
                -DEXPECT=68
                -DNAME=arg_byvalue
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_arg_byvalue
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/arg_byvalue.ins
        )
    endif()

    # A by-value call result that is a class with a destructor is destroyed
    # exactly once in every disposition: bound to a local / returned / assigned
    # (constructed directly into the destination via elision), or discarded / used
    # as a subexpression / passed as an argument (result temporary registered and
    # destroyed at statement end). A live-magic guard would flag any double free.
    # Returns 26.
    if(WIN32)
        add_test(
            NAME insty_e2e_call_result
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/call_result.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/call_result_test.exe
                -DEXPECT=26
                -DNAME=call_result
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_call_result
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/call_result.ins
        )
    endif()

    # Generic monomorphization. On Windows, build PE executables and run them
    # (both fixtures return 33); elsewhere, keep object-emission coverage.
    if(WIN32)
        add_test(
            NAME insty_e2e_generics
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generics.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/generics_test.exe
                -DEXPECT=33
                -DNAME=generics
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
        add_test(
            NAME insty_e2e_generic_classes
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generic_classes.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/generic_classes_test.exe
                -DEXPECT=33
                -DNAME=generic_classes
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
        add_test(
            NAME insty_e2e_generic_harden
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generic_harden.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/generic_harden_test.exe
                -DEXPECT=137
                -DNAME=generic_harden
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
        # One template, two type arguments of different sizes, with `T`-typed
        # locals and `new T[n]` inside the generic code. Those types come from
        # SemaResult::exprTypes, which is keyed by AST node: sharing one body
        # between instantiations let the last one's types win, so every earlier
        # instantiation allocated and strided at the wrong element size. Returns
        # 77 only if every element of every instantiation survives intact.
        add_test(
            NAME insty_e2e_generic_multi_instance
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generic_multi_instance.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/generic_multi_instance_test.exe
                -DEXPECT=77
                -DNAME=generic_multi_instance
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
        # `.insize` over primitives, aggregates, sum types, slices, arrays, values
        # and generic parameters. Every size is also checked against a measured
        # field spacing / array stride, so the test fails if `.insize` ever stops
        # matching the layout actually emitted. Returns 88 when all agree.
        add_test(
            NAME insty_e2e_insize
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/insize.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/insize_test.exe
                -DEXPECT=88
                -DNAME=insize
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
        # A float occupies its precision (f16 2, f32 4, f64 8, f128 16) rather than
        # a whole register. Checks the sizes, that they match measured field and
        # array strides, and that float aggregates survive by-value passing and
        # returning -- including two f32s packed into one SSE eightbyte and an
        # int+float pair that is classed INTEGER instead. Returns 55.
        add_test(
            NAME insty_e2e_float_layout
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/float_layout.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/float_layout_test.exe
                -DEXPECT=55
                -DNAME=float_layout
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
        # A C-style enum is laid out as its declared underlying type. Checks the
        # sizes against measured field/array strides, and -- because narrowing an
        # enum narrows its loads and stores -- that values survive, including
        # unsigned ones (255 in a u8 enum must not read back as -1). Returns 66.
        # `--check` and `--emit-ast` must resolve the import graph. Both used to
        # analyse each input in isolation with an empty module context, so every
        # call into an imported module was reported as an unknown function -- which
        # made them useless for any program that touches the standard library, and
        # broke the editor/playground tooling built on them. wsl_stdlib.ins imports
        # six std modules, so a regression here fails loudly. ctest treats a
        # non-zero exit as failure, which is exactly the assertion wanted.
        add_test(
            NAME insty_check_resolves_imports
            COMMAND insty --check
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wsl_stdlib.ins
        )
        add_test(
            NAME insty_emit_ast_resolves_imports
            COMMAND insty --emit-ast
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wsl_stdlib.ins
        )
        # ...and must still report a real error rather than passing everything.
        add_test(
            NAME insty_check_reports_real_errors
            COMMAND insty --check
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bad_type.ins
        )
        set_tests_properties(insty_check_reports_real_errors
                             PROPERTIES WILL_FAIL TRUE)

        # Raw `asm [keep(reg)]( ... NASM ... )` blocks: real assembly assembled by
        # the backend, `$var` operands resolved to stack slots, and the function
        # keeping its normal prologue/epilogue. Returns 99.
        add_test(
            NAME insty_e2e_asm_block
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/asm_block.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/asm_block_test.exe
                -DEXPECT=99
                -DNAME=asm_block
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
        add_test(
            NAME insty_e2e_enum_layout
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/enum_layout.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/enum_layout_test.exe
                -DEXPECT=66
                -DNAME=enum_layout
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_generics
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generics.ins
        )
        add_test(
            NAME insty_e2e_generic_classes
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generic_classes.ins
        )
        add_test(
            NAME insty_e2e_generic_harden
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generic_harden.ins
        )
        add_test(
            NAME insty_e2e_generic_multi_instance
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generic_multi_instance.ins
        )
        add_test(
            NAME insty_e2e_insize
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/insize.ins
        )
        add_test(
            NAME insty_e2e_float_layout
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/float_layout.ins
        )
        add_test(
            NAME insty_e2e_enum_layout
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/enum_layout.ins
        )
    endif()

    # Tagged-union enums (sum types) + `switch`: a recursive expression AST built
    # on the heap and evaluated by pattern matching, plus a by-value sum type with
    # a unit variant and a `_` arm. Native build+run returns 42; object-emission
    # guards the codegen path on every platform.
    add_test(
        NAME insty_e2e_sum_ast_emit
        COMMAND insty --no-std --target x86_64_windows -c --allocator runtime
                --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sum_ast.ins
    )
    # Self-hosted proof-of-concept: the compiler-v2 arithmetic compiler/interpreter
    # written in Insty (lexer -> Pratt parser -> evaluator across 5 modules; sum
    # types, generics, slices, classes). Build+run returns 42.
    #
    # This is the broadest end-to-end exercise of the language available, and the
    # only test that instantiates several generics (Vector<Tok>, Vector<Stmt>,
    # StringMap<i64>) with differently-sized type arguments in one program -- the
    # shape that used to be miscompiled (see generic_multi_instance).
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../compiler-v2/src/main.ins)
        if(WIN32)
            add_test(
                NAME insty_e2e_compiler_v2
                COMMAND ${CMAKE_COMMAND}
                    -DINSTY=$<TARGET_FILE:insty>
                    -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/../compiler-v2/src/main.ins
                    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/compiler_v2_calc.exe
                    -DEXPECT=42
                    -DUSE_STD=ON
                    -DNAME=compiler_v2
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
            )
        else()
            add_test(
                NAME insty_e2e_compiler_v2
                COMMAND ${CMAKE_COMMAND}
                    -DINSTY=$<TARGET_FILE:insty>
                    -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/../compiler-v2/src/main.ins
                    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/compiler_v2_calc
                    -DEXPECT=42
                    "-DFLAGS=--allocator runtime"
                    -DNAME=compiler_v2
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_run.cmake
            )
        endif()
    endif()

    # Regression: short-circuit `&&`/`||` as an `else if` condition. Guards the
    # selLogical fall-through fix (rhs block is not adjacent inside an else arm).
    # Native build+run returns 42 only when every arm evaluates correctly.
    if(NOT WIN32)
        add_test(
            NAME insty_e2e_short_circuit_elseif
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/short_circuit_elseif.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/short_circuit_elseif
                -DEXPECT=42
                -DNAME=short_circuit_elseif
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_run.cmake
        )
    endif()

    # Compile-time string-literal hashing (@hash) must agree bit-for-bit with the
    # runtime hash of the same bytes. Native build+run returns 42.
    if(NOT WIN32)
        add_test(
            NAME insty_e2e_hash_consistency
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hash_consistency.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/hash_consistency
                -DEXPECT=42
                -DNAME=hash_consistency
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_run.cmake
        )
    endif()

    # Content-based `text ==` / `!=` (a heap-built string equals an equal literal;
    # null-safe). Native build+run returns 42.
    if(NOT WIN32)
        add_test(
            NAME insty_e2e_text_equality
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/text_equality.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/text_equality
                -DEXPECT=42
                "-DFLAGS=--allocator runtime"
                -DNAME=text_equality
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_run.cmake
        )
    endif()

    # Cross-module sum types: a tagged union defined in one module, constructed +
    # matched in another. Returns 42.
    add_test(
        NAME insty_e2e_sum_xmod_emit
        COMMAND insty --target x86_64_windows -c --allocator runtime
                --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sum_xmod_main.ins
    )
    if(NOT WIN32)
        add_test(
            NAME insty_e2e_sum_xmod_run
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sum_xmod_main.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/sum_xmod_run
                -DEXPECT=42
                "-DFLAGS=--allocator runtime"
                -DNAME=sum_xmod_run
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_run.cmake
        )
        add_test(
            NAME insty_e2e_sum_ast_run
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sum_ast.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/sum_ast_run
                -DEXPECT=42
                "-DFLAGS=--allocator runtime"
                -DNAME=sum_ast_run
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_run.cmake
        )
    endif()

    # Standard-library containers / string / file I/O built on generics + slices.
    # Cross-module generics: std::vec.Vector<T> is defined in one module and
    # instantiated in another. Native build+run verifies real behavior (returns
    # 42); object-emission guards the codegen path on every platform.
    add_test(
        NAME insty_e2e_std_vec_emit
        COMMAND insty --target x86_64_windows -c --allocator runtime
                --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_vec.ins
    )
    add_test(
        NAME insty_e2e_std_string_emit
        COMMAND insty --target x86_64_windows -c --allocator runtime
                --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_string.ins
    )
    add_test(
        NAME insty_e2e_std_map_emit
        COMMAND insty --target x86_64_windows -c --allocator runtime
                --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_map.ins
    )
    add_test(
        NAME insty_e2e_std_str_rich_emit
        COMMAND insty --target x86_64_windows -c --allocator runtime
                --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_str_rich.ins
    )
    if(NOT WIN32)
        add_test(
            NAME insty_e2e_std_map_run
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_map.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/std_map_run
                -DEXPECT=42
                "-DFLAGS=--allocator runtime"
                -DNAME=std_map_run
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_run.cmake
        )
        add_test(
            NAME insty_e2e_std_str_rich_run
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_str_rich.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/std_str_rich_run
                -DEXPECT=42
                "-DFLAGS=--allocator runtime"
                -DNAME=std_str_rich_run
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_run.cmake
        )
        add_test(
            NAME insty_e2e_std_vec_run
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_vec.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/std_vec_run
                -DEXPECT=42
                "-DFLAGS=--allocator runtime"
                -DNAME=std_vec_run
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_run.cmake
        )
        add_test(
            NAME insty_e2e_std_string_run
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_string.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/std_string_run
                -DEXPECT=42
                "-DFLAGS=--allocator runtime"
                -DNAME=std_string_run
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_run.cmake
        )
        # File I/O is hosted-Linux only (raw syscalls); run it in the build dir so
        # the round-trip temp file lands somewhere writable.
        add_test(
            NAME insty_e2e_std_fs_run
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_fs.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/std_fs_run
                -DEXPECT=42
                "-DFLAGS=--allocator runtime"
                -DNAME=std_fs_run
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_run.cmake
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )
    endif()

    # Bounds checking (--bounds-check): in-range indexing/sub-slicing is unaffected
    # (object-emission coverage everywhere); out-of-range indexing traps at runtime
    # and a constant out-of-range fixed-array index is a compile-time error.
    add_test(
        NAME insty_e2e_bounds_check_emit
        COMMAND insty --no-std --target x86_64_windows -c --bounds-check
                --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bounds_check.ins
    )
    add_test(
        NAME insty_e2e_bounds_oob_const
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bounds_oob_const.ins
            -DEXE=${CMAKE_CURRENT_BINARY_DIR}/bounds_oob_const.exe
            -DFLAGS=--bounds-check
            -DNAME=bounds_oob_const
            -DMATCH=out of bounds for array of length 3
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_expect_fail.cmake
    )
    if(NOT WIN32)
        # Native build+run: in-range program returns 42; out-of-range traps.
        add_test(
            NAME insty_e2e_bounds_ok_run
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bounds_check.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/bounds_ok_run
                -DEXPECT=42
                -DNAME=bounds_ok_run
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bounds_run.cmake
        )
        add_test(
            NAME insty_e2e_bounds_trap_run
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bounds_trap.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/bounds_trap_run
                -DEXPECT=CRASH
                -DNAME=bounds_trap_run
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bounds_run.cmake
        )
    endif()

    # for-in loops: integer ranges + iteration over arrays/slices/sub-slices/text,
    # break/skip, nesting, and aggregate-element value copies. Returns 42.
    if(WIN32)
        add_test(
            NAME insty_e2e_for_in
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/for_in.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/for_in_test.exe
                -DEXPECT=42
                -DNAME=for_in
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_for_in
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/for_in.ins
        )
    endif()

    # Slices (`T[]` = { ptr, len }): fixed-array/array-literal slices, `.len`,
    # indexing, pass-by-value, and slice copy (no allocator). Returns 42.
    if(WIN32)
        add_test(
            NAME insty_e2e_slices
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/slices.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/slices_test.exe
                -DEXPECT=42
                -DNAME=slices
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_slices
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/slices.ins
        )
    endif()

    # Sub-slicing `s[a..b]` (and open-bound forms) over slices and fixed arrays.
    # Returns 42.
    if(WIN32)
        add_test(
            NAME insty_e2e_slice_range
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/slice_range.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/slice_range_test.exe
                -DEXPECT=42
                -DNAME=slice_range
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_slice_range
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/slice_range.ins
        )
    endif()

    # Heap-backed slices: `new i32[n]` carries its allocation count as `.len`.
    # Returns 42. (Runs on Windows; object-emission coverage elsewhere.)
    if(WIN32)
        add_test(
            NAME insty_e2e_slices_new
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/slices_new.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/slices_new_test.exe
                -DEXPECT=42
                -DNAME=slices_new
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_slices_new
            COMMAND insty --no-std --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/slices_new.ins
        )
    endif()

    # Allocator/free coverage: raw malloc/free, old-size-aware realloc grow and
    # shrink, realloc(null), and repeated new/delete destructor cleanup. On
    # Windows, build PE executables and run them; elsewhere, keep object
    # emission coverage for the Windows target.
    function(insty_allocator_fixture TEST_NAME FIXTURE EXPECTED_EXIT)
        if(WIN32)
            add_test(
                NAME ${TEST_NAME}
                COMMAND ${CMAKE_COMMAND}
                    -DINSTY=$<TARGET_FILE:insty>
                    -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${FIXTURE}
                    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/${TEST_NAME}.exe
                    -DEXPECT=${EXPECTED_EXIT}
                    -DNAME=${TEST_NAME}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
            )
        else()
            add_test(
                NAME ${TEST_NAME}
                COMMAND insty --no-std --target x86_64_windows -c
                        --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${FIXTURE}
            )
        endif()
    endfunction()

    insty_allocator_fixture(insty_e2e_allocator_malloc_free
                            allocator_malloc_free.ins 42)
    insty_allocator_fixture(insty_e2e_allocator_realloc_grow
                            allocator_realloc_grow.ins 49)
    insty_allocator_fixture(insty_e2e_allocator_realloc_shrink
                            allocator_realloc_shrink.ins 42)
    insty_allocator_fixture(insty_e2e_allocator_realloc_null
                            allocator_realloc_null.ins 42)
    insty_allocator_fixture(insty_e2e_allocator_new_delete_loop
                            allocator_new_delete_loop.ins 62)
    insty_allocator_fixture(insty_e2e_allocator_alignment
                            allocator_alignment.ins 42)
    insty_allocator_fixture(insty_e2e_allocator_overflow
                            allocator_overflow.ins 42)

    if(WIN32)
        add_test(
            NAME insty_e2e_std_alloc_block
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_alloc_block.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/std_alloc_block_test.exe
                -DEXPECT=42
                -DNAME=std_alloc_block
                -DUSE_STD=ON
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_std_alloc_block
            COMMAND insty --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_alloc_block.ins
        )
    endif()

    if(WIN32)
        add_test(
            NAME insty_e2e_imported_class_enum
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/import_meta_main.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/import_meta_test.exe
                -DEXPECT=42
                -DNAME=imported_class_enum
                -DUSE_STD=ON
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_build_run.cmake
        )
    else()
        add_test(
            NAME insty_e2e_imported_class_enum
            COMMAND insty --target x86_64_windows -c
                    --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/import_meta_main.ins
        )
    endif()

    # String interpolation: int/bool/char/text/float/struct/class formatting,
    # built into a stack buffer (no allocator, freestanding-safe).
    add_test(
        NAME insty_e2e_interpolation
        COMMAND insty --no-std --target x86_64_windows -c
                --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/interpolation.ins
    )

    # Indexing a `text` yields its u8 bytes (regression: void-element load).
    add_test(
        NAME insty_e2e_text_index
        COMMAND insty --no-std --target x86_64_windows -c
                --objects-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e_objects
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/text_index.ins
    )

    # Floating-point remainder (`%`): synthesized via a - trunc(a/b)*b on the
    # custom backend (no SSE remainder op). Builds to an executable (exit 7).
    add_test(
        NAME insty_e2e_fmod
        COMMAND insty --no-std --target x86_64_windows
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fmod.ins
                -o ${CMAKE_CURRENT_BINARY_DIR}/fmod_test
    )

    # Struct/class string interpolation on the custom backend: auto-derived
    # `Name { field: value, ... }` formatting, class `toString` dispatch, and
    # nested-aggregate recursion. Builds to an executable (exit 6).
    add_test(
        NAME insty_e2e_struct_interp
        COMMAND insty --no-std --target x86_64_windows
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/struct_interp.ins
                -o ${CMAKE_CURRENT_BINARY_DIR}/struct_interp_test
    )

    # f16 (half) support on the custom backend: packed 16-bit storage, compute as
    # f32 via F16C (vcvtph2ps/vcvtps2ph), the call ABI (packed-half params +
    # return), and casts to/from integer and f32/f64. On Windows we build a PE
    # exe and run it (must exit 45); elsewhere we just build it.
    if(WIN32)
        add_test(
            NAME insty_e2e_half
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/half.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/half_test.exe
                -DEXPECT=45
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_half_validate.cmake
        )
    else()
        add_test(
            NAME insty_e2e_half
            COMMAND insty --no-std --target x86_64_windows
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/half.ins
                    -o ${CMAKE_CURRENT_BINARY_DIR}/half_test
        )
    endif()

    # --- WebAssembly -------------------------------------------------------
    # End-to-end WebAssembly codegen: compile an Insty fixture to a .wasm
    # module, validate it, and check results. `main` runs as a WASI command so
    # its return value is the process exit status; the other functions are
    # exported and invoked directly to check each arithmetic form against a
    # known value. wasm-tools/wasmtime are optional (see tests/wasm_run.cmake).
    set(INSTY_WASM_FIXTURE ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm_straight.ins)
    set(INSTY_WASM_OUT ${CMAKE_CURRENT_BINARY_DIR}/wasm_straight.wasm)

    add_test(
        NAME insty_wasm_e2e_main
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${INSTY_WASM_FIXTURE}
            -DWASM=${INSTY_WASM_OUT}
            -DEXPECT=42
            -DNAME=insty_wasm_e2e_main
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    # Calls one exported function and compares its result. `fn_args` is a single
    # space-separated string, so it must stay quoted all the way through to
    # wasm_run.cmake's separate_arguments().
    function(insty_add_wasm_invoke_test case_name fn fn_args expected)
        add_test(
            NAME insty_wasm_e2e_${case_name}
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${INSTY_WASM_FIXTURE}
                -DWASM=${INSTY_WASM_OUT}
                -DINVOKE=${fn}
                "-DARGS=${fn_args}"
                "-DEXPECT_OUT=${expected}"
                -DNAME=insty_wasm_e2e_${case_name}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
        )
    endfunction()

    insty_add_wasm_invoke_test(addup_pos addup "20 22"  42)
    insty_add_wasm_invoke_test(addup_neg addup "-5 12"  7)
    insty_add_wasm_invoke_test(mix_pos   mix   "100 7"  916)
    insty_add_wasm_invoke_test(mix_neg   mix   "-100 7" -916)
    insty_add_wasm_invoke_test(bits      bits  "171"    1184)
    insty_add_wasm_invoke_test(unary     unary "5"      -11)

    # --- control flow ------------------------------------------------------
    # The Machine IR is an unstructured CFG and wasm only has structured scopes;
    # these cover the shapes the bridge has to handle: forward branches, back
    # edges, nesting, early returns and short-circuit joins.
    set(INSTY_WASM_FIXTURE ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm_control_flow.ins)
    set(INSTY_WASM_OUT ${CMAKE_CURRENT_BINARY_DIR}/wasm_control_flow.wasm)

    add_test(
        NAME insty_wasm_cf_main
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${INSTY_WASM_FIXTURE}
            -DWASM=${INSTY_WASM_OUT}
            -DEXPECT=42
            -DNAME=insty_wasm_cf_main
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    insty_add_wasm_invoke_test(cf_if_hi     classify  "200"   3)
    insty_add_wasm_invoke_test(cf_if_mid    classify  "50"    2)
    insty_add_wasm_invoke_test(cf_if_lo     classify  "5"     1)
    insty_add_wasm_invoke_test(cf_while     sumto     "10"    55)
    insty_add_wasm_invoke_test(cf_forrange  forsum    "5"     10)
    insty_add_wasm_invoke_test(cf_breakskip brk       "20"    18)
    insty_add_wasm_invoke_test(cf_nested    nested    "4"     16)
    insty_add_wasm_invoke_test(cf_and       logic     "1 1"   1)
    insty_add_wasm_invoke_test(cf_or        logic     "200 1" 3)
    insty_add_wasm_invoke_test(cf_neither   logic     "-1 5"  0)
    insty_add_wasm_invoke_test(cf_countdown countdown "9"     9)

    # --- linear memory -----------------------------------------------------
    # Values whose address is never taken stay in wasm locals; everything else
    # has to live in linear memory behind a shadow-stack pointer. These cover
    # both sides of that split plus the static data region.
    set(INSTY_WASM_FIXTURE ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm_memory.ins)
    set(INSTY_WASM_OUT ${CMAKE_CURRENT_BINARY_DIR}/wasm_memory.wasm)

    add_test(
        NAME insty_wasm_mem_main
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${INSTY_WASM_FIXTURE}
            -DWASM=${INSTY_WASM_OUT}
            -DEXPECT=42
            -DNAME=insty_wasm_mem_main
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    insty_add_wasm_invoke_test(mem_addrof   viaptr    "5"     6)
    insty_add_wasm_invoke_test(mem_struct   structsum "20 22" 42)
    insty_add_wasm_invoke_test(mem_array    arrsum    "4"     10)
    insty_add_wasm_invoke_test(mem_slice    slicelen  ""      93)
    insty_add_wasm_invoke_test(mem_global   getmagic  ""      4096)
    insty_add_wasm_invoke_test(mem_bss      getzero   ""      0)
    insty_add_wasm_invoke_test(mem_globalrw setget    "8"     4104)
    insty_add_wasm_invoke_test(mem_strdata  strbytes  ""      244)

    foreach(opt_level 0 1 2 3)
        add_test(
            NAME insty_wasm_mem_O${opt_level}
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${INSTY_WASM_FIXTURE}
                -DWASM=${CMAKE_CURRENT_BINARY_DIR}/wasm_mem_O${opt_level}.wasm
                -DOPT=-O${opt_level}
                -DEXPECT=42
                -DNAME=insty_wasm_mem_O${opt_level}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
        )
    endforeach()

    # --- calls -------------------------------------------------------------
    # The selector distributes arguments into ABI argument registers before a
    # call; the emitter replays that assignment to read them back out of the
    # locals standing in for those registers. Covers arity, recursion, mutual
    # recursion, monomorphized generics, class methods and discarded results.
    set(INSTY_WASM_FIXTURE ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm_calls.ins)
    set(INSTY_WASM_OUT ${CMAKE_CURRENT_BINARY_DIR}/wasm_calls.wasm)

    add_test(
        NAME insty_wasm_call_main
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${INSTY_WASM_FIXTURE}
            -DWASM=${INSTY_WASM_OUT}
            -DEXPECT=42
            -DNAME=insty_wasm_call_main
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    insty_add_wasm_invoke_test(call_sixargs  six      "1 2 3 4 5 6" 21)
    insty_add_wasm_invoke_test(call_recurse  fact     "5"           120)
    insty_add_wasm_invoke_test(call_mutual   parity   "7"           1)
    insty_add_wasm_invoke_test(call_mutual2  parity   "8"           0)
    insty_add_wasm_invoke_test(call_generic  generics "3 10"        26)
    insty_add_wasm_invoke_test(call_discard  discard  "4"           8)
    insty_add_wasm_invoke_test(call_method   counters ""            33)

    foreach(opt_level 0 1 2 3)
        add_test(
            NAME insty_wasm_call_O${opt_level}
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${INSTY_WASM_FIXTURE}
                -DWASM=${CMAKE_CURRENT_BINARY_DIR}/wasm_call_O${opt_level}.wasm
                -DOPT=-O${opt_level}
                -DEXPECT=42
                -DNAME=insty_wasm_call_O${opt_level}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
        )
    endforeach()

    set(INSTY_WASM_FIXTURE ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm_control_flow.ins)
    set(INSTY_WASM_OUT ${CMAKE_CURRENT_BINARY_DIR}/wasm_control_flow.wasm)

    # From -O1 the branch simplifier deletes terminators whose target is already
    # the next block, so blocks start relying on fall-through, and it leaves the
    # flag-producing Cmp of a deleted Jcc behind with no consumer. Both are
    # control-flow edge cases the wasm path has to absorb, so every level is
    # built and run rather than just the default.
    foreach(opt_level 0 1 2 3)
        add_test(
            NAME insty_wasm_cf_O${opt_level}
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${INSTY_WASM_FIXTURE}
                -DWASM=${CMAKE_CURRENT_BINARY_DIR}/wasm_cf_O${opt_level}.wasm
                -DOPT=-O${opt_level}
                -DEXPECT=42
                -DNAME=insty_wasm_cf_O${opt_level}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
        )
    endforeach()

    # wasm32_freestanding has no WASI: no proc_exit import and no `_start`, just
    # the exported functions. It must still produce a valid module. Names the
    # straight-line fixture explicitly, since INSTY_WASM_FIXTURE has since been
    # repointed at the control-flow one.
    add_test(
        NAME insty_wasm_freestanding_module
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm_straight.ins
            -DWASM=${CMAKE_CURRENT_BINARY_DIR}/wasm_freestanding.wasm
            -DINSTY_TARGET=wasm32_freestanding
            -DINVOKE=addup
            "-DARGS=20 22"
            -DEXPECT_OUT=42
            -DNAME=insty_wasm_freestanding_module
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    # --- floating point ----------------------------------------------------
    # XMM-class locals hold raw bits and each instruction reinterprets them at
    # its own width, so f32 rounds after every operation exactly as the hardware
    # does. `f32accum` is built so that computing in double precision and
    # rounding once would give a different answer.
    set(INSTY_WASM_FIXTURE ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm_float.ins)
    set(INSTY_WASM_OUT ${CMAKE_CURRENT_BINARY_DIR}/wasm_float.wasm)

    add_test(
        NAME insty_wasm_float_main
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${INSTY_WASM_FIXTURE}
            -DWASM=${INSTY_WASM_OUT}
            -DEXPECT=42
            -DNAME=insty_wasm_float_main
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    insty_add_wasm_invoke_test(f64_arith   arith64   "10 4"        62)
    insty_add_wasm_invoke_test(f32_arith   arith32   "7 2"         26)
    insty_add_wasm_invoke_test(f32_round   f32accum  "10"          1000000)
    # 10000002, not 10000000: per-operation f32 rounding is observable.
    insty_add_wasm_invoke_test(f32_precise f32accum  "100"         10000002)
    insty_add_wasm_invoke_test(f_cmp_lt    compare   "1 2"         3)
    insty_add_wasm_invoke_test(f_cmp_eq    compare   "2 2"         26)
    insty_add_wasm_invoke_test(f_cmp_gt    compare   "3 2"         12)
    insty_add_wasm_invoke_test(f_roundtrip roundtrip "1234567"     1234567)
    insty_add_wasm_invoke_test(f_negtrunc  negtrunc  "7.9"         -7)
    # Interleaved integer and SSE parameters: two register files, two cursors.
    insty_add_wasm_invoke_test(f_mixedargs mixed     "1 2 3 4 5 6" 21)

    foreach(opt_level 0 1 2 3)
        add_test(
            NAME insty_wasm_float_O${opt_level}
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${INSTY_WASM_FIXTURE}
                -DWASM=${CMAKE_CURRENT_BINARY_DIR}/wasm_float_O${opt_level}.wasm
                -DOPT=-O${opt_level}
                -DEXPECT=42
                -DNAME=insty_wasm_float_O${opt_level}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
        )
    endforeach()

    # --- WASI console I/O --------------------------------------------------
    # The only test that checks what a program *wrote* rather than what it
    # returned, so it covers the whole path: a literal in the data segment, a
    # Ciovec built on the shadow stack, and a wasm import call into the host.
    # `|` stands for a newline in the expected text.
    add_test(
        NAME insty_wasm_wasi_io
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm_wasi_io.ins
            -DWASM=${CMAKE_CURRENT_BINARY_DIR}/wasm_wasi_io.wasm
            -DUSE_STD=ON
            -DEXPECT=0
            "-DEXPECT_STDOUT=hello from wasm|the answer is 42|-1234|0|9876543210|abc|"
            "-DEXPECT_STDERR=to stderr|"
            -DNAME=insty_wasm_wasi_io
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    # --- WASI file access --------------------------------------------------
    # path_open takes nine arguments, three of them past the System V argument
    # registers, so this only works once outgoing stack arguments do. Needs a
    # host-granted directory, which the runner supplies with --dir.
    add_test(
        NAME insty_wasm_wasi_fs
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm_wasi_fs.ins
            -DWASM=${CMAKE_CURRENT_BINARY_DIR}/wasm_wasi_fs.wasm
            -DUSE_STD=ON
            -DEXPECT=0
            -DGRANT_DIR=${CMAKE_CURRENT_BINARY_DIR}/wasm_fs_scratch
            "-DEXPECT_STDOUT=hello from wasi fs|second line|bytes read: 31|"
            -DNAME=insty_wasm_wasi_fs
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    # Outgoing stack arguments: signatures wider than the argument-register file,
    # for both the integer and the SSE side.
    set(INSTY_WASM_FIXTURE ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm_stack_args.ins)
    set(INSTY_WASM_OUT ${CMAKE_CURRENT_BINARY_DIR}/wasm_stack_args.wasm)
    add_test(
        NAME insty_wasm_stackargs_main
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${INSTY_WASM_FIXTURE}
            -DWASM=${INSTY_WASM_OUT}
            -DEXPECT=42
            -DNAME=insty_wasm_stackargs_main
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )
    insty_add_wasm_invoke_test(stack_nine_ones nine "1 1 1 1 1 1 1 1 1" 45)
    insty_add_wasm_invoke_test(stack_nine_seq  nine "1 2 3 4 5 6 7 8 9" 285)
    insty_add_wasm_invoke_test(stack_mixed     manymixed "1 10 2 20 3 30 4 40 5 50" 165)

    # --- compile-time target dispatch --------------------------------------
    # `#if @targetIs(...)` picks a branch before semantic analysis, so the branch
    # not taken is never type-checked or lowered. The fixture relies on that: it
    # contains both a raw @syscall (impossible on wasm) and a WASI import
    # (meaningless on x86-64), and returns 42 either way.
    add_test(
        NAME insty_wasm_comptime_dispatch
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/comptime_dispatch.ins
            -DWASM=${CMAKE_CURRENT_BINARY_DIR}/comptime_dispatch.wasm
            -DEXPECT=42
            -DNAME=insty_wasm_comptime_dispatch
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    # `#if` selects between whole top-level declarations, imports included, so a
    # dependency that only exists or only compiles on one target is never resolved
    # on the others. Each target here imports a different module to reach 42.
    add_test(
        NAME insty_wasm_comptime_imports
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/comptime_imports.ins
            -DWASM=${CMAKE_CURRENT_BINARY_DIR}/comptime_imports.wasm
            -DUSE_STD=ON
            -DEXPECT=42
            -DNAME=insty_wasm_comptime_imports
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    # The self-hosted compiler-v2 through the wasm backend: five modules, sum
    # types, generics instantiated at several element sizes, slices, classes,
    # StringMap and Vector on the heap, all reaching the host only through WASI.
    # The largest real program the wasm backend is known to run correctly.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../compiler-v2/src/main.ins)
        add_test(
            NAME insty_wasm_compiler_v2
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/../compiler-v2/src/main.ins
                -DWASM=${CMAKE_CURRENT_BINARY_DIR}/compiler_v2_calc.wasm
                -DUSE_STD=ON
                -DEXPECT=42
                -DNAME=insty_wasm_compiler_v2
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
        )
    endif()

    # A condition that cannot be answered at compile time must be an error. The
    # dangerous failure mode is treating it as false and silently dropping code.
    add_test(
        NAME insty_comptime_bad_condition_is_diagnosed
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DINSTY_TARGET=wasm32_wasi
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/comptime_bad_condition.ins
            -DEXE=${CMAKE_CURRENT_BINARY_DIR}/comptime_bad.wasm
            "-DMATCH=cannot be evaluated at compile time"
            -DNAME=insty_comptime_bad_condition_is_diagnosed
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_expect_fail.cmake
    )

    # std::fs is portable now, so the same round-trip runs on wasm. It writes 42
    # bytes and reads them back through the String builder, which also exercises
    # the heap. WASI grants filesystem access per directory, hence --dir.
    add_test(
        NAME insty_wasm_std_fs
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/std_fs.ins
            -DWASM=${CMAKE_CURRENT_BINARY_DIR}/wasm_std_fs.wasm
            -DUSE_STD=ON
            -DEXPECT=42
            -DGRANT_DIR=${CMAKE_CURRENT_BINARY_DIR}/wasm_stdfs_scratch
            -DNAME=insty_wasm_std_fs
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    # Separate compilation makes no sense for wasm: the module is self-contained
    # and there is no wasm linker, so `-c` must say so rather than emit a file
    # that can never be combined with anything.
    add_test(
        NAME insty_wasm_object_mode_is_diagnosed
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DINSTY_TARGET=wasm32_wasi
            -DFLAGS=-c
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/return_value.ins
            -DEXE=${CMAKE_CURRENT_BINARY_DIR}/wasm_objmode.wasm
            "-DMATCH=is not supported for"
            -DNAME=insty_wasm_object_mode_is_diagnosed
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_expect_fail.cmake
    )

    # --- dead code elimination ---------------------------------------------
    # Imports the whole WASI surface (console plus files, around thirty
    # functions) and calls one of them. With elimination the module is ~800 bytes
    # and carries two imports; without it, every one of those functions and its
    # data would be baked in. The bound is deliberately loose so the library can
    # grow, but a regression would blow past it by several times.
    add_test(
        NAME insty_wasm_dead_code_eliminated
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm_dce.ins
            -DWASM=${CMAKE_CURRENT_BINARY_DIR}/wasm_dce.wasm
            -DUSE_STD=ON
            -DEXPECT=0
            -DMAX_BYTES=1600
            "-DEXPECT_STDOUT=one call|"
            -DNAME=insty_wasm_dead_code_eliminated
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )

    # --- differential: wasm vs x86-64 --------------------------------------
    # The strongest available check: compile the same fixture with both backends
    # and require identical exit codes. This catches a valid module that computes
    # the wrong answer, which wasm-tools cannot detect. Needs a runnable x86-64
    # reference, hence WIN32.
    if(WIN32)
        function(insty_add_wasm_diff_test fixture)
            add_test(
                NAME insty_wasm_diff_${fixture}
                COMMAND ${CMAKE_COMMAND}
                    -DINSTY=$<TARGET_FILE:insty>
                    -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${fixture}.ins
                    -DREF_EXE=${CMAKE_CURRENT_BINARY_DIR}/diff_${fixture}.exe
                    -DWASM=${CMAKE_CURRENT_BINARY_DIR}/diff_${fixture}.wasm
                    -DNAME=insty_wasm_diff_${fixture}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_differential.cmake
            )
        endfunction()

        # Aggregate-heavy fixtures: structs and slices passed and returned by
        # value, which is where the eightbyte classification has to agree with
        # the selector exactly.
        insty_add_wasm_diff_test(struct_abi)
        insty_add_wasm_diff_test(arg_byvalue)
        insty_add_wasm_diff_test(slices)
        insty_add_wasm_diff_test(slice_range)
        insty_add_wasm_diff_test(for_in)
        insty_add_wasm_diff_test(copy_init)
        insty_add_wasm_diff_test(return_value)
        # Classes, generics, destructors.
        insty_add_wasm_diff_test(classes)
        insty_add_wasm_diff_test(generic_classes)
        insty_add_wasm_diff_test(generics)
        insty_add_wasm_diff_test(raii_destructor)
        insty_add_wasm_diff_test(constructor_temporaries)
        # Control flow, calls, indexing, hashing.
        insty_add_wasm_diff_test(call_result)
        insty_add_wasm_diff_test(short_circuit_elseif)
        insty_add_wasm_diff_test(bounds_check)
        insty_add_wasm_diff_test(hash_consistency)
        # Heap: the native lowerings reach for mmap / HeapAlloc, so wasm supplies
        # its own bump allocator over linear memory. These exercise it through
        # new/delete, the allocator builtins, and the std containers built on it.
        insty_add_wasm_diff_test(allocator_malloc_free)
        insty_add_wasm_diff_test(allocator_realloc_grow)
        insty_add_wasm_diff_test(allocator_realloc_shrink)
        insty_add_wasm_diff_test(allocator_realloc_null)
        insty_add_wasm_diff_test(allocator_new_delete_loop)
        insty_add_wasm_diff_test(allocator_alignment)
        insty_add_wasm_diff_test(allocator_overflow)
        insty_add_wasm_diff_test(std_alloc_block)
        insty_add_wasm_diff_test(new_ctor)
        insty_add_wasm_diff_test(new_runtime_count)
        insty_add_wasm_diff_test(delete_destructor)
        insty_add_wasm_diff_test(delete_array_destructor)
        insty_add_wasm_diff_test(destructor_fields)
        insty_add_wasm_diff_test(std_vec)
        insty_add_wasm_diff_test(std_string)
        insty_add_wasm_diff_test(std_map)
        insty_add_wasm_diff_test(std_str_rich)
        insty_add_wasm_diff_test(struct_interp)
        # Floating point, including the software float remainder (a - trunc(a/b)*b)
        # and float string interpolation via __ins_fmt_float.
        insty_add_wasm_diff_test(fmod)
        insty_add_wasm_diff_test(interpolation)
        insty_add_wasm_diff_test(wasm_float)
        # f16 has no wasm counterpart, so the backend calls software conversion
        # helpers where x86 uses F16C (vcvtph2ps / vcvtps2ph). `half` covers the
        # storage and call ABI; `wasm_f16_rounding` checks that the software
        # round-to-nearest-even matches the hardware bit for bit, over values that
        # are not exactly representable plus subnormals, overflow and a tie.
        insty_add_wasm_diff_test(half)
        insty_add_wasm_diff_test(wasm_f16_rounding)
        # 128-bit integers: a register pair for parameters and RAX:RDX for
        # returns, with the widening multiply synthesized because wasm has none.
        insty_add_wasm_diff_test(i128)
        # Compile-time dispatch: both backends must reach 42 despite compiling
        # entirely different branches of the same source.
        insty_add_wasm_diff_test(comptime_dispatch)
        # Indirect calls: table thunks must reproduce the native ABI exactly,
        # including how a narrow return is widened.
        insty_add_wasm_diff_test(indirect_call)
        insty_add_wasm_diff_test(comptime_imports)
        insty_add_wasm_diff_test(text_equality)
        insty_add_wasm_diff_test(sum_ast)
        insty_add_wasm_diff_test(slices_new)
        insty_add_wasm_diff_test(generic_harden)
    insty_add_wasm_diff_test(generic_multi_instance)
    insty_add_wasm_diff_test(insize)
    insty_add_wasm_diff_test(float_layout)
    insty_add_wasm_diff_test(enum_layout)
    endif()

    # Constructs the backend does not handle yet must produce a clear diagnostic
    # naming the construct, never a silently wrong module. These track the
    # --- rejected constructs ------------------------------------------------
    # Things wasm cannot express must fail with an explanation of the constraint,
    # not a generic opcode name and not silence. Several of these used to be
    # accepted and quietly dropped, which is the worse failure: the user gets a
    # module that does not do what they asked without being told.
    #
    # name                            fixture                  expected text
    foreach(case
            "syscall;wasm_reject_syscall;cannot be translated to WebAssembly"
            "atomics;wasm_reject_atomics;require the WebAssembly threads proposal"
            "interrupt;wasm_reject_interrupt;there are no interrupts"
            "section;wasm_reject_section;no named code sections")
        string(REPLACE ";" "|" case_parts "${case}")
        string(REPLACE "|" ";" case_list "${case_parts}")
        list(GET case_list 0 case_name)
        list(GET case_list 1 case_fixture)
        list(GET case_list 2 case_text)
        add_test(
            NAME insty_wasm_reject_${case_name}
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DINSTY_TARGET=wasm32_wasi
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${case_fixture}.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/wasm_reject_${case_name}.wasm
                "-DMATCH=${case_text}"
                -DNAME=insty_wasm_reject_${case_name}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_expect_fail.cmake
        )
    endforeach()

    # Command-line options describing a native image cannot be honoured either.
    foreach(bad_flag multiboot2 raw-binary)
        add_test(
            NAME insty_wasm_reject_flag_${bad_flag}
            COMMAND ${CMAKE_COMMAND}
                -DINSTY=$<TARGET_FILE:insty>
                -DINSTY_TARGET=wasm32_wasi
                -DFLAGS=--${bad_flag}
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/return_value.ins
                -DEXE=${CMAKE_CURRENT_BINARY_DIR}/wasm_reject_${bad_flag}.wasm
                "-DMATCH=is not supported for"
                -DNAME=insty_wasm_reject_flag_${bad_flag}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_expect_fail.cmake
        )
    endforeach()

    # current frontier, so each one is EXPECTED to start failing when its feature
    # lands -- that failure is the signal to replace it with a real build+run test.
    #
    # Only one is left: inline assembly, which wasm can never support. Everything
    # else the language can express now compiles.
    add_test(
        NAME insty_wasm_indirect_call
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/indirect_call.ins
            -DWASM=${CMAKE_CURRENT_BINARY_DIR}/wasm_indirect_call.wasm
            -DEXPECT=42
            -DNAME=insty_wasm_indirect_call
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_run.cmake
    )
    add_test(
        NAME insty_wasm_inline_asm_is_diagnosed
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DINSTY_TARGET=wasm32_wasi
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/asm_constrained.ins
            -DEXE=${CMAKE_CURRENT_BINARY_DIR}/wasm_asm.wasm
            "-DMATCH=inline assembly is not supported on wasm"
            -DNAME=insty_wasm_inline_asm_is_diagnosed
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_expect_fail.cmake
    )

    # An unknown target must still be rejected by the CLI, and the diagnostic
    # must advertise the wasm targets.
    add_test(
        NAME insty_wasm_target_listed_in_usage
        COMMAND ${CMAKE_COMMAND}
            -DINSTY=$<TARGET_FILE:insty>
            -DINSTY_TARGET=x86_64_nonexistent
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/arithmetic.ins
            -DEXE=${CMAKE_CURRENT_BINARY_DIR}/wasm_bad_target.out
            -DMATCH=wasm32_wasi
            -DNAME=insty_wasm_target_listed_in_usage
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_expect_fail.cmake
    )


    # --- Linux (System V) coverage via WSL -----------------------------------
    #
    # Off by default: it needs a WSL distro. Turn on with -DINSTY_CTEST_WSL=ON.
    #
    # Worth having because the System V ABI is otherwise entirely untested here.
    # A Windows host exercises Win64, which never classifies a struct as SSE, so
    # multi-register float arguments are unreachable; wasm has no register ABI at
    # all. These also execute the Linux syscall paths in std::io / std::fs, which
    # no other test does.
    option(INSTY_CTEST_WSL "Run Linux-target tests through WSL" OFF)
    if(INSTY_CTEST_WSL AND WIN32)
        find_program(INSTY_WSL_EXE NAMES wsl)
        if(NOT INSTY_WSL_EXE)
            message(WARNING "INSTY_CTEST_WSL is ON but wsl was not found; skipping")
        else()
            function(insty_add_wsl_test name fixture expect)
                cmake_parse_arguments(A "USE_STD" "" "" ${ARGN})
                set(extra)
                if(A_USE_STD)
                    set(extra -DUSE_STD=ON)
                endif()
                add_test(
                    NAME insty_wsl_${name}
                    COMMAND ${CMAKE_COMMAND}
                        -DINSTY=$<TARGET_FILE:insty>
                        -DSRC=${fixture}
                        -DELF=${CMAKE_CURRENT_BINARY_DIR}/wsl_${name}.elf
                        -DEXPECT=${expect}
                        ${extra}
                        -DNAME=insty_wsl_${name}
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wsl_run.cmake
                )
            endfunction()

            # Layout and ABI. float_layout is the one that matters most: passing an
            # all-float struct in two XMM registers only happens under System V.
            insty_add_wsl_test(float_layout
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/float_layout.ins 55)
            insty_add_wsl_test(enum_layout
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/enum_layout.ins 66)
            insty_add_wsl_test(insize
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/insize.ins 88)
            insty_add_wsl_test(generic_multi_instance
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/generic_multi_instance.ins 77)
            insty_add_wsl_test(sum_ast
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sum_ast.ins 42)
            insty_add_wsl_test(asm_block
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/asm_block.ins 99)

            # The Linux syscall paths: std::io writes and std::fs opens/reads a
            # real file. Nothing else executes them.
            insty_add_wsl_test(stdlib
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wsl_stdlib.ins 77 USE_STD)

            # The self-hosted compiler end to end, on the target it was written for.
            if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../compiler-v2/src/main.ins)
                insty_add_wsl_test(compiler_v2
                    ${CMAKE_CURRENT_SOURCE_DIR}/../compiler-v2/src/main.ins 42 USE_STD)
            endif()
        endif()
    endif()
endif()





