#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "image_profile.h"
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

uint8_t *g_mem;
const int recomp_resumable_stacks=1;
uint32_t g_watch_base,g_watch_len,g_dirty_count;
RecompDirty g_dirty[RECOMP_DIRTY_SLOTS];
static uint8_t pages[GUEST_SIZE/4096];
static jmp_buf dispatch;
static X86 cpu;
static uint64_t executed, api_calls, feature_stub_calls, cpuid_stub_calls, environment_stub_calls, optional_export_calls;
static uint32_t block_pc, block_last;
typedef struct {uint32_t esp, cursor, end;int check_result;} InitializerFrame;
static InitializerFrame initializers[32];
static uint32_t initializer_depth;
static uint32_t c_initializers_completed,cpp_initializers_completed;
static uint32_t exit_callbacks[1024],exit_callback_count;
#define INITIALIZER_RETURN 0x0fd00000u
typedef struct {uint32_t references;int manual,signaled;uint16_t name[260];} EventObject;
typedef struct {uint32_t value;EventObject *event;} EventHandle;
static EventObject events[256];
static EventHandle event_handles[256];
static uint32_t handle_generation;
static uint32_t unhandled_exception_filter;
static uint8_t win32_tls_allocated[1088];
typedef struct {uint32_t base,size;int aligned;} HeapBlock;
static HeapBlock heap_blocks[8192];
static uint32_t heap_count;
#define HEAP_BASE 0x03000000u
#define HEAP_END 0x0e000000u

static void stop(const char *kind,uint32_t target) {
    printf("{\"stop\":\"%s\",\"target\":\"0x%08x\",\"block_pc\":\"0x%08x\","
           "\"block_last\":\"0x%08x\",\"blocks_executed\":%" PRIu64 ",\"api_calls\":%" PRIu64
           ",\"esp\":\"0x%08x\",\"c_initializers_completed\":%u,\"cpp_initializers_completed\":%u,"
           "\"stubs\":{\"optional_cpu_features_absent\":%" PRIu64
           ",\"baseline_cpuid\":%" PRIu64 ",\"empty_environment\":%" PRIu64
           ",\"optional_condition_variables_absent\":%" PRIu64 "},\"full_boot_verified\":false}\n",
           kind,target,block_pc,block_last,executed,api_calls,cpu.r[4],c_initializers_completed,cpp_initializers_completed,
           feature_stub_calls,cpuid_stub_calls,environment_stub_calls,optional_export_calls);
    exit(20);
}
static void permit(uint32_t base,uint32_t size,uint8_t flags) {
    if(!size || (uint64_t)base+size>GUEST_SIZE) stop("mapping_bounds",base);
    for(uint32_t p=base/4096;p<((uint64_t)base+size+4095)/4096;p++) pages[p]|=flags;
}
void runtime_memory_check(uint32_t address,uint32_t size,int write) {
    if(!size)return;
    if(address<0x10000 || (uint64_t)address+size>GUEST_SIZE) stop(write?"write_bounds":"read_bounds",address);
    uint8_t flag=write?2:1;
    for(uint32_t page=address/4096;page<((uint64_t)address+size+4095)/4096;page++)
        if(!(pages[page]&flag)) stop(write?"write_permission":"read_permission",address);
}
#define check runtime_memory_check
#define ACCESS(bits,type) \
type runtime_read##bits(uint32_t a) {type v;check(a,sizeof(v),0);memcpy(&v,g_mem+a,sizeof(v));return v;} \
void runtime_write##bits(uint32_t a,type v) {check(a,sizeof(v),1);memcpy(g_mem+a,&v,sizeof(v));}
ACCESS(8,uint8_t)
ACCESS(16,uint16_t)
ACCESS(32,uint32_t)
ACCESS(64,uint64_t)

