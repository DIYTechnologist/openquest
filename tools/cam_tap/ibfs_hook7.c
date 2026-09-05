// ibfs_hook7.c — camera identification under motion.
//
// notes/36: a single 16-slot dump on a static desk splits the pool cleanly into two groups of eight
// (left pair vs right pair) but cannot resolve four cameras, because parallax is the only thing
// that separates the members of a pair and a stationary headset has none.
//
// So dump all 16 slots REPEATEDLY while the headset is moved by hand. Each round freezes all 16 at
// one instant, so across rounds every slot has a trajectory; slots belonging to the same physical
// camera move together, and the two members of a pair separate by their baseline.
//
// One round = 16 x 307,840 B written as a single blob, so nothing ever has to readdir a large
// directory (the UFS failure mode -- see the ufs-link-death memory).
//
// hook5 established that our own dmabuf mappings give live pixels at FrameSet rate. What is still
// unknown is which slot is which camera. Three inferences have now been wrong about this
// (notes/10's `cam = id & 3`, notes/31's slot theory, and the slot<->buf bijection in notes/35,
// which cannot be the whole story: it assigns ONE slot per frameset index, yet a frameset carries
// FOUR camera images). Rather than infer a fourth time, dump one full frame from every slot and
// look at the pixels. The four cameras point in different directions, so identifying them is
// visual, not statistical.
//
// Dumps once, after IBFS_DUMPAT FrameSet reads, so the pool is warm and steady-state.
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
// WHY THIS EXISTS: hook4 tracked `id -> pixel VA` from the ctor and probed those VAs at FrameSet
// time. Every probe failed, via both process_vm_readv AND /proc/self/mem, including probes taken
// while ctor activity was live. The VA is not stale-and-sometimes-valid; it is simply gone.
// notes/10 already said why and it was read past: `this+0x60` is the **locked** CPU pixel VA. The
// gralloc buffer is locked for the ctor and unlocked afterwards, so between ctor bursts there is
// no mapping to read. No VA-tracking scheme can work.
//
// So take the ctor's *arguments* instead of its result. The signature is
//   ImageBuffer(const native_handle* meta, const native_handle* pixels)
// so x1/x2 carry the handles the buffer is built from. We dup the dmabuf fd out of the pixel
// handle and mmap it ourselves: our mapping is independent of Meta's lock/unlock cycle and stays
// valid, so pixels can be read at FrameSet rate (145 Hz) rather than only during allocation bursts.
//
// The fd must be dup'd — Meta owns and will close the original.
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
#include <sys/mman.h>

#define CTOR "_ZN3OVR7Sensors11ImageBufferC1EPK13native_handleS4_"
#define MQREAD "_ZN7android8hardware12MessageQueueIN6vendor6oculus8hardware7sensors4V1_08FrameSetELNS0_8MQFlavorE1EE4readEPS7_m"
#define DIR "/data/local/tmp/cap7"
#define LOG DIR"/map7.log"
#define NSLOT 16
#define NSAMP 64          /* bytes sampled per buffer, spread across the frame */

static int g_logfd=-1, g_maxfs=3000, g_fn, g_seq, g_dumpat=200, g_dumped;
static int g_every=128, g_ndumps=30, g_round;
static volatile uint64_t g_va[NSLOT];     /* id -> OUR mmap of that buffer (not Meta's VA) */
static volatile uint32_t g_wh[NSLOT];     /* id -> w*h */
static int      g_fd[NSLOT];              /* id -> dup'd dmabuf fd */
static uint64_t g_hva[NSLOT];             /* id -> pixel handle, to detect rebinding */
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
  // Direct reads, not /proc/self/mem: a dmabuf mapping is VM_PFNMAP, which access_remote_vm
  // refuses, so pread returns EIO on a perfectly valid mapping (this cost one debug cycle).
  // Dereferencing is safe here precisely because the mapping is OURS and pinned by a dup'd fd —
  // unlike Meta's transient locked VA, it cannot vanish under us.
  const volatile uint8_t* p1=(const volatile uint8_t*)(uintptr_t)(va + wh/4);
  const volatile uint8_t* p2=(const volatile uint8_t*)(uintptr_t)(va + wh/2 + 777);
  for(int i=0;i<32;i++) buf[i]=p1[i];
  for(int i=0;i<32;i++) buf[32+i]=p2[i];
  uint32_t h=2166136261u;
  for(int i=0;i<64;i++){ h^=buf[i]; h*=16777619u; }
  return h?h:1;
}

