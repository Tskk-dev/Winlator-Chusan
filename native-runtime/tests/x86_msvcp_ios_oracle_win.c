#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *(__thiscall *basic_ios_ctor)(void *);

int main(int argc, char **argv) {
    static const unsigned zero_offsets[] = {
        0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24,
        0x28, 0x2c, 0x30, 0x38, 0x3c
    };
    _Alignas(16) unsigned char object[96];
    HMODULE dll;
    basic_ios_ctor ctor;
    void *returned;
    uint32_t vftable;
    int fields_match = 1;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 2) {
        fputs("expected absolute MSVCP140.dll path\n", stderr);
        return 2;
    }
    dll = LoadLibraryExA(argv[1], NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) {
        fprintf(stderr, "LoadLibraryExA failed: %lu\n", GetLastError());
        return 3;
    }
    ctor = (basic_ios_ctor)GetProcAddress(
        dll, "??0?$basic_ios@DU?$char_traits@D@std@@@std@@IAE@XZ");
    if (!ctor) {
        fprintf(stderr, "GetProcAddress failed: %lu\n", GetLastError());
        FreeLibrary(dll);
        return 4;
    }
    memset(object, 0xa5, sizeof(object));
    returned = ctor(object);
    memcpy(&vftable, object, sizeof(vftable));
    if (returned != object || vftable != (uint32_t)((uintptr_t)dll + 0x2f64))
        fields_match = 0;
    for (size_t i = 0; i < sizeof(zero_offsets) / sizeof(zero_offsets[0]); ++i) {
        uint32_t value;
        memcpy(&value, object + zero_offsets[i], sizeof(value));
        if (value != 0)
            fields_match = 0;
    }
    if (object[0x40] != 0)
        fields_match = 0;
    for (size_t i = 0; i < sizeof(object); ++i) {
        int written = i < 4 || i == 0x40;
        for (size_t j = 0; !written && j < sizeof(zero_offsets) / sizeof(zero_offsets[0]); ++j)
            written = i >= zero_offsets[j] && i < zero_offsets[j] + 4;
        if (!written && object[i] != 0xa5)
            fields_match = 0;
    }
    if (!fields_match) {
        fputs("MSVCP140 basic_ios constructor oracle mismatch\n", stderr);
        FreeLibrary(dll);
        return 5;
    }
    puts("{\"return_this\":true,\"fields_match\":true,\"vftable_rva\":12132}");
    FreeLibrary(dll);
    return 0;
}
