#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "image_profile.h"
#include <errno.h>
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
static uint64_t optional_filesystem_export_calls;
static uint64_t fixed_stream_locale_calls;
static uint64_t single_thread_cpp_lock_calls;
static uint64_t virtual_system_info_calls;
static uint64_t optional_memory_scan_calls;
static uint64_t fixed_codecvt_calls;
static uint32_t optional_filesystem_exports_seen;
static uint32_t block_pc, block_last;
typedef struct {uint32_t esp, cursor, end;int check_result;} InitializerFrame;
static InitializerFrame initializers[32];
static uint32_t initializer_depth;
static uint32_t c_initializers_completed,cpp_initializers_completed;
static uint32_t exit_callbacks[1024],exit_callback_count;
#define INITIALIZER_RETURN 0x0fd00000u
enum {OBJECT_EVENT=1,OBJECT_MUTEX=2};
typedef struct {uint32_t references,kind,owner,recursion;int manual,signaled;uint16_t name[260];} KernelObject;
typedef struct {uint32_t value;KernelObject *object;} KernelHandle;
static KernelObject kernel_objects[256];
static KernelHandle kernel_handles[256];
static uint32_t handle_generation;
static uint32_t unhandled_exception_filter;
static uint32_t process_error_mode;
static uint8_t win32_tls_allocated[1088];
typedef struct {uint32_t base,size;int aligned;} HeapBlock;
static HeapBlock heap_blocks[8192];
static uint32_t heap_count;
typedef struct {uint32_t object;FILE *host;int binary;} NativeFile;
static NativeFile native_files[64];
static const char *content_root;
#define HEAP_BASE 0x03000000u
#define HEAP_END 0x0e000000u
#define BASIC_IOS_VTABLE 0x0fe52004u
#define BASIC_IOS_DELETING_DTOR 0x0fd40000u
#define BASIC_ISTREAM_VTABLE 0x0fe5200cu
#define CLASSIC_LOCALE_VTABLE 0x0fe52024u
#define BASIC_STREAMBUF_VTABLE 0x0fe5201cu
#define CLASSIC_LOCALE 0x0fe53000u
#define CLASSIC_LOCALE_NAME 0x0fe52100u
#define CLASSIC_CODECVT_ID 0x0fe52110u
#define CLASSIC_FACETS 0x0fe52120u
#define CLASSIC_CODECVT 0x0fe53040u
#define CLASSIC_CODECVT_VTABLE 0x0fe52050u