void* g_real_ctor;
// native_handle: { int version; int numFds; int numInts; int data[]; }  data[0..numFds-1] = fds.
struct nhandle { int version; int numFds; int numInts; int data[]; };

void dump_ib(void* thiz, const void* hmeta, const void* hpix);
void dump_ib(void* thiz, const void* hmeta, const void* hpix){
  if(!thiz || (((uintptr_t)thiz)&7)) return;
  const uint8_t* p=(const uint8_t*)thiz;
  uint64_t sd=*(const uint64_t*)(p+0x40), px=*(const uint64_t*)(p+0x60);
  if(!okptr(sd)||!okptr(px)) return;
  const uint8_t* s=(const uint8_t*)sd;
  uint32_t id=*(const uint32_t*)(s+0x00), w=*(const uint32_t*)(s+0x08), h=*(const uint32_t*)(s+0x0c);
  if(w==0||h==0||w>4096||h>4096) return;
  int mapped=-1, nfds=-1;
  if(id<NSLOT && hpix){
    const struct nhandle* nh=(const struct nhandle*)hpix;
    nfds=nh->numFds;
    // Re-map only when this id is bound to a different handle than last time; the pool rebinds
    // often (notes/33: ~40 distinct VAs per id) but the underlying dmabuf set is small.
    if(nh->numFds>0 && nh->numFds<16 && g_hva[id]!=(uint64_t)(uintptr_t)hpix){
      int fd=dup(nh->data[0]);
      if(fd>=0){
        size_t sz=(size_t)w*h;
        void* m=mmap(NULL,sz,PROT_READ,MAP_SHARED,fd,0);
        if(m!=MAP_FAILED){
          if(g_va[id]) munmap((void*)(uintptr_t)g_va[id],g_wh[id]);
          if(g_fd[id]>=0) close(g_fd[id]);
          g_va[id]=(uint64_t)(uintptr_t)m; g_wh[id]=sz; g_fd[id]=fd; g_hva[id]=(uint64_t)(uintptr_t)hpix;
          mapped=1;
        } else { close(fd); mapped=0; }
      }
    } else mapped=2;   // already mapped for this handle
  }
  L("IB seq=%d host=%llu id=%u %ux%u metapx=%llx nfds=%d mapped=%d ourva=%llx\n",
    __sync_fetch_and_add(&g_seq,1),(unsigned long long)mono_ns(),id,w,h,
    (unsigned long long)px,nfds,mapped,(unsigned long long)g_va[id]);
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

    // Freeze all 16 slots at one instant, once every g_every FrameSets, g_ndumps times.
    if(g_fn>=g_dumpat && g_round<g_ndumps && ((g_fn-g_dumpat)%g_every)==0){
      int rd=g_round++;
      char pth[96]; snprintf(pth,sizeof pth,DIR"/round_%03d.bin",rd);
      int f=open(pth,O_WRONLY|O_CREAT|O_TRUNC,0644);
      if(f>=0){
        for(int i=0;i<NSLOT;i++){
          if(g_va[i] && g_wh[i]){ if(write(f,(const void*)(uintptr_t)g_va[i],g_wh[i])){} }
          else { static const char z[4096]={0}; for(size_t n=0;n<307840;n+=4096) if(write(f,z,4096)){} }
        }
        close(f);
      }
      L("DUMP round=%d fs=%d host=%llu\n",rd,g_fn,(unsigned long long)mono_ns());
    }
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
  g_pid=getpid();
  for(int i=0;i<NSLOT;i++) g_fd[i]=-1;
  g_memfd=open("/proc/self/mem",O_RDONLY|O_CLOEXEC);
  g_maxfs=envint("IBFS_MAXFS",3000);
  g_dumpat=envint("IBFS_DUMPAT",200);
  g_every=envint("IBFS_DUMPEVERY",128);
  g_ndumps=envint("IBFS_NDUMPS",30);
  g_logfd=open(LOG,O_WRONLY|O_CREAT|O_APPEND,0666);
  g_real_ctor=dlsym(RTLD_NEXT,CTOR);
  g_real_mq=(mqrd_t)dlsym(RTLD_NEXT,MQREAD);
  L("== ibfs_hook7 init ctor=%p mqread=%p pid=%d memfd=%d nslot=%d maxfs=%d ==\n",
    g_real_ctor,(void*)g_real_mq,(int)g_pid,g_memfd,NSLOT,g_maxfs);
}
