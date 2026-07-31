# Runs the compiler and passes only when it fails with the expected diagnostic.
#
# Required -D vars: INSTY, SRC, EXE, MATCH, NAME.
# Optional: -DFLAGS=<space-separated extra flags> (e.g. "--bounds-check").
# Optional: -DINSTY_TARGET=<target> (defaults to x86_64_windows).

separate_arguments(EXTRA_FLAGS UNIX_COMMAND "${FLAGS}")

if(NOT DEFINED INSTY_TARGET OR INSTY_TARGET STREQUAL "")
    set(INSTY_TARGET "x86_64_windows")
endif()

execute_process(
    COMMAND "${INSTY}" "--target" "${INSTY_TARGET}" ${EXTRA_FLAGS}
            "-o" "${EXE}" "${SRC}"
    RESULT_VARIABLE build_rc
    OUTPUT_VARIABLE build_out
    ERROR_VARIABLE build_err
)

if(build_rc EQUAL 0)
    message(FATAL_ERROR "${NAME} unexpectedly succeeded:\n${build_out}\n${build_err}")
endif()

set(all_output "${build_out}\n${build_err}")
if(NOT all_output MATCHES "${MATCH}")
    message(FATAL_ERROR "${NAME} failed with unexpected diagnostic (${build_rc}):\n${all_output}")
endif()

message(STATUS "${NAME} failed as expected with diagnostic matching '${MATCH}'")
