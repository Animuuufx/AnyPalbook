#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

static HMODULE g_self = nullptr;
static std::uint8_t* g_palworld_base = nullptr;
static bool g_initialized = false;

// Exact build supplied by the user.
static constexpr std::uint32_t kAnalyzedSizeOfImage = 0x0A011000;
static constexpr std::uintptr_t kCanUseGateTestRva = 0x032CA25F;
static constexpr std::uintptr_t kCanUseGateJumpRva = 0x032CA261;
static constexpr std::uintptr_t kRankWithCharacterInternalRva = 0x02F803A0;
static constexpr std::uintptr_t kHasWorkSuitabilityRva = 0x02F80FD0;
static constexpr std::uintptr_t kHasWorkSuitabilityRankRva = 0x02F81040;

// UPalIndividualCharacterParameter offsets in this exact build.
// GotWorkSuitabilityAddRankList is TArray<FPalWorkSuitabilityInfo>.
static constexpr std::uintptr_t kAddedRankListOffset = 0x698;
static constexpr std::uintptr_t kAddedRankListNumOffset = 0x6A0;

struct FPalWorkSuitabilityInfoNative
{
    std::uint8_t WorkSuitability;
    std::uint8_t Pad[3];
    std::int32_t Rank;
};
static_assert(sizeof(FPalWorkSuitabilityInfoNative) == 8, "Unexpected work-suitability info layout");

using RankInternalFn = int(__fastcall*)(void*, std::uint8_t, std::uint8_t, std::uint8_t);
using HasWorkSuitabilityFn = bool(__fastcall*)(void*, std::uint8_t);
using HasWorkSuitabilityRankFn = bool(__fastcall*)(void*, std::uint8_t, int);

static RankInternalFn g_original_rank_internal = nullptr;
static HasWorkSuitabilityFn g_original_has_work = nullptr;
static HasWorkSuitabilityRankFn g_original_has_work_rank = nullptr;
static volatile LONG g_logged_fallback_mask = 0;

static void log_line(const char* fmt, ...)
{
    char module_path[MAX_PATH] = {};
    if (!GetModuleFileNameA(g_self, module_path, MAX_PATH)) {
        return;
    }

    char* slash = std::strrchr(module_path, '\\');
    if (slash) *slash = '\0'; // ...\\AnyPalbook\\Native
    slash = std::strrchr(module_path, '\\');
    if (slash) *slash = '\0'; // ...\\AnyPalbook

    char log_path[MAX_PATH] = {};
    std::snprintf(log_path, MAX_PATH, "%s\\anypalbook.log", module_path);

    FILE* fp = nullptr;
    fopen_s(&fp, log_path, "a");
    if (!fp) return;

    va_list args;
    va_start(args, fmt);
    std::vfprintf(fp, fmt, args);
    va_end(args);
    std::fputc('\n', fp);
    std::fclose(fp);
}

static bool write_memory(std::uint8_t* address, const void* bytes, std::size_t size)
{
    DWORD old_protect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        log_line("ERROR: VirtualProtect failed at %p size=%zu (GetLastError=%lu)",
                 address, size, GetLastError());
        return false;
    }

    std::memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD ignored = 0;
    VirtualProtect(address, size, old_protect, &ignored);
    return true;
}

static bool patch_two_nops(std::uint8_t* address)
{
    static const std::uint8_t nops[2] = { 0x90, 0x90 };
    return write_memory(address, nops, sizeof(nops));
}

static void emit_absolute_jump(std::uint8_t* out, const void* destination)
{
    // mov rax, imm64 ; jmp rax
    out[0] = 0x48;
    out[1] = 0xB8;
    const auto dst = reinterpret_cast<std::uintptr_t>(destination);
    std::memcpy(out + 2, &dst, sizeof(dst));
    out[10] = 0xFF;
    out[11] = 0xE0;
}

static bool verify_bytes(const std::uint8_t* address,
                         const std::uint8_t* expected,
                         std::size_t size,
                         const char* label)
{
    if (std::memcmp(address, expected, size) == 0) {
        return true;
    }

    log_line("ERROR: %s bytes do not match the analyzed Palworld build at %p.", label, address);
    return false;
}

