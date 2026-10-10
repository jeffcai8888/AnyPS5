#include "prx/libc/include/general/LogMacros.hpp"
#include "ControlFlow/GraphBuilder.hpp"
#include "ControlFlow/ExternalCallFuser.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "ControlFlow/Structurizer.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "CacheKey.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderPreparation.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderPreparationScope.hpp"
#include "CompiledVariant.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <list>
#include <mutex>
#include <set>
#include <stdexcept>
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"

namespace AgcDriver::DriverDetail {

std::shared_ptr<const ShaderSnapshot> ReadRawComputeShader(std::uint64_t address) {
    GuestMemory::CheckRange(reinterpret_cast<const void*>(address), sizeof(std::uint32_t), 256);
    static std::mutex cacheMutex;
    static std::list<std::shared_ptr<const ShaderSnapshot>> cache;
    static std::size_t cacheBytes = 0;
    std::shared_ptr<const ShaderSnapshot> cached;
    {
        std::lock_guard lock(cacheMutex);
        const auto found = std::find_if(cache.begin(), cache.end(), [address](const auto& entry) { return entry->codeAddress == address; });
        if (found != cache.end()) cached = *found;
    }
    if (cached) {
        const auto code = std::as_bytes(std::span(cached->code));
        GuestMemory::FlushGpuWrites(address, code.size());
        if (GuestMemory::CompareMapped(address, code) == GuestMemory::Compare::Equal) {
            std::lock_guard lock(cacheMutex);
            const auto found = std::find(cache.begin(), cache.end(), cached);
            if (found != cache.end()) cache.splice(cache.begin(), cache, found);
            return cached;
        }
    }
    constexpr std::size_t limit = 1024 * 1024;
    const auto ranges = GuestMemory::CommittedRanges(address, limit);
    std::uint64_t end = address;
    for (const auto& range : ranges) {
        if (range.first != end) break;
        end = range.second;
    }
    const auto available = static_cast<std::size_t>(end - address) / sizeof(std::uint32_t);
    ShaderSnapshot snapshot{address, 0, 0, {}, {}};
    while (snapshot.code.size() < available) {
        const auto previous = snapshot.code.size();
        snapshot.code.resize(std::min(available, std::max<std::size_t>(64, previous * 2)));
        GuestMemory::Read(address + previous * sizeof(std::uint32_t),
            std::as_writable_bytes(std::span(snapshot.code).subspan(previous)), alignof(std::uint32_t));
        try {
            const auto decoded = ShaderRecompiler::RdnaInstructionDecoder{}.Decode(snapshot.code);
            const auto& last = decoded.instructions.back();
            snapshot.code.resize(last.programCounter / sizeof(std::uint32_t) + last.wordCount);
            auto result = std::make_shared<const ShaderSnapshot>(std::move(snapshot));
            std::lock_guard lock(cacheMutex);
            const auto found = std::find_if(cache.begin(), cache.end(), [address](const auto& entry) { return entry->codeAddress == address; });
            if (found != cache.end()) {
                if ((*found)->code == result->code) {
                    result = *found;
                    cache.splice(cache.begin(), cache, found);
                    return result;
                }
                cacheBytes -= (*found)->code.size() * sizeof(std::uint32_t);
                cache.erase(found);
            }
            const auto bytes = result->code.size() * sizeof(std::uint32_t);
            while (!cache.empty() && (cache.size() >= 64 || cacheBytes + bytes > 8 * 1024 * 1024)) {
                cacheBytes -= cache.back()->code.size() * sizeof(std::uint32_t);
                cache.pop_back();
            }
            cache.push_front(result);
            cacheBytes += bytes;
            return result;
        } catch (const std::out_of_range&) {
            if (snapshot.code.size() == available) break;
        }
    }
    throw std::runtime_error("AGC driver: raw compute program has no reachable end within mapped code or the size limit");
}


struct ShaderPreparationTransaction::State {
    struct Change {
        std::shared_ptr<PreparedShaders> destination;
        PreparedShaderState prepared;
    };
    static std::mutex writers;
    static thread_local State* active;
    std::unique_lock<std::mutex> lock{writers};
    std::map<PreparedShaders*, Change> changes;
    bool failed = false;
};

std::mutex ShaderPreparationTransaction::State::writers;
thread_local ShaderPreparationTransaction::State* ShaderPreparationTransaction::State::active = nullptr;

ShaderPreparationTransaction::ShaderPreparationTransaction() {
    if (State::active == nullptr) {
        state = std::make_unique<State>();
        State::active = state.get();
    }
    root = State::active;
}

ShaderPreparationTransaction::~ShaderPreparationTransaction() {
    if (!committed) root->failed = true;
    if (state != nullptr) State::active = nullptr;
}

PreparedShaderState& ShaderPreparationTransaction::Edit(const ShaderSnapshot& snapshot) {
    require(!committed, "shader preparation transaction is already committed");
    auto& changes = root->changes;
    const auto found = changes.find(snapshot.prepared.get());
    if (found != changes.end()) return found->second.prepared;
    std::lock_guard lock(snapshot.prepared->mutex);
    return changes.emplace(snapshot.prepared.get(), State::Change{snapshot.prepared, *snapshot.prepared}).first->second.prepared;
}

const PreparedShaderState& ShaderPreparationTransaction::Read(const ShaderSnapshot& snapshot) const {
    require(!committed, "shader preparation transaction is already committed");
    const auto found = root->changes.find(snapshot.prepared.get());
    return found != root->changes.end() ? found->second.prepared : *snapshot.prepared;
}

void ShaderPreparationTransaction::Commit() {
    require(!committed && !root->failed, "shader preparation transaction was aborted");
    if (state != nullptr) {
        std::vector<std::unique_lock<std::mutex>> locks;
        locks.reserve(state->changes.size());
        for (const auto& [key, change] : state->changes) locks.emplace_back(change.destination->mutex);
        for (auto& [key, change] : state->changes) {
            auto& destination = *change.destination;
            destination.entries.swap(change.prepared.entries);
            destination.registeredAbis.swap(change.prepared.registeredAbis);
            destination.graphicsAbis.swap(change.prepared.graphicsAbis);
            destination.rectangles.swap(change.prepared.rectangles);
            destination.rectangleProgress.swap(change.prepared.rectangleProgress);
            destination.fragments.swap(change.prepared.fragments);
            std::swap(destination.rectangleRequested, change.prepared.rectangleRequested);
        }
    }
    committed = true;
}

void PublishRegisteredShader(std::shared_ptr<ShaderRegistry>& registry, const std::shared_ptr<const ShaderSnapshot>& snapshot) {
    ShaderPreparationTransaction transaction;
    if (registry != nullptr) {
        const auto found = registry->find(snapshot->codeAddress);
        if (found != registry->end()) {
            const auto& current = *found->second;
            if (current.headerAddress == snapshot->headerAddress && current.type == snapshot->type && current.code == snapshot->code && current.header == snapshot->header) {
                transaction.Commit();
                return;
            }
            std::fprintf(stderr, "[gpu] code 0x%llx re-registered: header 0x%llx type %u -> header 0x%llx type %u\n",
                static_cast<unsigned long long>(snapshot->codeAddress), static_cast<unsigned long long>(current.headerAddress), current.type,
                static_cast<unsigned long long>(snapshot->headerAddress), snapshot->type);
        }
    }
    auto next = registry != nullptr ? std::make_shared<ShaderRegistry>(*registry) : std::make_shared<ShaderRegistry>();
    next->insert_or_assign(snapshot->codeAddress, snapshot);
    registry = std::move(next);
    transaction.Commit();
}

std::shared_ptr<const ShaderRecompiler::SourceHandle> SourceHandleFor(const ShaderSnapshot& snapshot, std::size_t codeOffset, const ShaderRecompiler::RecompileRequest& request) {
    struct PreparedKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, PreparedKeyStorage>();
    ShaderRecompiler::BuildPreparedShaderKey(request, key);
    std::lock_guard lock(snapshot.prepared->mutex);
    for (const auto& entry : snapshot.prepared->entries) {
        if (entry.codeOffset == codeOffset && ShaderRecompiler::MatchesPreparedShader(request, *entry.handle, key)) return entry.handle;
    }
    if (snapshot.header.empty()) {
        if (snapshot.type != 0 || request.shader.stage != ShaderRecompiler::ShaderStage::Compute) throw std::runtime_error("AGC driver: unregistered program is not a compute shader");
        APS5_LOG_ERR("Compute shader 0x%llx was not registered; preparing its artifact at dispatch", static_cast<unsigned long long>(snapshot.codeAddress));
        auto handle = ShaderRecompiler::PrepareShader(request);
        snapshot.prepared->entries.push_back({codeOffset, handle});
        return handle;
    }
    std::string layouts;
    for (const auto& entry : snapshot.prepared->entries) {
        if (entry.codeOffset != codeOffset || entry.handle == nullptr || entry.handle->artifact == nullptr) continue;
        const auto& layout = entry.handle->artifact->layout;
        layouts += " [" + std::to_string(layout.pushConstantOffsetBytes) + "," + std::to_string(layout.pushConstantSizeBytes) + "]";
    }
    throw std::runtime_error("AGC driver: prepared shader artifact is missing for the requested static ABI: address=" + std::to_string(request.shader.codeAddress) + " stage=" + std::to_string(static_cast<std::uint32_t>(request.shader.stage)) + " wave=" + std::to_string(request.context.waveSize) + " pushOffset=" + std::to_string(request.layout.pushConstantOffsetBytes) + " pushCapacity=" + std::to_string(request.layout.pushConstantSizeBytes) + " preparedLayouts=" + layouts);
}

ShaderRecompiler::RectListShaders PreparedRectangle(const ShaderSnapshot& snapshot, std::uint64_t vertexId, std::uint64_t fragmentId) {
    std::lock_guard lock(snapshot.prepared->mutex);
    for (const auto& entry : snapshot.prepared->rectangles) {
        if (entry.vertexId == vertexId && entry.fragmentId == fragmentId) {
            require(entry.shaders != nullptr, "prepared rectangle artifact is missing");
            return *entry.shaders;
        }
    }
    throw std::runtime_error("AGC driver: prepared rectangle artifacts are missing");
}

// Reads up to `limit` bytes of guest shader code at `address`, shrinking the window until it fits
// mapped memory. Program addresses sometimes travel shifted by 8 (the SPI register encoding), so
// both forms are probed. Returns nothing when neither is readable.
std::optional<std::vector<std::uint32_t>> ReadGuestCodeWindow(std::uint64_t address, std::size_t limit) {
    for (const std::uint64_t candidate : {address, address << 8u}) {
        if ((candidate & 3u) != 0u) continue;
        for (std::size_t bytes = limit; bytes >= 256u; bytes /= 2u) {
            std::vector<std::byte> buffer(bytes);
            if (GuestMemory::CopyMapped(candidate, buffer) != GuestMemory::Compare::Equal) continue;
            GuestMemory::FlushGpuWrites(candidate, bytes);
            if (GuestMemory::CopyMapped(candidate, buffer) != GuestMemory::Compare::Equal) break;
            std::vector<std::uint32_t> words(bytes / 4u);
            std::memcpy(words.data(), buffer.data(), bytes);
            return words;
        }
    }
    return std::nullopt;
}

// Fuses the external scalar calls of a registered compute shader for one dispatch: every call site
// whose target address arrives in user data becomes a static branch into a private copy of the
// callee code, and the callee return branches back. Returns false when the shader has no such
// calls; throws when it does but the callees cannot be resolved.
bool FuseExternalCallsForDispatch(const ShaderSnapshot& snapshot, const ShaderRecompiler::RecompileRequest& request, std::vector<std::uint32_t>& fused) {
    if (snapshot.type != 0 || request.shader.stage != ShaderRecompiler::ShaderStage::Compute) return false;
    ShaderRecompiler::RdnaProgram decoded;
    std::vector<ShaderRecompiler::ExternalCallSite> sites;
    try {
        decoded = ShaderRecompiler::RdnaInstructionDecoder{}.Decode(request.shader.code);
        sites = ShaderRecompiler::AnalyzeExternalSwappcCalls(decoded, request.context.userDataBaseRegister, static_cast<std::uint32_t>(request.context.userData.size()));
    } catch (const std::exception&) {
        // The compile below reports decode failures with its request dump attached.
        return false;
    }
    if (sites.empty()) return false;
    std::vector<ShaderRecompiler::ExternalCallee> callees;
    callees.reserve(sites.size());
    for (const auto& site : sites) {
        const auto& userData = request.context.userData;
        if (site.targetUserDataBase + 1u >= userData.size()) {
            throw std::runtime_error("AGC driver: external scalar call reads its target outside the dispatched user data");
        }
        const auto target = static_cast<std::uint64_t>(userData[site.targetUserDataBase]) | (static_cast<std::uint64_t>(userData[site.targetUserDataBase + 1u]) << 32u);
        auto window = ReadGuestCodeWindow(target, 16u * 1024u);
        if (!window.has_value()) {
            char text[96];
            std::snprintf(text, sizeof(text), "AGC driver: external scalar call target 0x%llx has no readable guest code", static_cast<unsigned long long>(target));
            throw std::runtime_error(text);
        }
        callees.push_back(ShaderRecompiler::DecodeExternalCallee(*window, site.targetRegister));
    }
    fused = ShaderRecompiler::FuseExternalCalls(decoded, request.shader.code, sites, callees);
    static std::mutex reportedMutex;
    static std::set<std::pair<std::uint64_t, std::size_t>> reported;
    std::lock_guard lock(reportedMutex);
    if (reported.insert({request.shader.codeAddress, fused.size()}).second) {
        std::fprintf(stderr, "[gpu] fused %zu external scalar calls of compute shader 0x%llx into %zu code words\n", sites.size(), static_cast<unsigned long long>(request.shader.codeAddress), fused.size());
    }
    return true;
}

ShaderRecompiler::PreparedShaderInvocation InvocationFor(const ShaderSnapshot& snapshot, std::size_t codeOffset, const ShaderRecompiler::RecompileRequest& request) {
    require(codeOffset < snapshot.code.size(), "prepared shader code offset is outside the snapshot");
    const auto code = std::span(snapshot.code).subspan(codeOffset);
    auto invocationRequest = request;
    std::vector<std::uint32_t> fusedCode;
    const bool fused = FuseExternalCallsForDispatch(snapshot, request, fusedCode);
    if (fused) {
        invocationRequest.shader.code = fusedCode;
    } else {
        require(request.shader.code.data() == code.data() && request.shader.code.size() == code.size(), "prepared invocation does not refer to registered code");
    }
    struct PreparedKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, PreparedKeyStorage>();
    ShaderRecompiler::BuildPreparedShaderKey(request, key);
    std::lock_guard lock(snapshot.prepared->mutex);
    for (const auto& entry : snapshot.prepared->entries) {
        if (entry.codeOffset != codeOffset) continue;
        const auto& entryCode = ShaderRecompiler::GetPreparedCode(*entry.handle);
        if (fused && (entryCode.size() != fusedCode.size() || !std::ranges::equal(entryCode, fusedCode))) continue;
        invocationRequest.shader.code = entryCode;
        if (auto invocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(invocationRequest, entry.handle, key)) return std::move(*invocation);
    }
    if (fused || snapshot.header.empty()) {
        if (!fused && (snapshot.type != 0 || request.shader.stage != ShaderRecompiler::ShaderStage::Compute)) throw std::runtime_error("AGC driver: unregistered program is not a compute shader");
        APS5_LOG_ERR("Compute shader 0x%llx has no prepared artifact; preparing it at dispatch", static_cast<unsigned long long>(snapshot.codeAddress));
        auto handle = ShaderRecompiler::PrepareShader(invocationRequest);
        invocationRequest.shader.code = ShaderRecompiler::GetPreparedCode(*handle);
        auto invocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(invocationRequest, handle, key);
        if (!invocation.has_value()) throw std::runtime_error("AGC driver: raw compute artifact does not match its invocation");
        snapshot.prepared->entries.push_back({codeOffset, std::move(handle)});
        return std::move(*invocation);
    }
    std::string layouts;
    for (const auto& entry : snapshot.prepared->entries) {
        if (entry.codeOffset != codeOffset || entry.handle == nullptr || entry.handle->artifact == nullptr) continue;
        const auto& layout = entry.handle->artifact->layout;
        layouts += " [" + std::to_string(layout.pushConstantOffsetBytes) + "," + std::to_string(layout.pushConstantSizeBytes) + "]";
    }
    throw std::runtime_error("AGC driver: prepared shader artifact is missing for the requested static ABI: address=" + std::to_string(request.shader.codeAddress) + " stage=" + std::to_string(static_cast<std::uint32_t>(request.shader.stage)) + " wave=" + std::to_string(request.context.waveSize) + " pushOffset=" + std::to_string(request.layout.pushConstantOffsetBytes) + " pushCapacity=" + std::to_string(request.layout.pushConstantSizeBytes) + " preparedLayouts=" + layouts);
}
std::optional<ShaderRecompiler::ShaderFloatMode> RegisteredFloatMode(const ShaderSnapshot& snapshot) {
    if (snapshot.registeredState == nullptr) return std::nullopt;
    std::uint32_t rsrc1;
    std::uint32_t fp16OverflowBit;
    switch (snapshot.type) {
    case 0: rsrc1 = 0x212; fp16OverflowBit = 26; break;
    case 1: rsrc1 = 0x00a; fp16OverflowBit = 29; break;
    case 2: case 4: case 6: rsrc1 = 0x08a; fp16OverflowBit = 31; break;
    case 5: case 7: rsrc1 = 0x10a; fp16OverflowBit = 30; break;
    default: return std::nullopt;
    }
    const auto found = snapshot.registeredState->shader.find(rsrc1);
    if (found == snapshot.registeredState->shader.end()) return std::nullopt;
    const auto value = found->second;
    return ShaderRecompiler::ShaderFloatMode{(value >> 12u) & 0xffu, ((value >> 21u) & 1u) != 0u, ((value >> 23u) & 1u) != 0u, ((value >> fp16OverflowBit) & 1u) != 0u};
}

namespace {

template<typename TValue>
std::vector<TValue> ReadHeaderArray(const ShaderSnapshot& snapshot, const TValue* pointer, std::size_t count) {
    if (count == 0) return {};
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (address < snapshot.headerAddress || address - snapshot.headerAddress > snapshot.header.size()) throw std::runtime_error("AGC driver: shader metadata is outside the registered header");
    const auto offset = static_cast<std::size_t>(address - snapshot.headerAddress);
    if (count > (snapshot.header.size() - offset) / sizeof(TValue)) throw std::runtime_error("AGC driver: truncated shader metadata");
    std::vector<TValue> result(count);
    std::memcpy(result.data(), snapshot.header.data() + offset, count * sizeof(TValue));
    return result;
}

Shader ReadHeader(const ShaderSnapshot& snapshot) {
    Shader header;
    std::memcpy(&header, snapshot.header.data(), sizeof(header));
    return header;
}

RegisteredShaderState DecodeRegisteredState(const ShaderSnapshot& snapshot) {
    const auto header = ReadHeader(snapshot);
    RegisteredShaderState state{{}, InitialContextRegisters(), {{0x24a, 0}, {0x24b, 0}}};
    for (const auto reg : ReadHeaderArray(snapshot, header.sh_registers, header.num_sh_registers)) {
        state.shader.insert_or_assign(reg.offset, reg.value);
    }
    for (const auto reg : ReadHeaderArray(snapshot, header.cx_registers, header.num_cx_registers)) {
        state.context.insert_or_assign(reg.offset, reg.value);
    }
    if (header.specials != nullptr) {
        const auto special = ReadHeaderArray(snapshot, header.specials, 1).front();
        state.context[special.vgt_shader_stages_en.offset] = special.vgt_shader_stages_en.value;
        state.context[special.vgt_gs_out_prim_type.offset] = special.vgt_gs_out_prim_type.value;
        state.userConfig[special.ge_cntl.offset] = special.ge_cntl.value;
        state.userConfig[special.ge_user_vgpr_en.offset] = special.ge_user_vgpr_en.value;
    }
    return state;
}

std::uint32_t RegisterValue(const Registers& registers, std::uint32_t offset) {
    const auto found = registers.find(offset);
    if (found == registers.end()) throw std::runtime_error("AGC driver: missing static shader ABI register " + std::to_string(offset));
    return found->second;
}

void BuildRegisteredAbiKey(const QueueState& state, const VulkanDevice& device, std::vector<std::uint64_t>& key) {
    key.clear();
    key.reserve(7u + state.shader.size() + state.context.size() + state.userConfig.size());
    key.insert(key.end(), {device.Serial(), ShaderRecompiler::DebugProbeActive(), ShaderRecompiler::RayTracingStrict(), ShaderRecompiler::RayTracingMiss()});
    for (const auto* registers : {&state.shader, &state.context, &state.userConfig}) {
        key.push_back(registers->size());
        for (const auto& [offset, value] : *registers) key.push_back((static_cast<std::uint64_t>(offset) << 32u) | value);
    }
}

std::vector<PreparedShaders::Entry> PrepareRegistered(const ShaderSnapshot& snapshot, const VulkanDevice& device, const QueueState& state, bool registration) {
    using Stage = ShaderRecompiler::ShaderStage;
    const auto header = ReadHeader(snapshot);
    std::uint32_t programRegister;
    std::uint32_t resourceRegister;
    std::uint32_t firstUser = 0;
    Stage stage;
    try {
        switch (snapshot.type) {
    case 0: stage = Stage::Compute; programRegister = 0x20c; resourceRegister = 0x213; break;
    case 1: stage = Stage::Fragment; programRegister = 0x008; resourceRegister = 0x00b; break;
    case 2: stage = Stage::Vertex; programRegister = 0x0c8; resourceRegister = 0x08b; firstUser = 8; break;
    case 4: stage = Stage::Mesh; programRegister = 0x0c8; resourceRegister = 0x08b; break;
    case 5: stage = Stage::Local; programRegister = 0x148; resourceRegister = 0x10b; firstUser = 8; break;
    case 6: stage = Stage::Mesh; programRegister = 0x088; resourceRegister = 0x08b; break;
    case 7: stage = Stage::TessellationControl; programRegister = 0x108; resourceRegister = 0x10b; break;
    default: {
        // Unknown shader types cannot be prepared, but failing registration outright also takes
        // down every later stage; leave the artifact to fail loudly if the shader is ever used.
        static std::mutex unknownMutex;
        static std::set<std::pair<std::uint8_t, std::uint64_t>> unknown;
        std::lock_guard lock(unknownMutex);
        if (unknown.insert({snapshot.type, snapshot.codeAddress}).second) {
            char words[96] = "";
            for (std::size_t i = 0; i < snapshot.code.size() && i < 8u; ++i) {
                char word[12];
                std::snprintf(word, sizeof(word), "%s%08x", i != 0u ? " " : "", snapshot.code[i]);
                std::strncat(words, word, sizeof(words) - std::strlen(words) - 1);
            }
            std::fprintf(stderr, "[gpu] registered shader 0x%llx has unsupported type %u; leaving it unprepared (first words: %s)\n", static_cast<unsigned long long>(snapshot.codeAddress), snapshot.type, words);
        }
        return {};
    }
    }
    if ((snapshot.type == 4u || snapshot.type == 5u) && registration && !state.shader.contains(programRegister + 1u)) {
        // Geometry and hull front halves carry no program address pair of their own (the queue
        // binds them at draw time); their artifacts are prepared through the graphics ABI.
        static std::mutex frontMutex;
        static std::set<std::uint64_t> fronts;
        std::lock_guard lock(frontMutex);
        if (fronts.insert(snapshot.codeAddress).second) {
            std::fprintf(stderr, "[gpu] front half shader 0x%llx type %u has no program address pair; deferring its artifact to the graphics ABI\n", static_cast<unsigned long long>(snapshot.codeAddress), snapshot.type);
        }
        return {};
    }
    const auto high = RegisterValue(state.shader, programRegister + 1);
    if ((high & ~0xffu) != 0) throw std::runtime_error("AGC driver: invalid registered program address");
    const auto address = (static_cast<std::uint64_t>(RegisterValue(state.shader, programRegister)) << 8u) | (static_cast<std::uint64_t>(high) << 40u);
    if (address < snapshot.codeAddress || address - snapshot.codeAddress >= snapshot.code.size() * 4u) throw std::runtime_error("AGC driver: registered entry point is outside shader code");
    const auto codeOffset = static_cast<std::size_t>((address - snapshot.codeAddress) / 4u);
    const auto code = std::span(snapshot.code).subspan(codeOffset);
    const auto decoded = ShaderRecompiler::RdnaInstructionDecoder{}.Decode(code);
    std::optional<ShaderRecompiler::ShaderComputeStageInfo> compute;
    std::optional<ShaderRecompiler::ShaderPixelStageInfo> pixel;
    std::optional<ShaderRecompiler::ShaderVertexStageInfo> vertex;
    std::optional<ShaderRecompiler::GraphicsCompileContext> graphics;
    std::uint32_t wave;
    const auto resources = RegisterValue(state.shader, resourceRegister);
    auto userCount = (resources >> 1u) & 0x1fu;
    if (stage != Stage::Compute) userCount |= ((resources >> 27u) & 1u) << 5u;
    if (userCount > 32) throw std::runtime_error("AGC driver: static user SGPR count exceeds the register bank");
    if (stage == Stage::Compute) {
        compute = Graphics::DecodeComputeStageInfo(state.shader, snapshot.header);
        const auto special = ReadHeaderArray(snapshot, header.specials, 1).front();
        wave = (special.dispatch_modifier & 0x8000u) != 0 ? 32u : 64u;
    } else if (stage == Stage::Fragment) {
        const auto count = RegisterValue(state.context, 0x1b6) & 0x3fu;
        if (registration && count != 0) return {};
        pixel = Graphics::DecodePixelStageInfo(state.context, {});
        wave = pixel->wave32 ? 32u : 64u;
    } else {
        const auto routing = RegisterValue(state.context, 0x2d5);
        wave = (routing & 0x00400000u) != 0 ? 32u : 64u;
        if ((routing & 0x20u) != 0 || stage == Stage::Mesh || (routing & 4u) != 0) {
            if (registration) return {};
            auto stageState = state;
            stageState.context[0x1b6] = 0;
            const auto stages = Graphics::DecodeShaderStages(stageState);
            graphics = ShaderRecompiler::GraphicsCompileContext{0, {}, stages.mesh, stages.tessellation, {}};
            if (stages.mesh) { stage = Stage::Mesh; firstUser = 0; }
            if (stages.tessellation && snapshot.type == 2) stage = Stage::TessellationEvaluation;
        }
        if (firstUser == 0) userCount += 8;
    }
    std::vector<std::uint32_t> userData(userCount);
    if (stage != Stage::Compute && stage != Stage::Fragment && snapshot.type != 6) vertex = Graphics::DecodeVertexStageInfo(snapshot.header, snapshot.headerAddress, userData, nullptr, true);
    const ShaderRecompiler::SwappcInfo swappc{vertex.has_value(), firstUser, userCount};
    // Shaders that call code addressed through user data can only be compiled per dispatch, when
    // the targets are known. Registration defers them; InvocationFor fuses the callees in.
    if (stage == Stage::Compute && !ShaderRecompiler::AnalyzeExternalSwappcCalls(decoded, firstUser, userCount).empty()) {
        static std::mutex deferredMutex;
        static std::set<std::uint64_t> deferred;
        std::lock_guard lock(deferredMutex);
        if (deferred.insert(address).second) {
            std::fprintf(stderr, "[gpu] compute shader 0x%llx calls code addressed through user data; deferring its artifact to dispatch\n", static_cast<unsigned long long>(address));
        }
        return {};
    }
    auto graph = [&] {
        try {
            return ShaderRecompiler::GraphBuilder{}.Build(decoded, &swappc);
        } catch (const std::exception& error) {
            if (std::getenv("APS5_DUMP_SHADERS") != nullptr) {
                char name[64];
                std::snprintf(name, sizeof(name), "shader_fail_%llx_pre.req", static_cast<unsigned long long>(address));
                try {
                    const std::vector<ShaderRecompiler::MemoryRegion> noMemory;
                    const ShaderRecompiler::RecompileRequest failed{{stage, address, code, snapshot.headerAddress, snapshot.header}, {wave, firstUser, userData, compute, pixel, vertex, noMemory, RegisteredFloatMode(snapshot)}, stage == Stage::Compute ? device.ComputeTarget(wave) : device.Target(), {0, 0, 0, 128}, graphics};
                    const auto text = ShaderRecompiler::RequestSerializer{}.Serialize(failed);
                    if (std::FILE* file = std::fopen(name, "wb")) {
                        std::fwrite(text.data(), 1, text.size(), file);
                        std::fclose(file);
                    }
                } catch (...) {
                }
                std::fprintf(stderr, "[gpu] registered shader 0x%llx failed control flow analysis (%s), request dumped to %s\n", static_cast<unsigned long long>(address), error.what(), name);
            }
            throw;
        }
    }();
    ShaderRecompiler::Structurizer{}.Structurize(graph);
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}, {snapshot.headerAddress, snapshot.header}}};
    ShaderRecompiler::RecompileRequest request{{stage, address, code, snapshot.headerAddress, snapshot.header}, {wave, firstUser, userData, compute, pixel, vertex, memory, RegisteredFloatMode(snapshot)}, stage == Stage::Compute ? device.ComputeTarget(wave) : device.Target(), {0, 0, 0, 128}, graphics};
    if (graphics && graphics->mesh) request.layout.pushConstantSizeBytes = ShaderRecompiler::MeshDrawPushOffsetBytes;
    std::vector<PreparedShaders::Entry> entries;
    const auto append = [&] {
        PerformanceTimer timing("Shader.PrepareArtifact");
        entries.push_back({codeOffset, ShaderRecompiler::PrepareShader(request)});
    };
    append();
    if (compute) {
        request.context.compute->partialThreads = {1, 1, 1};
        append();
    } else if (pixel) {
        request.layout.pushConstantSizeBytes = 0;
        append();
    }
    return entries;
    } catch (const std::exception& error) {
        char context[96];
        std::snprintf(context, sizeof(context), " (preparing registered shader 0x%llx type %u)", static_cast<unsigned long long>(snapshot.codeAddress), snapshot.type);
        throw std::runtime_error(error.what() + std::string(context));
    }
}

}

