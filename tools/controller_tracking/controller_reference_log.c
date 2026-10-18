/* Read-only coherent right-controller reference on firmware 49845030443200410.
 * Layout verified by prior injection/readback experiments; not a portable API.
 * Reject stale device timestamps instead of labeling old ring slots as current.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static double mono(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char **argv) {
 if(argc!=3){fprintf(stderr,"usage: controller_reference_log PID SECONDS\n");return 2;}
 int pid=atoi(argv[1]);double seconds=atof(argv[2]);
 if(pid<=0 || !isfinite(seconds) || seconds<=0 || seconds>60)return 2;
 char path[80],line[512];snprintf(path,sizeof path,"/proc/%d/maps",pid);
 FILE *maps=fopen(path,"r");if(!maps)return 1;
 unsigned long long base=0,end=0;
 while(fgets(line,sizeof line,maps))if(strstr(line,"/dev/ashmem/TrackingServiceController ")) {
  if(sscanf(line,"%llx-%llx",&base,&end)==2)break;
 }
 fclose(maps);if(!base || end-base<0x1f78){fprintf(stderr,"controller mapping missing\n");return 1;}
 snprintf(path,sizeof path,"/proc/%d/mem",pid);int fd=open(path,O_RDONLY);if(fd<0)return 1;
 /* Two firmware-specific metadata entries: handedness 1=left, 2=right.
  * Each has its own publication buffer; entry order is not a hand identity. */
 size_t publication=0;int right_count=0;
 for(int i=0;i<2;i++){
  uint32_t metadata[5];
  if(pread(fd,metadata,sizeof metadata,base+0x5b8+i*0x114)!=sizeof metadata)return 1;
  if(metadata[2]==4000 && metadata[4]==2){publication=0x758+i*0x16b8;right_count++;}
 }
 if(right_count!=1){fprintf(stderr,"right-controller metadata ambiguous\n");close(fd);return 1;}
 fprintf(stderr,"right_entry_publication_offset=0x%zx\n",publication);
 unsigned count=0,stale=0,incoherent=0;uint32_t last=UINT32_MAX;
 printf("#host_mono_s,pose_mono_s,age_s,counter,px,py,pz,qx,qy,qz,qw\n");
 double start=mono();
 while(mono()-start<seconds){
  unsigned char buffer[0x168];uint32_t after[2],a,b;int good=0;
  for(int retry=0;retry<20;retry++){
   if(pread(fd,buffer,sizeof buffer,base+publication)!=sizeof buffer)goto done;
   if(pread(fd,after,8,base+publication)!=8)goto done;
   memcpy(&a,buffer,4);memcpy(&b,buffer+4,4);
   if(a==b && after[0]==a && after[1]==b){good=1;break;}
  }
  if(!good){incoherent++;usleep(5000);continue;}
  size_t offset=8+(b&1)*0xb0;float p[3],q[4];double timestamp;
  memcpy(p,buffer+offset+0x38,12);memcpy(q,buffer+offset+0x28,16);memcpy(&timestamp,buffer+offset+0x78,8);
  double now=mono(),age=now-timestamp,norm=0;int finite=1;
  for(int i=0;i<4;i++){norm+=q[i]*q[i];finite &= isfinite(q[i]);}
  for(int i=0;i<3;i++)finite &= isfinite(p[i]);
  if(!finite || !isfinite(timestamp) || age<-.05 || age>.25 || norm<.9 || norm>1.1 || !buffer[offset]){stale++;usleep(5000);continue;}
  if(b!=last){printf("%.9f,%.9f,%.9f,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",now,timestamp,age,b,p[0],p[1],p[2],q[0],q[1],q[2],q[3]);count++;last=b;}
  usleep(5000);
 }
 done:
 fprintf(stderr,"samples=%u stale_or_invalid=%u incoherent=%u\n",count,stale,incoherent);
 close(fd);return count?0:1;
}
