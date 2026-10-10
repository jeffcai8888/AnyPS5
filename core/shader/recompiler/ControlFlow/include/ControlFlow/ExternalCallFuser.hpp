#ifndef CORE_SHADER_RECOMPILIER_CONTROLFLOW_INCLUDE_CONTROLFLOW_EXTERNALCALLFUSER_HPP
#define CORE_SHADER_RECOMPILIER_CONTROLFLOW_INCLUDE_CONTROLFLOW_EXTERNALCALLFUSER_HPP

#include "RdnaDecoder/RdnaProgram.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace ShaderRecompiler {

struct ExternalCallSite {
    std::uint32_t copyLowProgramCounter = 0;
    std::uint32_t copyHighProgramCounter = 0;
    std::uint32_t callProgramCounter = 0;
    std::uint32_t targetRegister = 0;
    std::uint32_t targetUserDataBase = 0;
};

struct ExternalCallee {
    std::vector<std::uint32_t> code;
    std::uint32_t returnProgramCounter = 0;
};

[[nodiscard]] std::vector<ExternalCallSite> AnalyzeExternalSwappcCalls(const RdnaProgram& program, std::uint32_t userDataBaseRegister, std::uint32_t userDataCount);

[[nodiscard]] ExternalCallee DecodeExternalCallee(std::span<const std::uint32_t> code, std::uint32_t linkRegister);

[[nodiscard]] std::vector<std::uint32_t> FuseExternalCalls(const RdnaProgram& program, std::span<const std::uint32_t> code, std::span<const ExternalCallSite> sites, std::span<const ExternalCallee> callees);

}

#endif
