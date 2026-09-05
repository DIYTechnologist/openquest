// ibfs_hook8.c — track ALL the pooled buffers, keyed by handle, and identify cameras from the
// descriptor rather than from pixels.
//
// notes/37: the pool is 64 buffers (16 frameset ring indices x 4 cameras), not 16. `id` names only
// the frameset slot, so the four ctors sharing an `id` are the four cameras — and hook5's
// `g_va[id]` table kept whichever ran last, consistently the same camera. Every previous attempt to
// identify cameras from pixel content was therefore comparing one camera against itself at
// different times, which is why static, slow-motion and fast-motion captures all failed differently.
//
// Two changes fix it:
//
//   1. Key the table on the **pixel native_handle pointer**, with room for 64 entries, so every
//      distinct buffer keeps its own persistent mmap instead of being overwritten.
//   2. Record, per entry, the position `k` of its ctor within that id's run of four. The four ctors
//      for an id run in a fixed camera order, so `k` should be camId — a claim this hook makes
//      falsifiable rather than assuming, because the descriptor independently supplies camId.
//
// Validation is then self-contained: on each frameset exactly four entries should change, and their
// `k` values should be a permutation of 0..3. If they are, `k` is camId and camera identity is
// solved with no pixel inference at all. If they are not, the log says so directly.
//
// Logging is differential — only entries whose content changed are logged per FrameSet — which
// keeps it compact at 145 Hz.
//
// Build: see build8.sh.  Env: IBFS_MAXFS (default 6000).
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
#define DIR "/data/local/tmp/cap8"
#define LOG DIR"/ident.log"
#define NENT 96              /* >= 64 expected, with headroom for pool churn */

struct ent {
  uint64_t handle;           /* pixel native_handle pointer: the key */
  uint64_t va;               /* our own persistent mmap */
  uint32_t size;
  int      fd;               /* dup'd dmabuf fd, keeps the mapping alive */
  uint32_t id;               /* frameset ring index this ctor carried */
  uint32_t k;                /* position within this id's run of four ctors */
  uint32_t hash;
};
static struct ent g_e[NENT];
static int g_n, g_logfd=-1, g_maxfs=6000, g_fn, g_seq;
static uint32_t g_kcount[16];       /* per-id ctor counter, mod 4 */
static int g_dumped, g_dumpat=1500;

static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void L(const char* f,...){ char b[3000]; va_list a; va_start(a,f); vsnprintf(b,sizeof b,f,a); va_end(a);
  if(g_logfd>=0){ if(write(g_logfd,b,strlen(b))){} } }
static int okptr(uint64_t v){ return v>0x1000ull && v<0x1000000000000ull && (v&7)==0; }
static int envint(const char* k,int d){ const char* v=getenv(k); return v&&*v?(int)strtol(v,0,0):d; }

// Direct reads of OUR mapping (see notes/35: /proc/self/mem refuses VM_PFNMAP dmabuf mappings, and
// dereferencing is safe because the mapping is ours and pinned by a dup'd fd).
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
  if(nh->numFds<1 || nh->numFds>16) return;

  uint64_t key=(uint64_t)(uintptr_t)hpix;
  int slot=-1;
  for(int i=0;i<g_n;i++) if(g_e[i].handle==key){ slot=i; break; }
  uint32_t k = g_kcount[id] & 3;
  g_kcount[id]++;

  if(slot<0 && g_n<NENT){
    int fd=dup(nh->data[0]);
    if(fd<0) return;
    size_t sz=(size_t)w*h;
    void* m=mmap(NULL,sz,PROT_READ,MAP_SHARED,fd,0);
    if(m==MAP_FAILED){ close(fd); return; }
    slot=g_n++;
    g_e[slot].handle=key; g_e[slot].va=(uint64_t)(uintptr_t)m; g_e[slot].size=sz;
    g_e[slot].fd=fd; g_e[slot].id=id; g_e[slot].k=k; g_e[slot].hash=0;
    L("MAP e=%d host=%llu id=%u k=%u handle=%llx va=%llx %ux%u nfds=%d\n",
      slot,(unsigned long long)mono_ns(),id,k,(unsigned long long)key,
      (unsigned long long)g_e[slot].va,w,h,nh->numFds);
  } else if(slot>=0){
    // Same buffer reused. Record whether (id,k) is stable for this handle — if it is not, `k`
    // cannot be camId and the whole approach needs rethinking, so make it visible.
    if(g_e[slot].id!=id || g_e[slot].k!=k)
      L("REBIND e=%d id %u->%u k %u->%u\n",slot,g_e[slot].id,id,g_e[slot].k,k);
    g_e[slot].id=id; g_e[slot].k=k;
  }
  __sync_fetch_and_add(&g_seq,1);
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
    char b[3000]; int o=0;
    o+=snprintf(b+o,sizeof b-o,"FS n=%d fsseq=%llu buf=%llu",
                g_n,(unsigned long long)w[0],(unsigned long long)(w[2]>>32));
    for(int c=0;c<4;c++)
      o+=snprintf(b+o,sizeof b-o," c%d=%llu:%llu",c,
                  (unsigned long long)(w[2+12*c]&0xffffffff),
                  (unsigned long long)w[2+12*c+11]);
    // differential: only entries whose pixels changed since the last FrameSet
    o+=snprintf(b+o,sizeof b-o," CH:");
    for(int i=0;i<g_n && o<(int)sizeof b-40;i++){
      uint32_t hh=hashof(g_e[i].va,g_e[i].size);
      if(hh && hh!=g_e[i].hash){
        g_e[i].hash=hh;
        o+=snprintf(b+o,sizeof b-o," %d/id%u/k%u",i,g_e[i].id,g_e[i].k);
      }
    }
    L("%s\n",b);

    // Visual confirmation that k really is the camera: freeze one buffer per k at one instant.
    // Four distinct viewpoints proves the partition; four similar ones would falsify it.
    if(g_fn>=g_dumpat && !__sync_fetch_and_add(&g_dumped,0) && g_n>=64){
      __sync_fetch_and_add(&g_dumped,1);
      for(int kk=0;kk<4;kk++){
        for(int i=0;i<g_n;i++){
          if(g_e[i].k!=(uint32_t)kk || !g_e[i].va) continue;
          char pth[96]; snprintf(pth,sizeof pth,DIR"/cam%d.gray",kk);
          int f=open(pth,O_WRONLY|O_CREAT|O_TRUNC,0644);
          if(f>=0){ if(write(f,(const void*)(uintptr_t)g_e[i].va,g_e[i].size)){} close(f); }
          break;
        }
      }
      L("DUMP4 done at fs=%d\n",g_fn);
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
  for(int i=0;i<NENT;i++) g_e[i].fd=-1;
  g_maxfs=envint("IBFS_MAXFS",6000);
  g_dumpat=envint("IBFS_DUMPAT",1500);
  g_logfd=open(LOG,O_WRONLY|O_CREAT|O_APPEND,0666);
  g_real_ctor=dlsym(RTLD_NEXT,CTOR);
  g_real_mq=(mqrd_t)dlsym(RTLD_NEXT,MQREAD);
  L("== ibfs_hook8 init ctor=%p mqread=%p nent=%d maxfs=%d ==\n",
    g_real_ctor,(void*)g_real_mq,NENT,g_maxfs);
}
