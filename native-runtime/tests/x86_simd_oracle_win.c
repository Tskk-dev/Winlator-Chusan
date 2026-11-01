#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *mapping;

static LONG CALLBACK fault(EXCEPTION_POINTERS *exception) {
    uintptr_t pc=(uintptr_t)exception->ExceptionRecord->ExceptionAddress;
    if(pc>=(uintptr_t)mapping+0x1000 && pc<(uintptr_t)mapping+0x2000) {
        printf("{\"fault\":\"0x%08lx\"}\n",exception->ExceptionRecord->ExceptionCode);
        fflush(stdout);ExitProcess(0);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc,char **argv) {
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb");
    if(!input)return 3;
    mapping=VirtualAlloc(NULL,0x2000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    if(!mapping)return 4;
    if(fread(mapping,1,0x2000,input)!=0x2000 || fgetc(input)!=EOF)return 5;
    fclose(input);
    if(mapping[0]!='M' || mapping[1]!='Z')return 6;
    FILE *relocations=fopen(argv[2],"rb");
    if(!relocations || fseek(relocations,0,SEEK_END))return 8;
    long bytes=ftell(relocations);
    if(bytes<0 || bytes%4 || bytes>1024 || fseek(relocations,0,SEEK_SET))return 9;
    for(long index=0;index<bytes/4;index++) {
        uint32_t offset,value;
        if(fread(&offset,4,1,relocations)!=1 || offset<0x1000 || offset>0x1ffc)return 10;
        memcpy(&value,mapping+offset,4);
        if(value<0x400000 || value>=0x403000)return 11;
        value=value-0x400000+(uint32_t)(uintptr_t)mapping;
        memcpy(mapping+offset,&value,4);
    }
    fclose(relocations);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    PVOID handler=AddVectoredExceptionHandler(1,fault);
    if(!handler)return 7;
    FlushInstructionCache(GetCurrentProcess(),mapping,0x2000);
    uint32_t (__cdecl *entry)(void)=(uint32_t (__cdecl *)(void))(mapping+0x1000);
    uint32_t value=entry();
    printf("{\"return\":%lu}\n",(unsigned long)value);
    RemoveVectoredExceptionHandler(handler);
    VirtualFree(mapping,0,MEM_RELEASE);
    return 0;
}
