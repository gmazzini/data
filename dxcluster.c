// Gianluca Mazzini @2022- Version 3.01

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "radio_config.h"

#define SPOT_MAX 100000
#define LINE_MAX 8192
#define FILTER_MAX 64
#define ERRBUF 256

typedef struct {
  char dx[20];
  char spotter[20];
  long freq;
  time_t time;
} Spot;

typedef struct {
  int band;
  int mode;
  long start;
  long end;
} BandPlan;

static const BandPlan bands[]={
  {8,1,1810000,1838000},{8,2,1838000,1843000},{8,0,1843000,2000000},
  {7,1,3500000,3570000},{7,2,3570000,3600000},{7,0,3600000,3800000},
  {6,1,7000000,7040000},{6,2,7040000,7080000},{6,0,7060000,7200000},
  {5,1,14000000,14070000},{5,2,14070000,14112000},{5,0,14112000,14350000},
  {4,1,21000000,21070000},{4,2,21070000,21151000},{4,0,21151000,21450000},
  {3,1,28000000,28070000},{3,2,28070000,28320000},{3,0,28320000,29700000},
  {11,1,10100000,10130000},{11,2,10130000,10150000},
  {10,1,18068000,18095000},{10,2,18095000,18111000},{10,0,18111000,18168000},
  {9,1,24890000,24915000},{9,2,24915000,24931000},{9,0,24931000,24990000},
  {12,1,5151500,5153000},{12,2,5154000,5366000},{12,0,5366000,5366500}
};

static Spot *spots;
static long next_spot;
static pthread_mutex_t spots_mtx=PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t running=1;
static int remote_fd=-1;
static int local_fd=-1;
static RadioConfig cfg;

static void stop_handler(int sig) {
  (void)sig;
  running=0;
}

static int send_all(int fd,const char *buf,unsigned long len) {
  ssize_t n;
  unsigned long done;

  done=0;
  for (;done<len;) {
    n=send(fd,buf+done,len-done,MSG_NOSIGNAL);
    if (n<0) {
      if (errno==EINTR) continue;
      return 0;
    }
    if (n==0) return 0;
    done+=(unsigned long)n;
  }
  return 1;
}

static int band_index(long freq) {
  int i,n;

  n=(int)(sizeof(bands)/sizeof(bands[0]));
  for (i=0;i<n;i++) {
    if (freq>=bands[i].start && freq<=bands[i].end) return i;
  }
  return -1;
}

static void add_spot(const char *spotter,const char *dx,long freq) {
  Spot *s;

  if (strlen(spotter)>=sizeof(spots[0].spotter) || strlen(dx)>=sizeof(spots[0].dx)) return;
  pthread_mutex_lock(&spots_mtx);
  s=&spots[next_spot];
  strcpy(s->spotter,spotter);
  strcpy(s->dx,dx);
  s->freq=freq;
  s->time=time(NULL);
  next_spot=(next_spot+1)%SPOT_MAX;
  pthread_mutex_unlock(&spots_mtx);
}

static void process_cluster_line(char *line) {
  char *p;
  char spotter[20],dx[20];
  double khz;
  long freq;

  p=strstr(line,"DX de ");
  if (p==NULL) return;
  spotter[0]='\0';
  dx[0]='\0';
  khz=0.0;
  if (sscanf(p+6,"%19[^:]: %lf %19s",spotter,&khz,dx)!=3) return;
  if (spotter[0]=='\0' || dx[0]=='\0' || khz<=0.0 || khz>1000000.0) return;
  freq=(long)(khz*1000.0);
  add_spot(spotter,dx,freq);
}

static int open_local_listener(void) {
  struct addrinfo hints,*res,*rp;
  char port[16];
  int fd,one,rc;

  memset(&hints,0,sizeof(hints));
  hints.ai_family=AF_UNSPEC;
  hints.ai_socktype=SOCK_STREAM;
  hints.ai_flags=AI_PASSIVE;
  snprintf(port,sizeof(port),"%u",cfg.dxcluster_local_port);
  rc=getaddrinfo(cfg.dxcluster_local_host,port,&hints,&res);
  if (rc!=0) {
    fprintf(stderr,"local getaddrinfo error: %s\n",gai_strerror(rc));
    return -1;
  }
  fd=-1;
  for (rp=res;rp!=NULL;rp=rp->ai_next) {
    fd=socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
    if (fd<0) continue;
    one=1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    if (bind(fd,rp->ai_addr,rp->ai_addrlen)==0 && listen(fd,16)==0) break;
    close(fd);
    fd=-1;
  }
  freeaddrinfo(res);
  if (fd<0) fprintf(stderr,"cannot listen on %s:%u\n",cfg.dxcluster_local_host,cfg.dxcluster_local_port);
  return fd;
}

