#include "ControlFlow/ExternalCallFuser.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "RdnaDecoder/RdnaOpcode.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace ShaderRecompiler {

namespace {

constexpr std::uint32_t NoInstruction = std::numeric_limits<std::uint32_t>::max();

std::string toHexString(std::uint32_t value) {
    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "0x%08x", value);
    return std::string(buffer);
}

constexpr std::uint32_t SNop = 0xbf800000u;
constexpr std::uint32_t SBranch = 0xbf820000u;
constexpr std::uint32_t SEndpgm = 0xbf810000u;

// The s_branch immediate counts words from the next instruction (branchTarget = pc + 4 + imm * 4).
std::uint32_t branchWord(std::int32_t targetWord, std::uint32_t branchWordIndex) {
    const auto delta = targetWord - static_cast<std::int32_t>(branchWordIndex) - 1;
    if (delta < -0x8000 || delta > 0x7fff) {
        throw std::invalid_argument("fused scalar branch distance " + std::to_string(delta) + " words exceeds the s_branch range");
    }
    return SBranch | static_cast<std::uint16_t>(delta);
}

bool coversScalarRegister(const RdnaInstruction& instruction, std::uint32_t reg) {
    const auto covers = [&](const RdnaOperand& operand) {
        return operand.kind == RdnaOperandKind::ScalarRegister && operand.reg <= reg && reg < operand.reg + instruction.dataDwordCount;
    };
    return covers(instruction.destination) || covers(instruction.destination2);
}

// The last writer of `reg` before `end` in program order, or NoInstruction when none exists. The
// scan crosses branches: the setup copies and the call can sit in different blocks, and a call
// site only functions when every path into it has loaded its target.
std::uint32_t lastWriterIndex(const RdnaProgram& program, std::uint32_t end, std::uint32_t reg) {
    for (std::uint32_t index = end; index-- > 0u;) {
        const auto& instruction = program.instructions[index];
        if (instruction.op == RdnaOpcode::SEndpgm || instruction.op == RdnaOpcode::SCodeEnd) break;
        if (coversScalarRegister(instruction, reg)) return index;
    }
    return NoInstruction;
}

}

std::vector<ExternalCallSite> AnalyzeExternalSwappcCalls(const RdnaProgram& program, std::uint32_t userDataBaseRegister, std::uint32_t userDataCount) {
    std::vector<ExternalCallSite> sites;
    for (std::uint32_t index = 0; index < program.instructions.size(); ++index) {
        const auto& swappc = program.instructions[index];
        if (swappc.op != RdnaOpcode::SSwappcB64) continue;
        if (swappc.source0.kind != RdnaOperandKind::ScalarRegister || swappc.destination.kind != RdnaOperandKind::ScalarRegister) continue;
        const std::uint32_t target = swappc.source0.reg;
        if (target % 2u != 0u || swappc.destination.reg != target) continue;
        const std::uint32_t lowWriter = lastWriterIndex(program, index, target);
        const std::uint32_t highWriter = lastWriterIndex(program, index, target + 1u);
        if (lowWriter == NoInstruction || highWriter == NoInstruction) continue;
        const auto& low = program.instructions[lowWriter];
        const auto& high = program.instructions[highWriter];
        if (lowWriter == highWriter) {
            // s_mov_b64 s[target:target+1], s[userLo:userLo+1]
            if (low.op != RdnaOpcode::SMovB64) continue;
            if (low.destination.kind != RdnaOperandKind::ScalarRegister || low.destination.reg != target || low.source0.kind != RdnaOperandKind::ScalarRegister) continue;
            if (low.source0.reg < userDataBaseRegister || low.source0.reg + 1u >= userDataBaseRegister + userDataCount) continue;
            ExternalCallSite site;
            site.copyLowProgramCounter = low.programCounter;
            site.copyHighProgramCounter = low.programCounter;
            site.callProgramCounter = swappc.programCounter;
            site.targetRegister = target;
            site.targetUserDataBase = low.source0.reg - userDataBaseRegister;
            sites.push_back(site);
            continue;
        }
        if (low.op != RdnaOpcode::SMovB32 || high.op != RdnaOpcode::SMovB32) continue;
        if (low.destination.kind != RdnaOperandKind::ScalarRegister || low.destination.reg != target || low.source0.kind != RdnaOperandKind::ScalarRegister) continue;
        if (high.destination.kind != RdnaOperandKind::ScalarRegister || high.destination.reg != target + 1u || high.source0.kind != RdnaOperandKind::ScalarRegister) continue;
        if (high.source0.reg != low.source0.reg + 1u) continue;
        if (low.source0.reg < userDataBaseRegister || low.source0.reg + 1u >= userDataBaseRegister + userDataCount) continue;
        ExternalCallSite site;
        site.copyLowProgramCounter = low.programCounter;
        site.copyHighProgramCounter = high.programCounter;
        site.callProgramCounter = swappc.programCounter;
        site.targetRegister = target;
        site.targetUserDataBase = low.source0.reg - userDataBaseRegister;
        sites.push_back(site);
    }
    return sites;
}

