// ibfs_hook9.c — full-rate stereo capture with per-frame exposure timestamps.
//
// Everything this needs is now established:
//   notes/35  pixels are readable at FrameSet rate from our own dmabuf mappings (dup the fd out of
//             the ctor's pixel native_handle and mmap it; Meta's `this+0x60` VA is transient).
//   notes/38  the pool is 64 buffers and `k`, the ctor's position within its id's run of four, is
//             a stable camera index (0 rebinds, 64/64 entries single-valued, 99.7 % clean sets).
//   notes/39  `k` equals the factory calibration index — confirmed on all six camera pairs by
//             epipolar error, every one selecting the identity ordering.
//   notes/34  each FrameSet block carries its own camId and a CLOCK_MONOTONIC capture timestamp.
//
// So this dumps a chosen camera pair at full rate, stamping every frame with the capture time from
// the descriptor block for that camera — not a host read time. That closes notes/10's original open
// thread as a side effect.
//
// Defaults to cameras 0 and 2: the only pair with real overlap (19.6 deg, 11.2 cm baseline), all
// other pairs being 67-82 deg apart and useless for stereo matching (notes/12).
//
// Writes one blob plus a text index — never one file per frame, because readdir on a large
// directory is what wedges this device's UFS (see the ufs-link-death memory).
//
// Env: IBFS_CAMA (0), IBFS_CAMB (2), IBFS_MAX frames total (8000), IBFS_MAXFS (60000).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <dlfcn.h>

#define CTOR "_ZN3OVR7Sensors11ImageBufferC1EPK13native_handleS4_"
#define MQREAD "_ZN7android8hardware12MessageQueueIN6vendor6oculus8hardware7sensors4V1_08FrameSetELNS0_8MQFlavorE1EE4readEPS7_m"
#define DIR  "/data/local/tmp/cap9"
#define LOG  DIR"/cap.log"
#define BLOB DIR"/frames.bin"
#define IDX  DIR"/frames.idx"
#define GO   DIR"/GO"
#define NENT 96

struct ent { uint64_t handle, va; uint32_t size, id, k, hash; int fd; };
static struct ent g_e[NENT];
static int g_n, g_logfd=-1, g_blobfd=-1, g_idxfd=-1;
static int g_cama=0, g_camb=2, g_max=8000, g_maxfs=60000, g_fn, g_frames;
static uint64_t g_off;
static uint32_t g_kcount[16];
// Recently-emitted capture timestamps per camera. A single last-value check is not enough: the
// FrameSet arrives on TWO interleaved streams (notes/10's DualStream), so a timestamp can reappear
// after one from the other stream. Measured with a single-value check: cam2 still had 208 exact
// duplicates out of 647 rows. A short ring catches them.
#define NSEEN 16
static uint64_t g_seen[4][NSEEN];
static int      g_seenq[4];

static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void L(const char* f,...){ char b[1200]; va_list a; va_start(a,f); vsnprintf(b,sizeof b,f,a); va_end(a);
  if(g_logfd>=0){ if(write(g_logfd,b,strlen(b))){} } }
static int okptr(uint64_t v){ return v>0x1000ull && v<0x1000000000000ull && (v&7)==0; }
static int envint(const char* k,int d){ const char* v=getenv(k); return v&&*v?(int)strtol(v,0,0):d; }

static uint32_t hashof(uint64_t va, uint32_t sz){
  if(!va || sz<8192) return 0;
  const volatile uint8_t* p1=(const volatile uint8_t*)(uintptr_t)(va + sz/4);
  const volatile uint8_t* p2=(const volatile uint8_t*)(uintptr_t)(va + sz/2 + 777);
  uint32_t h=2166136261u;
  for(int i=0;i<32;i++){ h^=p1[i]; h*=16777619u; }
  for(int i=0;i<32;i++){ h^=p2[i]; h*=16777619u; }
  return h?h:1;
}

struct nhandle { int version; int numFds; int numInts; int data[]; };

void* g_real_ctor;
void dump_ib(void* thiz, const void* hmeta, const void* hpix);
void dump_ib(void* thiz, const void* hmeta, const void* hpix){
  if(!thiz || (((uintptr_t)thiz)&7) || !hpix) return;
  const uint8_t* p=(const uint8_t*)thiz;
  uint64_t sd=*(const uint64_t*)(p+0x40), px=*(const uint64_t*)(p+0x60);
  if(!okptr(sd)||!okptr(px)) return;
  const uint8_t* s=(const uint8_t*)sd;
  uint32_t id=*(const uint32_t*)(s+0x00), w=*(const uint32_t*)(s+0x08), h=*(const uint32_t*)(s+0x0c);
  if(w==0||h==0||w>4096||h>4096||id>=16) return;
  const struct nhandle* nh=(const struct nhandle*)hpix;
  if(nh->numFds<1||nh->numFds>16) return;
  uint64_t key=(uint64_t)(uintptr_t)hpix;
  for(int i=0;i<g_n;i++) if(g_e[i].handle==key){ g_e[i].id=id; g_e[i].k=g_kcount[id]&3; g_kcount[id]++; return; }
  uint32_t k=g_kcount[id]&3; g_kcount[id]++;
  if(g_n>=NENT) return;
  int fd=dup(nh->data[0]); if(fd<0) return;
  size_t sz=(size_t)w*h;
  void* m=mmap(NULL,sz,PROT_READ,MAP_SHARED,fd,0);
  if(m==MAP_FAILED){ close(fd); return; }
  int e=g_n++;
  g_e[e].handle=key; g_e[e].va=(uint64_t)(uintptr_t)m; g_e[e].size=sz;
  g_e[e].fd=fd; g_e[e].id=id; g_e[e].k=k; g_e[e].hash=0;
  L("MAP e=%d id=%u k=%u va=%llx %ux%u\n",e,id,k,(unsigned long long)g_e[e].va,w,h);
}

