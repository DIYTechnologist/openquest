// ibfs_hook2.c — capture-grade variant of ibfs_hook.c for the step 2 worn session (notes/18 track E).
//
// ibfs_hook.c is the proven tap (notes/10) but is built for ~10 s probe runs. Three things break at
// 2+ minutes, all of them fixed here without touching the interposition mechanism, which is
// unchanged and byte-for-byte the same idea:
//
//   1. MAX_DUMPS was 240. At 4 cams x 30 Hz that is 2 seconds. Now env-configurable.
//   2. One file per frame. A 2-minute stereo capture is ~9000 files in one directory, and on this
//      device readdir is exactly what hangs when the UFS link degrades (see the ufs-link-death
//      memory / notes/12). Frames now go into ONE blob with an index, so nothing ever enumerates.
//   3. open()+write()+close() per log line, at ~170 lines/s, inside trackingservice's delivery
//      thread. Now a single persistent fd.
//
// These matter because the writes happen INLINE in Meta's tracking pipeline: this hook runs in
// trackingservice, which is simultaneously our ground-truth reference. Cheap writes are not an
// optimisation here, they are a correctness concern — a stalled delivery thread would degrade the
// very poses we are measuring against.
//
// Env knobs (all optional):
//   IBFS_MAX=N        max frames dumped            (default 240, same as before)
//   IBFS_CAMMASK=M    bitmask of cams to dump      (default 0xf = all 4; 0x3 = stereo cam0+cam1)
//   IBFS_DUMPEMPTY=1  dump frames even if all-black (default 0)
//   IBFS_MAXFS=N      max FrameSet log lines       (default 600)
//
// IBFS_DUMPEMPTY exists to validate the write path on a desk: with the proximity sensor uncovered
// the ImageBuffer ctor still fires (notes/29: 448 IB lines in 12 s) but every frame is min=0 max=0,
// so the normal mx>mn gate means the dump path is never exercised until the headset is worn. That
// would leave the expensive session as the first-ever test of the writer. It is not.
//
// Build: see build2.sh.
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
#define DIR   "/data/local/tmp/cap"
#define LOG   DIR"/ib.log"
#define BLOB  DIR"/ib_frames.bin"
#define IDX   DIR"/ib.idx"
#define GO    DIR"/GO"
#define FRAME_MAX (640*512)

static int      g_max = 240, g_cammask = 0xf, g_dumpempty = 0, g_maxfs = 600;
static int      g_logfd = -1, g_blobfd = -1, g_idxfd = -1;
static uint64_t g_off;               // atomic append cursor into the blob
static int      g_seq, g_dumps, g_fn;

static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }

// Persistent-fd logger. Same format as ibfs_hook.c so existing offline parsing still works.
static void L(const char* f,...){ char b[1400]; va_list a; va_start(a,f); vsnprintf(b,sizeof b,f,a); va_end(a);
  if(g_logfd>=0){ if(write(g_logfd,b,strlen(b))){} } }

static int okptr(uint64_t v){ return v>0x1000ull && v<0x1000000000000ull && (v&7)==0; }

static int envint(const char* k,int d){ const char* v=getenv(k); return v&&*v?(int)strtol(v,0,0):d; }

