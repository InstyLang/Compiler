#include <backend/comptime_vm.hpp>

#include <cstring>
#include <iostream>
#include <fstream>
#include <cstdlib>

namespace Backend {

ComptimeVM::ComptimeVM() {
    memory_.resize(16 * 1024 * 1024, 0);
}

void ComptimeVM::addFunction(std::string name, std::shared_ptr<MFunction> fn) {
    functions_[name] = fn;
}

bool ComptimeVM::readMem(std::uint64_t addr, void* dst, std::size_t size) {
    if (addr + size > memory_.size()) return false;
    std::memcpy(dst, memory_.data() + addr, size);
    return true;
}

bool ComptimeVM::writeMem(std::uint64_t addr, const void* src, std::size_t size) {
    if (addr + size > memory_.size()) return false;
    std::memcpy(memory_.data() + addr, src, size);
    return true;
}

std::uint64_t ComptimeVM::evalOperandInt(const StackFrame& frame, const MOperand& op) {
    switch (op.kind) {
        case OperandKind::Imm:
            return static_cast<std::uint64_t>(op.imm);
        case OperandKind::VirtReg:
            if (op.vreg < frame.vregs.size()) {
                return frame.vregs[op.vreg].i;
            }
            return 0;
        case OperandKind::FrameSlot: {
            if (op.frameSlot < frame.fn->frameSlots().size()) {
                const auto& s = frame.fn->frameSlots()[op.frameSlot];
                std::uint64_t slotAddr = frame.frameBase + s.rbpOffset;
                std::uint64_t val = 0;
                readMem(slotAddr, &val, s.size ? s.size : 8);
                return val;
            }
            return 0;
        }
        default:
            return 0;
    }
}

double ComptimeVM::evalOperandFloat(const StackFrame& frame, const MOperand& op) {
    switch (op.kind) {
        case OperandKind::Imm: {
            double d = 0.0;
            std::memcpy(&d, &op.imm, sizeof(d));
            return d;
        }
        case OperandKind::VirtReg:
            if (op.vreg < frame.vregs.size()) {
                return frame.vregs[op.vreg].f;
            }
            return 0.0;
        case OperandKind::FrameSlot: {
            if (op.frameSlot < frame.fn->frameSlots().size()) {
                const auto& s = frame.fn->frameSlots()[op.frameSlot];
                std::uint64_t slotAddr = frame.frameBase + s.rbpOffset;
                double d = 0.0;
                readMem(slotAddr, &d, sizeof(d));
                return d;
            }
            return 0.0;
        }
        default:
            return 0.0;
    }
}

VmValue ComptimeVM::execute(const std::string& name, const std::vector<VmValue>& args, std::string& errorOut) {
    auto it = functions_.find(name);
    if (it == functions_.end()) {
        errorOut = "comptime VM: unknown function '" + name + "'";
        return {};
    }

    std::shared_ptr<MFunction> fn = it->second;
    StackFrame frame;
    frame.fn = fn;
    frame.vregs.resize(fn->numVRegs(), VmValue::fromInt(0));
    frame.frameBase = 512 * 1024;
    frame.currentBlock = 0;
    frame.currentInst = 0;

    for (size_t i = 0; i < args.size() && i < frame.vregs.size(); ++i) {
        frame.vregs[i] = args[i];
    }

    VmValue returnVal = VmValue::fromInt(0);
    if (!runFrame(frame, returnVal, errorOut)) {
        return {};
    }
    return returnVal;
}

bool ComptimeVM::runFrame(StackFrame& frame, VmValue& returnVal, std::string& errorOut) {
    auto& fn = *frame.fn;
    if (fn.blocks().empty()) return true;

    std::size_t stepLimit = 1000000;

    while (frame.currentBlock < fn.blocks().size() && stepLimit-- > 0) {
        const auto& block = fn.block(frame.currentBlock);
        if (frame.currentInst >= block.insts.size()) {
            frame.currentBlock++;
            frame.currentInst = 0;
            continue;
        }

        const auto& inst = block.insts[frame.currentInst++];
        switch (inst.op) {
            case MOpcode::MovRR:
            case MOpcode::MovRI: {
                if (!inst.operands.empty()) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t val = (inst.operands.size() > 1) ? evalOperandInt(frame, inst.operands[1]) : 0;
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(val);
                }
                break;
            }
            case MOpcode::Load: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint32_t slot = inst.operands[1].frameSlot;
                    if (slot < fn.frameSlots().size()) {
                        const auto& s = fn.frameSlots()[slot];
                        uint64_t addr = frame.frameBase + s.rbpOffset;
                        uint64_t val = 0;
                        readMem(addr, &val, inst.width ? inst.width : 8);
                        if (inst.isSigned) {
                            if (inst.width == 1) val = static_cast<int64_t>(static_cast<int8_t>(val));
                            else if (inst.width == 2) val = static_cast<int64_t>(static_cast<int16_t>(val));
                            else if (inst.width == 4) val = static_cast<int64_t>(static_cast<int32_t>(val));
                        }
                        if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(val);
                    }
                }
                break;
            }
            case MOpcode::Store: {
                if (inst.operands.size() >= 2) {
                    uint32_t slot = inst.operands[0].frameSlot;
                    uint64_t val = evalOperandInt(frame, inst.operands[1]);
                    if (slot < fn.frameSlots().size()) {
                        const auto& s = fn.frameSlots()[slot];
                        uint64_t addr = frame.frameBase + s.rbpOffset;
                        writeMem(addr, &val, inst.width ? inst.width : 8);
                    }
                }
                break;
            }
            case MOpcode::LoadInd: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t base = evalOperandInt(frame, inst.operands[1]);
                    int64_t disp = (inst.operands.size() >= 3) ? inst.operands[2].imm : 0;
                    uint64_t addr = base + disp;
                    uint64_t val = 0;
                    readMem(addr, &val, inst.width ? inst.width : 8);
                    if (inst.isSigned) {
                        if (inst.width == 1) val = static_cast<int64_t>(static_cast<int8_t>(val));
                        else if (inst.width == 2) val = static_cast<int64_t>(static_cast<int16_t>(val));
                        else if (inst.width == 4) val = static_cast<int64_t>(static_cast<int32_t>(val));
                    }
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(val);
                }
                break;
            }
            case MOpcode::StoreInd: {
                if (inst.operands.size() >= 3) {
                    uint64_t base = evalOperandInt(frame, inst.operands[0]);
                    int64_t disp = inst.operands[1].imm;
                    uint64_t val = evalOperandInt(frame, inst.operands[2]);
                    uint64_t addr = base + disp;
                    writeMem(addr, &val, inst.width ? inst.width : 8);
                }
                break;
            }
            case MOpcode::LeaSlot: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint32_t slot = inst.operands[1].frameSlot;
                    if (slot < fn.frameSlots().size()) {
                        const auto& s = fn.frameSlots()[slot];
                        uint64_t addr = frame.frameBase + s.rbpOffset;
                        if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(addr);
                    }
                }
                break;
            }
            case MOpcode::LeaDisp: {
                if (inst.operands.size() >= 3) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t base = evalOperandInt(frame, inst.operands[1]);
                    int64_t disp = inst.operands[2].imm;
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(base + disp);
                }
                break;
            }
            case MOpcode::Add: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t a = evalOperandInt(frame, inst.operands[0]);
                    uint64_t b = evalOperandInt(frame, inst.operands[1]);
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(a + b);
                }
                break;
            }
            case MOpcode::Sub: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t a = evalOperandInt(frame, inst.operands[0]);
                    uint64_t b = evalOperandInt(frame, inst.operands[1]);
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(a - b);
                }
                break;
            }
            case MOpcode::IMul: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    int64_t a = static_cast<int64_t>(evalOperandInt(frame, inst.operands[0]));
                    int64_t b = static_cast<int64_t>(evalOperandInt(frame, inst.operands[1]));
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(static_cast<uint64_t>(a * b));
                }
                break;
            }
            case MOpcode::Div: {
                if (inst.operands.size() >= 3) {
                    VReg dst = inst.operands[0].vreg;
                    int64_t a = static_cast<int64_t>(evalOperandInt(frame, inst.operands[1]));
                    int64_t b = static_cast<int64_t>(evalOperandInt(frame, inst.operands[2]));
                    if (b == 0) {
                        errorOut = "comptime VM: division by zero";
                        return false;
                    }
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(static_cast<uint64_t>(a / b));
                }
                break;
            }
            case MOpcode::Mod: {
                if (inst.operands.size() >= 3) {
                    VReg dst = inst.operands[0].vreg;
                    int64_t a = static_cast<int64_t>(evalOperandInt(frame, inst.operands[1]));
                    int64_t b = static_cast<int64_t>(evalOperandInt(frame, inst.operands[2]));
                    if (b == 0) {
                        errorOut = "comptime VM: modulo by zero";
                        return false;
                    }
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(static_cast<uint64_t>(a % b));
                }
                break;
            }
            case MOpcode::And: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t a = evalOperandInt(frame, inst.operands[0]);
                    uint64_t b = evalOperandInt(frame, inst.operands[1]);
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(a & b);
                }
                break;
            }
            case MOpcode::Or: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t a = evalOperandInt(frame, inst.operands[0]);
                    uint64_t b = evalOperandInt(frame, inst.operands[1]);
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(a | b);
                }
                break;
            }
            case MOpcode::Xor: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t a = evalOperandInt(frame, inst.operands[0]);
                    uint64_t b = evalOperandInt(frame, inst.operands[1]);
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(a ^ b);
                }
                break;
            }
            case MOpcode::Shl: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t a = evalOperandInt(frame, inst.operands[0]);
                    uint64_t b = evalOperandInt(frame, inst.operands[1]);
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(a << (b & 63));
                }
                break;
            }
            case MOpcode::Shr: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t a = evalOperandInt(frame, inst.operands[0]);
                    uint64_t b = evalOperandInt(frame, inst.operands[1]);
                    if (inst.isSigned) {
                        int64_t sa = static_cast<int64_t>(a);
                        if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(static_cast<uint64_t>(sa >> (b & 63)));
                    } else {
                        if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(a >> (b & 63));
                    }
                }
                break;
            }
            case MOpcode::Neg: {
                if (!inst.operands.empty()) {
                    VReg dst = inst.operands[0].vreg;
                    int64_t a = static_cast<int64_t>(evalOperandInt(frame, inst.operands[0]));
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(static_cast<uint64_t>(-a));
                }
                break;
            }
            case MOpcode::Not: {
                if (!inst.operands.empty()) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t a = evalOperandInt(frame, inst.operands[0]);
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(~a);
                }
                break;
            }
            case MOpcode::Cmp: {
                if (inst.operands.size() >= 2) {
                    int64_t a = static_cast<int64_t>(evalOperandInt(frame, inst.operands[0]));
                    int64_t b = static_cast<int64_t>(evalOperandInt(frame, inst.operands[1]));
                    if (a < b) frame.flags = -1;
                    else if (a > b) frame.flags = 1;
                    else frame.flags = 0;
                }
                break;
            }
            case MOpcode::SetCC: {
                if (!inst.operands.empty()) {
                    VReg dst = inst.operands[0].vreg;
                    bool condMet = false;
                    switch (inst.cond) {
                        case Cond::EQ: condMet = (frame.flags == 0); break;
                        case Cond::NE: condMet = (frame.flags != 0); break;
                        case Cond::LT: condMet = (frame.flags < 0); break;
                        case Cond::LE: condMet = (frame.flags <= 0); break;
                        case Cond::GT: condMet = (frame.flags > 0); break;
                        case Cond::GE: condMet = (frame.flags >= 0); break;
                        default: break;
                    }
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(condMet ? 1 : 0);
                }
                break;
            }
            case MOpcode::Jmp: {
                if (!inst.operands.empty() && inst.operands[0].kind == OperandKind::Label) {
                    uint32_t targetIdx = inst.operands[0].label;
                    if (targetIdx < fn.blocks().size()) {
                        frame.currentBlock = targetIdx;
                        frame.currentInst = 0;
                    }
                }
                break;
            }
            case MOpcode::Jcc: {
                if (!inst.operands.empty() && inst.operands[0].kind == OperandKind::Label) {
                    bool take = false;
                    switch (inst.cond) {
                        case Cond::EQ: take = (frame.flags == 0); break;
                        case Cond::NE: take = (frame.flags != 0); break;
                        case Cond::LT: take = (frame.flags < 0); break;
                        case Cond::LE: take = (frame.flags <= 0); break;
                        case Cond::GT: take = (frame.flags > 0); break;
                        case Cond::GE: take = (frame.flags >= 0); break;
                        default: break;
                    }
                    if (take) {
                        uint32_t targetIdx = inst.operands[0].label;
                        if (targetIdx < fn.blocks().size()) {
                            frame.currentBlock = targetIdx;
                            frame.currentInst = 0;
                        }
                    }
                }
                break;
            }
            case MOpcode::Call: {
                if (!inst.operands.empty() && inst.operands[0].kind == OperandKind::Symbol) {
                    const std::string& callee = inst.operands[0].symbol;

                    // Support builtins and file IO inside the VM
                    if (callee == "read_file" || callee == "embed_file") {
                        // arg 0 is pointer to file path string
                        uint64_t pathAddr = frame.vregs[0].i;
                        char pathBuf[512] = {0};
                        readMem(pathAddr, pathBuf, sizeof(pathBuf) - 1);
                        std::ifstream in(pathBuf, std::ios::binary);
                        if (!in.is_open()) {
                            errorOut = "comptime VM: could not open file '" + std::string(pathBuf) + "'";
                            return false;
                        }
                        std::string content((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
                        // Allocate on VM heap
                        uint64_t heapAddr = heapCursor_;
                        heapCursor_ += content.size() + 1;
                        writeMem(heapAddr, content.c_str(), content.size() + 1);
                        returnVal = VmValue::fromInt(heapAddr);
                        break;
                    }
                    if (callee == "get_env") {
                        uint64_t nameAddr = frame.vregs[0].i;
                        char nameBuf[256] = {0};
                        readMem(nameAddr, nameBuf, sizeof(nameBuf) - 1);
                        const char* val = std::getenv(nameBuf);
                        std::string res = val ? val : "";
                        uint64_t heapAddr = heapCursor_;
                        heapCursor_ += res.size() + 1;
                        writeMem(heapAddr, res.c_str(), res.size() + 1);
                        returnVal = VmValue::fromInt(heapAddr);
                        break;
                    }

                    auto fit = functions_.find(callee);
                    if (fit == functions_.end()) {
                        errorOut = "comptime VM: unresolvable call to '" + callee + "'";
                        return false;
                    }
                    StackFrame sub;
                    sub.fn = fit->second;
                    sub.vregs.resize(sub.fn->numVRegs(), VmValue::fromInt(0));
                    sub.frameBase = frame.frameBase + 64 * 1024;
                    sub.currentBlock = 0;
                    sub.currentInst = 0;
                    VmValue subRet = VmValue::fromInt(0);
                    if (!runFrame(sub, subRet, errorOut)) return false;
                    returnVal = subRet;
                }
                break;
            }
            case MOpcode::Ret: {
                if (!inst.operands.empty()) {
                    returnVal = VmValue::fromInt(evalOperandInt(frame, inst.operands[0]));
                }
                return true;
            }
            default:
                break;
        }
    }

    if (stepLimit == 0) {
        errorOut = "comptime VM: execution step limit exceeded (infinite loop?)";
        return false;
    }

    return true;
}

} // namespace Backend
