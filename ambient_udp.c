// Gianluca Mazzini @2026- Version 1.00

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <mysql/mysql.h>
#include "energy_config.h"

#define PORTAMBIENT 54321
#define MAXBUF 512
#define MAXDEVICE 32
#define MAXQUERY 512
#define MAXERROR 256

static volatile sig_atomic_t running=1;
static int listen_fd=-1;

static void stop_handler(int sig) {
  (void)sig;
  running=0;
  if (listen_fd>=0) close(listen_fd);
}

static int parse_ambient(char *buf,char *device,float *temperature,float *humidity) {
  char tag[64],k1[64],k2[64];

  tag[0]=0;
  device[0]=0;
  k1[0]=0;
  k2[0]=0;

  if (sscanf(buf,"%63s %31s %63s %f %63s %f",tag,device,k1,temperature,k2,humidity)!=6) return -1;
  if (strcmp(tag,"zigbee_sensor")!=0 && strcmp(tag,"switchbot_sensor")!=0) return -1;
  if (strcmp(k1,"temperature")!=0) return -1;
  if (strcmp(k2,"humidity")!=0) return -1;
  return 0;
}

static MYSQL *db_connect(const EnergyConfig *cfg) {
  MYSQL *con;

  con=mysql_init(NULL);
  if (con==NULL) {
    fprintf(stderr,"mysql_init failed\n");
    return NULL;
  }
  if (mysql_real_connect(con,cfg->db_host,cfg->db_user,cfg->db_pass,cfg->db_name,cfg->db_port,NULL,0)==NULL) {
    fprintf(stderr,"mysql connect error: %s\n",mysql_error(con));
    mysql_close(con);
    return NULL;
  }
  return con;
}

static int save_ambient(MYSQL **pcon,const EnergyConfig *cfg,time_t t,const char *device,float temperature,float humidity) {
  MYSQL *con;
  char escaped[MAXDEVICE*2+1];
  char query[MAXQUERY];
  unsigned long n;
  int qlen;

  con=*pcon;
  if (con==NULL || mysql_ping(con)!=0) {
    if (con!=NULL) mysql_close(con);
    con=db_connect(cfg);
    *pcon=con;
    if (con==NULL) return -1;
  }

  n=mysql_real_escape_string(con,escaped,device,(unsigned long)strlen(device));
  if (n>=sizeof(escaped)) {
    fprintf(stderr,"device escape overflow\n");
    return -1;
  }

  qlen=snprintf(query,sizeof(query),
    "insert into ambient (epoch,device,temperature,humidity) values(%ld,'%s',%.6f,%.6f)",
    (long)t,escaped,(double)temperature,(double)humidity);
  if (qlen<0 || (unsigned int)qlen>=sizeof(query)) {
    fprintf(stderr,"query too long\n");
    return -1;
  }

  if (mysql_query(con,query)==0) return 0;

  fprintf(stderr,"mysql error: %s\n",mysql_error(con));
  mysql_close(con);
  *pcon=NULL;
  return -1;
}

static int open_listener(void) {
  struct sockaddr_in6 addr;
  int fd,opt;

  fd=socket(AF_INET6,SOCK_DGRAM,0);
  if (fd<0) {
    fprintf(stderr,"socket error: %s\n",strerror(errno));
    return -1;
  }

  opt=1;
  if (setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt))<0) {
    fprintf(stderr,"setsockopt error: %s\n",strerror(errno));
    close(fd);
    return -1;
  }

  memset(&addr,0,sizeof(addr));
  addr.sin6_family=AF_INET6;
  addr.sin6_addr=in6addr_any;
  addr.sin6_port=htons(PORTAMBIENT);

  if (bind(fd,(struct sockaddr *)&addr,sizeof(addr))<0) {
    fprintf(stderr,"bind port %d error: %s\n",PORTAMBIENT,strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

int main(int argc,char **argv) {
  EnergyConfig cfg;
  MYSQL *con;
  time_t t;
  ssize_t n;
  char buf[MAXBUF];
  char device[MAXDEVICE];
  char err[MAXERROR];
  float temperature,humidity;

  if (argc>2 || (argc==2 && strcmp(argv[1],"--check")!=0)) {
    fprintf(stderr,"Usage: %s [--check]\n",argv[0]);
    return 1;
  }

  if (!energy_config_load(&cfg,ENERGY_CONFIG_FILE,err,sizeof(err))) {
    fprintf(stderr,"config error: %s\n",err);
    return 1;
  }

  con=db_connect(&cfg);
  if (con==NULL) return 1;

  if (argc==2) {
    printf("ambient_udp check ok: database connected, UDP port %d\n",PORTAMBIENT);
    mysql_close(con);
    return 0;
  }

  signal(SIGTERM,stop_handler);
  signal(SIGINT,stop_handler);

  listen_fd=open_listener();
  if (listen_fd<0) {
    mysql_close(con);
    return 1;
  }

  printf("ambient udp listen port %d\n",PORTAMBIENT);
  fflush(stdout);

  for (;running;) {
    n=recvfrom(listen_fd,buf,sizeof(buf)-1,0,NULL,NULL);
    if (n<0) {
      if (!running) break;
      if (errno==EINTR) continue;
      fprintf(stderr,"recvfrom error: %s\n",strerror(errno));
      fflush(stderr);
      continue;
    }
    if (n==0) continue;

    buf[n]=0;
    if (parse_ambient(buf,device,&temperature,&humidity)<0) {
      fprintf(stderr,"bad packet: %s\n",buf);
      fflush(stderr);
      continue;
    }

    t=time(NULL);
    if (save_ambient(&con,&cfg,t,device,temperature,humidity)<0) {
      fflush(stderr);
      continue;
    }

    printf("saved %ld %s %.2f %.2f\n",(long)t,device,(double)temperature,(double)humidity);
    fflush(stdout);
  }

  if (listen_fd>=0) close(listen_fd);
  listen_fd=-1;
  if (con!=NULL) mysql_close(con);
  printf("ambient udp stopped\n");
  fflush(stdout);
  return 0;
}
