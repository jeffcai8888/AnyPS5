// Regression tests for the external scalar call fusion: compute shaders that call code whose
// address arrives in user data are detected, their callees decoded, and both are rewritten into
// one program with static branches only.
#include "ControlFlow/ExternalCallFuser.hpp"
#include "ControlFlow/GraphBuilder.hpp"
#include "ControlFlow/Structurizer.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ShaderRecompiler;
static void Require(bool value) { if (!value) throw std::runtime_error("external call fusion regression"); }

template <typename Fn>
static void ExpectThrow(const char* needle, Fn fn) {
    try {
        fn();
    } catch (const std::exception& error) {
        if (std::string(error.what()).find(needle) == std::string::npos) {
            throw std::runtime_error(std::string("wrong error for \"") + needle + "\": " + error.what());
        }
        return;
    }
    throw std::runtime_error(std::string("expected throw containing \"") + needle + "\"");
}

constexpr std::uint32_t SMovB32(std::uint32_t dst, std::uint32_t src) { return 0xbe800000u | (dst << 16u) | (0x03u << 8u) | src; }
constexpr std::uint32_t SMovB64(std::uint32_t dst, std::uint32_t src) { return 0xbe800000u | (dst << 16u) | (0x04u << 8u) | src; }
constexpr std::uint32_t SSwappcSelf(std::uint32_t reg) { return 0xbe800000u | (reg << 16u) | (0x21u << 8u) | reg; }
constexpr std::uint32_t SSetpc(std::uint32_t src) { return 0xbe800000u | (0x20u << 8u) | src; }
constexpr std::uint32_t SEndpgmWord = 0xbf810000u;
constexpr std::uint32_t SNopWord = 0xbf800000u;
constexpr std::uint32_t VMovB32(std::uint32_t dst, std::uint32_t src) { return 0x7e000000u | (dst << 17u) | (0x01u << 9u) | (0x100u | src); }

// The UE5 pattern: a target pair copied from user data, parameter movs, then a self-targeted
// swappc; two call sites, an epilogue and an opaque trailer after s_endpgm.
constexpr std::array<std::uint32_t, 13> Caller{
    SMovB32(14u, 4u),          // 0x00: s_mov_b32 s14, s4
    SMovB32(15u, 5u),          // 0x04: s_mov_b32 s15, s5
    SMovB32(16u, 6u),          // 0x08: parameter
    SMovB32(17u, 7u),          // 0x0c: parameter
    SSwappcSelf(14u),          // 0x10: s_swappc_b64 s[14:15], s[14:15]
    VMovB32(2u, 1u),           // 0x14: consumes the call result
    SMovB32(14u, 8u),          // 0x18
    SMovB32(15u, 9u),          // 0x1c
    SSwappcSelf(14u),          // 0x20
    VMovB32(3u, 2u),           // 0x24
    SEndpgmWord,               // 0x28
    0xdeadbeefu,               // trailer
    0xcafebabeu,
};

constexpr std::array<std::uint32_t, 3> CalleeA{
    VMovB32(4u, 2u),           // 0x00
    VMovB32(5u, 4u),           // 0x04
    SSetpc(14u),               // 0x08: return through the caller's link pair
};

constexpr std::array<std::uint32_t, 2> CalleeB{
    VMovB32(6u, 3u),           // 0x00
    SSetpc(14u),               // 0x04
};

static void CheckDetection() {
    const auto program = RdnaInstructionDecoder{}.Decode(Caller);
    const auto sites = AnalyzeExternalSwappcCalls(program, 0u, 16u);
    Require(sites.size() == 2u);
    Require(sites[0].copyLowProgramCounter == 0x00u && sites[0].copyHighProgramCounter == 0x04u && sites[0].callProgramCounter == 0x10u);
    Require(sites[0].targetRegister == 14u && sites[0].targetUserDataBase == 4u);
    Require(sites[1].copyLowProgramCounter == 0x18u && sites[1].copyHighProgramCounter == 0x1cu && sites[1].callProgramCounter == 0x20u);
    Require(sites[1].targetRegister == 14u && sites[1].targetUserDataBase == 8u);

    // A target pair outside user data, or copied from a non-user-data pair, is not external.
    const std::array<std::uint32_t, 6> local{
        SMovB32(14u, 4u),
        SMovB32(15u, 5u),
        SSwappcSelf(14u),
        VMovB32(2u, 1u),
        VMovB32(3u, 2u),
        SEndpgmWord,
    };
    Require(AnalyzeExternalSwappcCalls(RdnaInstructionDecoder{}.Decode(local), 8u, 8u).empty());

    // The positional fetch form (swappc straight out of user data, no copies) stays fetch.
    const std::array<std::uint32_t, 4> fetch{
        SSwappcSelf(4u),
        VMovB32(2u, 1u),
        VMovB32(3u, 2u),
        SEndpgmWord,
    };
    Require(AnalyzeExternalSwappcCalls(RdnaInstructionDecoder{}.Decode(fetch), 0u, 16u).empty());

    // s_mov_b64 copies the whole target pair in one instruction.
    const std::array<std::uint32_t, 5> wide{
        SMovB64(14u, 4u),
        SMovB32(16u, 6u),
        VMovB32(2u, 1u),
        SSwappcSelf(14u),
        SEndpgmWord,
    };
    const auto wideSites = AnalyzeExternalSwappcCalls(RdnaInstructionDecoder{}.Decode(wide), 0u, 16u);
    Require(wideSites.size() == 1u);
    Require(wideSites[0].copyLowProgramCounter == 0x00u && wideSites[0].copyHighProgramCounter == 0x00u);
    Require(wideSites[0].callProgramCounter == 0x0cu);
    Require(wideSites[0].targetRegister == 14u && wideSites[0].targetUserDataBase == 4u);
}

