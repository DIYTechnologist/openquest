/* Passive, bounded SyncBoss reader. No ioctl/write to device; timestamp every received chunk.
 * Output raw bytes and a monotonic receipt index. Receipt times are not sensor exposure times.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
int main(int argc,char **argv){
 if(argc!=4)return 2;double seconds=atof(argv[1]);if(!isfinite(seconds) || seconds<=0 || seconds>60)return 2;
 int fd=open("/dev/syncboss_stream0",O_RDONLY|O_NONBLOCK);if(fd<0){perror("open");return 1;}
 FILE *raw=fopen(argv[2],"wb"),*index=fopen(argv[3],"w");if(!raw || !index){close(fd);return 1;}
 fprintf(index,"#host_mono_ns,byte_offset,length\n");unsigned char buffer[65536];uint64_t offset=0,end=now()+(uint64_t)(seconds*1e9);
 while(now()<end){struct pollfd p={.fd=fd,.events=POLLIN};if(poll(&p,1,100)<=0)continue;
  ssize_t n=read(fd,buffer,sizeof buffer);uint64_t receipt=now();if(n<=0)continue;
  if(fwrite(buffer,1,n,raw)!=(size_t)n)break;
  fprintf(index,"%llu,%llu,%zd\n",(unsigned long long)receipt,(unsigned long long)offset,n);offset+=n;
 }
 fclose(raw);fclose(index);close(fd);fprintf(stderr,"captured_bytes=%llu\n",(unsigned long long)offset);return offset?0:1;
}
