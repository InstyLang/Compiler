# Links the backend-emitted ELF64 object into an executable with the system C
# compiler driver and runs it, asserting an exit code of 42. Driven by the
# tests section of CMakeLists via -P.
#
# Required -D vars: CC (cc/clang/gcc), OBJ (input .o), EXE (output binary).
#
# The object defines a global `main` that returns 42. We link it normally so
# the C runtime's _start calls main() and exits with its return value.

execute_process(
    COMMAND "${CC}" "${OBJ}" "-o" "${EXE}"
    RESULT_VARIABLE link_rc
    OUTPUT_VARIABLE link_out
    ERROR_VARIABLE link_err
)
if(NOT link_rc EQUAL 0)
    message(FATAL_ERROR "link failed (${link_rc}):\n${link_out}\n${link_err}")
endif()

execute_process(
    COMMAND "${EXE}"
    RESULT_VARIABLE run_rc
)
if(NOT run_rc EQUAL 42)
    message(FATAL_ERROR "linked executable returned ${run_rc}, expected 42")
endif()

message(STATUS "backend ELF link+run: exit code ${run_rc} (expected 42) - OK")
