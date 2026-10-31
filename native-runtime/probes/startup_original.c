/* Execute only the hash-validated bounded initializer, with fixture APIs. */
#include <windows.h>
#include "startup_fixture.h"

FixtureInput oracle_input;
FixtureResult oracle_result;
uint32_t oracle_host_sp;
void oracle_enter(void);
void oracle_done(void);
void oracle_api0(void);
void oracle_api1(void);
void oracle_api2(void);
void oracle_api3(void);

uint32_t oracle_fixture(uint32_t id,uint32_t esp) {
    return fixture_api(NULL,&oracle_input,&oracle_result,id,esp);
}
static void map_region(uint32_t address,uint32_t size) {
    void *wanted=(void*)(uintptr_t)address;
    if(VirtualAlloc(wanted,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)!=wanted) {
        MEMORY_BASIC_INFORMATION info;
        VirtualQuery(wanted,&info,sizeof(info));
        fprintf(stderr,"Mapping OS error %lu, allocation %p, state %lu, size %zu\n",
                GetLastError(),info.AllocationBase,info.State,(size_t)info.RegionSize);
        fixture_fail("mapping_failed",address);
    }
}
static void executable(uint32_t address,uint32_t size) {
    DWORD old;
    if(!VirtualProtect((void*)(uintptr_t)address,size,PAGE_EXECUTE_READ,&old))
        fixture_fail("protection_failed",address);
    FlushInstructionCache(GetCurrentProcess(),(void*)(uintptr_t)address,size);
}
/* An absolute indirect jump preserves registers and flags. */
static void trampoline(uint32_t address,void (*destination)(void)) {
    uint8_t code[10]={0xff,0x25,0,0,0,0,0,0,0,0};
    uint32_t cell=address+6, target=(uint32_t)(uintptr_t)destination;
    memcpy(code+2,&cell,4); memcpy(code+6,&target,4);
    memcpy((void*)(uintptr_t)address,code,sizeof(code));
}
int main(int argc,char **argv) {
    _Static_assert(sizeof(void*)==4,"x86 Windows oracle required");
    if(argc!=3) return 2;
    FILE *source=fopen(argv[1],"rb");
    uint8_t bytes[156];
    if(!source || fread(bytes,1,sizeof(bytes),source)!=sizeof(bytes) || fgetc(source)!=EOF)
        fixture_fail("source_size",0);
    fclose(source);
    /* Reserve only pages consumed by this fixture, leaving the host heap alone. */
    map_region(STARTUP_VA&~0xffffu,0x10000u);
    map_region(COOKIE_VA&~0xffffu,0x10000u);
    map_region(fixture_iat[0]&~0xffffu,0x10000u);
    map_region(STACK_BASE,STACK_SIZE);
    map_region(0x0fdf0000u,0x10000u);
    map_region(SHIM_BASE,0x10000u);
    memcpy((void*)(uintptr_t)STARTUP_VA,bytes,sizeof(bytes));
    executable(STARTUP_VA&~0xfffu,0x1000);
    trampoline(RETURN_VA,oracle_done);
    executable(RETURN_VA&~0xfffu,0x1000);
    void (*const adapters[4])(void)={oracle_api0,oracle_api1,oracle_api2,oracle_api3};
    for(unsigned i=0;i<4;i++) trampoline(SHIM_BASE+16*i,adapters[i]);
    executable(SHIM_BASE,0x1000);
    FILE *vectors=fopen(argv[2],"wb");
    if(!vectors) fixture_fail("vectors_open",0);
    uint32_t random=0x4348554eu;
    for(unsigned trial=0;trial<CASE_COUNT;trial++) {
        oracle_input=fixture_input(trial,&random);
        memset(&oracle_result,0,sizeof(oracle_result));
        fixture_seed(NULL,&oracle_input);
        oracle_enter();
        oracle_result.eip=RETURN_VA;
        /* Capture PUSHFD uses the consumed sentinel slot; restore it. */
        fixture_write(NULL,INITIAL_ESP,RETURN_VA);
        fixture_capture(NULL,&oracle_result);
        FixtureRecord record={oracle_input,oracle_result};
        if(fwrite(&record,sizeof(record),1,vectors)!=1) fixture_fail("vectors_write",trial);
    }
    if(fclose(vectors)) fixture_fail("vectors_close",0);
    puts("{\"cases\":4096,\"reference\":\"original_x86_on_windows\",\"fixture_apis\":4}");
    return 0;
}
