// fs_atomic.c v1 (decode) — LD_PRELOAD into trackingservice. Interpose the sret-returning
// DualStreamHandle<FrameSet>::read() via an asm trampoline, and DUMP the returned
// OVR::Sensors::FrameSet (read-only) so we can decode its layout (timestamp, ImageBuffer ptrs,
// camId). Also host-stamps syncboss exposures for correlation. No pixel extraction yet.
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

#define READ_MANGLED "_ZN3OVR7Sensors11HidlWrapper16DualStreamHandleINS0_8FrameSetEN6vendor6oculus8hardware7sensors4V1_08FrameSetEE4readEv"
#define LOG "/data/local/tmp/fsa.log"
static uint64_t mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void L(const char* f,...){ char b[2048]; va_list a; va_start(a,f); vsnprintf(b,sizeof b,f,a); va_end(a);
  int fd=open(LOG,O_WRONLY|O_CREAT|O_APPEND,0666); if(fd>=0){ if(write(fd,b,strlen(b))){} close(fd);} }

void* g_real;   // real read()

// module ranges (filled at init) to classify pointers
static uintptr_t g_hidlwrap_lo,g_hidlwrap_hi,g_imgbuf_lo,g_imgbuf_hi;
static void ranges(void){
  FILE* f=fopen("/proc/self/maps","r"); if(!f) return; char line[512];
  while(fgets(line,sizeof line,f)){
    uintptr_t a,b; char perm[8],path[400]; path[0]=0;
    if(sscanf(line,"%lx-%lx %7s %*s %*s %*s %399[^\n]",&a,&b,perm,path)<3) continue;
    if(strstr(path,"libimagebuffer.so")){ if(!g_imgbuf_lo||a<g_imgbuf_lo)g_imgbuf_lo=a; if(b>g_imgbuf_hi)g_imgbuf_hi=b; }
    if(strstr(path,"libvrsensors-hidlwrapper.so")){ if(!g_hidlwrap_lo||a<g_hidlwrap_lo)g_hidlwrap_lo=a; if(b>g_hidlwrap_hi)g_hidlwrap_hi=b; }
  }
  fclose(f);
}
static int readable(const void* p){ // crude: pointer in a plausible heap/lib range
  uintptr_t v=(uintptr_t)p; return v>0x1000 && v<0x8000000000ull;
}

