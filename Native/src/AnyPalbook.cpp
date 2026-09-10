#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

static HMODULE g_self = nullptr;

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

static bool patch_bytes(std::uint8_t* address)
{
    DWORD old_protect = 0;
    if (!VirtualProtect(address, 2, PAGE_EXECUTE_READWRITE, &old_protect)) {
        log_line("ERROR: VirtualProtect failed at %p (GetLastError=%lu)", address, GetLastError());
        return false;
    }

    address[0] = 0x90;
    address[1] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), address, 2);

    DWORD ignored = 0;
    VirtualProtect(address, 2, old_protect, &ignored);
    return true;
}

static bool match_pattern(const std::uint8_t* p, const int* pattern, std::size_t length)
{
    for (std::size_t i = 0; i < length; ++i) {
        if (pattern[i] >= 0 && p[i] != static_cast<std::uint8_t>(pattern[i])) {
            return false;
        }
    }
    return true;
}

static bool apply_anypalbook_patch()
{
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) {
        log_line("ERROR: GetModuleHandleW(NULL) failed.");
        return false;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        log_line("ERROR: invalid DOS header.");
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        log_line("ERROR: invalid PE64 header.");
        return false;
    }

    constexpr std::uint32_t kAnalyzedSizeOfImage = 0x0A011000;
    constexpr std::uintptr_t kAnalyzedGateTestRva = 0x032CA25F;
    constexpr std::uintptr_t kAnalyzedGateJumpRva = 0x032CA261;

    log_line("AnyPalbook v1.0.0 initializing.");
    log_line("Palworld module base=%p SizeOfImage=0x%X", base, nt->OptionalHeader.SizeOfImage);

    // Fast path for the exact Palworld-Win64-Shipping.exe supplied for this build.
    // Native code at RVA 0x32CA25F:
    //   85 FF             test edi,edi
    //   74 1A             je   reject
    //   3B B8 A4 0F 00 00 cmp  edi,[rax+0xFA4]   ; WorkSuitabilityMaxRank
    //   7D 12             jge  reject
    // Removing only the JE allows rank 0 while preserving the max-rank check.
    static const std::uint8_t expected_gate[] = {
        0x85, 0xFF, 0x74, 0x1A, 0x3B, 0xB8, 0xA4, 0x0F, 0x00, 0x00, 0x7D, 0x12, 0xB0, 0x01
    };

    auto* exact_test = base + kAnalyzedGateTestRva;
    auto* exact_jump = base + kAnalyzedGateJumpRva;

    if (exact_jump[0] == 0x90 && exact_jump[1] == 0x90 &&
        exact_test[0] == 0x85 && exact_test[1] == 0xFF) {
        log_line("Patch already active at RVA 0x%llX.", static_cast<unsigned long long>(kAnalyzedGateJumpRva));
        return true;
    }

    if (std::memcmp(exact_test, expected_gate, sizeof(expected_gate)) == 0) {
        if (nt->OptionalHeader.SizeOfImage != kAnalyzedSizeOfImage) {
            log_line("WARNING: SizeOfImage differs from analyzed build, but the exact target bytes still match.");
        }
        if (!patch_bytes(exact_jump)) return false;
        log_line("SUCCESS: removed zero-rank handbook rejection at RVA 0x%llX.",
                 static_cast<unsigned long long>(kAnalyzedGateJumpRva));
        return true;
    }

    // Fallback signature for small executable layout shifts. This signature is
    // taken from the exact supplied build and includes the call that resolves
    // the handbook's target work type plus the vanilla max-rank comparison.
    static const int signature[] = {
        0x48,0x8B,0x46,0x28,0x48,0x8D,0x54,0x24,0x38,0x48,0x89,0x44,0x24,0x38,0xE8,
        -1,-1,-1,-1,
        0x48,0x85,0xC0,0x74,0x34,0x0F,0xB6,0x50,0x08,0x48,0x8B,0xCF,0xE8,
        -1,-1,-1,-1,
        0x48,0x8B,0xCB,0x8B,0xF8,0xE8,
        -1,-1,-1,-1,
        0x85,0xFF,0x74,0x1A,0x3B,0xB8,0xA4,0x0F,0x00,0x00,0x7D,0x12,0xB0,0x01
    };
    constexpr std::size_t signature_len = sizeof(signature) / sizeof(signature[0]);
    constexpr std::size_t jump_offset_in_signature = 48;

    auto* section = IMAGE_FIRST_SECTION(nt);
    std::uint8_t* text_start = nullptr;
    std::size_t text_size = 0;

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = {};
        std::memcpy(name, section[i].Name, 8);
        if (std::strcmp(name, ".text") == 0) {
            text_start = base + section[i].VirtualAddress;
            text_size = section[i].Misc.VirtualSize;
            break;
        }
    }

    if (!text_start || text_size < signature_len) {
        log_line("ERROR: could not locate a valid .text section.");
        return false;
    }

    std::uint8_t* match = nullptr;
    std::size_t matches = 0;
    for (std::size_t i = 0; i + signature_len <= text_size; ++i) {
        if (match_pattern(text_start + i, signature, signature_len)) {
            match = text_start + i;
            ++matches;
            if (matches > 1) break;
        }
    }

    if (matches != 1 || !match) {
        log_line("ERROR: compatibility signature count=%zu; refusing to patch unknown executable.", matches);
        return false;
    }

    auto* jump = match + jump_offset_in_signature;
    if (jump[0] != 0x74 || jump[1] != 0x1A) {
        log_line("ERROR: signature matched but zero-rank jump bytes were %02X %02X, expected 74 1A.", jump[0], jump[1]);
        return false;
    }

    if (!patch_bytes(jump)) return false;

    const auto rva = static_cast<unsigned long long>(jump - base);
    log_line("SUCCESS: signature fallback removed zero-rank handbook rejection at RVA 0x%llX.", rva);
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