static void CheckCalleeDecoding() {
    const auto a = DecodeExternalCallee(CalleeA, 14u);
    Require(a.returnProgramCounter == 0x08u);
    Require(a.code.size() == 3u);
    const auto b = DecodeExternalCallee(CalleeB, 14u);
    Require(b.returnProgramCounter == 0x04u);

    ExpectThrow("no s_setpc_b64 return", [] { static_cast<void>(DecodeExternalCallee(std::array<std::uint32_t, 2>{VMovB32(4u, 2u), VMovB32(5u, 4u)}, 14u)); });
    ExpectThrow("ends the program", [] { static_cast<void>(DecodeExternalCallee(std::array<std::uint32_t, 2>{VMovB32(4u, 2u), SEndpgmWord}, 14u)); });
    ExpectThrow("returns through another register pair", [] { static_cast<void>(DecodeExternalCallee(std::array<std::uint32_t, 2>{VMovB32(4u, 2u), SSetpc(12u)}, 14u)); });
    ExpectThrow("nests a scalar call", [] {
        static_cast<void>(DecodeExternalCallee(std::array<std::uint32_t, 2>{VMovB32(4u, 2u), SSwappcSelf(14u)}, 14u));
    });
}

static std::vector<std::uint32_t> FusedCaller() {
    const auto program = RdnaInstructionDecoder{}.Decode(Caller);
    const auto sites = AnalyzeExternalSwappcCalls(program, 0u, 16u);
    const std::array<ExternalCallee, 2> callees{DecodeExternalCallee(CalleeA, 14u), DecodeExternalCallee(CalleeB, 14u)};
    return FuseExternalCalls(program, Caller, sites, callees);
}

static void CheckFusion() {
    const auto fused = FusedCaller();
    // 11 caller code words + 3 + 2 callee words + endpgm + 2 trailer words.
    Require(fused.size() == 19u);
    Require(fused[0] == SNopWord && fused[1] == SNopWord);
    Require(fused[2] == SMovB32(16u, 6u) && fused[3] == SMovB32(17u, 7u));
    Require(fused[4] == (0xbf820000u | 6u));   // call site 1 -> callee A at word 11
    Require(fused[5] == VMovB32(2u, 1u));
    Require(fused[6] == SNopWord && fused[7] == SNopWord);
    Require(fused[8] == (0xbf820000u | 5u));   // call site 2 -> callee B at word 14
    Require(fused[9] == VMovB32(3u, 2u));
    Require(fused[10] == (0xbf820000u | 5u));  // old s_endpgm -> new trailing s_endpgm at word 16
    Require(fused[11] == VMovB32(4u, 2u) && fused[12] == VMovB32(5u, 4u));
    Require(fused[13] == (0xbf820000u | 0xfff7u)); // callee A return -> word 5 (after call site 1)
    Require(fused[14] == VMovB32(6u, 3u));
    Require(fused[15] == (0xbf820000u | 0xfff9u)); // callee B return -> word 9 (after call site 2)
    Require(fused[16] == SEndpgmWord);
    Require(fused[17] == 0xdeadbeefu && fused[18] == 0xcafebabeu);
}

static void CheckFusedProgramCompiles() {
    const auto fused = FusedCaller();
    const auto program = RdnaInstructionDecoder{}.Decode(fused);
    Require(program.instructions.back().op == RdnaOpcode::SEndpgm);
    auto graph = GraphBuilder{}.Build(program);
    Structurizer{}.Structurize(graph);
    Require(graph.blocks.size() >= 4u);
}

static void CheckUnfusedCallerStillThrows() {
    const auto program = RdnaInstructionDecoder{}.Decode(Caller);
    SwappcInfo info;
    info.userDataBaseRegister = 0u;
    info.userDataCount = 16u;
    ExpectThrow("not statically resolvable", [&] { static_cast<void>(GraphBuilder{}.Build(program, &info)); });
}

int main() {
    try {
        CheckDetection();
        CheckCalleeDecoding();
        CheckFusion();
        CheckFusedProgramCompiles();
        CheckUnfusedCallerStillThrows();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "external call fusion regression: %s\n", error.what());
        return 1;
    }
    return 0;
}
