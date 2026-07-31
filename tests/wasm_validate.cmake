# Validates emitted .wasm modules with a real WebAssembly toolchain.
#
# Neither wasm-tools nor wasmtime is a build dependency of the compiler, so this
# test degrades gracefully: if a tool is missing it is reported and skipped. The
# byte-exact golden tests in tests/wasm_writer_tests.cpp are the hard guarantee;
# this is the independent second opinion, and it starts working the moment
# either tool is on PATH.
#
# Required -D vars: ADD_WASM, START_WASM, ALL_WASM.

set(checked 0)
set(modules "${ADD_WASM}" "${START_WASM}" "${ALL_WASM}")

foreach(module IN LISTS modules)
    if(NOT EXISTS "${module}")
        message(FATAL_ERROR "expected module was not produced: ${module}")
    endif()
    # A module is at least the 8-byte preamble.
    file(SIZE "${module}" module_size)
    if(module_size LESS 8)
        message(FATAL_ERROR "${module} is too small to be a wasm module (${module_size} bytes)")
    endif()
    # Check the magic and version directly, so this test is worth something even
    # with no toolchain installed.
    file(READ "${module}" preamble HEX LIMIT 8)
    if(NOT preamble STREQUAL "0061736d01000000")
        message(FATAL_ERROR "${module} has a bad preamble: ${preamble}")
    endif()
endforeach()
message(STATUS "wasm preamble and size checks passed")

find_program(WASM_TOOLS NAMES wasm-tools)
if(WASM_TOOLS)
    foreach(module IN LISTS modules)
        execute_process(
            COMMAND "${WASM_TOOLS}" validate "${module}"
            RESULT_VARIABLE rc
            OUTPUT_VARIABLE out
            ERROR_VARIABLE err
        )
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "wasm-tools validate rejected ${module}:\n${out}\n${err}")
        endif()
        message(STATUS "wasm-tools validate: ${module} OK")
    endforeach()
    math(EXPR checked "${checked}+1")
else()
    message(STATUS "wasm-tools not found; skipping validation")
endif()

find_program(WASMTIME NAMES wasmtime)
if(WASMTIME)
    # The _start module is a WASI command: it should instantiate, run and exit 0.
    execute_process(
        COMMAND "${WASMTIME}" run "${START_WASM}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "wasmtime failed to run ${START_WASM} (${rc}):\n${out}\n${err}")
    endif()
    message(STATUS "wasmtime run: ${START_WASM} exited 0")
    math(EXPR checked "${checked}+1")
else()
    message(STATUS "wasmtime not found; skipping execution")
endif()

if(checked EQUAL 0)
    message(STATUS "no wasm toolchain available; preamble checks only")
endif()
