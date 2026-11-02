/* No kit interpreter, loader, import table or permissive dispatch is linked. */
#include "x86.h"
#include "startup_fixture.h"

uint8_t *g_mem;
const int recomp_resumable_stacks=0;
static const FixtureInput *active_input;
static FixtureResult active_result;
static int returned;

static uint32_t strict_read32(uint32_t address) {
    int allowed=(address>=INITIAL_ESP-40 && address<=INITIAL_ESP+20)
        || address==COOKIE_VA || address==COMPLEMENT_VA;
    for(unsigned i=0;i<4;i++) allowed |= address==fixture_iat[i];
    if(!allowed || address%4) fixture_fail("memory_read",address);
    return fixture_read(g_mem,address);
}
static void strict_write32(uint32_t address,uint32_t value) {
    if(address%4 || !((address>=INITIAL_ESP-40 && address<=INITIAL_ESP+20)
                     || address==COOKIE_VA || address==COMPLEMENT_VA))
        fixture_fail("memory_write",address);
    fixture_write(g_mem,address,value);
}
/* This admitted body uses only rd32/wr32; checked again by the generator. */
#define rd32 strict_read32
#define wr32 strict_write32
#include "initializer.c"
#undef rd32
#undef wr32

void recomp_callback_return(X86 *c) {
    if(c->eip!=RETURN_VA || returned) fixture_fail("unknown_return",c->eip);
    returned=1;
}
int recomp_is_call_return(uint32_t target) {
    /* The sole generated RET must consume our top-level sentinel. */
    fixture_fail("unknown_return",target); return 0;
}
int recomp_module_is_call_return(uint32_t target) {fixture_fail("unknown_return",target);return 0;}
int32_t recomp_index_of(uint32_t target) {fixture_fail("unknown_target",target);return -1;}
int32_t recomp_module_lookup(uint32_t target) {fixture_fail("unknown_target",target);return -1;}
void recomp_jump(X86 *c,uint32_t target) {(void)c;fixture_fail("unknown_jump",target);}
void recomp_unknown_jump(X86 *c,uint32_t target) {recomp_jump(c,target);}
void recomp_unknown_call(X86 *c,uint32_t target) {(void)c;fixture_fail("unknown_call",target);}
void recomp_unmodelled(X86 *c,uint32_t target) {(void)c;fixture_fail("unmodelled",target);}
void recomp_call(X86 *c,uint32_t target) {
    if(target<SHIM_BASE || target>=SHIM_BASE+64 || (target-SHIM_BASE)%16)
        recomp_unknown_call(c,target);
    uint32_t id=(target-SHIM_BASE)/16;
    c->r[0]=fixture_api(g_mem,active_input,&active_result,id,c->r[4]);
    c->eip=strict_read32(c->r[4]);
    c->r[4]+=id==0 || id==3?8:4;
}

int main(int argc,char **argv) {
    if(argc!=2) return 2;
    g_mem=(uint8_t*)calloc(1,GUEST_SIZE);
    if(!g_mem) fixture_fail("arena_allocation",0);
    X86 cpu; memset(&cpu,0,sizeof(cpu));
    if(!strncmp(argv[1],"--negative=",11)) {
        const char *mode=argv[1]+11;
        if(!strcmp(mode,"unknown-call")) recomp_call(&cpu,0x12345678);
        if(!strcmp(mode,"unknown-jump")) recomp_jump(&cpu,0x12345678);
        if(!strcmp(mode,"unknown-return")) {cpu.eip=0x12345678;recomp_return(&cpu);}
        if(!strcmp(mode,"memory-read")) (void)strict_read32(0);
        if(!strcmp(mode,"memory-write")) strict_write32(0,0);
        if(!strcmp(mode,"unknown-api")) {
            FixtureInput input={0}; fixture_api(g_mem,&input,&active_result,4,INITIAL_ESP-40);
        }
        if(!strcmp(mode,"fixture-write")) {
            FixtureInput input={0};
            fixture_write(g_mem,INITIAL_ESP-40,fixture_returns[0]);
            fixture_write(g_mem,INITIAL_ESP-36,0);
            fixture_api(g_mem,&input,&active_result,0,INITIAL_ESP-40);
        }
        fixture_fail("negative_not_triggered",0);
    }
    FILE *vectors=fopen(argv[1],"rb");
    if(!vectors) fixture_fail("vectors_open",0);
    FixtureRecord record;
    unsigned trial=0;
    for(;trial<CASE_COUNT;trial++) {
        if(fread(&record,sizeof(record),1,vectors)!=1) fixture_fail("vector_count",trial);
        active_input=&record.input;
        memset(&active_result,0,sizeof(active_result));
        memset(&cpu,0,sizeof(cpu)); returned=0;
        memcpy(cpu.r,record.input.r,sizeof(cpu.r));
        x86_set_eflags(&cpu,record.input.flags);
        fixture_seed(g_mem,&record.input);
        fn_00e3d42f(&cpu);
        if(!returned) fixture_fail("missing_return",cpu.eip);
        memcpy(active_result.r,cpu.r,sizeof(cpu.r));
        active_result.eip=cpu.eip;
        active_result.flags=x86_get_eflags(&cpu);
        fixture_capture(g_mem,&active_result);
        if(memcmp(&active_result,&record.expected,sizeof(active_result))) {
            const uint32_t *actual=(const uint32_t*)&active_result;
            const uint32_t *expected=(const uint32_t*)&record.expected;
            for(unsigned i=0;i<sizeof(active_result)/4;i++) if(actual[i]!=expected[i]) {
                fprintf(stderr,"{\"error\":\"mismatch\",\"case\":%u,\"word\":%u,\"actual\":%u,\"expected\":%u}\n",
                        trial,i,actual[i],expected[i]); break;
            }
            return 21;
        }
    }
    if(fgetc(vectors)!=EOF || ferror(vectors)) fixture_fail("vector_count",trial);
    fclose(vectors); free(g_mem);
    printf("{\"cases\":%u,\"mismatches\":0,\"interpreter_linked\":false,\"fixture_apis\":4}\n",trial);
    return 0;
}
