// img_shim.c v5 — LD_PRELOAD into trackingservice. Precise-timestamp stereo capture.
//  thread A (fs_hook): hooks MessageQueue<FrameSet>::read; logs {host mono_ns, w11 exposure ns}.
//  thread B (pixel_scanner): change-detects camera dmabufs; dumps each new frame + host mono_ns
//                            + dmabuf address (address-group => camId).
// Both on CLOCK_MONOTONIC. Offline: assign each frame the nearest w11 (true exposure ts, same
// clock) and pair cameras by exposure -> precise, truly-simultaneous stereo. Starts on GO file.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <dlfcn.h>

#define DIR "/data/local/tmp/cap"
#define FRAME_MIN 300000
#define FRAME_MAX 400000
#define MAXR 256
#define CAP_NS (12ull*1000000000ull)
#define MQREAD "_ZN7android8hardware12MessageQueueIN6vendor6oculus8hardware7sensors4V1_08FrameSetELNS0_8MQFlavorE1EE4readEPS7_m"

static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static volatile int g_go, g_stop;
static void wait_go(void){ while(!g_go && access(DIR"/GO",F_OK)!=0) usleep(20000); g_go=1; }

// ---- FrameSet hook: log w11 exposure ts with host time ----
static int g_fsfd=-1;
typedef int (*rd_t)(void*,void*,unsigned long);
int mq_read(void* thiz, void* out, unsigned long cnt) asm(MQREAD);
int mq_read(void* thiz, void* out, unsigned long cnt){
  static rd_t real; if(!real) real=(rd_t)dlsym(RTLD_NEXT,MQREAD);
  int r=real(thiz,out,cnt);
  if(r && out && g_go && !g_stop){
    uint64_t h=mono_ns(); uint64_t w11=((const uint64_t*)out)[11];
    if(g_fsfd<0) g_fsfd=open(DIR"/fs.log",O_WRONLY|O_CREAT|O_TRUNC,0666);
    if(g_fsfd>=0){ char b[64]; int n=snprintf(b,sizeof b,"%llu %llu\n",(unsigned long long)h,(unsigned long long)w11); if(write(g_fsfd,b,n)){} }
  }
  return r;
}


// ---- syncboss reader: host-stamped IMU(0x50)+exposure(0x51) for clock mapping ----
static void* sb_reader(void* _){ (void)_; wait_go();
  int sfd=open("/dev/syncboss_stream0",O_RDONLY); int out=open(DIR"/sb.rec",O_WRONLY|O_CREAT|O_TRUNC,0666);
  if(sfd<0||out<0) return 0; uint64_t t0=mono_ns(); uint8_t pkt[4096];
  while(mono_ns()-t0 < CAP_NS){ int n=read(sfd,pkt,sizeof pkt); if(n<=0) continue;
    uint64_t h=mono_ns(); uint16_t L=n; if(write(out,&h,8)){} if(write(out,&L,2)){} if(write(out,pkt,n)){} }
  close(sfd); close(out); return 0;
}

// ---- pixel scanner ----
struct rg { uintptr_t a; size_t sz; uint64_t hash; int seq; };
static struct rg R[MAXR]; static int NR;
static uint64_t shash(const volatile uint8_t* p, size_t sz){ uint64_t h=1469598103934665603ull; for(size_t k=1000;k<sz;k+=4096){h^=p[k];h*=1099511628211ull;} return h; }
static int findr(uintptr_t a){ for(int i=0;i<NR;i++) if(R[i].a==a) return i; return -1; }
static void* pixel_scanner(void* _){ (void)_;
  mkdir(DIR,0777); wait_go();
  int idx=open(DIR"/frames.idx",O_WRONLY|O_CREAT|O_TRUNC,0666);
  static uint8_t buf[FRAME_MAX]; uint64_t t0=mono_ns();
  while(mono_ns()-t0 < CAP_NS){
    usleep(5000);
    FILE* f=fopen("/proc/self/maps","r"); if(!f) continue; char line[512];
    while(fgets(line,sizeof line,f)){
      if(!strstr(line,"anon_inode:dmabuf")) continue;
      uintptr_t a,b; if(sscanf(line,"%lx-%lx",&a,&b)!=2) continue;
      size_t sz=b-a; if(sz<FRAME_MIN||sz>FRAME_MAX) continue;
      const volatile uint8_t* p=(const volatile uint8_t*)a; uint64_t hh=shash(p,sz); int i=findr(a);
      if(i<0){ if(NR<MAXR){i=NR++;R[i].a=a;R[i].sz=sz;R[i].hash=hh;R[i].seq=0;} continue; }
      if(hh==R[i].hash) continue; R[i].hash=hh;
      uint32_t mn=255,mx=0; for(size_t k=1000;k<sz;k+=6143){uint8_t v=p[k]; if(v<mn)mn=v; if(v>mx)mx=v;}
      if(mx-mn<24) continue;
      uint64_t ts=mono_ns(); int seq=R[i].seq++;
      for(size_t k=0;k<sz;k++) buf[k]=p[k];
      char op[96]; snprintf(op,sizeof op,DIR"/f_%lx_%04d.gray",a,seq);
      int of=open(op,O_WRONLY|O_CREAT|O_TRUNC,0644); if(of>=0){ if(write(of,buf,sz)){} close(of); }
      char l[128]; int n=snprintf(l,sizeof l,"%llu %lx %d\n",(unsigned long long)ts,a,seq); if(write(idx,l,n)){}
    }
    fclose(f);
  }
  g_stop=1; close(idx); if(g_fsfd>=0) close(g_fsfd); return 0;
}
__attribute__((constructor)) static void init_(void){ pthread_t u,s2; if(!pthread_create(&u,0,pixel_scanner,0)) pthread_detach(u); if(!pthread_create(&s2,0,sb_reader,0)) pthread_detach(s2); }
