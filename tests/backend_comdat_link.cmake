# Separately-compiled objects that share a generic instantiation must link.
#
# `comdat_a` and `comdat_b` both instantiate Vector<i64>, so every object carries
# its own copy of each method. Nothing else in the suite links independently
# emitted objects together, which is how duplicate-definition bugs in the object
# writers went unnoticed: whole-program mode dedupes by name before emitting, so
# it cannot expose them.
#
# Each module is compiled in its OWN insty invocation, because that is what a
# build system does and it removes any chance of the compiler deduping across
# modules within one process.
#
# Required -D vars: INSTY, LLD (lld-link), OBJDIR, SRC_A, SRC_B, SRC_MAIN.

foreach(src "${SRC_A}" "${SRC_B}" "${SRC_MAIN}")
    execute_process(
        COMMAND "${INSTY}" --target x86_64_windows -c --objects-dir "${OBJDIR}" "${src}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "insty -c failed on ${src} (${rc}):\n${o}\n${e}")
    endif()
endforeach()

file(GLOB OBJS "${OBJDIR}/*.o")
set(EXE "${OBJDIR}/comdat_link.exe")
execute_process(
    COMMAND "${LLD}" "/out:${EXE}" /subsystem:console /entry:main ${OBJS} kernel32.lib
    RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "linking separately-compiled objects failed (${rc}). A duplicate-symbol "
        "error here means a definition emitted by several objects is not marked "
        "foldable (COFF: a COMDAT section):\n${o}\n${e}")
endif()

execute_process(COMMAND "${EXE}" RESULT_VARIABLE run_rc)
if(NOT run_rc EQUAL 30)
    message(FATAL_ERROR "linked program exited ${run_rc}, expected 30")
endif()
message(STATUS "comdat link: separate objects linked, exit ${run_rc} - OK")