static void stop(const char *kind,uint32_t target) {
    printf("{\"stop\":\"%s\",\"target\":\"0x%08x\",\"block_pc\":\"0x%08x\","
           "\"block_last\":\"0x%08x\",\"blocks_executed\":%" PRIu64 ",\"api_calls\":%" PRIu64
           ",\"esp\":\"0x%08x\",\"c_initializers_completed\":%u,\"cpp_initializers_completed\":%u,"
           "\"stubs\":{\"optional_cpu_features_absent\":%" PRIu64
           ",\"baseline_cpuid\":%" PRIu64 ",\"empty_environment\":%" PRIu64
           ",\"optional_condition_variables_absent\":%" PRIu64
           ",\"optional_filesystem_exports_absent\":%" PRIu64
           ",\"fixed_c_stream_locale\":%" PRIu64 ",\"single_thread_cpp_locks\":%" PRIu64
           ",\"virtual_x86_system_info\":%" PRIu64 ",\"optional_memory_scan_queries\":%" PRIu64
           ",\"fixed_c_codecvt_queries\":%" PRIu64 "},\"full_boot_verified\":false}\n",
           kind,target,block_pc,block_last,executed,api_calls,cpu.r[4],c_initializers_completed,cpp_initializers_completed,
           feature_stub_calls,cpuid_stub_calls,environment_stub_calls,optional_export_calls,optional_filesystem_export_calls,fixed_stream_locale_calls,single_thread_cpp_lock_calls,virtual_system_info_calls,optional_memory_scan_calls,fixed_codecvt_calls);
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
static KernelHandle *kernel_handle(uint32_t value) {
    KernelHandle *handle=&kernel_handles[value&255u];
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
static uint32_t stream_locale(void) {
    uint32_t locale=heap_allocate(8,4,0);if(!locale)stop("cpp_locale_allocation",0);
    /* shortcut: fixed classic C locale; retain guest ownership and readable state. */
    wr32(locale+4,CLASSIC_LOCALE);wr32(CLASSIC_LOCALE+4,rd32(CLASSIC_LOCALE+4)+1);
    if(!fixed_stream_locale_calls++)fputs("Compatibility stream locale: C\n",stderr);
    return locale;
}
static void streambuf_init(uint32_t object) {
    check(object,0x34,1);
    for(uint32_t offset=4;offset<=0x28;offset+=0x10) {
        wr32(object+offset,0);wr32(object+offset+4,0);
        wr32(object+offset+8,object+offset);wr32(object+offset+12,object+offset+4);
    }
}
static void classic_codecvt_used(void) {
    if(!fixed_codecvt_calls++)fputs("Compatibility: fixed C character conversion\n",stderr);
}
static uint32_t file_open(uint32_t filename,uint32_t mode,uint32_t share) {
    if((mode!=1 && mode!=0x21) || share!=0x40)stop("file_open_mode",mode);
    char relative[1024],path[4096];unsigned length=0;
    while(length<sizeof(relative)-1) {
        uint8_t ch=rd8(filename+length);if(!ch)break;
        if(ch>127)stop("file_path_encoding",filename);
        relative[length++]=ch=='\\'?'/':(char)ch;
    }
    relative[length]=0;
    int invalid=!length || length==sizeof(relative)-1 || relative[0]=='/' || strchr(relative,':')!=NULL;
    for(unsigned begin=0;begin<length;) {
        unsigned end=begin;while(end<length && relative[end]!='/')end++;
        if(end-begin==2 && relative[begin]=='.' && relative[begin+1]=='.')invalid=1;
        begin=end+1;
    }
    if(invalid){wr32(0x0fe50028u,22);return 0;}
    if(!content_root){wr32(0x0fe50028u,2);return 0;}
    int size=snprintf(path,sizeof(path),"%s/%s",content_root,relative);
    if(size<0 || (unsigned)size>=sizeof(path)){wr32(0x0fe50028u,22);return 0;}
    NativeFile *file=NULL;
    for(unsigned i=0;i<64;i++)if(!native_files[i].object){file=&native_files[i];break;}
    if(!file){wr32(0x0fe50028u,24);return 0;}
    FILE *host=fopen(path,"rb");
    if(!host){wr32(0x0fe50028u,errno==EACCES?13:2);return 0;}
    uint32_t object=heap_allocate(16,4,0);
    if(!object){fclose(host);return 0;}
    memset(g_mem+object,0,16);*file=(NativeFile){object,host,!!(mode&0x20)};
    fprintf(stderr,"Opened read-only content: %s\n",relative);return object;
}
static NativeFile *file_lookup(uint32_t object) {
    for(unsigned i=0;i<64;i++)if(native_files[i].object==object && object)return &native_files[i];
    stop("file_handle",object);return NULL;
}
static uint32_t create_kernel_object(X86 *c,uint32_t kind,int wide) {
    uint32_t attributes=rd32(c->r[4]+4),name_address=rd32(c->r[4]+(kind==OBJECT_EVENT?16:12));
    if(attributes)stop("kernel_object_security_attributes",attributes);
    uint16_t name[260]={0};
    if(name_address) {
        unsigned i;
        for(i=0;i<260;i++) {
            name[i]=wide?rd16(name_address+2*i):rd8(name_address+i);
            if(!wide && name[i]>127)stop("kernel_object_ansi_codepage",name_address);
            if(!name[i])break;
        }
        if(i==260)stop("kernel_object_name_length",name_address);
    }
    KernelHandle *handle=NULL;
    for(unsigned i=0;i<256;i++)if(!kernel_handles[i].value){handle=&kernel_handles[i];break;}
    if(!handle || handle_generation==0x3fffffu)stop("kernel_handle_capacity",0);
    KernelObject *object=NULL;
    if(name[0])for(unsigned i=0;i<256;i++)
        if(kernel_objects[i].references && !memcmp(kernel_objects[i].name,name,sizeof(name))){object=&kernel_objects[i];break;}
    if(object && object->kind!=kind){wr32(c->fs_base+0x34,6);return 0;}
    uint32_t error=183; /* existing named objects ignore the supplied reset/initial state */
    if(!object) {
        for(unsigned i=0;i<256;i++)if(!kernel_objects[i].references){object=&kernel_objects[i];break;}
        if(!object)stop("kernel_object_capacity",0);
        memset(object,0,sizeof(*object));object->kind=kind;
        if(kind==OBJECT_EVENT){object->manual=rd32(c->r[4]+8)!=0;object->signaled=rd32(c->r[4]+12)!=0;}
        else if(rd32(c->r[4]+8)){object->owner=1;object->recursion=1;}
        memcpy(object->name,name,sizeof(name));error=0;
    }
    object->references++;handle->object=object;
    handle->value=0x40000000u|(++handle_generation<<8)|(uint32_t)(handle-kernel_handles);
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
    if(!strcmp(name,"GetCurrentProcess")) {api_return(c,0,0xffffffffu);return;}
    if(!strcmp(name,"GetSystemInfo")) {
        /* shortcut: report the bounded, single-processor x86 guest machine. */
        uint32_t output=rd32(c->r[4]+4);check(output,36,1);
        const uint32_t fields[]={0,4096,0x10000,GUEST_SIZE-1,1,1,586,0x10000,6};
        for(unsigned i=0;i<9;i++)wr32(output+4*i,fields[i]);
        if(!virtual_system_info_calls++)fputs("Compatibility: single-processor x86 system information\n",stderr);
        api_return(c,1,0);return;
    }
    if(!strcmp(name,"VirtualQuery")) {
        uint32_t address=rd32(c->r[4]+4),size=rd32(c->r[4]+12);
        if(block_last!=0x007dedd4u || RUNTIME_IMAGE_SIZE!=32104448u)stop("virtual_memory_query",block_last);
        if(address<RUNTIME_IMAGE_BASE || (uint64_t)address>=RUNTIME_IMAGE_BASE+(uint64_t)RUNTIME_IMAGE_SIZE || size!=28)
            stop("optional_memory_scan_request",address);
        /* shortcut: this inspected startup scan skips a page on failure and ignores the final count. */
        if(!optional_memory_scan_calls++)fputs("Compatibility: optional startup page scan unavailable\n",stderr);
        wr32(c->fs_base+0x34,50);api_return(c,3,0);return;
    }
    if(!strcmp(name,"K32GetModuleInformation")) {
        uint32_t process=rd32(c->r[4]+4),module=rd32(c->r[4]+8);
        uint32_t output=rd32(c->r[4]+12),size=rd32(c->r[4]+16);
        if(process!=0xffffffffu){wr32(c->fs_base+0x34,6);api_return(c,4,0);return;}
        if(module!=RUNTIME_IMAGE_BASE)stop("module_information_module",module);
        if(size<12)stop("module_information_size",size);
        check(output,12,1);wr32(output,RUNTIME_IMAGE_BASE);wr32(output+4,RUNTIME_IMAGE_SIZE);wr32(output+8,RUNTIME_ENTRY);
        api_return(c,4,1);return;
    }
    if(!strcmp(name,"K32EnumProcessModules")) {
        uint32_t process=rd32(c->r[4]+4),output=rd32(c->r[4]+8);
        uint32_t size=rd32(c->r[4]+12),needed=rd32(c->r[4]+16);
        if(process!=0xffffffffu){wr32(c->fs_base+0x34,6);api_return(c,4,0);return;}
        check(needed,4,1);if(size>=4)check(output,4,1);
        wr32(needed,4);if(size>=4)wr32(output,RUNTIME_IMAGE_BASE);
        api_return(c,4,1);return;
    }
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
        api_return(c,4,create_kernel_object(c,OBJECT_EVENT,!strcmp(name,"CreateEventW")));return;
    }
    if(!strcmp(name,"CreateMutexW") || !strcmp(name,"CreateMutexA")) {
        api_return(c,3,create_kernel_object(c,OBJECT_MUTEX,!strcmp(name,"CreateMutexW")));return;
    }
    if(!strcmp(name,"SetEvent") || !strcmp(name,"ResetEvent") || !strcmp(name,"CloseHandle")) {
        KernelHandle *handle=kernel_handle(rd32(c->r[4]+4));
        if(!handle || (strcmp(name,"CloseHandle") && handle->object->kind!=OBJECT_EVENT)) {
            wr32(c->fs_base+0x34,6);api_return(c,1,0);return;
        }
        if(!strcmp(name,"CloseHandle")){
            if(!--handle->object->references)memset(handle->object,0,sizeof(*handle->object));
            handle->value=0;handle->object=NULL;
        }
        else handle->object->signaled=!strcmp(name,"SetEvent");
        api_return(c,1,1);return;
    }
    if(!strcmp(name,"ReleaseMutex")) {
        KernelHandle *handle=kernel_handle(rd32(c->r[4]+4));
        if(!handle || handle->object->kind!=OBJECT_MUTEX){wr32(c->fs_base+0x34,6);api_return(c,1,0);return;}
        KernelObject *object=handle->object;
        if(object->owner!=1 || !object->recursion){wr32(c->fs_base+0x34,288);api_return(c,1,0);return;}
        if(!--object->recursion)object->owner=0;
        api_return(c,1,1);return;
    }
    if(!strcmp(name,"WaitForSingleObject")) {
        uint32_t value=rd32(c->r[4]+4),milliseconds=rd32(c->r[4]+8);
        KernelHandle *handle=kernel_handle(value);
        if(!handle){wr32(c->fs_base+0x34,6);api_return(c,2,0xffffffffu);return;}
        KernelObject *object=handle->object;
        if(object->kind==OBJECT_MUTEX) {
            if(!object->owner || object->owner==1) {
                if(object->recursion==0xffffffffu)stop("mutex_recursion_overflow",value);
                object->owner=1;object->recursion++;api_return(c,2,0);return;
            }
            if(milliseconds)stop("pending_mutex_wait",value);
            api_return(c,2,258);return;
        }
        if(object->signaled) {
            if(!object->manual)object->signaled=0;
            api_return(c,2,0);return;
        }
        /* shortcut: blocking waits need guest scheduling; never manufacture a wake-up. */
        if(milliseconds){fprintf(stderr,"Pending event wait, timeout %u ms\n",milliseconds);stop("pending_event_wait",value);}
        api_return(c,2,258);return;
    }
    if(!strcmp(name,"GetModuleHandleW") || !strcmp(name,"GetModuleHandleA")) {
        a0=rd32(c->r[4]+4);
        if(!a0){api_return(c,1,RUNTIME_IMAGE_BASE);return;}
        char module[128];guest_name(a0,!strcmp(name,"GetModuleHandleW"),module);
        for(unsigned i=0;module[i];i++)if(module[i]>='A' && module[i]<='Z')module[i]+='a'-'A';
        if(strcmp(module,"kernel32.dll")){fprintf(stderr,"Pending module %s\n",module);stop("module_lookup",a0);}
        api_return(c,1,0x0fd80000u);return;
    }
    if(!strcmp(name,"GetStartupInfoW")) {
        /* x86 STARTUPINFOW for a GUI launch with no inherited console or window overrides. */
        a0=rd32(c->r[4]+4);check(a0,68,1);
        memset(g_mem+a0,0,68);wr32(a0,68);api_return(c,1,0);return;
    }
    if(!strcmp(name,"_get_narrow_winmain_command_line")) {
        /* Stable zero-initialized argument buffer: this launch passes no game arguments. */
        api_return(c,0,0x0fe51000u);return;
    }
    if(!strcmp(name,"GetErrorMode")){api_return(c,0,process_error_mode);return;}
    if(!strcmp(name,"SetErrorMode")) {
        uint32_t mode=rd32(c->r[4]+4),previous=process_error_mode;
        if(mode&~0x8003u)stop("error_mode_flags",mode);
        process_error_mode=mode;api_return(c,1,previous);return;
    }
    if(!strcmp(import->dll,"MSVCP140.dll") && !strcmp(name,"??0?$basic_ios@DU?$char_traits@D@std@@@std@@IAE@XZ")) {
        uint32_t object=c->r[1];check(object,65,1);
        wr32(object,BASIC_IOS_VTABLE);
        for(uint32_t offset=8;offset<=0x30;offset+=4)wr32(object+offset,0);
        wr32(object+0x38,0);wr32(object+0x3c,0);wr8(object+0x40,0);
        api_return(c,0,object);return;
    }
    if(!strcmp(import->dll,"MSVCP140.dll") && !strcmp(name,"??0?$basic_istream@DU?$char_traits@D@std@@@std@@QAE@PAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z")) {
        uint32_t object=c->r[1],buffer=rd32(c->r[4]+4),standard=rd32(c->r[4]+8),complete=rd32(c->r[4]+12);
        if(standard || complete)stop("cpp_stream_ctor_mode",standard?standard:complete);
        uint32_t offset=rd32(rd32(object)+4);
        if(offset<0x18 || offset>0x10000 || (uint64_t)object+offset+65>GUEST_SIZE)stop("cpp_stream_base_offset",offset);
        uint32_t ios=object+offset;check(object,16,1);check(ios-4,69,1);
        if(rd32(ios)!=BASIC_IOS_VTABLE)stop("cpp_stream_base_state",ios);
        uint32_t locale=stream_locale();
        wr32(ios-4,offset-0x18);wr32(ios,BASIC_ISTREAM_VTABLE);wr32(object+8,0);wr32(object+12,0);
        wr32(ios+8,0);wr32(ios+12,buffer?0:4);wr32(ios+16,0);wr32(ios+20,0x201);
        wr64(ios+24,6);wr64(ios+32,0);wr32(ios+40,0);wr32(ios+44,0);wr32(ios+48,locale);
        wr32(ios+56,buffer);wr32(ios+60,0);wr8(ios+64,0x20);
        api_return(c,3,object);return;
    }
    if(!strcmp(import->dll,"MSVCP140.dll") && !strcmp(name,"??0?$basic_streambuf@DU?$char_traits@D@std@@@std@@IAE@XZ")) {
        uint32_t object=c->r[1];check(object,0x38,1);
        wr32(object,BASIC_STREAMBUF_VTABLE);streambuf_init(object);wr32(object+0x34,stream_locale());
        api_return(c,0,object);return;
    }
    if(!strcmp(import->dll,"MSVCP140.dll") && !strcmp(name,"?_Init@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IAEXXZ")) {
        streambuf_init(c->r[1]);api_return(c,0,0);return;
    }
    if(!strcmp(import->dll,"MSVCP140.dll") && !strcmp(name,"?_Fiopen@std@@YAPAU_iobuf@@PBDHH@Z")) {
        api_return(c,0,file_open(rd32(c->r[4]+4),rd32(c->r[4]+8),rd32(c->r[4]+12)));return;
    }
    if(!strcmp(name,"fclose")) {
        NativeFile *file=file_lookup(rd32(c->r[4]+4));uint32_t object=file->object;
        int result=fclose(file->host);*file=(NativeFile){0};heap_release(object,0);
        api_return(c,0,result?0xffffffffu:0);return;
    }
    if(!strcmp(name,"_get_stream_buffer_pointers")) {
        NativeFile *file=file_lookup(rd32(c->r[4]+4));
        const uint32_t offsets[]={4,0,8};
        for(unsigned i=0;i<3;i++) {
            uint32_t output=rd32(c->r[4]+8+4*i);
            if(output)wr32(output,file->object+offsets[i]);
        }
        api_return(c,0,0);return;
    }
    if(!strcmp(import->dll,"MSVCP140.dll") && !strcmp(name,"?getloc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QBE?AVlocale@2@XZ")) {
        uint32_t result=rd32(c->r[4]+4),locale=rd32(c->r[1]+0x34);
        if(rd32(locale+4)!=CLASSIC_LOCALE)stop("cpp_stream_locale",locale);
        check(result,8,1);wr32(result+4,CLASSIC_LOCALE);wr32(CLASSIC_LOCALE+4,rd32(CLASSIC_LOCALE+4)+1);
        api_return(c,1,result);return;
    }
    if(!strcmp(import->dll,"MSVCP140.dll") && !strcmp(name,"??0_Lockit@std@@QAE@H@Z")) {
        /* shortcut: only the primary guest thread exists, so library locks cannot contend. */
        wr32(c->r[1],rd32(c->r[4]+4));
        if(!single_thread_cpp_lock_calls++)fputs("Compatibility: single-thread C++ library locks\n",stderr);
        api_return(c,1,c->r[1]);return;
    }
    if(!strcmp(import->dll,"MSVCP140.dll") && !strcmp(name,"??1_Lockit@std@@QAE@XZ")) {
        check(c->r[1],4,0);
        if(!single_thread_cpp_lock_calls++)fputs("Compatibility: single-thread C++ library locks\n",stderr);
        api_return(c,0,0);return;
    }
    if(!strcmp(import->dll,"MSVCP140.dll") && !strcmp(name,"??Bid@locale@std@@QAEIXZ")) {
        if(c->r[1]!=CLASSIC_CODECVT_ID)stop("cpp_locale_id",c->r[1]);
        classic_codecvt_used();
        api_return(c,0,rd32(CLASSIC_CODECVT_ID));return;
    }
    if(!strcmp(import->dll,"MSVCP140.dll") && !strcmp(name,"?always_noconv@codecvt_base@std@@QBE_NXZ")) {
        if(c->r[1]!=CLASSIC_CODECVT)stop("cpp_codecvt",c->r[1]);
        classic_codecvt_used();
        api_return(c,0,1);return;
    }
    if(!strcmp(import->dll,"MSVCP140.dll") && !strcmp(name,"?fail@ios_base@std@@QBE_NXZ")) {
        api_return(c,0,!!(rd32(c->r[1]+12)&6));return;
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
        if(handle==0x0fd80000u && (!strcmp(export_name,"CreateHardLinkW") || !strcmp(export_name,"CreateSymbolicLinkW"))) {
            /* shortcut: the inspected filesystem wrapper reports unsupported when this pointer is null. */
            uint32_t bit=!strcmp(export_name,"CreateHardLinkW")?1u:2u;
            if(!(optional_filesystem_exports_seen&bit))fprintf(stderr,"Optional filesystem export unavailable: %s\n",export_name);
            optional_filesystem_exports_seen|=bit;optional_filesystem_export_calls++;
            wr32(c->fs_base+0x34,127);api_return(c,2,0);return;
        }
        fprintf(stderr,"Pending export %s\n",export_name);stop("dynamic_export",symbol);
    }
    if(!strcmp(name,"__std_exception_copy")) {
        uint32_t source=rd32(c->r[4]+4),destination=rd32(c->r[4]+8);
        uint32_t message=rd32(source);int owned=rd8(source+4)!=0;
        if(owned && message) {
            uint32_t length=0;while(rd8(message+length))length++;
            uint32_t copy=heap_allocate(length+1,4,0);
            if(!copy){api_return(c,0,0);return;} /* The CRT leaves the destination unchanged on allocation failure. */
            memcpy(g_mem+copy,g_mem+message,length+1);message=copy;
        } else owned=0;
        check(destination,5,1);wr32(destination,message);wr8(destination+4,(uint8_t)owned);
        api_return(c,0,0);return;
    }
    if(!strcmp(name,"__std_exception_destroy")) {
        uint32_t data=rd32(c->r[4]+4);check(data,5,1);
        uint32_t message=rd32(data);if(rd8(data+4))heap_release(message,0);
        wr32(data,0);wr8(data+4,0);api_return(c,0,0);return;
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
    if(argc!=2 && argc!=3) {fputs("Usage: runtime image.bin [read-only-content-root]\n",stderr);return 2;}
    content_root=argc==3?argv[2]:NULL;
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
    wr32(BASIC_IOS_VTABLE,BASIC_IOS_DELETING_DTOR);
    wr32(BASIC_ISTREAM_VTABLE,BASIC_IOS_DELETING_DTOR+16);
    wr32(CLASSIC_LOCALE_VTABLE,BASIC_IOS_DELETING_DTOR+32);
    wr32(CLASSIC_LOCALE_VTABLE+4,BASIC_IOS_DELETING_DTOR+64);
    wr32(CLASSIC_LOCALE_VTABLE+8,BASIC_IOS_DELETING_DTOR+80);
    wr32(BASIC_STREAMBUF_VTABLE,BASIC_IOS_DELETING_DTOR+48);
    wr8(CLASSIC_LOCALE_NAME,'C');
    wr32(CLASSIC_LOCALE,CLASSIC_LOCALE_VTABLE);wr32(CLASSIC_LOCALE+4,1);
    wr32(CLASSIC_LOCALE+8,CLASSIC_FACETS);wr32(CLASSIC_LOCALE+12,2);
    wr32(CLASSIC_LOCALE+16,0x3f);wr32(CLASSIC_LOCALE+24,CLASSIC_LOCALE_NAME);
    wr32(CLASSIC_CODECVT_ID,1);wr32(CLASSIC_FACETS+4,CLASSIC_CODECVT);
    wr32(CLASSIC_CODECVT,CLASSIC_CODECVT_VTABLE);wr32(CLASSIC_CODECVT+4,1);
    for(unsigned i=0;i<4;i++)wr32(CLASSIC_CODECVT_VTABLE+4*i,BASIC_IOS_DELETING_DTOR+(i==3?96:112));
    pages[BASIC_IOS_VTABLE/4096]=1; /* Guest vtable data is read-only after loader setup. */
    for(unsigned i=0;i<sizeof(image_imports)/sizeof(image_imports[0]);i++) {
        /* Loader writes IAT slots before guest protections take effect. */
        uint32_t value=GUEST_SHIM_BASE+16*i;
        if(!strcmp(image_imports[i].dll,"MSVCP140.dll") &&
           !strcmp(image_imports[i].name,"?id@?$codecvt@DDU_Mbstatet@@@std@@2V0locale@2@A"))value=CLASSIC_CODECVT_ID;
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
        if(cpu.eip==BASIC_IOS_DELETING_DTOR+64 || cpu.eip==BASIC_IOS_DELETING_DTOR+80) {
            if(cpu.r[1]!=CLASSIC_LOCALE)stop("cpp_stream_locale",cpu.r[1]);
            uint32_t count=rd32(CLASSIC_LOCALE+4);
            if(cpu.eip==BASIC_IOS_DELETING_DTOR+64)wr32(CLASSIC_LOCALE+4,count+1);
            else {if(count<=1)stop("cpp_locale_reference",count);wr32(CLASSIC_LOCALE+4,count-1);}
            api_return(&cpu,0,0);continue;
        }
        if(cpu.eip==BASIC_IOS_DELETING_DTOR+96) {
            if(cpu.r[1]!=CLASSIC_CODECVT)stop("cpp_codecvt",cpu.r[1]);
            classic_codecvt_used();
            api_return(&cpu,0,1);continue;
        }
        if(cpu.eip==BASIC_IOS_DELETING_DTOR+112)stop("cpp_virtual_method",cpu.eip);
        if(cpu.eip>=BASIC_IOS_DELETING_DTOR && cpu.eip<=BASIC_IOS_DELETING_DTOR+48 && !((cpu.eip-BASIC_IOS_DELETING_DTOR)%16)) {
            fputs("Unsupported C++ virtual method\n",stderr);
            stop("cpp_virtual_method",cpu.eip);
        }
        const RuntimeBlock *block=find_block(cpu.eip);
        if(!block)stop("missing_code",cpu.eip);
        block_pc=block->pc;block_last=block->last;executed++;
        block->body(&cpu);
    }
    stop("block_limit",cpu.eip);return 20;
}