void recomp_call(X86 *c,uint32_t target) {c->eip=target;longjmp(dispatch,1);}
void recomp_jump(X86 *c,uint32_t target) {recomp_call(c,target);}
void recomp_callback_return(X86 *c) {stop("entry_returned",c->r[0]);}
int recomp_is_call_return(uint32_t target) {(void)target;return 0;}
int recomp_module_is_call_return(uint32_t target) {(void)target;return 0;}
int32_t recomp_index_of(uint32_t target) {(void)target;return -1;}
int32_t recomp_module_lookup(uint32_t target) {(void)target;return -1;}
void recomp_unknown_call(X86 *c,uint32_t target) {(void)c;stop("unknown_call",target);}
void recomp_unknown_jump(X86 *c,uint32_t target) {(void)c;stop("unknown_jump",target);}
void recomp_unmodelled(X86 *c,uint32_t target) {(void)c;stop("unmodelled_instruction",target);}
void recomp_breakpoint(X86 *c,uint32_t target) {(void)c;stop("breakpoint",target);}
void recomp_div_error(X86 *c,uint32_t target) {(void)c;stop("divide_error",target);}
void recomp_int(X86 *c,uint32_t vector) {(void)c;stop("guest_exception",vector);}
void recomp_null_access(uint32_t a,int write) {stop(write?"null_write":"null_read",a);}
void recomp_watch_hit(uint32_t a,uint32_t n,uint64_t value) {(void)n;(void)value;stop("unexpected_watch",a);}
void recomp_cpuid(X86 *c) {
    /* shortcut: expose baseline x86 only; qualify optional SIMD before advertising it. */
    uint32_t leaf=c->r[0];cpuid_stub_calls++;
    c->r[0]=c->r[1]=c->r[2]=c->r[3]=0;
    if(leaf==0) {c->r[0]=1;c->r[3]=0x756e6547;c->r[2]=0x49656e69;c->r[1]=0x6c65746e;}
    if(leaf==1) c->r[0]=0x00000600;
}
void recomp_rdtsc(X86 *c) {(void)c;stop("pending_rdtsc",block_last);}
uint32_t recomp_in(X86 *c,uint32_t port,int size) {(void)c;(void)size;stop("port_read",port);return 0;}
void recomp_out(X86 *c,uint32_t port,uint32_t value,int size) {(void)c;(void)value;(void)size;stop("port_write",port);}
void recomp_cli(X86 *c) {(void)c;stop("privileged_cli",block_last);}
void recomp_sti(X86 *c) {(void)c;stop("privileged_sti",block_last);}
void recomp_hlt(X86 *c) {(void)c;stop("privileged_hlt",block_last);}