ExternalCallee DecodeExternalCallee(std::span<const std::uint32_t> code, std::uint32_t linkRegister) {
    ExternalCallee result;
    std::uint32_t furthestBranchWord = 0;
    std::uint32_t wordIndex = 0;
    while (wordIndex < code.size()) {
        const std::uint32_t programCounter = wordIndex * 4u;
        const RdnaInstruction instruction = DecodeRdnaInstruction(programCounter, code, wordIndex);
        wordIndex += instruction.wordCount;
        if (instruction.op == RdnaOpcode::SSetpcB64) {
            if (instruction.source0.kind != RdnaOperandKind::ScalarRegister || instruction.source0.reg != linkRegister) {
                throw std::invalid_argument("external callee returns through another register pair at program counter " + toHexString(programCounter));
            }
            if (furthestBranchWord > programCounter / 4u) {
                throw std::invalid_argument("external callee branches past its s_setpc_b64 return at program counter " + toHexString(programCounter));
            }
            result.returnProgramCounter = programCounter;
            result.code.assign(code.begin(), code.begin() + wordIndex);
            return result;
        }
        if (instruction.op == RdnaOpcode::SSwappcB64 || instruction.op == RdnaOpcode::SCallB64) {
            throw std::invalid_argument("external callee nests a scalar call at program counter " + toHexString(programCounter));
        }
        if (instruction.op == RdnaOpcode::SEndpgm || instruction.op == RdnaOpcode::SCodeEnd) {
            throw std::invalid_argument("external callee ends the program at program counter " + toHexString(programCounter) + " instead of returning");
        }
        if (IsDirectBranchOpcode(instruction.op)) {
            if ((instruction.branchTarget & 3u) != 0u || instruction.branchTarget / 4u >= code.size()) {
                throw std::invalid_argument("external callee branch at program counter " + toHexString(programCounter) + " escapes its code window");
            }
            furthestBranchWord = std::max(furthestBranchWord, instruction.branchTarget / 4u);
        }
    }
    throw std::out_of_range("external callee code window has no s_setpc_b64 return");
}

std::vector<std::uint32_t> FuseExternalCalls(const RdnaProgram& program, std::span<const std::uint32_t> code, std::span<const ExternalCallSite> sites, std::span<const ExternalCallee> callees) {
    if (program.instructions.empty()) throw std::invalid_argument("cannot fuse an empty program");
    if (sites.size() != callees.size()) throw std::invalid_argument("external callee count does not match the call sites");
    const auto& terminator = program.instructions.back();
    if (terminator.op != RdnaOpcode::SEndpgm && terminator.op != RdnaOpcode::SCodeEnd) {
        throw std::invalid_argument("fused program does not end with s_endpgm");
    }
    const std::uint32_t terminatorWord = terminator.programCounter / 4u;
    const std::uint32_t callerCodeWords = terminatorWord + terminator.wordCount;

    const auto instructionWordCountAt = [&](std::uint32_t programCounter) {
        for (const auto& instruction : program.instructions) {
            if (instruction.programCounter == programCounter) return instruction.wordCount;
        }
        throw std::invalid_argument("fused rewrite target " + toHexString(programCounter) + " is not an instruction boundary");
    };
    for (std::size_t index = 0; index < sites.size(); ++index) {
        const auto& site = sites[index];
        if (instructionWordCountAt(site.copyLowProgramCounter) != 1u || instructionWordCountAt(site.copyHighProgramCounter) != 1u ||
            instructionWordCountAt(site.callProgramCounter) != 1u) {
            throw std::invalid_argument("external call at program counter " + toHexString(site.callProgramCounter) + " does not fit the fused rewrite");
        }
        if (site.callProgramCounter / 4u >= callerCodeWords) {
            throw std::invalid_argument("external call at program counter " + toHexString(site.callProgramCounter) + " is not inside the caller code");
        }
    }

    std::vector<std::uint32_t> calleeWordOffsets(callees.size());
    std::uint32_t cursor = callerCodeWords;
    for (std::size_t index = 0; index < callees.size(); ++index) {
        calleeWordOffsets[index] = cursor;
        cursor += static_cast<std::uint32_t>(callees[index].code.size());
    }
    const std::uint32_t finalEndpgmWord = cursor;
    cursor += 1u;

    std::vector<std::uint32_t> fused;
    fused.reserve(cursor + (code.size() - callerCodeWords));
    for (std::uint32_t word = 0; word < callerCodeWords; ++word) {
        const std::uint32_t programCounter = word * 4u;
        if (programCounter == terminator.programCounter) {
            fused.push_back(branchWord(static_cast<std::int32_t>(finalEndpgmWord), word));
            continue;
        }
        std::uint32_t value = code[word];
        for (std::size_t index = 0; index < sites.size(); ++index) {
            const auto& site = sites[index];
            if (programCounter == site.copyLowProgramCounter || programCounter == site.copyHighProgramCounter) {
                value = SNop;
            } else if (programCounter == site.callProgramCounter) {
                value = branchWord(static_cast<std::int32_t>(calleeWordOffsets[index]), word);
            }
        }
        fused.push_back(value);
    }
    for (std::size_t index = 0; index < callees.size(); ++index) {
        const auto& site = sites[index];
        const auto& callee = callees[index];
        for (std::uint32_t word = 0; word < callee.code.size(); ++word) {
            if (word * 4u == callee.returnProgramCounter) {
                fused.push_back(branchWord(static_cast<std::int32_t>(site.callProgramCounter / 4u + 1u), word + calleeWordOffsets[index]));
            } else {
                fused.push_back(callee.code[word]);
            }
        }
    }
    fused.push_back(SEndpgm);
    fused.insert(fused.end(), code.begin() + callerCodeWords, code.end());
    return fused;
}

}
