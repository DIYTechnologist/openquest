// ibfs_hook.c — SINGLE-preload merge of the ImageBuffer ctor hook (dense pixels + host ts) and
// the FrameSet read hook (per-frame exposure timestamps). One .so avoids the two-preload
// instability that killed trackingservice when ib_hook.so + fs_atomic.so were loaded together.
//
// Two interpositions, same delivery thread, sequential per frame group:
//   1. DualStreamHandle<FrameSet>::read()  -> logs the FrameSet descriptor (52 words:
//        4 image blocks {imageIndex/camId, 640, 480, stride, floats, ts1/ts2/ts3 mono_ns})
//        + host mono_ns. Caches the whole descriptor for offline camId->exposure_ts mapping.
//   2. ImageBuffer::ImageBuffer(native_handle*, native_handle*)  -> dense pixels @this+0x60,
//        host mono_ns, camId=id&3. (The exposure ts is NOT in the ImageBuffer/sd — see
//        notes/09; it lives in the FrameSet descriptor from hook 1.)
// Offline: join each IB frame to the read() that preceded it (host-time order) + camId -> ts.
// Build: clang -O2 -fPIC -shared -o ibfs_hook.so ibfs_hook.c -ldl
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <dlfcn.h>

#define CTOR "_ZN3OVR7Sensors11ImageBufferC1EPK13native_handleS4_"
// FrameSet exposure ts: MessageQueue<FrameSet,sync>::read(FrameSet* out, unsigned long) — the
// out-of-line FMQ read that img_shim proved is interceptable (DualStreamHandle::read is an
// inlined template wrapper, so it has no call site to preempt). out[11] = exposure ts (mono ns).
#define MQREAD "_ZN7android8hardware12MessageQueueIN6vendor6oculus8hardware7sensors4V1_08FrameSetELNS0_8MQFlavorE1EE4readEPS7_m"
#define DIR  "/data/local/tmp/cap"
#define LOG  DIR"/ib.log"
#define GO   DIR"/GO"
#define MAX_DUMPS 240
#define FRAME_MAX (640*512)

static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void L(const char* f,...){ char b[1400]; va_list a; va_start(a,f); vsnprintf(b,sizeof b,f,a); va_end(a);
  int fd=open(LOG,O_WRONLY|O_CREAT|O_APPEND,0666); if(fd>=0){ if(write(fd,b,strlen(b))){} close(fd); } }
static int okptr(uint64_t v){ return v>0x1000ull && v<0x1000000000000ull && (v&7)==0; }
static int rd(uint64_t v){ return v>0x7000000000ull && v<0x8000000000ull && (v&7)==0; }

// ---- hook 2: ImageBuffer ctor (dense pixels) ----
void* g_real_ctor; static int g_seq, g_dumps;
void dump_ib(void* thiz);
void dump_ib(void* thiz){
  if(!thiz || (((uintptr_t)thiz)&7)) return;
  const uint8_t* p=(const uint8_t*)thiz;
  uint64_t sd=*(const uint64_t*)(p+0x40), px=*(const uint64_t*)(p+0x60);
  if(!okptr(sd)||!okptr(px)) return;
  const uint8_t* s=(const uint8_t*)sd;
  uint32_t id=*(const uint32_t*)(s+0x00), w=*(const uint32_t*)(s+0x08), h=*(const uint32_t*)(s+0x0c);
  uint16_t fmt=*(const uint16_t*)(s+0x10);
  if(w==0||h==0||w>4096||h>4096) return;
  int seq=__sync_fetch_and_add(&g_seq,1); uint64_t host=mono_ns();
  const uint8_t* pix=(const uint8_t*)px; uint32_t mn=255,mx=0;
  size_t n=(size_t)w*h; if(n>FRAME_MAX) n=FRAME_MAX;
  for(size_t k=1000;k<n;k+=4096){ uint8_t v=pix[k]; if(v<mn)mn=v; if(v>mx)mx=v; }
  L("IB seq=%d host=%llu id=%u %ux%u fmt=%u px=%llx min=%u max=%u cam=%d\n",
    seq,(unsigned long long)host,id,w,h,fmt,(unsigned long long)px,mn,mx,id&3);
  if(access(GO,F_OK)==0 && __sync_fetch_and_add(&g_dumps,0)<MAX_DUMPS && mx>mn){
    int d=__sync_fetch_and_add(&g_dumps,1);
    char op[96]; snprintf(op,sizeof op,DIR"/ib_%05d.gray",d);
    int of=open(op,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(of>=0){ if(write(of,pix,(size_t)w*h)){} close(of);
      char ix[160]; int nn=snprintf(ix,sizeof ix,"%d %llu %u %u %u %d\n",
        d,(unsigned long long)host,id,w,h,id&3);
      int xf=open(DIR"/ib.idx",O_WRONLY|O_CREAT|O_APPEND,0666);
      if(xf>=0){ if(write(xf,ix,nn)){} close(xf); } }
  }
}

// ---- hook 1: MessageQueue<FrameSet>::read (exposure timestamps) — img_shim's proven pattern ----
// Plain C interposer with asm() label (returns bool in x0, standard ABI — no sret trampoline).
typedef int (*mqrd_t)(void*,void*,unsigned long);
static mqrd_t g_real_mq; static int g_fn;
int mq_read(void* thiz, void* out, unsigned long cnt) asm(MQREAD);
int mq_read(void* thiz, void* out, unsigned long cnt){
  if(!g_real_mq) g_real_mq=(mqrd_t)dlsym(RTLD_NEXT,MQREAD);
  int r=g_real_mq(thiz,out,cnt);
  if(r && out && __sync_fetch_and_add(&g_fn,0)<600){
    __sync_fetch_and_add(&g_fn,1);
    uint64_t host=mono_ns(); const uint64_t* w=(const uint64_t*)out;
    // dump the FrameSet element head (16 u64 words = 128B; out[11] is the known exposure ts)
    char b[420]; int o=0; o+=snprintf(b+o,sizeof b-o,"FS host=%llu W:",(unsigned long long)host);
    for(int m=0;m<16;m++) o+=snprintf(b+o,sizeof b-o," %d:%llx",m,(unsigned long long)w[m]);
    L("%s\n",b);
  }
  return r;
}

// ---- ctor trampoline (dense pixels) ----
__asm__(".text\n.global "CTOR"\n.type "CTOR",%function\n"CTOR":\n"
"  stp x29,x30,[sp,#-48]!\n  mov x29,sp\n  str x0,[sp,#16]\n"
"  adrp x9, :got:g_real_ctor\n  ldr x9,[x9,#:got_lo12:g_real_ctor]\n  ldr x9,[x9]\n  blr x9\n"
"  ldr x0,[sp,#16]\n  bl dump_ib\n"
"  ldr x0,[sp,#16]\n  ldp x29,x30,[sp],#48\n  ret\n");

__attribute__((constructor)) static void init_(void){
  mkdir(DIR,0777);
  g_real_ctor=dlsym(RTLD_NEXT,CTOR);
  g_real_mq=(mqrd_t)dlsym(RTLD_NEXT,MQREAD);
  L("== ibfs_hook init ctor=%p mqread=%p ==\n",g_real_ctor,(void*)g_real_mq);
}
