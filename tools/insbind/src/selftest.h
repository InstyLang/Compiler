// insbind: golden-test harness.
//
// Golden tests are the project's primary correctness tool: fixed inputs in,
// expected text out, byte-compared. They are language-neutral by design --
// when insbind is eventually ported to Insty, the same tests verify the port
// against the C++ reference output.
#pragma once

#include <string>

namespace insbind {

// Runs golden tests under `dir` (recursive). For each *.c file, the token
// dump is compared against the sibling *.tokens file. With `bless`, the
// expected files are (re)written from actual output instead -- the workflow
// for updating goldens, always followed by human review of the diff.
// Prints per-file results and a summary; returns a process exit code.
int selfTest(const std::string& dir, bool bless);

} // namespace insbind