std::vector<PreparedGraphicsStage> PrepareGraphicsStages(const DrawDecode& decoded, const ShaderRecompiler::SpirvTarget& target) {
    ShaderPreparationTransaction transaction;
    require(decoded.programs.size() == decoded.roles.size(), "graphics ABI program roles are incomplete");
    for (const auto& program : decoded.programs) require(program.snapshot != nullptr, "graphics ABI has no registered shader snapshot");
    std::vector<ShaderRecompiler::ProgramRole> expected;
    if (decoded.state.stages.path == Graphics::ShaderPath::Tessellation) expected = {ShaderRecompiler::ProgramRole::Local, ShaderRecompiler::ProgramRole::Hull, ShaderRecompiler::ProgramRole::Domain};
    else {
        expected.push_back(ShaderRecompiler::ProgramRole::Main);
        if (decoded.state.stages.path == Graphics::ShaderPath::Geometry && !decoded.programs.empty() && decoded.programs.front().snapshot->type == 4) expected.push_back(ShaderRecompiler::ProgramRole::GeometryBack);
    }
    if (!decoded.roles.empty() && decoded.roles.back() == ShaderRecompiler::ProgramRole::Fragment) expected.push_back(ShaderRecompiler::ProgramRole::Fragment);
    require(expected == decoded.roles, "graphics ABI linked programs do not match the stage routing");
    std::vector<ShaderRecompiler::LinkedProgram> linked;
    std::vector<ShaderRecompiler::MemoryRegion> memory;
    for (std::size_t index = 0; index < decoded.programs.size(); ++index) {
        const auto& program = decoded.programs[index];
        require(program.snapshot != nullptr && program.codeOffset < program.snapshot->code.size(), "graphics ABI has an invalid registered entry point");
        const auto code = std::span(program.snapshot->code).subspan(program.codeOffset);
        require(program.binary.code.data() == code.data() && program.binary.code.size() == code.size() && program.binary.codeAddress == program.snapshot->codeAddress + program.codeOffset * sizeof(std::uint32_t), "graphics ABI entry point differs from its snapshot");
        linked.push_back({decoded.roles[index], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
        memory.insert(memory.end(), program.memory.begin(), program.memory.end());
    }
    std::vector<PreparedGraphicsStage> prepared;
    std::uint32_t pushOffset = 0;
    const auto capacity = decoded.state.stages.mesh ? ShaderRecompiler::MeshDrawPushOffsetBytes : Graphics::PipelinePushConstantBytes;
    for (std::size_t index = 0; index < decoded.programs.size(); ++index) {
        if (decoded.roles[index] == ShaderRecompiler::ProgramRole::GeometryBack) continue;
        const auto& program = decoded.programs[index];
        const bool fragment = program.binary.stage == ShaderRecompiler::ShaderStage::Fragment;
        const auto wave = fragment ? decoded.state.stages.fragmentWaveSize : decoded.state.stages.vertexWaveSize;
        std::optional<ShaderRecompiler::ShaderVertexStageInfo> vertex;
        if (!fragment) vertex = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, nullptr, true);
        ShaderRecompiler::RecompileRequest request{program.binary, {wave, program.firstUserSgpr, program.userData, {}, fragment ? std::optional(decoded.pixel) : std::nullopt, vertex, memory, RegisteredFloatMode(*program.snapshot)}, target, {0, 0, pushOffset, capacity - pushOffset}, ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, decoded.state.stages.mesh, decoded.state.stages.tessellation, {}}};
        std::vector<std::uint64_t> key;
        ShaderRecompiler::BuildPreparedShaderKey(request, key);
        std::shared_ptr<const ShaderRecompiler::SourceHandle> handle;
        for (const auto& entry : transaction.Read(*program.snapshot).entries) {
            if (entry.codeOffset == program.codeOffset && ShaderRecompiler::MatchesPreparedShader(request, *entry.handle, key)) {
                handle = entry.handle;
                break;
            }
        }
        if (handle == nullptr) {
            PerformanceTimer timing("Shader.PrepareArtifact");
            handle = ShaderRecompiler::PrepareShader(request);
        }
        const auto bytes = handle->artifact->bindings.pushConstantSizeBytes;
        require(bytes <= capacity - pushOffset, "prepared graphics stages exceed the push constant block");
        prepared.push_back({program.snapshot, {program.codeOffset, handle}});
        pushOffset += bytes;
    }
    transaction.Commit();
    return prepared;
}

