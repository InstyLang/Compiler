# Builds an Insty fixture natively with --bounds-check and runs it, asserting the
# exit code. Used to verify that in-range programs still work and that an
# out-of-range access traps at runtime.
#
# Required -D vars: INSTY, SRC, EXE, EXPECT, NAME.
#   EXPECT is an integer exit code, or the literal CRASH to require a non-zero /
#   signal-terminated exit (the bounds trap is `ud2` -> SIGILL).
# Optional: -DALLOC=ON to pass --allocator runtime (for `new`).

set(alloc_flag)
if(DEFINED ALLOC AND ALLOC)
    set(alloc_flag "--allocator" "runtime")
endif()

execute_process(
    COMMAND "${INSTY}" "--bounds-check" ${alloc_flag} "-o" "${EXE}" "${SRC}"
    RESULT_VARIABLE build_rc
    OUTPUT_VARIABLE build_out
    ERROR_VARIABLE build_err
)
if(NOT build_rc EQUAL 0)
    message(FATAL_ERROR "${NAME} build failed (${build_rc}):\n${build_out}\n${build_err}")
endif()

execute_process(COMMAND "${EXE}" RESULT_VARIABLE run_rc)

if(EXPECT STREQUAL "CRASH")
    # A signal-terminated process yields a non-integer descriptor string; a clean
    # exit yields 0. Either way, anything other than 0 means the trap fired.
    if(run_rc STREQUAL "0" OR run_rc EQUAL 0)
        message(FATAL_ERROR "${NAME}: expected an out-of-bounds trap, but it exited 0")
    endif()
    message(STATUS "${NAME}: trapped as expected (${run_rc})")
else()
    if(NOT run_rc EQUAL ${EXPECT})
        message(FATAL_ERROR "${NAME} returned ${run_rc}, expected ${EXPECT}")
    endif()
    message(STATUS "${NAME}: exit ${run_rc} (expected ${EXPECT}) - OK")
endif()
