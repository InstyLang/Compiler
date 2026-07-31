// Backend bring-up test: build a minimal `main` that returns 42 using the
// custom (LLVM-free) backend, write it as both a COFF and an ELF64 object, and
// sanity-check each container. Companion CMake tests link + run the native
// object to confirm the whole encode -> object -> link -> execute pipeline.
//
// Usage: backend_tests <output-base>
//   writes <output-base>.obj (COFF) and <output-base>.o (ELF64)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <backend/coff_writer.hpp>
#include <backend/elf_writer.hpp>
#include <backend/encoder.hpp>
#include <backend/machine_code.hpp>

using namespace Backend;

static int g_failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

static std::vector<std::uint8_t> readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    const std::string base = (argc > 1) ? argv[1] : "backend_main";
    const std::string coffPath = base + ".obj";
    const std::string elfPath = base + ".o";

    // Build:  int main() { return 42; }
    //   mov eax, 42
    //   ret
    MachineCode code;
    Encoder enc(code);

    const std::uint64_t fnStart = enc.offset();
    enc.movImm32ToEax(42);
    enc.ret();

    code.defineSymbol("main", SectionKind::Text, fnStart, SymbolBinding::Global,
                      /*isFunction=*/true);

    // The encoded body should be: B8 2A 00 00 00 (mov eax,42) + C3 (ret) = 6 bytes.
    check(code.text.bytes.size() == 6, "text section is 6 bytes");
    check(code.text.bytes[0] == 0xB8, "first byte is mov eax,imm32 (0xB8)");
    check(code.text.bytes[1] == 42, "immediate low byte is 42");
    check(code.text.bytes[5] == 0xC3, "last byte is ret (0xC3)");
    check(code.symbols.size() == 1, "one symbol defined");
    check(code.symbols[0].name == "main", "symbol is 'main'");

    std::string err;

    // --- COFF ----------------------------------------------------------------
    bool wroteCoff = CoffWriter::write(code, coffPath, err);
    check(wroteCoff, "CoffWriter::write succeeded");
    if (!wroteCoff) std::printf("  error: %s\n", err.c_str());

    if (wroteCoff) {
        std::vector<std::uint8_t> buf = readAll(coffPath);
        check(buf.size() >= 20, "COFF file is at least a header");
        if (buf.size() >= 20) {
            std::uint16_t machine = buf[0] | (buf[1] << 8);
            std::uint32_t numSymbols =
                buf[12] | (buf[13] << 8) | (buf[14] << 16) | (buf[15] << 24);
            check(machine == 0x8664, "COFF machine is AMD64 (0x8664)");
            check(numSymbols == 1, "COFF symbol count is 1");
        }
    }

    // --- ELF64 ---------------------------------------------------------------
    bool wroteElf = ElfWriter::write(code, elfPath, err);
    check(wroteElf, "ElfWriter::write succeeded");
    if (!wroteElf) std::printf("  error: %s\n", err.c_str());

    if (wroteElf) {
        std::vector<std::uint8_t> buf = readAll(elfPath);
        check(buf.size() >= 64, "ELF file is at least a header");
        if (buf.size() >= 64) {
            check(buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F',
                  "ELF magic present");
            check(buf[4] == 2, "ELFCLASS64");
            check(buf[5] == 1, "little-endian (ELFDATA2LSB)");
            std::uint16_t etype = buf[16] | (buf[17] << 8);
            std::uint16_t emachine = buf[18] | (buf[19] << 8);
            check(etype == 1, "ELF type is ET_REL");
            check(emachine == 62, "ELF machine is EM_X86_64 (62)");
        }
    }

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASSED" : "FAILED",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
