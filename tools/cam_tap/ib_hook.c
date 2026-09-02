// ib_hook.c — LD_PRELOAD into trackingservice. Asm-trampoline interpose of
//   OVR::Sensors::ImageBuffer::ImageBuffer(native_handle* meta, native_handle* gralloc)
// (mangled C1). This ctor is called per FrameSet delivery (per frame) from
// libvrsensors-hidlwrapper.so's make_shared<ImageBuffer>(hidl_handle,hidl_handle); it mmaps the
// shared metadata region and GraphicBuffer::lock()s the pixels. AFTER it returns:
//   this+0x40 = ImageBufferSharedData*  (id@0, w@8, h@0xc, fmt@0x10(u16), usage@0x18, ts@0x38(u64))
//   this+0x60 = void* locked CPU pixel VA (8bpp mono for the OV7251 tracking cams)
// So per frame we get pixels + an authoritative shared-memory timestamp, no pool-index resolution.
// See notes/09-imagebuffer-pool-re.md. GO-gated pixel dump; metadata always logged (bounded).
// Build: clang -O2 -fPIC -shared -o ib_hook.so ib_hook.c -ldl
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
#define DIR  "/data/local/tmp/cap"
#define LOG  DIR"/ib.log"
#define GO   DIR"/GO"
#define MAX_DUMPS 240          // ~2 s of 4-cam @30 Hz; bounded so we never fill /data
#define FRAME_MAX (640*512)    // sanity cap on a single pixel copy

static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void L(const char* f,...){ char b[512]; va_list a; va_start(a,f); vsnprintf(b,sizeof b,f,a); va_end(a);
  int fd=open(LOG,O_WRONLY|O_CREAT|O_APPEND,0666); if(fd>=0){ if(write(fd,b,strlen(b))){} close(fd); } }
static int okptr(uint64_t v){ return v>0x1000ull && v<0x1000000000000ull && (v&7)==0; }

void* g_real; static int g_seq, g_dumps;
void dump_ib(void* thiz);
void dump_ib(void* thiz){
  if(!thiz || (((uintptr_t)thiz)&7)) return;
  const uint8_t* p = (const uint8_t*)thiz;
  uint64_t sd = *(const uint64_t*)(p+0x40);
  uint64_t px = *(const uint64_t*)(p+0x60);
  if(!okptr(sd) || !okptr(px)) return;
  const uint8_t* s = (const uint8_t*)sd;
  uint32_t id  = *(const uint32_t*)(s+0x00);
  uint32_t w   = *(const uint32_t*)(s+0x08);
  uint32_t h   = *(const uint32_t*)(s+0x0c);
  uint16_t fmt = *(const uint16_t*)(s+0x10);
  uint64_t ts  = *(const uint64_t*)(s+0x38);
  if(w==0||h==0||w>4096||h>4096) return;
  int seq = __sync_fetch_and_add(&g_seq,1);
  uint64_t host = mono_ns();
  // quick content check on the mapped pixels (validates px is real image memory)
  const uint8_t* pix = (const uint8_t*)px; uint32_t mn=255,mx=0;
  size_t n = (size_t)w*h; if(n>FRAME_MAX) n=FRAME_MAX;
  for(size_t k=1000;k<n;k+=4096){ uint8_t v=pix[k]; if(v<mn)mn=v; if(v>mx)mx=v; }
  L("IB seq=%d host=%llu ts=%llu id=%u %ux%u fmt=%u px=%llx min=%u max=%u cam?=%d\n",
    seq,(unsigned long long)host,(unsigned long long)ts,id,w,h,fmt,
    (unsigned long long)px,mn,mx,seq&3);
  // wider metadata dump on the first few frames only: locate the true exposure-ts offset.
  // Log sd[0..0x60] as u64 words + the this[] neighbourhood of the shared_ptr/handle.
  if(seq<8){
    char b[900]; int o=0; const uint64_t* sw=(const uint64_t*)s;
    o+=snprintf(b+o,sizeof b-o,"  SD seq=%d:",seq);
    for(int i=0;i<13;i++) o+=snprintf(b+o,sizeof b-o," %d=%llx",i*8,(unsigned long long)sw[i]);
    L("%s\n",b);
    o=0; const uint64_t* tw=(const uint64_t*)thiz;
    o+=snprintf(b+o,sizeof b-o,"  THIS seq=%d:",seq);
    for(int i=0;i<14;i++) o+=snprintf(b+o,sizeof b-o," %d=%llx",i*8,(unsigned long long)tw[i]);
    L("%s\n",b);
  }
  // GO-gated raw dump (only when the trigger file exists and we have budget)
  if(access(GO,F_OK)==0 && __sync_fetch_and_add(&g_dumps,0)<MAX_DUMPS && mx>mn){
    int d=__sync_fetch_and_add(&g_dumps,1);
    char op[96]; snprintf(op,sizeof op,DIR"/ib_%05d.gray",d);
    int of=open(op,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(of>=0){ if(write(of,pix,(size_t)w*h)){} close(of);
      char ix[160]; int nn=snprintf(ix,sizeof ix,"%d %llu %llu %u %u %u %d\n",
        d,(unsigned long long)host,(unsigned long long)ts,id,w,h,seq&3);
      int xf=open(DIR"/ib.idx",O_WRONLY|O_CREAT|O_APPEND,0666);
      if(xf>=0){ if(write(xf,ix,nn)){} close(xf); } }
  }
}

__asm__(".text\n.global "CTOR"\n.type "CTOR",%function\n"CTOR":\n"
"  stp x29,x30,[sp,#-48]!\n  mov x29,sp\n  str x0,[sp,#16]\n"     // save this; x1,x2 (handles) preserved
"  adrp x9, :got:g_real\n  ldr x9,[x9,#:got_lo12:g_real]\n  ldr x9,[x9]\n  blr x9\n" // real ctor(this,nh1,nh2)
"  ldr x0,[sp,#16]\n  bl dump_ib\n"                                // dump_ib(this)
"  ldr x0,[sp,#16]\n  ldp x29,x30,[sp],#48\n  ret\n");             // return this

__attribute__((constructor)) static void init_(void){
  mkdir(DIR,0777);
  g_real=dlsym(RTLD_NEXT,CTOR);
  L("== ib_hook init real=%p ==\n",g_real);
}
