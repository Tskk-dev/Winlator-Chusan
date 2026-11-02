#ifndef CHUSAN_RUNTIME_H
#define CHUSAN_RUNTIME_H
#include "x86.h"

typedef struct {uint32_t pc, last; void (*body)(X86*);} RuntimeBlock;
typedef struct {uint32_t base, size, flags;} RuntimeRegion;
typedef struct {uint32_t iat; const char *dll, *name;} RuntimeImport;
typedef struct {uint32_t start, end, index, callbacks, zero_fill;} RuntimeTls;
extern const RuntimeBlock runtime_blocks[];
extern const uint32_t runtime_block_count;

uint8_t runtime_read8(uint32_t address);
uint16_t runtime_read16(uint32_t address);
uint32_t runtime_read32(uint32_t address);
uint64_t runtime_read64(uint32_t address);
void runtime_write8(uint32_t address,uint8_t value);
void runtime_write16(uint32_t address,uint16_t value);
void runtime_write32(uint32_t address,uint32_t value);
void runtime_write64(uint32_t address,uint64_t value);

/* CALL ends the compiled block; the dispatch loop owns the guest call stack. */
#define CALL_FN(address) recomp_call(c,0x##address##u)
#define rd8 runtime_read8
#define rd16 runtime_read16
#define rd32 runtime_read32
#define rd64 runtime_read64
#define wr8 runtime_write8
#define wr16 runtime_write16
#define wr32 runtime_write32
#define wr64 runtime_write64
#endif
