# Builds an Insty fixture for the host (default target) and runs it, asserting
# the process exit code. Used for hosted-Linux stdlib tests that must actually
# execute (containers, string builder, file I/O).
#
# Required -D vars: INSTY, SRC, EXE, EXPECT, NAME.
# Optional: -DFLAGS=<space-separated extra flags> (e.g. "--allocator runtime").

separate_arguments(EXTRA_FLAGS UNIX_COMMAND "${FLAGS}")

execute_process(
    COMMAND "${INSTY}" ${EXTRA_FLAGS} "-o" "${EXE}" "${SRC}"
    RESULT_VARIABLE build_rc
    OUTPUT_VARIABLE build_out
    ERROR_VARIABLE build_err
)
if(NOT build_rc EQUAL 0)
    message(FATAL_ERROR "${NAME} build failed (${build_rc}):\n${build_out}\n${build_err}")
endif()

execute_process(COMMAND "${EXE}" RESULT_VARIABLE run_rc)
if(NOT run_rc EQUAL ${EXPECT})
    message(FATAL_ERROR "${NAME} returned ${run_rc}, expected ${EXPECT}")
endif()
message(STATUS "${NAME}: exit ${run_rc} (expected ${EXPECT}) - OK")
