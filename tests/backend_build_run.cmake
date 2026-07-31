# Builds an Insty fixture into a Windows PE executable and runs it, asserting the
# expected exit code. Driven by CMakeLists via -P.
#
# Required -D vars: INSTY, SRC, EXE, EXPECT, NAME.
# Optional: -DUSE_STD=ON omits --no-std so std imports resolve.

set(INSTY_STD_FLAG "--no-std")
if(DEFINED USE_STD AND USE_STD)
    set(INSTY_STD_FLAG)
endif()

execute_process(
    COMMAND "${INSTY}" ${INSTY_STD_FLAG} "--target" "x86_64_windows"
            "-O0" "-o" "${EXE}" "${SRC}"
    RESULT_VARIABLE build_rc
    OUTPUT_VARIABLE build_out
    ERROR_VARIABLE build_err
)
if(NOT build_rc EQUAL 0)
    message(FATAL_ERROR "${NAME} build failed (${build_rc}):\n${build_out}\n${build_err}")
endif()

execute_process(
    COMMAND "${EXE}"
    RESULT_VARIABLE run_rc
)
if(NOT run_rc EQUAL ${EXPECT})
    message(FATAL_ERROR "${NAME} executable returned ${run_rc}, expected ${EXPECT}")
endif()

message(STATUS "${NAME} build+run: exit code ${run_rc} (expected ${EXPECT}) - OK")
