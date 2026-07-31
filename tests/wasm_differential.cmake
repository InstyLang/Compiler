# Differential test: the same Insty source through both backends must agree.
#
# This is the strongest check available for the WebAssembly backend. The x86-64
# path is mature and independently tested, so compiling a fixture for both
# targets and comparing the process exit status validates wasm codegen against a
# known-good implementation of the same language semantics -- rather than against
# expectations hand-written by whoever wrote the wasm backend.
#
# It catches the whole class of bugs that produce a *valid* module computing the
# *wrong* answer, which wasm-tools cannot see.
#
# Requires a runnable x86-64 reference, so callers should gate this on WIN32
# (where the compiler emits a directly runnable PE) and on wasmtime being
# available.
#
# Required -D vars: INSTY, SRC, REF_EXE, WASM, NAME.
# Optional: -DOPT=-O2.

set(INSTY_OPT_FLAG)
if(DEFINED OPT AND NOT OPT STREQUAL "")
    set(INSTY_OPT_FLAG "${OPT}")
endif()

# --- reference: x86-64 --------------------------------------------------------
execute_process(
    COMMAND "${INSTY}" "--target" "x86_64_windows" ${INSTY_OPT_FLAG}
            "-o" "${REF_EXE}" "${SRC}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${NAME}: x86-64 reference build failed (${rc}):\n${out}\n${err}")
endif()
execute_process(COMMAND "${REF_EXE}" RESULT_VARIABLE reference_status
                OUTPUT_QUIET ERROR_QUIET)

# --- subject: wasm ------------------------------------------------------------
execute_process(
    COMMAND "${INSTY}" "--target" "wasm32_wasi" ${INSTY_OPT_FLAG}
            "-o" "${WASM}" "${SRC}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${NAME}: wasm build failed (${rc}):\n${out}\n${err}")
endif()

find_program(WASM_TOOLS NAMES wasm-tools)
if(WASM_TOOLS)
    execute_process(COMMAND "${WASM_TOOLS}" validate "${WASM}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${NAME}: wasm-tools rejected the module:\n${out}\n${err}")
    endif()
endif()

find_program(WASMTIME NAMES wasmtime)
if(NOT WASMTIME)
    message(STATUS "${NAME}: wasmtime not found; built both targets, cannot compare")
    return()
endif()

# WASI's proc_exit only accepts a status in [0, 126). A fixture whose result is
# larger cannot round-trip through it, so the exit status carries no information
# and there is nothing to compare -- the module itself is still built and
# validated above. This is a property of WASI, not of code generation: native
# Linux is also lossy here (its _start masks the result to & 0xFF).
if(reference_status LESS 0 OR reference_status GREATER_EQUAL 126)
    message(STATUS
        "${NAME}: reference exited ${reference_status}, outside WASI's [0,126) "
        "exit-status range; built and validated the module but cannot compare")
    return()
endif()

execute_process(COMMAND "${WASMTIME}" run "${WASM}" RESULT_VARIABLE wasm_status
                OUTPUT_QUIET ERROR_QUIET)

if(NOT wasm_status EQUAL ${reference_status})
    message(FATAL_ERROR
        "${NAME}: backends disagree -- x86-64 exited ${reference_status}, "
        "wasm exited ${wasm_status}")
endif()
message(STATUS "${NAME}: both backends exited ${reference_status} - OK")
