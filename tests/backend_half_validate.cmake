# Builds the f16 (half) fixture into a Windows PE executable with the custom
# backend and runs it, asserting the expected exit code. Driven by CMakeLists
# via -P. Verifies end-to-end f16 support: packed 16-bit storage, F16C
# (vcvtph2ps/vcvtps2ph) compute-as-f32, the call ABI (params + return), and
# casts to/from integer and f32/f64.
#
# Required -D vars: INSTY (insty path), SRC (half.ins), EXE (output .exe),
#                   EXPECT (expected exit code).

execute_process(
    COMMAND "${INSTY}" "--no-std" "--target" "x86_64_windows"
            "-O0" "-o" "${EXE}" "${SRC}"
    RESULT_VARIABLE build_rc
    OUTPUT_VARIABLE build_out
    ERROR_VARIABLE build_err
)
if(NOT build_rc EQUAL 0)
    message(FATAL_ERROR "insty build failed (${build_rc}):\n${build_out}\n${build_err}")
endif()

execute_process(
    COMMAND "${EXE}"
    RESULT_VARIABLE run_rc
)
if(NOT run_rc EQUAL ${EXPECT})
    message(FATAL_ERROR "f16 executable returned ${run_rc}, expected ${EXPECT}")
endif()

message(STATUS "f16 build+run: exit code ${run_rc} (expected ${EXPECT}) - OK")
