# Compiles an Insty fixture to a WebAssembly module, then validates and runs it.
#
# The compile step and the module's structural sanity are always checked, so the
# test is meaningful with no WebAssembly toolchain installed. Validation
# (wasm-tools) and execution (wasmtime) are layered on when those tools are
# present -- see tests/wasm_validate.cmake for the same policy.
#
# Required -D vars: INSTY, SRC, WASM, NAME.
# Optional: -DEXPECT=<exit code> to require `wasmtime run` to exit with it.
# Optional: -DINVOKE=<name> -DARGS=<space-separated> -DEXPECT_OUT=<text> to call
#           a single exported function and compare its printed result.
# Optional: -DINSTY_TARGET=<target> (defaults to wasm32_wasi). Deliberately not
#           named TARGET: that is a reserved word in CMake's if().
# Optional: -DOPT=-O2 to select an optimization level. Matters for control flow:
#           the Machine-IR branch simplifier drops terminators and relies on
#           fall-through from -O1 up.
# Optional: -DUSE_STD=ON omits --no-std so std imports resolve.

set(INSTY_STD_FLAG "--no-std")
if(DEFINED USE_STD AND USE_STD)
    set(INSTY_STD_FLAG)
endif()
if(NOT DEFINED INSTY_TARGET OR INSTY_TARGET STREQUAL "")
    set(INSTY_TARGET "wasm32_wasi")
endif()
set(INSTY_OPT_FLAG)
if(DEFINED OPT AND NOT OPT STREQUAL "")
    set(INSTY_OPT_FLAG "${OPT}")
endif()

execute_process(
    COMMAND "${INSTY}" ${INSTY_STD_FLAG} "--target" "${INSTY_TARGET}"
            ${INSTY_OPT_FLAG} "-o" "${WASM}" "${SRC}"
    RESULT_VARIABLE build_rc
    OUTPUT_VARIABLE build_out
    ERROR_VARIABLE build_err
)
if(NOT build_rc EQUAL 0)
    message(FATAL_ERROR "${NAME} compile failed (${build_rc}):\n${build_out}\n${build_err}")
endif()
if(NOT EXISTS "${WASM}")
    message(FATAL_ERROR "${NAME}: compiler reported success but produced no ${WASM}")
endif()

# Always verifiable: the preamble. Catches a module truncated or written to the
# wrong path even with no toolchain present.
file(READ "${WASM}" preamble HEX LIMIT 8)
if(NOT preamble STREQUAL "0061736d01000000")
    message(FATAL_ERROR "${NAME}: bad wasm preamble: ${preamble}")
endif()
message(STATUS "${NAME}: compiled and preamble OK")

# An upper bound on module size. Useful for pinning dead-code elimination, which
# nothing else would notice breaking: the module would still be correct, just
# carrying every function of every library it imported.
if(DEFINED MAX_BYTES AND NOT MAX_BYTES STREQUAL "")
    file(SIZE "${WASM}" actual_size)
    if(actual_size GREATER ${MAX_BYTES})
        message(FATAL_ERROR
            "${NAME}: module is ${actual_size} bytes, expected at most ${MAX_BYTES}. "
            "Unused library code is most likely no longer being eliminated.")
    endif()
    message(STATUS "${NAME}: ${actual_size} bytes (limit ${MAX_BYTES}) - OK")
endif()

find_program(WASM_TOOLS NAMES wasm-tools)
if(WASM_TOOLS)
    execute_process(
        COMMAND "${WASM_TOOLS}" validate "${WASM}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    )
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${NAME}: wasm-tools rejected the module:\n${out}\n${err}")
    endif()
    message(STATUS "${NAME}: wasm-tools validate OK")
else()
    message(STATUS "${NAME}: wasm-tools not found; skipping validation")
endif()

find_program(WASMTIME NAMES wasmtime)
if(NOT WASMTIME)
    message(STATUS "${NAME}: wasmtime not found; skipping execution")
    return()
endif()

# WASI is capability based: a module cannot touch the filesystem unless the host
# grants a directory. -DGRANT_DIR=<path> passes it through and runs there, which
# is what a file-I/O fixture needs.
set(WASMTIME_DIR_ARGS)
set(WASMTIME_WORKDIR "${CMAKE_CURRENT_BINARY_DIR}")
if(DEFINED GRANT_DIR AND NOT GRANT_DIR STREQUAL "")
    file(MAKE_DIRECTORY "${GRANT_DIR}")
    set(WASMTIME_DIR_ARGS "--dir" ".")
    set(WASMTIME_WORKDIR "${GRANT_DIR}")
endif()

if(DEFINED EXPECT AND NOT EXPECT STREQUAL "")
    execute_process(
        COMMAND "${WASMTIME}" run ${WASMTIME_DIR_ARGS} "${WASM}"
        WORKING_DIRECTORY "${WASMTIME_WORKDIR}"
        RESULT_VARIABLE run_rc OUTPUT_VARIABLE run_out ERROR_VARIABLE run_err
    )
    if(NOT run_rc EQUAL ${EXPECT})
        message(FATAL_ERROR
            "${NAME}: wasmtime exited ${run_rc}, expected ${EXPECT}\n${run_out}\n${run_err}")
    endif()
    message(STATUS "${NAME}: wasmtime run exited ${run_rc} (expected ${EXPECT}) - OK")

    # Checking what the program actually wrote, rather than only what it
    # returned, is the only way to test the I/O path end to end.
    if(DEFINED EXPECT_STDOUT)
        string(REPLACE "\r\n" "\n" run_out "${run_out}")
        string(REPLACE "|" "\n" expected_out "${EXPECT_STDOUT}")
        if(NOT run_out STREQUAL "${expected_out}")
            message(FATAL_ERROR
                "${NAME}: stdout mismatch\n--- got ---\n${run_out}\n--- want ---\n${expected_out}")
        endif()
        message(STATUS "${NAME}: stdout matched - OK")
    endif()
    if(DEFINED EXPECT_STDERR)
        string(REPLACE "\r\n" "\n" run_err "${run_err}")
        string(REPLACE "|" "\n" expected_err "${EXPECT_STDERR}")
        if(NOT run_err STREQUAL "${expected_err}")
            message(FATAL_ERROR
                "${NAME}: stderr mismatch\n--- got ---\n${run_err}\n--- want ---\n${expected_err}")
        endif()
        message(STATUS "${NAME}: stderr matched - OK")
    endif()
endif()

if(DEFINED INVOKE AND NOT INVOKE STREQUAL "")
    separate_arguments(INVOKE_ARGS UNIX_COMMAND "${ARGS}")
    execute_process(
        COMMAND "${WASMTIME}" run --invoke "${INVOKE}" "${WASM}" ${INVOKE_ARGS}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    )
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${NAME}: invoking ${INVOKE} failed (${rc}):\n${out}\n${err}")
    endif()
    string(STRIP "${out}" out)
    if(NOT out STREQUAL "${EXPECT_OUT}")
        message(FATAL_ERROR
            "${NAME}: ${INVOKE}(${ARGS}) returned '${out}', expected '${EXPECT_OUT}'")
    endif()
    message(STATUS "${NAME}: ${INVOKE}(${ARGS}) = ${out} - OK")
endif()