void Driver::ResolveGraphicsStagesAbi(std::span<const Shader* const> stages, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
    PerformanceContext timingContext(FrameTiming::Preparation());
    PerformanceTimer timing("Shader.ResolveGraphicsStagesAbi");
    ShaderPreparationTransaction transaction;
    CheckFailure();
    require(!stages.empty(), "graphics ABI has no shader headers");
    QueueState state{};
    std::shared_ptr<const ShaderRegistry> registry;
    std::shared_ptr<const ShaderSnapshot> owner;
    {
        std::lock_guard lock(mutex);
        registry = shaders;
        for (const auto* shader : stages) {
            GuestMemory::CheckRange(shader, sizeof(Shader), alignof(Shader));
            const auto address = reinterpret_cast<std::uintptr_t>(const_cast<const void*>(shader->code));
            require(registry != nullptr && registry->contains(address), "graphics ABI refers to an unregistered shader");
            if (owner == nullptr) owner = registry->at(address);
            const auto& snapshot = *registry->at(address);
            // PSO precache passes patched header copies; the code identity selects the artifact.
            if (snapshot.headerAddress != reinterpret_cast<std::uintptr_t>(shader)) {
                static std::mutex mismatchMutex;
                static std::set<std::uint64_t> mismatched;
                std::lock_guard lock(mismatchMutex);
                if (mismatched.insert(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(shader->code))).second) {
                    std::fprintf(stderr, "[gpu] graphics ABI header is a copy; resolving by code identity\n");
                }
            }
            require(snapshot.registeredState != nullptr, "registered shader state is missing");
            const auto& registered = *snapshot.registeredState;
            for (const auto& [offset, value] : registered.shader) state.shader.insert_or_assign(offset, value);
            for (const auto& [offset, value] : registered.context) state.context.insert_or_assign(offset, value);
            for (const auto& [offset, value] : registered.userConfig) state.userConfig.insert_or_assign(offset, value);
        }
    }
    for (const auto reg : context) state.context.insert_or_assign(reg.offset, reg.value);
    for (const auto reg : primitive) state.userConfig.insert_or_assign(reg.offset, reg.value);
    const auto localDevice = device.Load();
    require(localDevice != nullptr, "graphics ABI device is missing");
    struct GraphicsAbiKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, GraphicsAbiKeyStorage>();
    BuildRegisteredAbiKey(state, *localDevice, key);
    const auto primitiveType = state.userConfig.find(0x242u);
    for (const auto& abi : transaction.Read(*owner).graphicsAbis) {
        if (abi.key != key || abi.registry.lock() != registry) continue;
        for (const auto& stage : abi.stages) {
            const auto snapshot = stage.lock();
            require(snapshot != nullptr, "prepared graphics ABI lost a registered stage");
            if (snapshot->type != 1) ResolvePreparedGraphics(*snapshot, {}, primitiveType == state.userConfig.end() ? 0u : primitiveType->second, localDevice->Target());
        }
        transaction.Commit();
        return;
    }
    DrawDecode decoded{};
    decoded.state.stages = Graphics::DecodeShaderStages(state);
    DecodeGraphicsPrograms(decoded, state, *registry, true, false);
    auto prepared = PrepareGraphicsStages(decoded, localDevice->Target());
    for (auto& stage : prepared) {
        const auto& entries = transaction.Read(*stage.snapshot).entries;
        const auto duplicate = std::ranges::any_of(entries, [&](const auto& existing) { return existing.codeOffset == stage.entry.codeOffset && existing.handle->artifact == stage.entry.handle->artifact; });
        if (!duplicate) transaction.Edit(*stage.snapshot).entries.push_back(std::move(stage.entry));
    }
    for (const auto& stage : prepared) {
        if (stage.snapshot->type == 1) continue;
        ResolvePreparedGraphics(*stage.snapshot, {}, primitiveType == state.userConfig.end() ? 0u : primitiveType->second, localDevice->Target());
    }
    PreparedShaderState::GraphicsAbi abi{key, registry, {}};
    for (const auto& stage : prepared) abi.stages.push_back(stage.snapshot);
    auto& graphicsAbis = transaction.Edit(*owner).graphicsAbis;
    std::erase_if(graphicsAbis, [](const auto& entry) { return entry.registry.expired(); });
    graphicsAbis.push_back(std::move(abi));
    transaction.Commit();
}

