// img_shim.c v4 — LD_PRELOAD into trackingservice. Synchronized VIO capture:
//  thread A (sb_reader): reads /dev/syncboss_stream0, host-timestamps every packet.
//  thread B (pixel_scanner): change-detects camera dmabufs, dumps each new frame + host ts.
// Both share CLOCK_MONOTONIC, so pixel frames align to syncboss type-0x51 exposure timestamps
// by host time; Basalt then uses the precise nRF (syncboss) timestamps. Capture starts when
// /data/local/tmp/cap/GO appears and runs CAP_NS, so tracking can converge first.
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

#define DIR "/data/local/tmp/cap"
#define FRAME_MIN 300000
#define FRAME_MAX 400000
#define MAXR 256
#define CAP_NS (12ull*1000000000ull)

static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static int wait_go(void){ while(access(DIR"/GO",F_OK)!=0) usleep(50000); return 1; }

static void* sb_reader(void* _){ (void)_;
  wait_go();
  int sfd=open("/dev/syncboss_stream0",O_RDONLY);
  int out=open(DIR"/sb.rec",O_WRONLY|O_CREAT|O_TRUNC,0666);
  if(sfd<0||out<0) return 0;
  uint64_t t0=mono_ns(); uint8_t pkt[4096];
  while(mono_ns()-t0 < CAP_NS){
    int n=read(sfd,pkt,sizeof pkt);
    if(n<=0) continue;
    uint64_t h=mono_ns(); uint16_t L=n;
    if(write(out,&h,8)){} if(write(out,&L,2)){} if(write(out,pkt,n)){}
  }
  close(sfd); close(out); return 0;
}

struct rg { uintptr_t a; size_t sz; uint64_t hash; int seq; };
static struct rg R[MAXR]; static int NR;
static uint64_t sample_hash(const volatile uint8_t* p, size_t sz){
  uint64_t h=1469598103934665603ull;
  for(size_t k=1000;k<sz;k+=4096){ h^=p[k]; h*=1099511628211ull; } return h;
}
static int find_rg(uintptr_t a){ for(int i=0;i<NR;i++) if(R[i].a==a) return i; return -1; }

static void* pixel_scanner(void* _){ (void)_;
  mkdir(DIR,0777); wait_go();
  int idx=open(DIR"/frames.idx",O_WRONLY|O_CREAT|O_TRUNC,0666);
  static uint8_t buf[FRAME_MAX];
  uint64_t t0=mono_ns();
  while(mono_ns()-t0 < CAP_NS){
    usleep(5000);
    FILE* f=fopen("/proc/self/maps","r"); if(!f) continue;
    char line[512];
    while(fgets(line,sizeof line,f)){
      if(!strstr(line,"anon_inode:dmabuf")) continue;
      uintptr_t a,b; if(sscanf(line,"%lx-%lx",&a,&b)!=2) continue;
      size_t sz=b-a; if(sz<FRAME_MIN||sz>FRAME_MAX) continue;
      const volatile uint8_t* p=(const volatile uint8_t*)a;
      uint64_t hh=sample_hash(p,sz); int i=find_rg(a);
      if(i<0){ if(NR<MAXR){ i=NR++; R[i].a=a; R[i].sz=sz; R[i].hash=hh; R[i].seq=0; } continue; }
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
  close(idx); return 0;
}
__attribute__((constructor)) static void init_(void){
  pthread_t t; if(!pthread_create(&t,0,sb_reader,0)) pthread_detach(t);
  pthread_t u; if(!pthread_create(&u,0,pixel_scanner,0)) pthread_detach(u);
}
