/* Standalone, libc-free ARM64 correctness probe. No APK or game installation is needed. */
typedef unsigned char u8;
typedef unsigned int u32;
extern int chusan_probe(const u8*, u32, u32, u32, u32*, u32*);
#include "reference_vectors.h"

static u8 memory[512];

static long syscall3(long number, long arg0, long arg1, long arg2) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

static void store32(u32 offset, u32 value) {
    for (unsigned byte = 0; byte < 4; ++byte) memory[offset + byte] = (u8)(value >> (byte * 8));
}

__attribute__((noreturn)) void _start(void) {
    int failed = 0;
    const u32 base = 0x10000000u;
    store32(0x70, base + 0x100);
    for (unsigned index = 0; index < sizeof(probe_vectors) / sizeof(probe_vectors[0]); ++index) {
        store32(0x100, probe_vectors[index][0]);
        u32 eax = 0, flags = 0;
        if (!chusan_probe(memory, base, sizeof(memory), base + 0x40, &eax, &flags)
                || eax != probe_vectors[index][1] || flags != probe_vectors[index][2]) {
            failed = 1;
            break;
        }
    }
    u32 eax, flags;
    if (chusan_probe(memory, base, sizeof(memory), base - 0x100, &eax, &flags)) failed = 1;
    static const char pass[] = "{\"cases\":4096,\"mismatches\":0,\"execution\":\"android_arm64\",\"passed\":true}\n";
    static const char fail[] = "{\"passed\":false,\"error\":\"routine_or_bounds_mismatch\"}\n";
    syscall3(64, 1, (long)(failed ? fail : pass), failed ? sizeof(fail) - 1 : sizeof(pass) - 1);
    syscall3(93, failed, 0, 0);
    for (;;) { }
}