void Driver::ResolveShaderAbi(const Shader* shader, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
    PerformanceContext timingContext(FrameTiming::Preparation());
    PerformanceTimer timing("Shader.ResolveAbi");
    ShaderPreparationTransaction transaction;
    CheckFailure();
    GuestMemory::CheckRange(shader, sizeof(Shader), alignof(Shader));
    if (shader->type != 0 && shader->type != 1) {
        const std::array<const Shader*, 1> stages{shader};
        ResolveGraphicsStagesAbi(stages, context, primitive);
        transaction.Commit();
        return;
    }
    std::shared_ptr<const ShaderSnapshot> snapshot;
    {
        std::lock_guard lock(mutex);
        const auto address = reinterpret_cast<std::uintptr_t>(const_cast<const void*>(shader->code));
        require(shaders != nullptr && shaders->contains(address), "static ABI refers to an unregistered shader");
        snapshot = shaders->at(address);
        // PSO precache passes patched header copies; the code identity selects the artifact.
        if (snapshot->headerAddress != reinterpret_cast<std::uintptr_t>(shader)) {
            static std::mutex mismatchMutex;
            static std::set<std::uint64_t> mismatched;
            std::lock_guard lock(mismatchMutex);
            if (mismatched.insert(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(shader->code))).second) {
                std::fprintf(stderr, "[gpu] static ABI header is a copy; resolving by code identity\n");
            }
        }
    }
    require(snapshot->registeredState != nullptr, "registered shader state is missing");
    QueueState state{};
    state.shader = snapshot->registeredState->shader;
    state.context = snapshot->registeredState->context;
    state.userConfig = snapshot->registeredState->userConfig;
    for (const auto reg : context) state.context[reg.offset] = reg.value;
    for (const auto reg : primitive) state.userConfig[reg.offset] = reg.value;
    const auto localDevice = device.Load();
    require(localDevice != nullptr, "shader registration device is missing");
    struct RegisteredAbiKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, RegisteredAbiKeyStorage>();
    BuildRegisteredAbiKey(state, *localDevice, key);
    const auto& registeredAbis = transaction.Read(*snapshot).registeredAbis;
    if (std::ranges::find(registeredAbis, key) != registeredAbis.end()) {
        transaction.Commit();
        return;
    }
    auto entries = PrepareRegistered(*snapshot, *localDevice, state, false);
    auto& prepared = transaction.Edit(*snapshot);
    for (auto& entry : entries) {
        const auto duplicate = std::ranges::any_of(prepared.entries, [&](const auto& existing) { return existing.handle->artifact == entry.handle->artifact; });
        if (!duplicate) prepared.entries.push_back(std::move(entry));
    }
    prepared.registeredAbis.push_back(key);
    transaction.Commit();
}