static const RuntimeBlock *find_block(uint32_t target) {
    uint32_t lo=0,hi=runtime_block_count;
    while(lo<hi) {uint32_t mid=lo+(hi-lo)/2;if(runtime_blocks[mid].pc<target)lo=mid+1;else hi=mid;}
    return lo<runtime_block_count && runtime_blocks[lo].pc==target?&runtime_blocks[lo]:NULL;
}
static uint64_t counter(void) {
#ifdef _WIN32
    LARGE_INTEGER value,frequency;QueryPerformanceCounter(&value);QueryPerformanceFrequency(&frequency);
    return (uint64_t)((long double)value.QuadPart*1000000000.0L/frequency.QuadPart);
#else
    struct timespec value;clock_gettime(CLOCK_MONOTONIC,&value);
    return (uint64_t)value.tv_sec*1000000000u+value.tv_nsec;
#endif
}
static void api_return(X86 *c,uint32_t args,uint32_t result) {
    c->r[0]=result;c->eip=rd32(c->r[4]);c->r[4]+=4+4*args;
}
static void guest_name(uint32_t address,int wide,char out[128]) {
    for(unsigned i=0;i<128;i++) {
        uint32_t ch=wide?rd16(address+2*i):rd8(address+i);
        if(ch>127)stop("non_ascii_module_or_export",address);
        out[i]=(char)ch;if(!ch)return;
    }
    stop("module_or_export_name_length",address);
}
static EventHandle *event_handle(uint32_t value) {
    EventHandle *handle=&event_handles[value&255u];
    return value && handle->value==value?handle:NULL;
}
static uint32_t win32_tls_slot(X86 *c,uint32_t index) {
    return index<64?c->fs_base+0xe10+index*4:0x0fe20000u+(index-64)*4;
}
static uint32_t heap_allocate(uint32_t size,uint32_t alignment,int aligned) {
    if(!alignment || (alignment&(alignment-1)))stop("heap_alignment",alignment);
    if(alignment<4)alignment=4;
    if(!size)size=1;
    uint64_t cursor=HEAP_BASE;
    for(uint32_t index=0;index<=heap_count;index++) {
        uint64_t base=(cursor+alignment-1)&~((uint64_t)alignment-1);
        uint32_t limit=index<heap_count?heap_blocks[index].base:HEAP_END;
        if(base+size<=limit && heap_count<8192) {
            memmove(heap_blocks+index+1,heap_blocks+index,(heap_count-index)*sizeof(HeapBlock));
            heap_blocks[index]=(HeapBlock){(uint32_t)base,size,aligned};heap_count++;
            permit((uint32_t)base,size,3);return (uint32_t)base;
        }
        if(index<heap_count)cursor=(uint64_t)heap_blocks[index].base+heap_blocks[index].size;
    }
    wr32(0x0fe50028u,12);return 0; /* ENOMEM */
}
static void heap_release(uint32_t address,int aligned) {
    if(!address)return;
    for(uint32_t index=0;index<heap_count;index++)if(heap_blocks[index].base==address) {
        if(heap_blocks[index].aligned!=aligned)stop("heap_free_kind",address);
        memmove(heap_blocks+index,heap_blocks+index+1,(heap_count-index-1)*sizeof(HeapBlock));
        heap_count--;return;
    }
    stop("heap_invalid_free",address);
}
static uint32_t create_event(X86 *c,int wide) {
    uint32_t attributes=rd32(c->r[4]+4),name_address=rd32(c->r[4]+16);
    if(attributes)stop("event_security_attributes",attributes);
    uint16_t name[260]={0};
    if(name_address) {
        unsigned i;
        for(i=0;i<260;i++) {
            name[i]=wide?rd16(name_address+2*i):rd8(name_address+i);
            if(!wide && name[i]>127)stop("event_ansi_codepage",name_address);
            if(!name[i])break;
        }
        if(i==260)stop("event_name_length",name_address);
    }
    EventHandle *handle=NULL;
    for(unsigned i=0;i<256;i++)if(!event_handles[i].value){handle=&event_handles[i];break;}
    if(!handle || handle_generation==0x3fffffu)stop("event_handle_capacity",0);
    EventObject *event=NULL;
    if(name[0])for(unsigned i=0;i<256;i++)
        if(events[i].references && !memcmp(events[i].name,name,sizeof(name))){event=&events[i];break;}
    uint32_t error=183; /* existing named objects ignore the supplied reset/initial state */
    if(!event) {
        for(unsigned i=0;i<256;i++)if(!events[i].references){event=&events[i];break;}
        if(!event)stop("event_object_capacity",0);
        event->manual=rd32(c->r[4]+8)!=0;event->signaled=rd32(c->r[4]+12)!=0;
        memcpy(event->name,name,sizeof(name));error=0;
    }
    event->references++;handle->event=event;
    handle->value=0x40000000u|(++handle_generation<<8)|(uint32_t)(handle-event_handles);
    wr32(c->fs_base+0x34,error);return handle->value;
}
static void initializer_next(X86 *c,int returned) {
    InitializerFrame *frame=&initializers[initializer_depth-1];
    if(c->r[4]!=frame->esp)stop("initializer_stack",c->r[4]);
    if(returned) {
        if(frame->check_result)c_initializers_completed++;
        else cpp_initializers_completed++;
    }
    if(!(returned && frame->check_result && c->r[0])) {
        while(frame->cursor<frame->end) {
            uint32_t callback=rd32(frame->cursor);frame->cursor+=4;
            if(!callback)continue;
            c->r[4]-=4;wr32(c->r[4],INITIALIZER_RETURN+16*(initializer_depth-1));
            c->eip=callback;return;
        }
        c->r[0]=0;
    }
    initializer_depth--;api_return(c,0,c->r[0]); /* cdecl: caller owns table arguments */
}
static void api(X86 *c,uint32_t index) {
    const RuntimeImport *import=&image_imports[index];
    const char *name=import->name;
    uint32_t a0=0;
    api_calls++;
    if(api_calls<=40) fprintf(stderr,"API %s!%s from %08x\n",import->dll,name,block_last);
    if(!strcmp(name,"GetCurrentThreadId")) {api_return(c,0,1);return;}
    if(!strcmp(name,"GetCurrentProcessId")) {api_return(c,0,1);return;}
    if(!strcmp(name,"GetLastError")) {api_return(c,0,rd32(c->fs_base+0x34));return;}
    if(!strcmp(name,"SetLastError")) {wr32(c->fs_base+0x34,rd32(c->r[4]+4));api_return(c,1,0);return;}
    if(!strcmp(name,"SetUnhandledExceptionFilter")) {
        uint32_t previous=unhandled_exception_filter;
        a0=rd32(c->r[4]+4);if(a0)check(a0,1,0);
        /* Guest exception delivery still stops explicitly in recomp_int. */
        unhandled_exception_filter=a0;api_return(c,1,previous);return;
    }
    if(!strcmp(name,"TlsAlloc")) {
        for(uint32_t index=0;index<1088;index++)if(!win32_tls_allocated[index]) {
            win32_tls_allocated[index]=1;wr32(win32_tls_slot(c,index),0);api_return(c,0,index);return;
        }
        wr32(c->fs_base+0x34,8);api_return(c,0,0xffffffffu);return;
    }
    if(!strcmp(name,"TlsGetValue") || !strcmp(name,"TlsSetValue") || !strcmp(name,"TlsFree")) {
        uint32_t index=rd32(c->r[4]+4),result=0,args=!strcmp(name,"TlsSetValue")?2:1;
        if(index>=1088 || !win32_tls_allocated[index])wr32(c->fs_base+0x34,87);
        else if(!strcmp(name,"TlsGetValue")){result=rd32(win32_tls_slot(c,index));wr32(c->fs_base+0x34,0);}
        else {
            if(!strcmp(name,"TlsFree")){win32_tls_allocated[index]=0;wr32(win32_tls_slot(c,index),0);}
            else wr32(win32_tls_slot(c,index),rd32(c->r[4]+8));
            result=1;
        }
        api_return(c,args,result);return;
    }
    if(!strcmp(name,"memset")) {
        uint32_t destination=rd32(c->r[4]+4),value=rd32(c->r[4]+8),size=rd32(c->r[4]+12);
        check(destination,size,1);if(size)memset(g_mem+destination,(uint8_t)value,size);
        api_return(c,0,destination);return;
    }
    if(!strcmp(name,"memcpy") || !strcmp(name,"memmove")) {
        uint32_t destination=rd32(c->r[4]+4),source=rd32(c->r[4]+8),size=rd32(c->r[4]+12);
        check(destination,size,1);check(source,size,0);
        if(size)memmove(g_mem+destination,g_mem+source,size);
        api_return(c,0,destination);return;
    }
    if(!strcmp(name,"_aligned_malloc") || !strcmp(name,"malloc")) {
        int aligned=!strcmp(name,"_aligned_malloc");
        uint32_t size=rd32(c->r[4]+4),alignment=aligned?rd32(c->r[4]+8):8;
        api_return(c,0,heap_allocate(size,alignment,aligned));return;
    }
    if(!strcmp(name,"_aligned_free") || !strcmp(name,"free")) {
        heap_release(rd32(c->r[4]+4),!strcmp(name,"_aligned_free"));api_return(c,0,0);return;
    }
    if(!strcmp(name,"_errno")) {api_return(c,0,0x0fe50028u);return;}
    if(!strcmp(name,"_mktime64")) {
        _Static_assert(sizeof(time_t)>=8,"Calendar conversion requires 64-bit time_t");
        a0=rd32(c->r[4]+4);check(a0,36,0);check(a0,36,1);
        struct tm value={0};
        int *fields[]={&value.tm_sec,&value.tm_min,&value.tm_hour,&value.tm_mday,&value.tm_mon,
                       &value.tm_year,&value.tm_wday,&value.tm_yday,&value.tm_isdst};
        for(unsigned i=0;i<9;i++)*fields[i]=(int32_t)rd32(a0+4*i);
        time_t converted=mktime(&value);
        uint64_t result=UINT64_MAX;
        if(converted<0 || (uint64_t)converted>32535215999ull)wr32(0x0fe50028u,22);
        else {
            result=(uint64_t)converted;
            for(unsigned i=0;i<9;i++)wr32(a0+4*i,(uint32_t)*fields[i]);
        }
        c->r[2]=(uint32_t)(result>>32);api_return(c,0,(uint32_t)result);return;
    }
    if(!strcmp(name,"wcsncpy_s")) {
        uint32_t destination=rd32(c->r[4]+4),capacity=rd32(c->r[4]+8);
        uint32_t source=rd32(c->r[4]+12),count=rd32(c->r[4]+16);
        if(!destination || !capacity || (!source && count))stop("wstring_invalid_parameter",destination);
        if(capacity>GUEST_SIZE/2)stop("wstring_capacity",capacity);
        check(destination,capacity*2,1);
        uint32_t limit=count==0xffffffffu?capacity-1:count,index=0;
        while(index<limit && index<capacity) {
            uint16_t character=rd16(source+2*index);
            if(!character)break;
            wr16(destination+2*index,character);index++;
        }
        if(index==capacity) {
            wr16(destination,0);wr32(0x0fe50028u,34);
            stop("wstring_range_invalid_parameter",destination);
        }
        wr16(destination+2*index,0);
        uint32_t result=count==0xffffffffu && rd16(source+2*index)?80:0;
        api_return(c,0,result);return;
    }
    if(!strcmp(name,"__std_type_info_compare")) {
        uint32_t left=rd32(c->r[4]+4),right=rd32(c->r[4]+8);
        if(left==right){api_return(c,0,0);return;}
        /* x86 data has a four-byte cached-name pointer; comparison skips the decorated prefix. */
        for(uint32_t index=5;;index++) {
            uint8_t a=rd8(left+index),b=rd8(right+index);
            if(a!=b){api_return(c,0,a<b?0xffffffffu:1);return;}
            if(!a){api_return(c,0,0);return;}
        }
    }
    if(!strcmp(name,"CreateEventW") || !strcmp(name,"CreateEventA")) {
        api_return(c,4,create_event(c,!strcmp(name,"CreateEventW")));return;
    }
    if(!strcmp(name,"SetEvent") || !strcmp(name,"ResetEvent") || !strcmp(name,"CloseHandle")) {
        EventHandle *handle=event_handle(rd32(c->r[4]+4));
        if(!handle){wr32(c->fs_base+0x34,6);api_return(c,1,0);return;}
        if(!strcmp(name,"CloseHandle")){handle->event->references--;handle->value=0;handle->event=NULL;}
        else handle->event->signaled=!strcmp(name,"SetEvent");
        api_return(c,1,1);return;
    }
    if(!strcmp(name,"WaitForSingleObject")) {
        uint32_t value=rd32(c->r[4]+4),milliseconds=rd32(c->r[4]+8);
        EventHandle *handle=event_handle(value);
        if(!handle){wr32(c->fs_base+0x34,6);api_return(c,2,0xffffffffu);return;}
        if(handle->event->signaled) {
            if(!handle->event->manual)handle->event->signaled=0;
            api_return(c,2,0);return;
        }
        /* shortcut: blocking waits need guest scheduling; never manufacture a wake-up. */
        if(milliseconds){fprintf(stderr,"Pending event wait, timeout %u ms\n",milliseconds);stop("pending_event_wait",value);}
        api_return(c,2,258);return;
    }
    if(!strcmp(name,"GetModuleHandleW")) {
        a0=rd32(c->r[4]+4);
        if(!a0){api_return(c,1,RUNTIME_IMAGE_BASE);return;}
        char module[128];guest_name(a0,1,module);
        for(unsigned i=0;module[i];i++)if(module[i]>='A' && module[i]<='Z')module[i]+='a'-'A';
        if(strcmp(module,"kernel32.dll")){fprintf(stderr,"Pending module %s\n",module);stop("module_lookup",a0);}
        api_return(c,1,0x0fd80000u);return;
    }
    if(!strcmp(name,"GetProcAddress")) {
        uint32_t handle=rd32(c->r[4]+4),symbol=rd32(c->r[4]+8);
        char export_name[128];guest_name(symbol,0,export_name);
        if(handle==0x0fd80000u && (!strcmp(export_name,"InitializeConditionVariable")
             || !strcmp(export_name,"SleepConditionVariableCS") || !strcmp(export_name,"WakeAllConditionVariable"))) {
            /* shortcut: select the CRT's inspected legacy condition-variable path. */
            optional_export_calls++;wr32(c->fs_base+0x34,127);
            fprintf(stderr,"Optional export unavailable: %s\n",export_name);api_return(c,2,0);return;
        }
        fprintf(stderr,"Pending export %s\n",export_name);stop("dynamic_export",symbol);
    }
    if(!strcmp(name,"InitializeSListHead")) {
        a0=rd32(c->r[4]+4);if(a0%8)stop("slist_alignment",a0);
        wr64(a0,0);api_return(c,1,0);return;
    }
    if(!strcmp(name,"EnterCriticalSection") || !strcmp(name,"TryEnterCriticalSection")
       || !strcmp(name,"LeaveCriticalSection") || !strcmp(name,"DeleteCriticalSection")) {
        a0=rd32(c->r[4]+4);check(a0,24,1);check(a0,24,0);
        uint32_t recursion=rd32(a0+8),owner=rd32(a0+12);
        /* Only the primary guest thread exists; another owner needs scheduling. */
        if(owner && owner!=1)stop("critical_section_other_owner",a0);
        if((!owner && recursion) || (owner && !recursion) || rd32(a0+4)!=recursion-1)
            stop("critical_section_state",a0);
        if(!strcmp(name,"DeleteCriticalSection")) {
            if(recursion)stop("critical_section_delete_locked",a0);
            memset(g_mem+a0,0,24);api_return(c,1,0);return;
        }
        if(!strcmp(name,"LeaveCriticalSection")) {
            if(!recursion)stop("critical_section_not_owned",a0);
            recursion--;
        } else {
            if(recursion==0x7fffffffu)stop("critical_section_recursion",a0);
            recursion++;
        }
        wr32(a0+4,recursion-1);wr32(a0+8,recursion);wr32(a0+12,recursion?1:0);
        api_return(c,1,!strcmp(name,"TryEnterCriticalSection")?1:0);return;
    }
    if(!strcmp(name,"__vcrt_InitializeCriticalSectionEx") || !strcmp(name,"InitializeCriticalSection")
       || !strcmp(name,"InitializeCriticalSectionEx")) {
        int wrapper=!strcmp(name,"__vcrt_InitializeCriticalSectionEx");
        int extended=wrapper || !strcmp(name,"InitializeCriticalSectionEx");
        a0=rd32(c->r[4]+4);uint32_t spin=extended?rd32(c->r[4]+8):0,flags=extended?rd32(c->r[4]+12):0;
        if(flags)stop("critical_section_flags",flags);
        check(a0,24,1);memset(g_mem+a0,0,24);
        wr32(a0+4,0xffffffffu);wr32(a0+20,spin);
        api_return(c,wrapper?0:(extended?3:1),extended?1:0);return; /* vcruntime wrapper is cdecl */
    }
    if(!strcmp(name,"IsProcessorFeaturePresent")) {
        /* Optional feature queries select the baseline CRT path. */
        feature_stub_calls++;api_return(c,1,0);return;
    }
    if(!strcmp(name,"GetSystemTimeAsFileTime")) {
        struct timespec now;timespec_get(&now,TIME_UTC);
        a0=rd32(c->r[4]+4);
        wr64(a0,((uint64_t)now.tv_sec+11644473600ull)*10000000ull+now.tv_nsec/100);
        api_return(c,1,0);return;
    }
    if(!strcmp(name,"QueryPerformanceCounter")) {a0=rd32(c->r[4]+4);wr64(a0,counter());api_return(c,1,1);return;}
    if(!strcmp(name,"QueryPerformanceFrequency")) {a0=rd32(c->r[4]+4);wr64(a0,1000000000);api_return(c,1,1);return;}
    if(!strcmp(name,"_set_app_type")) {wr32(0x0fe50000u,rd32(c->r[4]+4));api_return(c,0,0);return;}
    if(!strcmp(name,"_set_fmode")) {wr32(0x0fe50004u,rd32(c->r[4]+4));api_return(c,0,0);return;}
    if(!strcmp(name,"__p__commode")) {api_return(c,0,0x0fe50008u);return;}
    if(!strcmp(name,"_crt_atexit") || !strcmp(name,"atexit")) {
        if(exit_callback_count==1024)stop("exit_callback_capacity",exit_callback_count);
        a0=rd32(c->r[4]+4);check(a0,1,0);
        exit_callbacks[exit_callback_count++]=a0;api_return(c,0,0);return;
    }
    if(!strcmp(name,"_configure_narrow_argv")) {
        a0=rd32(c->r[4]+4);
        if(a0>1)stop("expanded_argv_not_implemented",a0);
        wr32(0x0fe5000cu,a0);api_return(c,0,0);return;
    }
    if(!strcmp(name,"__setusermatherr")) {wr32(0x0fe50010u,rd32(c->r[4]+4));api_return(c,0,0);return;}
    if(!strcmp(name,"_configthreadlocale")) {
        uint32_t mode=rd32(c->r[4]+4),previous=rd32(0x0fe50014u);
        if(mode>2 && mode!=0xffffffffu)stop("locale_configuration",mode);
        if(mode==0xffffffffu)wr32(0x0fe50018u,mode);
        else if(mode)wr32(0x0fe50014u,mode);
        api_return(c,0,previous);return;
    }
    if(!strcmp(name,"_initialize_narrow_environment")) {
        /* shortcut: empty process environment during startup; add configured values when reached. */
        environment_stub_calls++;wr32(0x0fe50020u,0);wr32(0x0fe50024u,0x0fe50020u);
        api_return(c,0,0);return;
    }
    if(!strcmp(name,"_get_initial_narrow_environment")) {api_return(c,0,0x0fe50020u);return;}
    if(!strcmp(name,"__p__environ")) {api_return(c,0,0x0fe50024u);return;}
    if(!strcmp(name,"_set_new_mode")) {
        uint32_t previous=rd32(0x0fe5001cu);a0=rd32(c->r[4]+4);
        if(a0>1)stop("new_mode",a0);
        wr32(0x0fe5001cu,a0);api_return(c,0,previous);return;
    }
    if(!strcmp(name,"_controlfp_s")) {
        uint32_t output=rd32(c->r[4]+4),value=rd32(c->r[4]+8),mask=rd32(c->r[4]+12);
        /* Reached CRT request is PC_53. Other modes need numerical qualification. */
        if(mask && (mask!=0x30000u || (value&mask)!=0x10000u))stop("fp_control_mode",mask);
        if(mask)c->fpu_cw=(c->fpu_cw&~0x300u)|0x200u;
        if(c->fpu_cw!=0x027fu)stop("fp_control_query",c->fpu_cw);
        if(output)wr32(output,0x0009001fu);
        api_return(c,0,0);return;
    }
    if(!strcmp(name,"_initterm_e") || !strcmp(name,"_initterm")) {
        if(initializer_depth==32)stop("initializer_depth",initializer_depth);
        uint32_t first=rd32(c->r[4]+4),last=rd32(c->r[4]+8);
        if(first%4 || last%4 || last<first || last-first>0x100000)stop("initializer_table",first);
        check(first,last-first,0);
        initializers[initializer_depth++]=(InitializerFrame){c->r[4],first,last,!strcmp(name,"_initterm_e")};
        initializer_next(c,0);return;
    }
    fprintf(stderr,"Unsupported import %s!%s (IAT %08x)\n",import->dll,name,import->iat);
    stop("missing_import",import->iat);
}