static int g_n;
void dump_fs(void* thiz, void* x0v, void* x1v);
void dump_fs(void* thiz, void* x0v, void* x1v){
  if(__sync_fetch_and_add(&g_n,0)>=12) return;
  int id=__sync_fetch_and_add(&g_n,1); uint64_t h=mono_ns();
  L("RET %d host=%llu this=%llx x0=%llx x1=%llx\n",id,(unsigned long long)h,(unsigned long long)(uintptr_t)thiz,(unsigned long long)(uintptr_t)x0v,(unsigned long long)(uintptr_t)x1v);
  // dump DualStreamHandle members 0..0x120 (find frameset storage / ImageBuffer ptrs)
  if(readable(thiz)&&(((uintptr_t)thiz)&7)==0){ const uint64_t* t=(const uint64_t*)thiz; char b[3072]; int o=0;
    o+=snprintf(b+o,sizeof(b)-o,"  this.W:");
    for(int i=0;i<40;i++){ uint64_t v=t[i]; const char* tg=""; if(v>=g_imgbuf_lo&&v<g_imgbuf_hi)tg="<img>"; else if(v>0x7000000000ull&&v<0x8000000000ull&&(v&7)==0)tg="<p>";
      o+=snprintf(b+o,sizeof(b)-o," %d:%llx%s",i,(unsigned long long)v,tg);} L("%s\n",b);
    // follow this+0xc0 (word 24) as pointer, dump its region
    uint64_t p=t[24]; if(p>0x7000000000ull&&p<0x8000000000ull&&(p&7)==0){ const uint64_t* q=(const uint64_t*)p; char c[3072]; int oo=0; oo+=snprintf(c+oo,sizeof(c)-oo,"  *[+0xc0]:");
      for(int i=0;i<48;i++){ uint64_t v=q[i]; const char* tg=""; if(v>=g_imgbuf_lo&&v<g_imgbuf_hi)tg="<img>"; else if(v>0x7000000000ull&&v<0x8000000000ull&&(v&7)==0)tg="<p>";
        oo+=snprintf(c+oo,sizeof(c)-oo," %d:%llx%s",i,(unsigned long long)v,tg);} L("%s\n",c);
      for(int i=0;i<48;i++){ uint64_t v=q[i]; if(v>0x7000000000ull&&v<0x8000000000ull&&(v&7)==0){ uint64_t vt=*(volatile uint64_t*)v; if(vt>=g_imgbuf_lo&&vt<g_imgbuf_hi) L("    +0xc0.w%d -> IMAGEBUFFER@%llx\n",i,(unsigned long long)v);} }
    }
  }
  // scan objects that 'this' points to, looking for frame dims (640x480) + native_handle/dmabuf
  if(readable(thiz)&&(((uintptr_t)thiz)&7)==0){ const uint64_t* t=(const uint64_t*)thiz;
    for(int i=0;i<80;i++){ uint64_t p=t[i];
      if(p>0x7000000000ull&&p<0x8000000000ull&&(p&7)==0){ const uint32_t* q=(const uint32_t*)p;
        for(int k=0;k<64;k++){ uint32_t v=q[k];
          if(v==640){ uint32_t v2=q[k+1];
            if(v2==480||v2==481||v2==640){ char b[256]; int o=0;
              o+=snprintf(b+o,sizeof(b)-o,"  DIMS@ this[%d]=%llx +%d: ",i,(unsigned long long)p,k*4);
              for(int m=-4;m<12;m++) o+=snprintf(b+o,sizeof(b)-o,"%x ",q[k+m]);
              L("%s\n",b); break; } } }
      }
    }
  }
  return;
  // deref x0 (and x1) as struct pointers; dump 40 words each
  for(int which=0;which<2;which++){
    void* base=which?x1v:x0v; if(!(readable(base)&&(((uintptr_t)base)&7)==0)) continue;
    const uint64_t* w=(const uint64_t*)base; char b[3072]; int o=0;
    o+=snprintf(b+o,sizeof(b)-o,"  *%c W:",which?'1':'0');
    for(int i=0;i<40;i++){ uint64_t v; if(readable((void*)(w+i))) v=w[i]; else break;
      const char* tag=""; if(v>=g_imgbuf_lo&&v<g_imgbuf_hi)tag="<img>"; else if(v>0x7000000000ull&&v<0x8000000000ull&&(v&7)==0)tag="<p>";
      o+=snprintf(b+o,sizeof(b)-o," %d:%llx%s",i,(unsigned long long)v,tag);
    }
    L("%s\n",b);
    // deref each ptr word, check vtable in imgbuf
    for(int i=0;i<40;i++){ uint64_t v=w[i];
      if(v>0x7000000000ull&&v<0x8000000000ull&&(v&7)==0){ uint64_t vt=*(volatile uint64_t*)v;
        if(vt>=g_imgbuf_lo&&vt<g_imgbuf_hi) L("    x%d.w%d -> IMAGEBUFFER obj@%llx (vtab %llx)\n",which,i,(unsigned long long)v,(unsigned long long)vt); }
    }
  }
}

// asm trampoline: preserves x8(sret)+x0(this), calls real, dumps, returns
__asm__(
".text\n.global "READ_MANGLED"\n.type "READ_MANGLED",%function\n"
READ_MANGLED":\n"
"  stp x29,x30,[sp,#-64]!\n  mov x29,sp\n"
"  str x8,[sp,#16]\n  str x0,[sp,#24]\n"
"  adrp x9, :got:g_real\n  ldr x9,[x9,#:got_lo12:g_real]\n  ldr x9,[x9]\n"
"  blr x9\n"
"  str x0,[sp,#32]\n  str x1,[sp,#40]\n"
"  ldr x0,[sp,#24]\n  ldr x1,[sp,#32]\n  ldr x2,[sp,#40]\n  bl dump_fs\n"
"  ldr x0,[sp,#32]\n  ldr x1,[sp,#40]\n  ldr x8,[sp,#16]\n"
"  ldp x29,x30,[sp],#64\n  ret\n"
);

static void* sbthread(void* _){ (void)_;
  int sfd=open("/dev/syncboss_stream0",O_RDONLY); if(sfd<0) return 0; uint8_t p[4096];
  for(;;){ int n=read(sfd,p,sizeof p); if(n<=0) continue; uint64_t h=mono_ns();
    int j=0; while(j+6<=n){ if(p[j]==1&&p[j+1]==3&&p[j+2]==0&&p[j+4]==0){ int t=p[j+3],l=p[j+5];
      if(t==0x51&&j+10<=n){ uint32_t ts; memcpy(&ts,p+j+6,4); L("SB51 host=%llu nrf=%u\n",(unsigned long long)h,ts);} j+=6+l;} else j++; } }
  return 0;
}
__attribute__((constructor)) static void init_(void){
  ranges();
  g_real=dlsym(RTLD_NEXT,READ_MANGLED);
  L("== fs_atomic init: real=%p imgbuf=[%lx,%lx] ==\n",g_real,g_imgbuf_lo,g_imgbuf_hi);
  pthread_t t; if(!pthread_create(&t,0,sbthread,0)) pthread_detach(t);
}