static int parse_request(const char *buf,int *limit,char *filter,unsigned long cap) {
  const char *comma;
  char num[32],*end;
  long n;
  unsigned long len;

  comma=strchr(buf,',');
  if (comma==NULL) return 0;
  len=(unsigned long)(comma-buf);
  if (len==0 || len>=sizeof(num)) return 0;
  memcpy(num,buf,len);
  num[len]='\0';
  errno=0;
  n=strtol(num,&end,10);
  if (errno!=0 || *end!='\0' || n<1 || n>SPOT_MAX) return 0;
  comma++;
  len=strcspn(comma,"\r\n");
  if (len==0 || len>=cap) return 0;
  memcpy(filter,comma,len);
  filter[len]='\0';
  *limit=(int)n;
  return 1;
}

static void serve_client(int cs) {
  Spot *result;
  char req[256],filter[FILTER_MAX],out[128];
  ssize_t r;
  long i,idx,nresult;
  int limit,b;

  r=recv(cs,req,sizeof(req)-1,0);
  if (r<=0) return;
  req[r]='\0';
  if (!parse_request(req,&limit,filter,sizeof(filter))) return;

  result=(Spot *)malloc((unsigned long)limit*sizeof(Spot));
  if (result==NULL) return;
  nresult=0;

  pthread_mutex_lock(&spots_mtx);
  for (i=0;i<SPOT_MAX && nresult<limit;i++) {
    idx=(next_spot-1-i+SPOT_MAX)%SPOT_MAX;
    if (spots[idx].freq<=0) continue;
    b=band_index(spots[idx].freq);
    if (b<0) continue;
    if ((unsigned int)bands[b].band>=strlen(filter) || (unsigned int)bands[b].mode>=strlen(filter)) continue;
    if (filter[bands[b].band]=='0' || filter[bands[b].mode]=='0') continue;
    result[nresult++]=spots[idx];
  }
  pthread_mutex_unlock(&spots_mtx);

  for (i=0;i<nresult;i++) {
    snprintf(out,sizeof(out),"%lld,%s,%ld,%s\n",(long long)result[i].time,result[i].spotter,result[i].freq,result[i].dx);
    if (!send_all(cs,out,(unsigned long)strlen(out))) break;
  }
  free(result);
}

static void *server_thread(void *arg) {
  struct pollfd pfd;
  int cs,rc;

  (void)arg;
  local_fd=open_local_listener();
  if (local_fd<0) {
    running=0;
    return NULL;
  }
  printf("dxcluster local service %s:%u\n",cfg.dxcluster_local_host,cfg.dxcluster_local_port);
  fflush(stdout);
  pfd.fd=local_fd;
  pfd.events=POLLIN;
  pfd.revents=0;
  for (;running;) {
    rc=poll(&pfd,1,500);
    if (rc<0) {
      if (errno==EINTR) continue;
      fprintf(stderr,"local poll error: %s\n",strerror(errno));
      break;
    }
    if (rc==0) continue;
    if ((pfd.revents&POLLIN)==0) {
      if (pfd.revents&(POLLERR|POLLHUP|POLLNVAL)) break;
      continue;
    }
    cs=accept(local_fd,NULL,NULL);
    if (cs<0) {
      if (errno==EINTR) continue;
      fprintf(stderr,"accept error: %s\n",strerror(errno));
      continue;
    }
    serve_client(cs);
    close(cs);
  }
  if (local_fd>=0) close(local_fd);
  local_fd=-1;
  return NULL;
}

static int connect_cluster(void) {
  struct addrinfo hints,*res,*rp;
  struct timeval timeout;
  char port[16];
  int fd,one,rc;

  memset(&hints,0,sizeof(hints));
  hints.ai_family=AF_UNSPEC;
  hints.ai_socktype=SOCK_STREAM;
  snprintf(port,sizeof(port),"%u",cfg.dxcluster_port);
  rc=getaddrinfo(cfg.dxcluster_host,port,&hints,&res);
  if (rc!=0) {
    fprintf(stderr,"cluster getaddrinfo error: %s\n",gai_strerror(rc));
    return -1;
  }
  fd=-1;
  for (rp=res;rp!=NULL;rp=rp->ai_next) {
    fd=socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
    if (fd<0) continue;
    if (connect(fd,rp->ai_addr,rp->ai_addrlen)==0) break;
    close(fd);
    fd=-1;
  }
  freeaddrinfo(res);
  if (fd<0) return -1;

  timeout.tv_sec=(time_t)cfg.dxcluster_timeout;
  timeout.tv_usec=0;
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  one=1;
  setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof(one));
  return fd;
}

