// ibfs_hook4.c — resolve which pooled ImageBuffer holds which camera's pixels, cheaply and safely.
//
// State after notes/34: the FrameSet tells us, at 145 Hz, a frameset buffer index (0..15), four
// camIds, and a per-camera capture timestamp. The ImageBuffer ctor tells us (id 0..15 -> pixel VA)
// but only in bursts, and `id` is a rolling index whose VA churns (notes/33). What is missing is the
// join: at FS-read time, which VA holds cam C of this frameset?
//
// Rather than guess a formula (notes/10's `cam = id & 3` and notes/31's slot theory were both
// inferred from bursts, where consecutive ids are trivially consecutive cameras), this measures it:
//
//   - ctor hook keeps a 16-entry table `id -> most recent pixel VA`.
//   - on every FS read, hash ~64 sampled bytes from each of the 16 tracked VAs and log the hashes
//     alongside the frameset index and camIds.
//
// Offline, the entry whose hash changes in lockstep with a given camId IS that camera's buffer. No
// full-frame writes, so this is cheap enough to run inline in Meta's tracking thread and light on a
// device whose UFS wedges under write load.
//
// Safety: a tracked VA may have been freed since its ctor, and a bare read would SIGSEGV inside
// trackingservice and kill tracking. Probes therefore go through a file descriptor, which reports
// an error instead of faulting — see probe() for why /proc/self/mem and not process_vm_readv.
//
// Build: see build4.sh.   Env: IBFS_MAXFS (default 3000).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <dlfcn.h>

#define CTOR "_ZN3OVR7Sensors11ImageBufferC1EPK13native_handleS4_"
#define MQREAD "_ZN7android8hardware12MessageQueueIN6vendor6oculus8hardware7sensors4V1_08FrameSetELNS0_8MQFlavorE1EE4readEPS7_m"
#define DIR "/data/local/tmp/cap4"
#define LOG DIR"/join.log"
#define NSLOT 16
#define NSAMP 64          /* bytes sampled per buffer, spread across the frame */

static int g_logfd=-1, g_maxfs=3000, g_fn, g_seq;
static volatile uint64_t g_va[NSLOT];     /* id -> most recent pixel VA */
static volatile uint32_t g_wh[NSLOT];     /* id -> w*h, for stride of the sampling */
static pid_t g_pid;

static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void L(const char* f,...){ char b[2200]; va_list a; va_start(a,f); vsnprintf(b,sizeof b,f,a); va_end(a);
  if(g_logfd>=0){ if(write(g_logfd,b,strlen(b))){} } }
static int okptr(uint64_t v){ return v>0x1000ull && v<0x1000000000000ull && (v&7)==0; }
static int envint(const char* k,int d){ const char* v=getenv(k); return v&&*v?(int)strtol(v,0,0):d; }

// Sample bytes spread across the buffer and fold to 32 bits. Returns 0 if unreadable.
//
// Reads go through /proc/self/mem rather than dereferencing the VA. A pooled buffer may have been
// freed since its ctor, and a bare load would SIGSEGV inside trackingservice and kill tracking;
// pread just returns -1/EIO. process_vm_readv was tried first and returned EFAULT for *every*
// probe including ones concurrent with live ctor activity, so it is blocked here (seccomp) rather
// than reporting genuinely stale pages — /proc/self/mem has no such restriction.
//
// Two 32-byte windows, well separated, rather than 64 single-byte reads: same discrimination at
// 1/32 the syscall cost, which matters because this runs inline in Meta's delivery thread.
static int g_memfd = -1;
static uint32_t probe(uint64_t va, uint32_t wh){
  if(!okptr(va) || wh<4096 || g_memfd<0) return 0;
  uint8_t buf[64];
  off_t o1 = (off_t)va + (off_t)(wh/4);
  off_t o2 = (off_t)va + (off_t)(wh/2) + 777;
  if(pread(g_memfd,buf,32,o1)!=32) return 0;
  if(pread(g_memfd,buf+32,32,o2)!=32) return 0;
  uint32_t h=2166136261u;
  for(int i=0;i<64;i++){ h^=buf[i]; h*=16777619u; }
  return h?h:1;
}

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
  if(id<NSLOT){ g_va[id]=px; g_wh[id]=w*h; }
  L("IB seq=%d host=%llu id=%u %ux%u px=%llx\n",
    __sync_fetch_and_add(&g_seq,1),(unsigned long long)mono_ns(),id,w,h,(unsigned long long)px);
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
    char b[2200]; int o=0;
    // frameset seq, buffer index, and the per-camera capture timestamps (block k at 2+12k, +11)
    o+=snprintf(b+o,sizeof b-o,"FS host=%llu seq=%llu buf=%llu",
                (unsigned long long)mono_ns(),(unsigned long long)w[0],
                (unsigned long long)(w[2]>>32));
    for(int k=0;k<4;k++)
      o+=snprintf(b+o,sizeof b-o," c%d=%llu:%llu",k,
                  (unsigned long long)(w[2+12*k]&0xffffffff),
                  (unsigned long long)w[2+12*k+11]);
    o+=snprintf(b+o,sizeof b-o," H:");
    for(int i=0;i<NSLOT;i++)
      o+=snprintf(b+o,sizeof b-o," %d:%08x",i,probe(g_va[i],g_wh[i]));
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
  g_pid=getpid();
  g_memfd=open("/proc/self/mem",O_RDONLY|O_CLOEXEC);
  g_maxfs=envint("IBFS_MAXFS",3000);
  g_logfd=open(LOG,O_WRONLY|O_CREAT|O_APPEND,0666);
  g_real_ctor=dlsym(RTLD_NEXT,CTOR);
  g_real_mq=(mqrd_t)dlsym(RTLD_NEXT,MQREAD);
  L("== ibfs_hook4 init ctor=%p mqread=%p pid=%d memfd=%d nslot=%d maxfs=%d ==\n",
    g_real_ctor,(void*)g_real_mq,(int)g_pid,g_memfd,NSLOT,g_maxfs);
}
