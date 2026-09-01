// fs_atomic.c — LD_PRELOAD into trackingservice. Safe asm-trampoline interpose of
// DualStreamHandle<FrameSet>::read(). Extracts per-frame metadata (exposure ts + camId) from the
// OVR::FrameSet descriptor held in the DualStreamHandle 'this' (at this[17..19] -> a mapped region
// with 4 image blocks: {imageIndex(camId 0-3), 640x480, floats, ts1/ts2/ts3 (CLOCK_MONOTONIC ns)}).
// NOTE: read() returns a STATUS {1,count}, not the FrameSet. Pixels are POOL-REFERENCED (not
// pointer-reachable from this object graph) -> a dense pixel link needs RE of the ImageBuffer pool
// resolution, or timing-correlation with a pixel tap using this precise camId. read() fires only
// during ACTIVE tracking. Trampoline verified to never crash TS (the image-content scan variant DID
// crash it -> removed).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <dlfcn.h>
#define READ_MANGLED "_ZN3OVR7Sensors11HidlWrapper16DualStreamHandleINS0_8FrameSetEN6vendor6oculus8hardware7sensors4V1_08FrameSetEE4readEv"
#define LOG "/data/local/tmp/fsa.log"
static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void L(const char* f,...){ char b[2048]; va_list a; va_start(a,f); vsnprintf(b,sizeof b,f,a); va_end(a);
  int fd=open(LOG,O_WRONLY|O_CREAT|O_APPEND,0666); if(fd>=0){ if(write(fd,b,strlen(b))){} close(fd);} }
static int rd(uint64_t v){ return v>0x7000000000ull&&v<0x8000000000ull&&(v&7)==0; }
void* g_real; static int g_n;
void dump_fs(void* thiz, void* x0v, void* x1v);
void dump_fs(void* thiz, void* x0v, void* x1v){ (void)x0v;(void)x1v;
  if(!thiz||(((uintptr_t)thiz)&7)) return; if(__sync_fetch_and_add(&g_n,0)>=200) return;
  const uint64_t* t=(const uint64_t*)thiz;
  for(int i=0;i<80;i++){ uint64_t op=t[i]; if(!rd(op)) continue; const uint32_t* q=(const uint32_t*)op;
    for(int k=0;k<96;k++){ if(q[k]==640&&(q[k+1]==480||q[k+1]==481)){
      __sync_fetch_and_add(&g_n,1); uint64_t h=mono_ns(); const uint64_t* w=(const uint64_t*)op;
      // 4 image blocks of 12 words starting near k; log frameset ts + per-image ts + camId
      L("FS host=%llu obj=%llx W:",(unsigned long long)h,(unsigned long long)op);
      char b[1200]; int o=0; for(int m=0;m<52;m++) o+=snprintf(b+o,sizeof b-o," %d:%llx",m,(unsigned long long)w[m]);
      L("%s\n",b); break; } }
    break; }
}
__asm__(".text\n.global "READ_MANGLED"\n.type "READ_MANGLED",%function\n"READ_MANGLED":\n"
"  stp x29,x30,[sp,#-64]!\n  mov x29,sp\n  str x8,[sp,#16]\n  str x0,[sp,#24]\n"
"  adrp x9, :got:g_real\n  ldr x9,[x9,#:got_lo12:g_real]\n  ldr x9,[x9]\n  blr x9\n"
"  str x0,[sp,#32]\n  str x1,[sp,#40]\n  ldr x0,[sp,#24]\n  ldr x1,[sp,#32]\n  ldr x2,[sp,#40]\n  bl dump_fs\n"
"  ldr x0,[sp,#32]\n  ldr x1,[sp,#40]\n  ldr x8,[sp,#16]\n  ldp x29,x30,[sp],#64\n  ret\n");
__attribute__((constructor)) static void init_(void){ g_real=dlsym(RTLD_NEXT,READ_MANGLED); L("== fs_atomic init real=%p ==\n",g_real); }