typedef int (*mqrd_t)(void*,void*,unsigned long);
static mqrd_t g_real_mq;
int mq_read(void* thiz, void* out, unsigned long cnt) asm(MQREAD);
int mq_read(void* thiz, void* out, unsigned long cnt){
  if(!g_real_mq) g_real_mq=(mqrd_t)dlsym(RTLD_NEXT,MQREAD);
  int r=g_real_mq(thiz,out,cnt);
  if(!r || !out) return r;
  if(__sync_fetch_and_add(&g_fn,0)>=g_maxfs) return r;
  __sync_fetch_and_add(&g_fn,1);
  const uint64_t* w=(const uint64_t*)out;
  if(access(GO,F_OK)!=0) return r;
  if(__sync_fetch_and_add(&g_frames,0)>=g_max) return r;

  for(int i=0;i<g_n;i++){
    int k=(int)g_e[i].k;
    if(k!=g_cama && k!=g_camb) continue;
    uint32_t hh=hashof(g_e[i].va,g_e[i].size);
    if(!hh || hh==g_e[i].hash) continue;
    g_e[i].hash=hh;
    // The pixel hash says a buffer's content moved, but it fires more than once per frame because
    // we can sample a buffer mid-write (measured: cam0=543 vs cam2=711 over the same 20 s, where a
    // 30 Hz stereo pair must be equal). The descriptor's capture timestamp is authoritative and
    // changes exactly once per frame, so use it to dedupe.
    uint64_t cts=w[2+12*k+11];
    int dup=0;
    for(int q=0;q<NSEEN;q++) if(g_seen[k][q]==cts){ dup=1; break; }
    if(dup) continue;
    g_seen[k][g_seenq[k]]=cts; g_seenq[k]=(g_seenq[k]+1)%NSEEN;
    int fno=__sync_fetch_and_add(&g_frames,1);
    if(fno>=g_max) break;
    // The descriptor block for this camera carries its own capture time (notes/34): block k at
    // word 2+12k, field +11. This is the real exposure timestamp, not when we happened to read it.
    uint64_t cap_ns = cts;
    uint64_t off=__sync_fetch_and_add(&g_off,(uint64_t)g_e[i].size);
    size_t done=0;
    while(done<g_e[i].size){
      ssize_t n=pwrite(g_blobfd,(const void*)(uintptr_t)(g_e[i].va+done),g_e[i].size-done,(off_t)(off+done));
      if(n<=0) break;
      done+=(size_t)n;
    }
    char ix[192];
    int nn=snprintf(ix,sizeof ix,"%d %d %llu %llu %llu %u\n",
      fno,k,(unsigned long long)cap_ns,(unsigned long long)mono_ns(),
      (unsigned long long)off,g_e[i].size);
    if(g_idxfd>=0){ if(write(g_idxfd,ix,nn)){} }
  }
  return r;
}

__asm__(".text\n.global "CTOR"\n.type "CTOR",%function\n"CTOR":\n"
"  stp x29,x30,[sp,#-48]!\n  mov x29,sp\n"
"  stp x0,x1,[sp,#16]\n  str x2,[sp,#32]\n"
"  adrp x9, :got:g_real_ctor\n  ldr x9,[x9,#:got_lo12:g_real_ctor]\n  ldr x9,[x9]\n  blr x9\n"
"  ldp x0,x1,[sp,#16]\n  ldr x2,[sp,#32]\n  bl dump_ib\n"
"  ldr x0,[sp,#16]\n  ldp x29,x30,[sp],#48\n  ret\n");

__attribute__((constructor)) static void init_(void){
  mkdir(DIR,0777);
  for(int i=0;i<NENT;i++) g_e[i].fd=-1;
  g_cama=envint("IBFS_CAMA",0); g_camb=envint("IBFS_CAMB",2);
  g_max=envint("IBFS_MAX",8000); g_maxfs=envint("IBFS_MAXFS",60000);
  g_logfd=open(LOG,O_WRONLY|O_CREAT|O_APPEND,0666);
  g_blobfd=open(BLOB,O_WRONLY|O_CREAT|O_TRUNC,0644);
  g_idxfd=open(IDX,O_WRONLY|O_CREAT|O_APPEND,0666);
  g_real_ctor=dlsym(RTLD_NEXT,CTOR);
  g_real_mq=(mqrd_t)dlsym(RTLD_NEXT,MQREAD);
  L("== ibfs_hook9 init camA=%d camB=%d max=%d blobfd=%d ==\n",g_cama,g_camb,g_max,g_blobfd);
}