int main(int argc,char **argv) {
    if(argc!=2) {fputs("Usage: runtime image.bin\n",stderr);return 2;}
    g_mem=(uint8_t*)calloc(1,GUEST_SIZE);
    if(!g_mem) stop("memory_allocation",0);
    FILE *image=fopen(argv[1],"rb");
    if(!image || fread(g_mem+RUNTIME_IMAGE_BASE,1,RUNTIME_IMAGE_SIZE,image)!=RUNTIME_IMAGE_SIZE || fgetc(image)!=EOF)
        stop("image_size",0);
    fclose(image);
    for(unsigned i=0;i<sizeof(image_regions)/sizeof(image_regions[0]);i++)
        permit(image_regions[i].base,image_regions[i].size,image_regions[i].flags);
    permit(0x0ef00000u,0x100000u,3); /* primary guest stack */
    permit(0x0fe00000u,0x60000u,3); /* TEB, PEB, TLS slots and initial TLS data */
    for(unsigned i=0;i<sizeof(image_imports)/sizeof(image_imports[0]);i++) {
        /* Loader writes IAT slots before guest protections take effect. */
        uint32_t value=GUEST_SHIM_BASE+16*i;
        memcpy(g_mem+image_imports[i].iat,&value,4);
    }
    memset(&cpu,0,sizeof(cpu));cpu.fs_base=0x0fe00000u;
    wr32(cpu.fs_base,0xffffffffu);wr32(cpu.fs_base+4,0x0f000000u);wr32(cpu.fs_base+8,0x0ef00000u);
    wr32(cpu.fs_base+0x18,cpu.fs_base);wr32(cpu.fs_base+0x2c,0x0fe30000u);wr32(cpu.fs_base+0x30,0x0fe10000u);
    wr32(cpu.fs_base+0xf94,0x0fe20000u); /* Win32 expansion slots are separate from module static TLS. */
    if(image_tls.start) {
        if(image_tls.end<image_tls.start || (uint64_t)image_tls.end-image_tls.start+image_tls.zero_fill>0x10000)
            stop("tls_size",image_tls.start);
        check(image_tls.start,image_tls.end-image_tls.start,0);
        memcpy(g_mem+0x0fe40000u,g_mem+image_tls.start,image_tls.end-image_tls.start);
        wr32(image_tls.index,0);wr32(0x0fe30000u,0x0fe40000u);
        if(image_tls.callbacks && rd32(image_tls.callbacks)) stop("pending_tls_callback",rd32(image_tls.callbacks));
    }
    cpu.r[4]=0x0efff000u;wr32(cpu.r[4],GUEST_RETURN_SENTINEL);
    cpu.eip=RUNTIME_ENTRY;x86_set_eflags(&cpu,0x202u);
    cpu.fpu_cw=0x027f;cpu.fpu_tag=0xffff;
    wr32(0x0fe50014u,2); /* default C locale, per-thread locale disabled */
    (void)setjmp(dispatch);
    while(executed<1000000) {
        if(cpu.eip>=INITIALIZER_RETURN && cpu.eip<INITIALIZER_RETURN+16*32) {
            if(!initializer_depth || cpu.eip!=INITIALIZER_RETURN+16*(initializer_depth-1))stop("initializer_return",cpu.eip);
            initializer_next(&cpu,1);continue;
        }
        if(cpu.eip>=GUEST_SHIM_BASE && cpu.eip<GUEST_SHIM_BASE+sizeof(image_imports)/sizeof(image_imports[0])*16) {
            if((cpu.eip-GUEST_SHIM_BASE)%16)stop("unaligned_import",cpu.eip);
            api(&cpu,(cpu.eip-GUEST_SHIM_BASE)/16);continue;
        }
        const RuntimeBlock *block=find_block(cpu.eip);
        if(!block)stop("missing_code",cpu.eip);
        block_pc=block->pc;block_last=block->last;executed++;
        block->body(&cpu);
    }
    stop("block_limit",cpu.eip);return 20;
}
