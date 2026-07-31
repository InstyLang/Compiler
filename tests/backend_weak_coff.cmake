# Validates the custom backend's COFF weak-symbol support end to end:
#   1. weak_def.o alone links and runs, using the weak fallback `wfn` (exit 7).
#   2. weak_def.o + weak_strong.o links and runs, the strong `wfn` overriding the
#      weak default (exit 99).
# Both objects are emitted by insty (custom COFF backend) and linked with
# lld-link. Driven by tests/CMakeLists via -P.
#
# Required -D vars: INSTY (insty), LLD (lld-link), OBJDIR (object output dir),
#                   WEAK_SRC, STRONG_SRC (the .ins fixtures).

# --- Emit both objects ------------------------------------------------------
execute_process(
    COMMAND "${INSTY}" --no-std --target x86_64_windows -c
            --objects-dir "${OBJDIR}" "${WEAK_SRC}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "insty failed on weak_def (${rc}):\n${o}\n${e}")
endif()
execute_process(
    COMMAND "${INSTY}" --no-std --target x86_64_windows -c
            --objects-dir "${OBJDIR}" "${STRONG_SRC}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "insty failed on weak_strong (${rc}):\n${o}\n${e}")
endif()

set(WEAK_OBJ "${OBJDIR}/weak_def.o")
set(STRONG_OBJ "${OBJDIR}/weak_strong.o")

# --- 1. Weak fallback alone -> exit 7 ---------------------------------------
set(EXE1 "${OBJDIR}/weak_fallback.exe")
execute_process(
    COMMAND "${LLD}" "/entry:main" "/subsystem:console" "/nodefaultlib"
            "/out:${EXE1}" "${WEAK_OBJ}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lld-link (fallback) failed (${rc}):\n${o}\n${e}")
endif()
execute_process(COMMAND "${EXE1}" RESULT_VARIABLE run1)
if(NOT run1 EQUAL 7)
    message(FATAL_ERROR "weak fallback returned ${run1}, expected 7")
endif()

# --- 2. Strong override -> exit 99 ------------------------------------------
set(EXE2 "${OBJDIR}/weak_override.exe")
execute_process(
    COMMAND "${LLD}" "/entry:main" "/subsystem:console" "/nodefaultlib"
            "/out:${EXE2}" "${WEAK_OBJ}" "${STRONG_OBJ}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lld-link (override) failed (${rc}):\n${o}\n${e}")
endif()
execute_process(COMMAND "${EXE2}" RESULT_VARIABLE run2)
if(NOT run2 EQUAL 99)
    message(FATAL_ERROR "strong override returned ${run2}, expected 99 (weak not overridden)")
endif()

message(STATUS "COFF weak symbols: fallback=7, override=99 - OK")
