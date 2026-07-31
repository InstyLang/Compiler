# Links the backend-emitted COFF object into an executable with lld-link and
# runs it, asserting an exit code of 42. Driven by tests/CMakeLists via -P.
#
# Required -D vars: LLD (lld-link path), OBJ (input .obj), EXE (output .exe).

execute_process(
    COMMAND "${LLD}" "/entry:main" "/subsystem:console" "/nodefaultlib"
            "/out:${EXE}" "${OBJ}"
    RESULT_VARIABLE link_rc
    OUTPUT_VARIABLE link_out
    ERROR_VARIABLE link_err
)
if(NOT link_rc EQUAL 0)
    message(FATAL_ERROR "lld-link failed (${link_rc}):\n${link_out}\n${link_err}")
endif()

execute_process(
    COMMAND "${EXE}"
    RESULT_VARIABLE run_rc
)
if(NOT run_rc EQUAL 42)
    message(FATAL_ERROR "linked executable returned ${run_rc}, expected 42")
endif()

message(STATUS "backend link+run: exit code ${run_rc} (expected 42) - OK")