// ---- hook 2: ImageBuffer ctor (dense pixels) ----
void* g_real_ctor;
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
  int cam=id&3;
  const uint8_t* pix=(const uint8_t*)px; uint32_t mn=255,mx=0;
  size_t n=(size_t)w*h; if(n>FRAME_MAX) n=FRAME_MAX;
  for(size_t k=1000;k<n;k+=4096){ uint8_t v=pix[k]; if(v<mn)mn=v; if(v>mx)mx=v; }
  L("IB seq=%d host=%llu id=%u %ux%u fmt=%u px=%llx min=%u max=%u cam=%d\n",
    seq,(unsigned long long)host,id,w,h,fmt,(unsigned long long)px,mn,mx,cam);

  if(access(GO,F_OK)!=0) return;
  if(!((g_cammask>>cam)&1)) return;
  if(!(mx>mn) && !g_dumpempty) return;
  if(__sync_fetch_and_add(&g_dumps,0) >= g_max) return;
  int d=__sync_fetch_and_add(&g_dumps,1);
  if(d>=g_max) return;

  size_t nb=(size_t)w*h;
  // Lock-free append: reserve the range, then pwrite into it. No seek race between threads, and
  // no per-frame open/close.
  uint64_t off=__sync_fetch_and_add(&g_off,(uint64_t)nb);
  if(g_blobfd>=0){
    size_t done=0;
    while(done<nb){
      ssize_t r=pwrite(g_blobfd,pix+done,nb-done,(off_t)(off+done));
      if(r<=0) break;
      done+=(size_t)r;
    }
    char ix[192]; int nn=snprintf(ix,sizeof ix,"%d %llu %u %u %u %d %llu %zu\n",
      d,(unsigned long long)host,id,w,h,cam,(unsigned long long)off,nb);
    if(g_idxfd>=0){ if(write(g_idxfd,ix,nn)){} }
  }
}

// ---- hook 1: MessageQueue<FrameSet>::read (exposure timestamps) ----
typedef int (*mqrd_t)(void*,void*,unsigned long);
static mqrd_t g_real_mq;
int mq_read(void* thiz, void* out, unsigned long cnt) asm(MQREAD);
int mq_read(void* thiz, void* out, unsigned long cnt){
  if(!g_real_mq) g_real_mq=(mqrd_t)dlsym(RTLD_NEXT,MQREAD);
  int r=g_real_mq(thiz,out,cnt);
  if(r && out && __sync_fetch_and_add(&g_fn,0)<g_maxfs){
    __sync_fetch_and_add(&g_fn,1);
    uint64_t host=mono_ns(); const uint64_t* w=(const uint64_t*)out;
    char b[420]; int o=0; o+=snprintf(b+o,sizeof b-o,"FS host=%llu W:",(unsigned long long)host);
    for(int m=0;m<16;m++) o+=snprintf(b+o,sizeof b-o," %d:%llx",m,(unsigned long long)w[m]);
    L("%s\n",b);
  }
  return r;
}

// ---- ctor trampoline (unchanged from ibfs_hook.c) ----
__asm__(".text\n.global "CTOR"\n.type "CTOR",%function\n"CTOR":\n"
"  stp x29,x30,[sp,#-48]!\n  mov x29,sp\n  str x0,[sp,#16]\n"
"  adrp x9, :got:g_real_ctor\n  ldr x9,[x9,#:got_lo12:g_real_ctor]\n  ldr x9,[x9]\n  blr x9\n"
"  ldr x0,[sp,#16]\n  bl dump_ib\n"
"  ldr x0,[sp,#16]\n  ldp x29,x30,[sp],#48\n  ret\n");

__attribute__((constructor)) static void init_(void){
  mkdir(DIR,0777);
  g_max       = envint("IBFS_MAX",240);
  g_cammask   = envint("IBFS_CAMMASK",0xf);
  g_dumpempty = envint("IBFS_DUMPEMPTY",0);
  g_maxfs     = envint("IBFS_MAXFS",600);
  g_logfd  = open(LOG, O_WRONLY|O_CREAT|O_APPEND,0666);
  g_blobfd = open(BLOB,O_WRONLY|O_CREAT|O_TRUNC, 0644);
  g_idxfd  = open(IDX, O_WRONLY|O_CREAT|O_APPEND,0666);
  g_real_ctor=dlsym(RTLD_NEXT,CTOR);
  g_real_mq=(mqrd_t)dlsym(RTLD_NEXT,MQREAD);
  L("== ibfs_hook2 init ctor=%p mqread=%p max=%d cammask=0x%x dumpempty=%d maxfs=%d blobfd=%d ==\n",
    g_real_ctor,(void*)g_real_mq,g_max,g_cammask,g_dumpempty,g_maxfs,g_blobfd);
}
