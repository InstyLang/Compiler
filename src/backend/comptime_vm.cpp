#include <backend/comptime_vm.hpp>
#include <backend/isel.hpp>
#include <backend/const_eval.hpp>

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
        case OperandKind::PhysReg: {
            auto it = frame.physRegs.find(op.phys);
            if (it != frame.physRegs.end()) return it->second;
            if (op.phys == PhysReg::RAX) return frame.rax;
            return 0;
        }
        case OperandKind::FrameSlot: {
            if (op.frameSlot < frame.fn->frameSlots().size()) {
                const auto& s = frame.fn->frameSlots()[op.frameSlot];
                std::uint64_t slotAddr = frame.frameBase + op.frameSlot * 8;
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
                std::uint64_t slotAddr = frame.frameBase + op.frameSlot * 8;
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

    static const PhysReg kSysVArgs[] = {PhysReg::RDI, PhysReg::RSI, PhysReg::RDX, PhysReg::RCX, PhysReg::R8, PhysReg::R9};
    static const PhysReg kWin64Args[] = {PhysReg::RCX, PhysReg::RDX, PhysReg::R8, PhysReg::R9};
    const PhysReg* argRegList = (fn->abi() == Abi::Win64) ? kWin64Args : kSysVArgs;
    size_t numArgRegs = (fn->abi() == Abi::Win64) ? 4 : 6;

    for (size_t i = 0; i < args.size() && i < numArgRegs; ++i) {
        frame.physRegs[argRegList[i]] = args[i].i;
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
                    uint64_t val = (inst.operands.size() > 1) ? evalOperandInt(frame, inst.operands[1]) : 0;
                    if (inst.operands[0].kind == OperandKind::VirtReg) {
                        VReg dst = inst.operands[0].vreg;
                        if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(val);
                    } else if (inst.operands[0].kind == OperandKind::PhysReg) {
                        frame.physRegs[inst.operands[0].phys] = val;
                        if (inst.operands[0].phys == PhysReg::RAX) {
                            frame.rax = val;
                        }
                    }
                }
                break;
            }
            case MOpcode::Load: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint32_t slot = inst.operands[1].frameSlot;
                    if (slot < fn.frameSlots().size()) {
                        uint64_t addr = frame.frameBase + slot * 8;
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
                        uint64_t addr = frame.frameBase + slot * 8;
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
                        uint64_t addr = frame.frameBase + slot * 8;
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
                    uint64_t res = a + b;
                    if (inst.width == 4) res = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(res)));
                    else if (inst.width == 2) res = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(res)));
                    else if (inst.width == 1) res = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(res)));
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(res);
                }
                break;
            }
            case MOpcode::Sub: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    uint64_t a = evalOperandInt(frame, inst.operands[0]);
                    uint64_t b = evalOperandInt(frame, inst.operands[1]);
                    uint64_t res = a - b;
                    if (inst.width == 4) res = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(res)));
                    else if (inst.width == 2) res = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(res)));
                    else if (inst.width == 1) res = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(res)));
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(res);
                }
                break;
            }
            case MOpcode::IMul: {
                if (inst.operands.size() >= 2) {
                    VReg dst = inst.operands[0].vreg;
                    int64_t a = static_cast<int64_t>(evalOperandInt(frame, inst.operands[0]));
                    int64_t b = static_cast<int64_t>(evalOperandInt(frame, inst.operands[1]));
                    int64_t res = a * b;
                    if (inst.width == 4) res = static_cast<int64_t>(static_cast<int32_t>(res));
                    if (dst < frame.vregs.size()) frame.vregs[dst] = VmValue::fromInt(static_cast<uint64_t>(res));
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
                    if (inst.width == 4) {
                        a = static_cast<int64_t>(static_cast<int32_t>(a));
                        b = static_cast<int64_t>(static_cast<int32_t>(b));
                    }
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
                        continue;
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
                            continue;
                        }
                    }
                }
                break;
            }
            case MOpcode::Call: {
                if (!inst.operands.empty() && inst.operands[0].kind == OperandKind::Symbol) {
                    const std::string& callee = inst.operands[0].symbol;

                    if (callee == "read_file" || callee == "embed_file") {
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
                    sub.physRegs = frame.physRegs; // Forward argument registers!
                    VmValue subRet = VmValue::fromInt(0);
                    if (!runFrame(sub, subRet, errorOut)) return false;
                    frame.rax = subRet.i; // Result in RAX for caller to read
                    returnVal = subRet;
                }
                break;
            }
            case MOpcode::Ret: {
                if (!inst.operands.empty()) {
                    returnVal = VmValue::fromInt(evalOperandInt(frame, inst.operands[0]));
                } else {
                    returnVal = VmValue::fromInt(frame.rax);
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

bool ComptimeVM::evalConstExpr(const AST::ExprAST* expr, VmValue& out) {
    if (!expr) return false;
    Utilities::Int128 iVal{};
    if (evalConstInt(expr, iVal)) {
        out = VmValue::fromInt(iVal.low64());
        return true;
    }
    if (expr->nodeType() == AST::NodeType::FloatLiteral) {
        out = VmValue::fromFloat(static_cast<const AST::FloatLiteral*>(expr)->value);
        return true;
    }
    return false;
}

namespace {

bool foldExprInPlace(AST::NodePtr& node, const Sema::SemaResult& sema,
                     InstructionSelector& isel, ComptimeVM& vm, std::string& errorOut);

bool foldNodeList(AST::NodeList& list, const Sema::SemaResult& sema,
                  InstructionSelector& isel, ComptimeVM& vm, std::string& errorOut) {
    for (auto& item : list) {
        if (!foldExprInPlace(item, sema, isel, vm, errorOut)) return false;
    }
    return true;
}

bool foldExprInPlace(AST::NodePtr& node, const Sema::SemaResult& sema,
                     InstructionSelector& isel, ComptimeVM& vm, std::string& errorOut) {
    if (!node) return true;

    if (node->nodeType() == AST::NodeType::FunctionCall) {
        auto* call = static_cast<AST::FunctionCallExpr*>(node.get());
        for (auto& arg : call->arguments) {
            if (!foldExprInPlace(arg, sema, isel, vm, errorOut)) return false;
        }

        auto itTarget = sema.callTargets.find(call);
        if (itTarget != sema.callTargets.end()) {
            const std::string& targetSymbol = itTarget->second;
            for (const auto& fi : sema.functions) {
                const std::string& sym = fi.mangledName.empty() ? fi.name : fi.mangledName;
                if (sym == targetSymbol && fi.isComptime) {
                    std::vector<VmValue> args;
                    for (const auto& arg : call->arguments) {
                        VmValue v;
                        if (!ComptimeVM::evalConstExpr(arg.get(), v)) {
                            errorOut = "comptime call to '" + fi.name + "' requires constant arguments";
                            return false;
                        }
                        args.push_back(v);
                    }

                    std::string selErr;
                    auto mfn = isel.select(fi, selErr);
                    if (!mfn) {
                        errorOut = "could not compile comptime function '" + fi.name + "': " + selErr;
                        return false;
                    }
                    vm.addFunction(targetSymbol, std::move(mfn));

                    VmValue res = vm.execute(targetSymbol, args, errorOut);
                    if (!errorOut.empty()) return false;

                    Types::TypeRef retTy = sema.typeOf(call);
                    if (retTy && retTy->isFloat()) {
                        auto fl = std::make_shared<AST::FloatLiteral>();
                        fl->value = res.f;
                        fl->raw = std::to_string(res.f);
                        fl->range = call->range;
                        node = fl;
                    } else {
                        auto il = std::make_shared<AST::IntegerLiteral>();
                        il->value = Utilities::Int128(res.i);
                        il->raw = std::to_string(static_cast<int64_t>(res.i));
                        il->range = call->range;
                        node = il;
                    }
                    return true;
                }
            }
        }
        return true;
    }

    switch (node->nodeType()) {
        case AST::NodeType::VariableDeclaration: {
            auto* v = static_cast<AST::VariableDeclarationExpr*>(node.get());
            if (v->initialValue) {
                if (!foldExprInPlace(v->initialValue, sema, isel, vm, errorOut)) return false;
            }
            break;
        }
        case AST::NodeType::AssignmentExpr: {
            auto* a = static_cast<AST::AssignmentExpr*>(node.get());
            if (a->value && !foldExprInPlace(a->value, sema, isel, vm, errorOut)) return false;
            break;
        }
        case AST::NodeType::IfStatement: {
            auto* i = static_cast<AST::IfStatement*>(node.get());
            if (i->condition && !foldExprInPlace(i->condition, sema, isel, vm, errorOut)) return false;
            if (!foldNodeList(i->consequent, sema, isel, vm, errorOut)) return false;
            if (!foldNodeList(i->alternate, sema, isel, vm, errorOut)) return false;
            break;
        }
        case AST::NodeType::WhileLoop: {
            auto* w = static_cast<AST::WhileLoop*>(node.get());
            if (w->condition && !foldExprInPlace(w->condition, sema, isel, vm, errorOut)) return false;
            if (!foldNodeList(w->body, sema, isel, vm, errorOut)) return false;
            break;
        }
        case AST::NodeType::ReturnStatement: {
            auto* r = static_cast<AST::ReturnStatement*>(node.get());
            if (r->returnValue && !foldExprInPlace(r->returnValue, sema, isel, vm, errorOut)) return false;
            break;
        }
        case AST::NodeType::BinaryOperation: {
            auto* b = static_cast<AST::BinaryOperationExpr*>(node.get());
            if (b->lhs && !foldExprInPlace(b->lhs, sema, isel, vm, errorOut)) return false;
            if (b->rhs && !foldExprInPlace(b->rhs, sema, isel, vm, errorOut)) return false;
            break;
        }
        case AST::NodeType::FunctionDeclaration: {
            auto* f = static_cast<AST::FunctionDeclaration*>(node.get());
            if (f->isComptime) return true;
            if (!foldNodeList(f->body, sema, isel, vm, errorOut)) return false;
            break;
        }
        default:
            break;
    }
    return true;
}

} // namespace

bool ComptimeVM::foldComptimeCalls(AST::ProgramRoot& program, const Sema::SemaResult& sema,
                                   InstructionSelector& isel, std::string& errorOut) {
    if (sema.comptimeCalls.empty()) return true;

    ComptimeVM vm;
    return foldNodeList(program.body, sema, isel, vm, errorOut);
}

} // namespace Backend
