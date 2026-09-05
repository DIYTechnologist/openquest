// ibfs_hook3.c — DIAGNOSTIC: resolve the FrameSet-slot <-> ImageBuffer-pool mapping.
//
// notes/31 established the fix for sustained frame capture: the ImageBuffer ctor is a pool
// allocation event, so instead of waiting for it, learn `slot -> pixel VA` at ctor time and read
// pixels on every MessageQueue<FrameSet>::read(). That needs one unknown resolved first: which
// field of the FrameSet names the pool slot.
//
// notes/31 guessed w2/w14 high dwords (values 0..15, matching the ImageBuffer `id`). That guess was
// made from only **16** logged words. The descriptor is **52** words = 4 image blocks on a 12-word
// stride (block heads at w2, w14, w26, w38), so 16 words showed only the first two blocks — which
// is also why w2 and w14 both read slot 5 with lo = 0 and 1. With 4 cameras and 4 blocks that
// reading cannot be right as stated.
//
// So: log the FULL 52 words, and log every ctor's (id, pixel VA). Correlate offline. This hook
// writes NO pixels — it is deliberately cheap, because heavy writes are what precede the UFS wedge
// on this device.
//
// Build: see build3.sh.  Env: IBFS_MAXFS (default 4000).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <dlfcn.h>

#define CTOR "_ZN3OVR7Sensors11ImageBufferC1EPK13native_handleS4_"
#define MQREAD "_ZN7android8hardware12MessageQueueIN6vendor6oculus8hardware7sensors4V1_08FrameSetELNS0_8MQFlavorE1EE4readEPS7_m"
#define DIR "/data/local/tmp/cap3"
#define LOG DIR"/map.log"
#define FS_WORDS 52

static int g_logfd = -1, g_maxfs = 4000, g_fn, g_seq;

static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void L(const char* f,...){ char b[2600]; va_list a; va_start(a,f); vsnprintf(b,sizeof b,f,a); va_end(a);
  if(g_logfd>=0){ if(write(g_logfd,b,strlen(b))){} } }
static int okptr(uint64_t v){ return v>0x1000ull && v<0x1000000000000ull && (v&7)==0; }
static int envint(const char* k,int d){ const char* v=getenv(k); return v&&*v?(int)strtol(v,0,0):d; }

void* g_real_ctor;
void dump_ib(void* thiz);
void dump_ib(void* thiz){
  if(!thiz || (((uintptr_t)thiz)&7)) return;
  const uint8_t* p=(const uint8_t*)thiz;
  uint64_t sd=*(const uint64_t*)(p+0x40), px=*(const uint64_t*)(p+0x60);
  if(!okptr(sd)||!okptr(px)) return;
  const uint8_t* s=(const uint8_t*)sd;
  uint32_t id=*(const uint32_t*)(s+0x00), w=*(const uint32_t*)(s+0x08), h=*(const uint32_t*)(s+0x0c);
  if(w==0||h==0||w>4096||h>4096) return;
  // The mapping we are trying to learn: id -> pixel VA. `this` and sd are logged too, in case the
  // pool is keyed on the object rather than the id.
  L("IB seq=%d host=%llu id=%u %ux%u this=%llx sd=%llx px=%llx\n",
    __sync_fetch_and_add(&g_seq,1),(unsigned long long)mono_ns(),id,w,h,
    (unsigned long long)(uintptr_t)thiz,(unsigned long long)sd,(unsigned long long)px);
}

typedef int (*mqrd_t)(void*,void*,unsigned long);
static mqrd_t g_real_mq;
int mq_read(void* thiz, void* out, unsigned long cnt) asm(MQREAD);
int mq_read(void* thiz, void* out, unsigned long cnt){
  if(!g_real_mq) g_real_mq=(mqrd_t)dlsym(RTLD_NEXT,MQREAD);
  int r=g_real_mq(thiz,out,cnt);
  if(r && out && __sync_fetch_and_add(&g_fn,0)<g_maxfs){
    __sync_fetch_and_add(&g_fn,1);
    const uint64_t* w=(const uint64_t*)out;
    char b[2600]; int o=0;
    o+=snprintf(b+o,sizeof b-o,"FS host=%llu W:",(unsigned long long)mono_ns());
    for(int m=0;m<FS_WORDS;m++) o+=snprintf(b+o,sizeof b-o," %d:%llx",m,(unsigned long long)w[m]);
    L("%s\n",b);
  }
  return r;
}

__asm__(".text\n.global "CTOR"\n.type "CTOR",%function\n"CTOR":\n"
"  stp x29,x30,[sp,#-48]!\n  mov x29,sp\n  str x0,[sp,#16]\n"
"  adrp x9, :got:g_real_ctor\n  ldr x9,[x9,#:got_lo12:g_real_ctor]\n  ldr x9,[x9]\n  blr x9\n"
"  ldr x0,[sp,#16]\n  bl dump_ib\n"
"  ldr x0,[sp,#16]\n  ldp x29,x30,[sp],#48\n  ret\n");

__attribute__((constructor)) static void init_(void){
  mkdir(DIR,0777);
  g_maxfs=envint("IBFS_MAXFS",4000);
  g_logfd=open(LOG,O_WRONLY|O_CREAT|O_APPEND,0666);
  g_real_ctor=dlsym(RTLD_NEXT,CTOR);
  g_real_mq=(mqrd_t)dlsym(RTLD_NEXT,MQREAD);
  L("== ibfs_hook3 init ctor=%p mqread=%p fswords=%d maxfs=%d ==\n",
    g_real_ctor,(void*)g_real_mq,FS_WORDS,g_maxfs);
}