static bool install_detour(std::uint8_t* target,
                           const void* hook,
                           std::size_t stolen_len,
                           const std::uint8_t* expected_prefix,
                           std::size_t expected_prefix_len,
                           void** original_out,
                           const char* label)
{
    if (stolen_len < 12) {
        log_line("ERROR: %s detour length %zu is too small.", label, stolen_len);
        return false;
    }

    if (!verify_bytes(target, expected_prefix, expected_prefix_len, label)) {
        return false;
    }

    const std::size_t trampoline_size = stolen_len + 12;
    auto* trampoline = reinterpret_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, trampoline_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log_line("ERROR: VirtualAlloc failed for %s trampoline (GetLastError=%lu).", label, GetLastError());
        return false;
    }

    std::memcpy(trampoline, target, stolen_len);
    emit_absolute_jump(trampoline + stolen_len, target + stolen_len);
    FlushInstructionCache(GetCurrentProcess(), trampoline, trampoline_size);

    std::uint8_t patch[32] = {};
    if (stolen_len > sizeof(patch)) {
        log_line("ERROR: %s detour length exceeds local patch buffer.", label);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    std::memset(patch, 0x90, stolen_len);
    emit_absolute_jump(patch, hook);
    if (!write_memory(target, patch, stolen_len)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    *original_out = trampoline;
    log_line("SUCCESS: installed %s detour at RVA 0x%llX.",
             label,
             static_cast<unsigned long long>(target - g_palworld_base));
    return true;
}

static int get_added_rank(void* individual_parameter, std::uint8_t work)
{
    if (!individual_parameter || work == 0 || work > 32) {
        return 0;
    }

    __try {
        auto* base = reinterpret_cast<std::uint8_t*>(individual_parameter);
        auto* list = *reinterpret_cast<FPalWorkSuitabilityInfoNative**>(base + kAddedRankListOffset);
        const int count = *reinterpret_cast<int*>(base + kAddedRankListNumOffset);

        if (!list || count <= 0 || count > 64) {
            return 0;
        }

        for (int i = 0; i < count; ++i) {
            if (list[i].WorkSuitability == work && list[i].Rank > 0) {
                return list[i].Rank;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    return 0;
}

static void log_fallback_once(std::uint8_t work, int rank)
{
    if (work == 0 || work >= 31 || rank <= 0) return;

    const LONG bit = static_cast<LONG>(1u << work);
    const LONG old = InterlockedOr(&g_logged_fallback_mask, bit);
    if ((old & bit) == 0) {
        log_line("ACTIVE: added-only work suitability detected (type=%u rank=%d).", work, rank);
    }
}

static int effective_rank_with_added_only(void* individual_parameter,
                                          std::uint8_t work,
                                          std::uint8_t flag8,
                                          std::uint8_t flag9)
{
    int original = 0;
    if (g_original_rank_internal) {
        original = g_original_rank_internal(individual_parameter, work, flag8, flag9);
    }

    if (original > 0) {
        return original;
    }

    // Vanilla GetWorkSuitabilityRankWithCharacterRank exits early when the
    // species' innate/base work-suitability map has no entry for this work type.
    // Applied Handbooks are stored separately in GotWorkSuitabilityAddRankList,
    // so a newly granted suitability exists in the save but vanilla never reads it.
    const int added = get_added_rank(individual_parameter, work);
    if (added > 0) {
        log_fallback_once(work, added);
        return added;
    }

    return original;
}

static int __fastcall hook_rank_internal(void* individual_parameter,
                                         std::uint8_t work,
                                         std::uint8_t flag8,
                                         std::uint8_t flag9)
{
    return effective_rank_with_added_only(individual_parameter, work, flag8, flag9);
}

static bool __fastcall hook_has_work(void* individual_parameter, std::uint8_t work)
{
    if (g_original_has_work && g_original_has_work(individual_parameter, work)) {
        return true;
    }

    return get_added_rank(individual_parameter, work) > 0;
}

static bool __fastcall hook_has_work_rank(void* individual_parameter,
                                          std::uint8_t work,
                                          int required_rank)
{
    if (g_original_has_work_rank &&
        g_original_has_work_rank(individual_parameter, work, required_rank)) {
        return true;
    }

    const int rank = effective_rank_with_added_only(individual_parameter, work, 1, 1);
    return rank >= required_rank;
}

static bool apply_anypalbook_patch()
{
    if (g_initialized) {
        log_line("AnyPalbook v1.1.0 already initialized.");
        return true;
    }

    g_palworld_base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!g_palworld_base) {
        log_line("ERROR: GetModuleHandleW(NULL) failed.");
        return false;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_palworld_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        log_line("ERROR: invalid DOS header.");
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_palworld_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        log_line("ERROR: invalid PE64 header.");
        return false;
    }

    log_line("AnyPalbook v1.1.0 initializing.");
    log_line("Palworld module base=%p SizeOfImage=0x%X", g_palworld_base, nt->OptionalHeader.SizeOfImage);

    if (nt->OptionalHeader.SizeOfImage != kAnalyzedSizeOfImage) {
        log_line("ERROR: SizeOfImage 0x%X does not match analyzed build 0x%X; refusing native detours.",
                 nt->OptionalHeader.SizeOfImage, kAnalyzedSizeOfImage);
        return false;
    }

    // Validate every target before changing any executable code.
    static const std::uint8_t expected_gate[] = {
        0x85, 0xFF, 0x74, 0x1A, 0x3B, 0xB8, 0xA4, 0x0F,
        0x00, 0x00, 0x7D, 0x12, 0xB0, 0x01
    };
    static const std::uint8_t expected_rank_prefix[] = {
        0x48,0x89,0x5C,0x24,0x08,
        0x48,0x89,0x6C,0x24,0x10,
        0x48,0x89,0x74,0x24,0x18
    };
    static const std::uint8_t expected_has_prefix[] = {
        0x8B,0x81,0xD0,0x07,0x00,0x00,
        0x3B,0x81,0xFC,0x07,0x00,0x00
    };
    static const std::uint8_t expected_has_rank_prefix[] = {
        0x48,0x89,0x5C,0x24,0x08,
        0x57,
        0x48,0x83,0xEC,0x20,
        0x8B,0x81,0xD0,0x07,0x00,0x00
    };

    auto* gate_test = g_palworld_base + kCanUseGateTestRva;
    auto* gate_jump = g_palworld_base + kCanUseGateJumpRva;
    auto* rank_target = g_palworld_base + kRankWithCharacterInternalRva;
    auto* has_target = g_palworld_base + kHasWorkSuitabilityRva;
    auto* has_rank_target = g_palworld_base + kHasWorkSuitabilityRankRva;

    if (!verify_bytes(gate_test, expected_gate, sizeof(expected_gate), "handbook zero-rank gate") ||
        !verify_bytes(rank_target, expected_rank_prefix, sizeof(expected_rank_prefix), "rank accessor") ||
        !verify_bytes(has_target, expected_has_prefix, sizeof(expected_has_prefix), "HasWorkSuitability") ||
        !verify_bytes(has_rank_target, expected_has_rank_prefix, sizeof(expected_has_rank_prefix), "HasWorkSuitabilityRank")) {
        log_line("ERROR: analyzed target validation failed; no patch was applied.");
        return false;
    }

    // Keep vanilla item/type/target/max-rank validation, but allow the very
    // first handbook on a suitability whose current effective rank is zero.
    if (!patch_two_nops(gate_jump)) {
        return false;
    }
    log_line("SUCCESS: removed zero-rank handbook rejection at RVA 0x%llX.",
             static_cast<unsigned long long>(kCanUseGateJumpRva));

    // The key fix missing from v1.0: vanilla stores the handbook bonus in
    // GotWorkSuitabilityAddRankList, but its rank/has accessors return early
    // when the species has no innate entry. Detour those accessors so the
    // saved handbook rank becomes a real work suitability everywhere those
    // central checks are used.
    if (!install_detour(rank_target,
                        reinterpret_cast<const void*>(&hook_rank_internal),
                        15,
                        expected_rank_prefix,
                        sizeof(expected_rank_prefix),
                        reinterpret_cast<void**>(&g_original_rank_internal),
                        "added-only rank accessor")) {
        return false;
    }

    if (!install_detour(has_target,
                        reinterpret_cast<const void*>(&hook_has_work),
                        12,
                        expected_has_prefix,
                        sizeof(expected_has_prefix),
                        reinterpret_cast<void**>(&g_original_has_work),
                        "added-only HasWorkSuitability")) {
        return false;
    }

    if (!install_detour(has_rank_target,
                        reinterpret_cast<const void*>(&hook_has_work_rank),
                        16,
                        expected_has_rank_prefix,
                        sizeof(expected_has_rank_prefix),
                        reinterpret_cast<void**>(&g_original_has_work_rank),
                        "added-only HasWorkSuitabilityRank")) {
        return false;
    }

    g_initialized = true;
    log_line("SUCCESS: AnyPalbook v1.1.0 compatibility hooks active.");
    return true;
}

extern "C" __declspec(dllexport) int luaopen_anypalbook(void*)
{
    apply_anypalbook_patch();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
