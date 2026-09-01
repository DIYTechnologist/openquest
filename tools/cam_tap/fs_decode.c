// fs_decode.c — LD_PRELOAD into trackingservice. Decode the HIDL FrameSet wire struct.
// Hooks MessageQueue<FrameSet>::read(out,n); on success dumps the first bytes of the 984-byte
// FrameSet with a host timestamp, and (thread B) host-stamps syncboss type-0x51 exposure ts,
// so we can correlate a FrameSet field to the known nRF camera timestamp.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <dlfcn.h>

#define MQREAD "_ZN7android8hardware12MessageQueueIN6vendor6oculus8hardware7sensors4V1_08FrameSetELNS0_8MQFlavorE1EE4readEPS7_m"
#define LOG "/data/local/tmp/fs.log"
static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void L(const char* f,...){ char b[1024]; va_list a; va_start(a,f); vsnprintf(b,sizeof b,f,a); va_end(a);
  int fd=open(LOG,O_WRONLY|O_CREAT|O_APPEND,0666); if(fd>=0){ if(write(fd,b,strlen(b))){} close(fd);} }

static void* sbthread(void* _){ (void)_;
  int sfd=open("/dev/syncboss_stream0",O_RDONLY); if(sfd<0) return 0;
  uint8_t p[4096];
  for(;;){ int n=read(sfd,p,sizeof p); if(n<=0) continue; uint64_t h=mono_ns();
    int j=0; while(j+6<=n){ if(p[j]==1&&p[j+1]==3&&p[j+2]==0&&p[j+4]==0){ int t=p[j+3],l=p[j+5];
      if(t==0x51&&j+10<=n){ uint32_t ts; memcpy(&ts,p+j+6,4); L("SB51 host=%llu nrf=%u\n",(unsigned long long)h,ts);} j+=6+l;} else j++; } }
  return 0;
}

static int g_n=0;
typedef int (*rd_t)(void*,void*,unsigned long);
int mq_read(void* thiz, void* out, unsigned long cnt) asm(MQREAD);
int mq_read(void* thiz, void* out, unsigned long cnt){
  static rd_t real; if(!real) real=(rd_t)dlsym(RTLD_NEXT,MQREAD);
  int r=real(thiz,out,cnt);
  if(r && out && __sync_fetch_and_add(&g_n,0)<200){
    uint64_t h=mono_ns(); const uint8_t* b=out; int id=__sync_fetch_and_add(&g_n,1);
    for(int base=0;base<384;base+=128){ char hx[3*128+1]; int o=0;
      for(int i=0;i<128;i++) o+=snprintf(hx+o,sizeof(hx)-o,"%02x ",b[base+i]);
      L("FS %d +%d host=%llu %s\n", id, base, (unsigned long long)h, hx); }
  }
  return r;
}
__attribute__((constructor)) static void init_(void){ pthread_t t; if(!pthread_create(&t,0,sbthread,0)) pthread_detach(t); }
