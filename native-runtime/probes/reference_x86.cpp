// Runs one validated, isolated source routine on actual x86 hardware.
// No game entry point, DLL initializer, services or network code is invoked.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int chusan_probe(const uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t*, uint32_t*);

int main(int argc, char** argv) {
    static_assert(sizeof(void*) == 4, "The reference runner must be 32-bit x86");
    if (argc != 3) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() != 9) return 3;
    std::ofstream vectors(argv[2], std::ios::binary);
    if (!vectors) return 9;
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!code || !memory) return 4;
    std::memcpy(code, bytes.data(), bytes.size());
    DWORD oldProtection;
    if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &oldProtection)) return 5;
    FlushInstructionCache(GetCurrentProcess(), code, bytes.size());
    uint32_t base = reinterpret_cast<uint32_t>(memory), object = base + 0x100;
    uint32_t second = base + 0x200;
    std::memcpy(memory + 0x130, &second, 4);
    const uint32_t edges[] = {0, 1, 0x7ffffff3u, 0x7ffffff4u, 0x80000000u, 0xfffffff3u, 0xfffffff4u, 0xffffffffu};
    uint32_t random = 0x4348554eu;
    for (unsigned trial = 0; trial < 4096; ++trial) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        uint32_t value = trial < 8 ? edges[trial] : random;
        std::memcpy(memory + 0x200, &value, 4);
        uint32_t original, originalFlags;
        __asm__ volatile("mov %[object], %%ecx\n\t"
                         "call *%[code]\n\t"
                         "pushfl\n\tpopl %%edx"
                         : "=a"(original), "=d"(originalFlags)
                         : [object] "r"(object), [code] "r"(code)
                         : "ecx", "memory", "cc");
        uint32_t translated = 0, translatedFlags = 0;
        if (!chusan_probe(memory, base, 65536, object, &translated, &translatedFlags)) return 6;
        if (original != translated || (originalFlags & 0x8d5u) != translatedFlags) {
            std::fprintf(stderr, "Mismatch in case %u: EAX %08x/%08x flags %03x/%03x\n",
                         trial, original, translated, originalFlags & 0x8d5u, translatedFlags);
            return 7;
        }
        const uint32_t record[] = {value, original, originalFlags & 0x8d5u};
        vectors.write(reinterpret_cast<const char*>(record), sizeof(record));
    }
    vectors.close();
    if (!vectors) return 9;
    uint32_t result, flags;
    if (chusan_probe(memory, base, 65536, base - 0x100, &result, &flags)) return 8;
    VirtualFree(code, 0, MEM_RELEASE);
    VirtualFree(memory, 0, MEM_RELEASE);
    std::puts("{\"cases\":4096,\"mismatches\":0,\"reference\":\"original_x86_on_windows\",\"translated\":\"C_on_windows_x86\"}");
    return 0;
}