static void stream_cluster(int fd) {
  struct pollfd pfd;
  char in[4096],line[LINE_MAX],login[64];
  ssize_t n;
  unsigned long used,i,start;
  int logged,rc;

  used=0;
  logged=0;
  pfd.fd=fd;
  pfd.events=POLLIN;
  pfd.revents=0;
  for (;running;) {
    rc=poll(&pfd,1,500);
    if (rc<0) {
      if (errno==EINTR) continue;
      return;
    }
    if (rc==0) continue;
    if ((pfd.revents&POLLIN)==0) return;
    n=recv(fd,in,sizeof(in),0);
    if (n<=0) return;

    if (!logged && memmem(in,(size_t)n,"login:",6)!=NULL) {
      snprintf(login,sizeof(login),"%s\n",cfg.dxcluster_callsign);
      if (!send_all(fd,login,(unsigned long)strlen(login))) return;
      logged=1;
      printf("dxcluster login sent as %s\n",cfg.dxcluster_callsign);
      fflush(stdout);
    }

    if (used+(unsigned long)n>=sizeof(line)) {
      used=0;
      fprintf(stderr,"cluster input line overflow, buffer reset\n");
    }
    memcpy(line+used,in,(size_t)n);
    used+=(unsigned long)n;

    start=0;
    for (i=0;i<used;i++) {
      if (line[i]=='\n') {
        line[i]='\0';
        if (i>start && line[i-1]=='\r') line[i-1]='\0';
        process_cluster_line(line+start);
        start=i+1;
      }
    }
    if (start>0) {
      memmove(line,line+start,used-start);
      used-=start;
    }
  }
}

int main(int argc,char **argv) {
  pthread_t server;
  struct addrinfo hints,*res;
  char err[ERRBUF],port[16];
  int rc;

  if (argc>2 || (argc==2 && strcmp(argv[1],"--check")!=0)) {
    fprintf(stderr,"Usage: %s [--check]\n",argv[0]);
    return 1;
  }
  if (!radio_config_load(&cfg,RADIO_CONFIG_FILE,err,sizeof(err))) {
    fprintf(stderr,"config error: %s\n",err);
    return 1;
  }
  if (cfg.dxcluster_host[0]=='\0' || cfg.dxcluster_port==0 ||
      cfg.dxcluster_callsign[0]=='\0' || cfg.dxcluster_local_host[0]=='\0' ||
      cfg.dxcluster_local_port==0 || cfg.dxcluster_timeout==0 || cfg.dxcluster_reconnect==0) {
    fprintf(stderr,"config error: missing dxcluster configuration value\n");
    return 1;
  }

  if (argc==2) {
    memset(&hints,0,sizeof(hints));
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_STREAM;
    snprintf(port,sizeof(port),"%u",cfg.dxcluster_port);
    rc=getaddrinfo(cfg.dxcluster_host,port,&hints,&res);
    if (rc!=0) {
      fprintf(stderr,"dxcluster check failed: %s\n",gai_strerror(rc));
      return 1;
    }
    freeaddrinfo(res);
    printf("dxcluster check ok: %s:%u -> local %s:%u\n",cfg.dxcluster_host,cfg.dxcluster_port,cfg.dxcluster_local_host,cfg.dxcluster_local_port);
    return 0;
  }

  spots=(Spot *)calloc(SPOT_MAX,sizeof(Spot));
  if (spots==NULL) {
    fprintf(stderr,"cannot allocate spot buffer\n");
    return 1;
  }
  next_spot=0;
  signal(SIGPIPE,SIG_IGN);
  signal(SIGTERM,stop_handler);
  signal(SIGINT,stop_handler);

  rc=pthread_create(&server,NULL,server_thread,NULL);
  if (rc!=0) {
    fprintf(stderr,"pthread_create error: %s\n",strerror(rc));
    free(spots);
    return 1;
  }

  for (;running;) {
    remote_fd=connect_cluster();
    if (remote_fd<0) {
      if (!running) break;
      fprintf(stderr,"cluster connect failed, retry in %u seconds\n",cfg.dxcluster_reconnect);
      sleep(cfg.dxcluster_reconnect);
      continue;
    }
    printf("dxcluster connected to %s:%u\n",cfg.dxcluster_host,cfg.dxcluster_port);
    fflush(stdout);
    stream_cluster(remote_fd);
    if (remote_fd>=0) close(remote_fd);
    remote_fd=-1;
    if (running) {
      fprintf(stderr,"cluster disconnected, retry in %u seconds\n",cfg.dxcluster_reconnect);
      sleep(cfg.dxcluster_reconnect);
    }
  }

  if (local_fd>=0) close(local_fd);
  local_fd=-1;
  pthread_join(server,NULL);
  free(spots);
  printf("dxcluster stopped\n");
  fflush(stdout);
  return 0;
}
