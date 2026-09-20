#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <backend/machine_ir.hpp>
#include <extra/type_system.hpp>

namespace Backend {

struct VmValue {
    enum class Kind { Integer, Float, None };
    Kind kind = Kind::None;
    std::uint64_t i = 0;
    double f = 0.0;

    static VmValue fromInt(std::uint64_t v) {
        VmValue val; val.kind = Kind::Integer; val.i = v; return val;
    }
    static VmValue fromFloat(double v) {
        VmValue val; val.kind = Kind::Float; val.f = v; return val;
    }
};

class ComptimeVM {
public:
    ComptimeVM();

    // Register an MFunction by symbol/name so calls can dispatch to it
    void addFunction(std::string name, std::shared_ptr<MFunction> fn);

    // Execute an entry function with given arguments, returning result
    VmValue execute(const std::string& name, const std::vector<VmValue>& args, std::string& errorOut);

    // Access emulated sandbox memory
    std::uint8_t* memory() { return memory_.data(); }
    std::size_t memorySize() const { return memory_.size(); }

    // Helpers to read/write memory
    bool readMem(std::uint64_t addr, void* dst, std::size_t size);
    bool writeMem(std::uint64_t addr, const void* src, std::size_t size);

private:
    struct StackFrame {
        std::shared_ptr<MFunction> fn;
        std::vector<VmValue> vregs;
        std::uint64_t frameBase = 0; // offset in memory_
        std::uint32_t currentBlock = 0;
        std::size_t currentInst = 0;
        int flags = 0; // compare result: -1, 0, 1, or unordered
    };

    std::unordered_map<std::string, std::shared_ptr<MFunction>> functions_;
    std::vector<std::uint8_t> memory_;
    std::uint64_t heapCursor_ = 1024 * 1024; // start heap at 1MB
    std::uint64_t stackCursor_ = 1024 * 1024; // stack grows down from 1MB

    bool runFrame(StackFrame& frame, VmValue& returnVal, std::string& errorOut);
    std::uint64_t evalOperandInt(const StackFrame& frame, const MOperand& op);
    double evalOperandFloat(const StackFrame& frame, const MOperand& op);
};

} // namespace Backend
