/* Controlled ABI for one startup initializer; not a general Win32 runtime. */
#ifndef CHUSAN_STARTUP_FIXTURE_H
#define CHUSAN_STARTUP_FIXTURE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STARTUP_VA 0x00e3d42fu
#define STACK_BASE 0x0eff0000u
#define STACK_SIZE 0x10000u
#define INITIAL_ESP 0x0efff000u
#define RETURN_VA 0x0fdfff00u
#define SHIM_BASE 0x0ff00000u
#define COOKIE_VA 0x01c21eecu
#define COMPLEMENT_VA 0x01c21ee8u
#define STACK_WORDS 16u
#define CASE_COUNT 4096u

typedef struct {
    uint32_t r[8], flags, cookie, complement, entropy[6], stack_seed;
} FixtureInput;
typedef struct {
    uint32_t id, esp, continuation, argument, low, high, result;
} FixtureCall;
typedef struct {
    uint32_t r[8], flags, eip, cookie, complement, calls;
    FixtureCall trace[4];
    uint32_t stack[STACK_WORDS];
} FixtureResult;
typedef struct { FixtureInput input; FixtureResult expected; } FixtureRecord;

static const uint32_t fixture_iat[4] = {0x0221a250u,0x0221a30cu,0x0221a37cu,0x0221a3a8u};
static const uint32_t fixture_returns[4] = {0x00e3d469u,0x00e3d478u,0x00e3d481u,0x00e3d48eu};

static inline void fixture_fail(const char *kind, uint32_t address) {
    fprintf(stderr,"{\"error\":\"%s\",\"address\":\"0x%08x\"}\n",kind,address);
    exit(20);
}
static inline uint32_t fixture_read(const uint8_t *memory, uint32_t address) {
    uint32_t value; memcpy(&value,(const void*)((uintptr_t)memory+address),4); return value;
}
static inline void fixture_write(uint8_t *memory, uint32_t address, uint32_t value) {
    memcpy((void*)((uintptr_t)memory+address),&value,4);
}
static inline void fixture_seed(uint8_t *memory, const FixtureInput *input) {
    for(uint32_t i=0;i<STACK_SIZE/4;i++)
        fixture_write(memory,STACK_BASE+4*i,input->stack_seed^(0x9e3779b9u*i));
    fixture_write(memory,INITIAL_ESP,RETURN_VA);
    fixture_write(memory,COOKIE_VA,input->cookie);
    fixture_write(memory,COMPLEMENT_VA,input->complement);
    for(uint32_t i=0;i<4;i++) fixture_write(memory,fixture_iat[i],SHIM_BASE+16*i);
}
static inline uint32_t fixture_api(uint8_t *memory,const FixtureInput *input,
                                  FixtureResult *out,uint32_t id,uint32_t esp) {
    if(id>=4) fixture_fail("unknown_api",id);
    if(out->calls>=4 || id!=out->calls) fixture_fail("api_order",id);
    if(esp<INITIAL_ESP-40 || esp>INITIAL_ESP-36) fixture_fail("api_stack",esp);
    FixtureCall *call=&out->trace[out->calls++];
    call->id=id; call->esp=esp;
    call->continuation=fixture_read(memory,esp);
    if(call->continuation!=fixture_returns[id]) fixture_fail("api_continuation",call->continuation);
    if(id==0 || id==3) {
        call->argument=fixture_read(memory,esp+4);
        if(call->argument<STACK_BASE || (uint64_t)call->argument+8>STACK_BASE+STACK_SIZE)
            fixture_fail("fixture_write",call->argument);
        uint32_t offset=id==0?0:4;
        call->low=input->entropy[offset]; call->high=input->entropy[offset+1];
        fixture_write(memory,call->argument,call->low);
        fixture_write(memory,call->argument+4,call->high);
        call->result=id==0?0:1;
    } else call->result=input->entropy[id+1];
    return call->result;
}
static inline void fixture_capture(uint8_t *memory,FixtureResult *out) {
    out->cookie=fixture_read(memory,COOKIE_VA);
    out->complement=fixture_read(memory,COMPLEMENT_VA);
    memcpy(out->stack,(const void*)((uintptr_t)memory+INITIAL_ESP-40),sizeof(out->stack));
    /* AF is undefined after logical operations; compare only defined bits. */
    out->flags &= 0x0cc5u;
}
static inline uint32_t fixture_random(uint32_t *state) {
    uint32_t v=*state; v^=v<<13; v^=v>>17; v^=v<<5; return *state=v;
}
static inline FixtureInput fixture_input(uint32_t trial,uint32_t *random) {
    FixtureInput input;
    memset(&input,0,sizeof(input));
    for(unsigned i=0;i<8;i++) input.r[i]=fixture_random(random);
    input.r[4]=INITIAL_ESP;
    input.flags=0x202u|(fixture_random(random)&0xcd5u);
    input.cookie=trial%3==0?0xbb40e64eu:trial%3==1?(fixture_random(random)&0xffffu):fixture_random(random);
    input.complement=fixture_random(random);
    for(unsigned i=0;i<6;i++) input.entropy[i]=fixture_random(random);
    input.stack_seed=fixture_random(random);
    /* Force collision, zero-high-word and ordinary paths independently of entropy. */
    if(trial<8) {
        static const uint32_t desired[8]={0xbb40e64eu,0,1,0xffffu,0x10000u,0xffffffffu,0x80000000u,0x47110000u};
        input.cookie=0xbb40e64eu;
        input.entropy[5]=desired[trial]^input.entropy[0]^input.entropy[1]^input.entropy[2]
            ^input.entropy[3]^input.entropy[4]^(INITIAL_ESP-8);
    }
    return input;
}
#endif
