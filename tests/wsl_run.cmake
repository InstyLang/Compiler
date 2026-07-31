# Cross-compiles a fixture for x86_64_linux and runs it under WSL, asserting the
# exit code.
#
# This exists because the System V ABI is otherwise untested. A Windows host runs
# Win64 (which never classifies a struct as SSE, so multi-register float arguments
# are unreachable) and wasm has no register ABI at all -- so roughly half the
# argument-passing code had no coverage. It is also the only way to *execute* the
# Linux syscall paths in std::io and std::fs rather than reading their
# disassembly.
#
# Driven by CMakeLists via -P, gated on INSTY_CTEST_WSL.
#
# Required -D vars: INSTY, SRC, ELF, EXPECT, NAME.
# Optional: -DUSE_STD=ON omits --no-std so std imports resolve.

set(INSTY_STD_FLAG "--no-std")
if(DEFINED USE_STD AND USE_STD)
    set(INSTY_STD_FLAG)
endif()

execute_process(
    COMMAND "${INSTY}" ${INSTY_STD_FLAG} "--target" "x86_64_linux"
            "-o" "${ELF}" "${SRC}"
    RESULT_VARIABLE build_rc
    OUTPUT_VARIABLE build_out
    ERROR_VARIABLE build_err
)
if(NOT build_rc EQUAL 0)
    message(FATAL_ERROR "${NAME}: linux build failed (${build_rc}):\n${build_out}\n${build_err}")
endif()

# Translate the Windows path for the Linux side. wslpath handles the drive
# mapping; hand-rolling it would break on anything but C:.
execute_process(
    COMMAND wsl -e wslpath -a "${ELF}"
    RESULT_VARIABLE path_rc
    OUTPUT_VARIABLE wsl_elf
    ERROR_VARIABLE path_err
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT path_rc EQUAL 0)
    message(FATAL_ERROR "${NAME}: wslpath failed (${path_rc}):\n${path_err}")
endif()

# No chmod: drvfs presents files as executable already, and chmod on it fails.
# The working directory is the ELF's own, so a fixture doing file I/O writes
# somewhere harmless.
get_filename_component(ELF_DIR "${ELF}" DIRECTORY)
execute_process(
    COMMAND wsl -e "${wsl_elf}"
    WORKING_DIRECTORY "${ELF_DIR}"
    RESULT_VARIABLE run_rc
    OUTPUT_VARIABLE run_out
    ERROR_VARIABLE run_err
)
if(NOT run_rc EQUAL ${EXPECT})
    message(FATAL_ERROR
        "${NAME}: linux run exited ${run_rc}, expected ${EXPECT}.\n"
        "A failure here that passes on Windows usually means System V ABI "
        "behaviour the Win64 path cannot reach.\n${run_out}\n${run_err}")
endif()
message(STATUS "${NAME}: linux run exited ${run_rc} (expected ${EXPECT}) - OK")