alignas(256) static const std::uint32_t NullPixelCode[64] = {0xbf810000u};
static const Shader NullPixelShader = [] {
    Shader shader{};
    shader.file_header = 0x34333231u;
    shader.version = 0x18u;
    shader.code = NullPixelCode;
    shader.header_size = sizeof(Shader);
    shader.shader_size = sizeof(NullPixelCode);
    shader.type = 1;
    return shader;
}();

std::uint64_t NullPixelProgramAddress() {
    return reinterpret_cast<std::uintptr_t>(NullPixelCode);
}

void Driver::ResolveGraphicsAbi(const Shader* vertex, const Shader* pixel, std::uint32_t primitiveType) {
    PerformanceContext timingContext(FrameTiming::Preparation());
    PerformanceTimer timing("Shader.ResolveGraphicsAbi");
    CheckFailure();
    if (primitiveType != 0 && primitiveType != 7 && primitiveType != 17) return;
    ShaderPreparationTransaction transaction;
    require(vertex != nullptr && pixel != nullptr, "rectangle ABI requires vertex and fragment shaders");
    std::shared_ptr<const ShaderSnapshot> front;
    std::shared_ptr<const ShaderSnapshot> fragment;
    {
        std::lock_guard lock(mutex);
        GuestMemory::CheckRange(vertex, sizeof(Shader), alignof(Shader));
        GuestMemory::CheckRange(pixel, sizeof(Shader), alignof(Shader));
        const auto lookup = [&](const Shader* shader) {
            const auto address = reinterpret_cast<std::uintptr_t>(const_cast<const void*>(shader->code));
            require(shaders != nullptr && shaders->contains(address), "rectangle ABI refers to an unregistered shader");
            const auto snapshot = shaders->at(address);
            // Title runtimes pass stack or heap COPIES of a registered header with patched fields
            // (PSO precache interpolant mapping); the code identity selects the artifact.
            if (snapshot->headerAddress != reinterpret_cast<std::uintptr_t>(shader)) {
                static std::mutex mismatchMutex;
                static std::set<std::uint64_t> mismatched;
                std::lock_guard lock(mismatchMutex);
                if (mismatched.insert(address).second) {
                    std::fprintf(stderr, "[gpu] rectangle ABI header 0x%llx is a copy of registered shader 0x%llx; resolving by code identity\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(shader)), static_cast<unsigned long long>(address));
                }
            }
            return snapshot;
        };
        front = lookup(vertex);
        fragment = lookup(pixel);
    }
    require(front != fragment, "rectangle stages refer to the same shader");
    const auto localDevice = device.Load();
    require(localDevice != nullptr, "shader registration device is missing");
    ResolvePreparedGraphics(*front, fragment, primitiveType, localDevice->Target());
    transaction.Commit();
}

void ResolvePreparedGraphics(const ShaderSnapshot& front, const std::shared_ptr<const ShaderSnapshot>& fragment, std::uint32_t primitiveType, const ShaderRecompiler::SpirvTarget& target) {
    ShaderPreparationTransaction transaction;
    require(fragment.get() != &front, "rectangle stages refer to the same shader");
    std::vector<std::shared_ptr<const ShaderSnapshot>> fragments;
    {
        const auto& current = transaction.Read(front);
        const bool newFragment = fragment && !std::ranges::any_of(current.fragments, [&](const auto& entry) { return entry.lock() == fragment; });
        const bool newRequest = !current.rectangleRequested && (primitiveType == 7 || primitiveType == 17);
        const bool expiredFragment = std::ranges::any_of(current.fragments, [](const auto& entry) { return entry.expired(); });
        if (newFragment || newRequest || expiredFragment) {
            auto& prepared = transaction.Edit(front);
            std::erase_if(prepared.fragments, [](const auto& entry) { return entry.expired(); });
            std::erase_if(prepared.rectangleProgress, [](const auto& entry) { return entry.fragment.expired(); });
            if (newFragment) prepared.fragments.push_back(fragment);
            prepared.rectangleRequested |= newRequest;
        }
        const auto& prepared = transaction.Read(front);
        if (!prepared.rectangleRequested) {
            transaction.Commit();
            return;
        }
        for (const auto& entry : prepared.fragments) {
            if (auto snapshot = entry.lock()) fragments.push_back(std::move(snapshot));
        }
    }
    for (const auto& pixel : fragments) {
        const auto& current = transaction.Read(front);
        const auto& pixelPrepared = transaction.Read(*pixel);
        require(!current.entries.empty() && !pixelPrepared.entries.empty(), "rectangle ABI has missing stage artifacts");
        const auto progress = std::ranges::find_if(current.rectangleProgress, [&](const auto& entry) { return entry.fragment.lock() == pixel; });
        const auto vertexCount = progress == current.rectangleProgress.end() ? 0u : progress->vertexCount;
        const auto fragmentCount = progress == current.rectangleProgress.end() ? 0u : progress->fragmentCount;
        if (vertexCount == current.entries.size() && fragmentCount == pixelPrepared.entries.size()) continue;
        auto& prepared = transaction.Edit(front);
        std::vector<PreparedShaders::Rectangle> rectangles;
        for (std::size_t vertexIndex = 0; vertexIndex < prepared.entries.size(); ++vertexIndex) {
            const auto& vertexEntry = prepared.entries[vertexIndex];
            for (std::size_t fragmentIndex = vertexIndex < vertexCount ? fragmentCount : 0u; fragmentIndex < pixelPrepared.entries.size(); ++fragmentIndex) {
                const auto& fragmentEntry = pixelPrepared.entries[fragmentIndex];
                const auto& vertexArtifact = ShaderRecompiler::GetPreparedArtifact(*vertexEntry.handle);
                const auto& fragmentArtifact = ShaderRecompiler::GetPreparedArtifact(*fragmentEntry.handle);
                const auto exists = [&](const auto& entry) { return entry.vertexId == vertexArtifact.variantId && entry.fragmentId == fragmentArtifact.variantId; };
                if (std::ranges::any_of(prepared.rectangles, exists) || std::ranges::any_of(rectangles, exists)) continue;
                ShaderRecompiler::RecompileResult vertexResult;
                ShaderRecompiler::RecompileResult fragmentResult;
                static_cast<ShaderRecompiler::CompiledShaderArtifact&>(vertexResult) = vertexArtifact;
                static_cast<ShaderRecompiler::CompiledShaderArtifact&>(fragmentResult) = fragmentArtifact;
                rectangles.push_back({vertexArtifact.variantId, fragmentArtifact.variantId, std::make_shared<const ShaderRecompiler::RectListShaders>(ShaderRecompiler::BuildRectListShaders(vertexResult, fragmentResult, target))});
            }
        }
        prepared.rectangles.insert(prepared.rectangles.end(), std::make_move_iterator(rectangles.begin()), std::make_move_iterator(rectangles.end()));
        std::erase_if(prepared.rectangleProgress, [&](const auto& entry) { return entry.fragment.expired() || entry.fragment.lock() == pixel; });
        prepared.rectangleProgress.push_back({pixel, prepared.entries.size(), pixelPrepared.entries.size()});
    }
    transaction.Commit();
}

void Driver::RegisterShader(const Shader* shader) {
    PerformanceContext timingContext(FrameTiming::Preparation());
    PerformanceTimer timing("Shader.Register");
    ShaderPreparationTransaction transaction;
    CheckFailure();
    GuestMemory::CheckRange(shader, sizeof(Shader), 1);
    Shader fields;
    std::memcpy(&fields, static_cast<const void*>(shader), sizeof(Shader));
    // Experiment switch: drop type-8 (Fs) registrations entirely to test crash correlation.
    if (fields.type == 8u && std::getenv("APS5_SKIP_FS") != nullptr) {
        static std::once_flag once;
        std::call_once(once, [] { std::fprintf(stderr, "[gpu] APS5_SKIP_FS: type-8 registration dropped\n"); });
        return;
    }
    require(fields.file_header == 0x34333231u && fields.version == 0x18u, "invalid shader header");
    require(fields.header_size >= sizeof(Shader), "shader header is smaller than its fixed fields");
    require(fields.shader_size != 0 && (fields.shader_size & 3u) == 0, "invalid shader size");
    GuestMemory::CheckRange(shader, fields.header_size, 1);
    const auto* code = const_cast<const void*>(fields.code);
    GuestMemory::CheckRange(code, fields.shader_size, 256);
    ShaderSnapshot snapshot{reinterpret_cast<std::uintptr_t>(code), reinterpret_cast<std::uintptr_t>(shader), fields.type, {}, {}};
    snapshot.code.resize(fields.shader_size / sizeof(std::uint32_t));
    std::memcpy(snapshot.code.data(), code, fields.shader_size);
    snapshot.header.resize(fields.header_size);
    std::memcpy(snapshot.header.data(), static_cast<const void*>(shader), fields.header_size);

    {
        std::lock_guard lock(mutex);
        rethrowFailure();
        if (shaders != nullptr) {
            const auto found = shaders->find(snapshot.codeAddress);
            if (found != shaders->end()) {
                const auto& current = *found->second;
                if (current.headerAddress == snapshot.headerAddress && current.type == snapshot.type && current.code == snapshot.code && current.header == snapshot.header) {
                    transaction.Commit();
                    return;
                }
            }
        }
    }

    std::shared_ptr<VulkanDevice> localDevice = device.Load();
    if (localDevice == nullptr) {
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;
    }
    snapshot.registeredState = std::make_shared<const RegisteredShaderState>(DecodeRegisteredState(snapshot));
    QueueState registered{};
    registered.shader = snapshot.registeredState->shader;
    registered.context = snapshot.registeredState->context;
    registered.userConfig = snapshot.registeredState->userConfig;
    snapshot.prepared->entries = PrepareRegistered(snapshot, *localDevice, registered, true);
    if ((snapshot.type == 0 || snapshot.type == 1) && !snapshot.prepared->entries.empty()) {
        std::vector<std::uint64_t> key;
        BuildRegisteredAbiKey(registered, *localDevice, key);
        snapshot.prepared->registeredAbis.push_back(std::move(key));
    }
    static const char* traceRegs = std::getenv("APS5_TRACE_SHADER_REGS");
    if (traceRegs != nullptr && (std::string(traceRegs) == "all" || std::strtoull(traceRegs, nullptr, 16) == snapshot.codeAddress)) {
        const auto print = [](const ShaderRegister* registers, std::uint32_t count) {
            for (std::uint32_t i = 0; i < count && registers != nullptr; ++i) {
                ShaderRegister value;
                std::memcpy(&value, static_cast<const void*>(registers + i), sizeof(value));
                std::fprintf(stderr, " %x=%08x", value.offset, value.value);
            }
        };
        std::fprintf(stderr, "[shader] 0x%llx type %u cx", static_cast<unsigned long long>(snapshot.codeAddress), fields.type);
        print(fields.cx_registers, fields.num_cx_registers);
        std::fprintf(stderr, " sh");
        print(fields.sh_registers, fields.num_sh_registers);
        std::fprintf(stderr, "\n");
    }
    std::lock_guard lock(mutex);
    rethrowFailure();
    PublishRegisteredShader(shaders, std::make_shared<const ShaderSnapshot>(std::move(snapshot)));
    if (shaders->find(NullPixelProgramAddress()) == shaders->end()) {
        ShaderSnapshot null{NullPixelProgramAddress(), reinterpret_cast<std::uintptr_t>(&NullPixelShader), NullPixelShader.type, {}, {}};
        null.code.assign(std::begin(NullPixelCode), std::end(NullPixelCode));
        null.header.resize(sizeof(Shader));
        std::memcpy(null.header.data(), &NullPixelShader, sizeof(Shader));
        auto nullRegisteredState = DecodeRegisteredState(null);
        nullRegisteredState.shader.emplace(0x008u, static_cast<std::uint32_t>(null.codeAddress >> 8u));
        nullRegisteredState.shader.emplace(0x009u, static_cast<std::uint32_t>(null.codeAddress >> 40u));
        nullRegisteredState.shader.emplace(0x00bu, 0u);
        nullRegisteredState.context.emplace(0x1b3u, 0x2u);
        nullRegisteredState.context.emplace(0x1b4u, 0x2u);
        null.registeredState = std::make_shared<const RegisteredShaderState>(std::move(nullRegisteredState));
        QueueState nullState{};
        nullState.shader = null.registeredState->shader;
        nullState.context = null.registeredState->context;
        nullState.userConfig = null.registeredState->userConfig;
        null.prepared->entries = PrepareRegistered(null, *localDevice, nullState, true);
        PublishRegisteredShader(shaders, std::make_shared<const ShaderSnapshot>(std::move(null)));
    }
    transaction.Commit();
}

}

extern "C" AgcDriver::DriverDetail::ShaderPreparationTransaction* AgcDriverBeginShaderPreparation_nid_postfix() {
    return new AgcDriver::DriverDetail::ShaderPreparationTransaction();
}

extern "C" void AgcDriverCommitShaderPreparation_nid_postfix(AgcDriver::DriverDetail::ShaderPreparationTransaction* transaction) {
    transaction->Commit();
}

extern "C" void AgcDriverEndShaderPreparation_nid_postfix(AgcDriver::DriverDetail::ShaderPreparationTransaction* transaction) noexcept {
    delete transaction;
}
