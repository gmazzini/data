// Gianluca Mazzini @2026- Version 1.00
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdarg.h>
#include <termios.h>
#include <inttypes.h>
#include <mysql/mysql.h>
#include "energy_config.h"

#define RWTIMEOUT 3000
#define CTIMEOUT 3000
#define CHSLEEP 10000
#define FAKE -999999.0

union uw {uint16_t w; uint8_t u[2]; };
union uf {float f; uint8_t u[4]; };
union ul {uint32_t l; uint8_t u[4]; };

static uint16_t crc(uint8_t *buf,uint8_t lenbuf){
  int i,j;
  uint16_t out=0xFFFF;
  for(j=0;j<lenbuf;j++){
    out^=(uint16_t)buf[j];
    for(i=0;i<8;++i){
      if(out&1)out=(out>>1)^0xA001;
      else out=(out>>1);
    }
  }
  return out;
}

static int readn(int fd,uint8_t *buf,int len){
  int i,r,n;
  struct pollfd pfd;

  i=0;
  while(i<len){
    pfd.fd=fd;
    pfd.events=POLLIN;
    pfd.revents=0;

    r=poll(&pfd,1,RWTIMEOUT);
    if(r<0 && errno==EINTR)continue;
    if(r<=0)return -1;
    if(pfd.revents&(POLLERR|POLLHUP|POLLNVAL))return -1;

    n=read(fd,buf+i,len-i);
    if(n<0 && errno==EINTR)continue;
    if(n<=0)return -1;
    i+=n;
  }
  return 0;
}

static int writen(int fd,uint8_t *buf,int len){
  int i,r,n;
  struct pollfd pfd;

  i=0;
  while(i<len){
    pfd.fd=fd;
    pfd.events=POLLOUT;
    pfd.revents=0;

    r=poll(&pfd,1,RWTIMEOUT);
    if(r<0 && errno==EINTR)continue;
    if(r<=0)return -1;
    if(pfd.revents&(POLLERR|POLLHUP|POLLNVAL))return -1;

    n=write(fd,buf+i,len-i);
    if(n<0 && errno==EINTR)continue;
    if(n<=0)return -1;
    i+=n;
  }
  return 0;
}

static int myconnect(int fd,struct sockaddr *addr,socklen_t len){
  int r,err,flags;
  socklen_t errlen;
  struct pollfd pfd;

  flags=fcntl(fd,F_GETFL,0);
  if(flags<0)return -1;

  if(fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return -1;

  r=connect(fd,addr,len);
  if(r==0){
    fcntl(fd,F_SETFL,flags);
    return 0;
  }

  if(errno!=EINPROGRESS){
    fcntl(fd,F_SETFL,flags);
    return -1;
  }

  pfd.fd=fd;
  pfd.events=POLLOUT;
  pfd.revents=0;

  r=poll(&pfd,1,CTIMEOUT);
  if(r<=0){
    fcntl(fd,F_SETFL,flags);
    return -1;
  }

  if(pfd.revents&(POLLERR|POLLHUP|POLLNVAL)){
    fcntl(fd,F_SETFL,flags);
    return -1;
  }

  err=0;
  errlen=sizeof(err);

  if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&errlen)<0){
    fcntl(fd,F_SETFL,flags);
    return -1;
  }

  fcntl(fd,F_SETFL,flags);

  if(err!=0){
    errno=err;
    return -1;
  }

  return 0;
}

static int myw(int fd,uint8_t *ss,uint8_t nn){
  union uw uw;
  uint8_t aux[8];

  memcpy(aux,ss,4);
  aux[4]=0;
  aux[5]=nn;
  uw.w=crc(aux,6);
  aux[6]=uw.u[0];
  aux[7]=uw.u[1];

  if(writen(fd,aux,8)<0)return -1;
  usleep(8*CHSLEEP);
  return 0;
}

static float myr_f(int fd){
  union uw uw;
  union uf uf;
  uint8_t aux[9];

  if(readn(fd,aux,9)<0)return FAKE;

  uw.u[0]=aux[7];
  uw.u[1]=aux[8];

  if(crc(aux,7)!=uw.w)return FAKE;
  if(aux[1]&0x80)return FAKE;
  if(aux[2]!=4)return FAKE;

  uf.u[3]=aux[3];
  uf.u[2]=aux[4];
  uf.u[1]=aux[5];
  uf.u[0]=aux[6];

  return uf.f;
}

static uint32_t *myr_ln(int fd,int n){
  union uw uw;
  static union ul ul[10];
  int i;
  uint8_t aux[50];

  if(n<1 || n>10)return NULL;

  if(readn(fd,aux,5+4*n)<0)return NULL;

  uw.u[0]=aux[3+4*n];
  uw.u[1]=aux[4+4*n];

  if(crc(aux,3+4*n)!=uw.w)return NULL;
  if(aux[1]&0x80)return NULL;
  if(aux[2]!=(4*n))return NULL;

  for(i=0;i<n;i++){
    ul[i].u[3]=aux[3+4*i];
    ul[i].u[2]=aux[4+4*i];
    ul[i].u[1]=aux[5+4*i];
    ul[i].u[0]=aux[6+4*i];
  }

  return &ul[0].l;
}

static int read_line_cr(int fd,char *buf,int len){
  int i,r,n;
  char c;
  struct pollfd pfd;

  if(len<2)return -1;

  i=0;
  while(i<len-1){
    pfd.fd=fd;
    pfd.events=POLLIN;
    pfd.revents=0;

    r=poll(&pfd,1,RWTIMEOUT);
    if(r<0 && errno==EINTR)continue;
    if(r<=0)return -1;
    if(pfd.revents&(POLLERR|POLLHUP|POLLNVAL))return -1;

    n=read(fd,&c,1);
    if(n<0 && errno==EINTR)continue;
    if(n<=0)return -1;

    if(c=='\r' || c=='\n')break;

    buf[i]=c;
    i++;
  }

  buf[i]=0;

  if(i<=0)return -1;
  return 0;
}

static int wj150_read_a0_b0(int fd,int *a0,int *b0){
  char rx[80],*p,*endp;
  long a,b;

  if(writen(fd,(uint8_t *)"#015\r",5)<0)return -1;

  if(read_line_cr(fd,rx,sizeof(rx))<0)return -1;

  if(rx[0]!='!')return -1;

  errno=0;
  p=rx+1;
  a=strtol(p,&endp,10);
  if(errno!=0 || endp==p || *endp!=',')return -1;

  p=endp+1;
  errno=0;
  b=strtol(p,&endp,10);
  if(errno!=0 || endp==p)return -1;

  *a0=(int)a;
  *b0=(int)b;

  return 0;
}

int main(int argc,char **argv){
  int fd,mode,status;
  int water_w,traffic_p;
  long mode_l;
  char *endp;
  float v1,v2,v3,i1,i2,i3,e1,e2,e3;
  uint32_t *ol;
  struct sockaddr_in server;
  MYSQL *con;
  EnergyConfig cfg;
  time_t t;
  char query[512],cfg_err[256];
  const char *host;
  unsigned int port;

  fd=-1;
  mode=0;
  status=1;
  water_w=0;
  traffic_p=0;
  mode_l=0;
  endp=NULL;
  ol=NULL;
  con=NULL;
  host=NULL;
  port=0;

  if(argc!=2){
    fprintf(stderr,"usage: %s mode\n",argv[0]);
    return 1;
  }

  errno=0;
  mode_l=strtol(argv[1],&endp,10);
  if(errno!=0 || endp==argv[1] || *endp!='\0' || mode_l<1 || mode_l>6){
    fprintf(stderr,"bad mode: %s\n",argv[1]);
    return 1;
  }
  mode=(int)mode_l;

  if(!energy_config_load(&cfg,ENERGY_CONFIG_FILE,cfg_err,sizeof(cfg_err))){
    fprintf(stderr,"energy config error: %s\n",cfg_err);
    return 1;
  }

  switch(mode){
    case 1:
    case 2:
      host=cfg.so_host;
      port=cfg.so_port;
      break;
    case 3:
    case 4:
      host=cfg.cc1_host;
      port=cfg.cc1_port;
      break;
    case 5:
      host=cfg.cc2_host;
      port=cfg.cc2_port;
      break;
    case 6:
      host=cfg.cc3_host;
      port=cfg.cc3_port;
      break;
  }

  con=mysql_init(NULL);
  if(con==NULL){
    fprintf(stderr,"mysql init error\n");
    return 1;
  }
  if(mysql_real_connect(con,cfg.db_host,cfg.db_user,cfg.db_pass,cfg.db_name,
      cfg.db_port,NULL,0)==NULL){
    fprintf(stderr,"mysql connect error: %s\n",mysql_error(con));
    mysql_close(con);
    return 1;
  }

  fd=socket(AF_INET,SOCK_STREAM,0);
  if(fd<0){
    fprintf(stderr,"socket error: %s\n",strerror(errno));
    mysql_close(con);
    return 1;
  }

  memset(&server,0,sizeof(server));
  server.sin_family=AF_INET;
  server.sin_port=htons((uint16_t)port);
  if(inet_pton(AF_INET,host,&server.sin_addr)!=1){
    fprintf(stderr,"invalid meter address: %s\n",host);
    goto cleanup;
  }
  if(myconnect(fd,(struct sockaddr *)&server,sizeof(server))<0){
    fprintf(stderr,"connect error: %s\n",strerror(errno));
    goto cleanup;
  }

  t=time(NULL);
  if(t==(time_t)-1){
    fprintf(stderr,"time error\n");
    goto cleanup;
  }

  switch(mode){
    case 1:
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x0E",2)<0)break;
      v1=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x10",2)<0)break;
      v2=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x12",2)<0)break;
      v3=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x16",2)<0)break;
      i1=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x18",2)<0)break;
      i2=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x1A",2)<0)break;
      i3=myr_f(fd);
      if(v1<0 || v2<0 || v3<0 || i1<0 || i2<0 || i3<0){
        fprintf(stderr,"invalid SO voltage/current reading\n");
        break;
      }
      snprintf(query,sizeof(query),
          "insert into vi_so (epoch,v1,v2,v3,i1,i2,i3) values(%ld,%f,%f,%f,%f,%f,%f)",
          (long)t,v1,v2,v3,i1,i2,i3);
      if(mysql_query(con,query)!=0){
        fprintf(stderr,"mysql query error: %s\n",mysql_error(con));
        break;
      }
      status=0;
      break;

    case 2:
      if(myw(fd,(uint8_t *)"\x01\x03\x01\x02",2)<0)break;
      e1=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x01\x04",2)<0)break;
      e2=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x01\x06",2)<0)break;
      e3=myr_f(fd);
      if(e1<0 || e2<0 || e3<0){
        fprintf(stderr,"invalid SO energy reading\n");
        break;
      }
      snprintf(query,sizeof(query),
          "insert into energy_so (epoch,e1,e2,e3) values(%ld,%f,%f,%f)",
          (long)t,e1,e2,e3);
      if(mysql_query(con,query)!=0){
        fprintf(stderr,"mysql query error: %s\n",mysql_error(con));
        break;
      }
      status=0;
      break;

    case 3:
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x0E",2)<0)break;
      v1=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x10",2)<0)break;
      v2=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x12",2)<0)break;
      v3=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x16",2)<0)break;
      i1=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x18",2)<0)break;
      i2=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x00\x1A",2)<0)break;
      i3=myr_f(fd);
      if(v1<0 || v2<0 || v3<0 || i1<0 || i2<0 || i3<0){
        fprintf(stderr,"invalid CC voltage/current reading\n");
        break;
      }
      snprintf(query,sizeof(query),
          "insert into vi_cc (epoch,v1,v2,v3,i1,i2,i3) values(%ld,%f,%f,%f,%f,%f,%f)",
          (long)t,v1,v2,v3,i1,i2,i3);
      if(mysql_query(con,query)!=0){
        fprintf(stderr,"mysql query error: %s\n",mysql_error(con));
        break;
      }
      status=0;
      break;

    case 4:
      if(myw(fd,(uint8_t *)"\x01\x03\x01\x02",2)<0)break;
      e1=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x01\x04",2)<0)break;
      e2=myr_f(fd);
      if(myw(fd,(uint8_t *)"\x01\x03\x01\x06",2)<0)break;
      e3=myr_f(fd);
      if(e1<0 || e2<0 || e3<0){
        fprintf(stderr,"invalid CC energy reading\n");
        break;
      }
      snprintf(query,sizeof(query),
          "insert into energy_cc (epoch,e1,e2,e3) values(%ld,%f,%f,%f)",
          (long)t,e1,e2,e3);
      if(mysql_query(con,query)!=0){
        fprintf(stderr,"mysql query error: %s\n",mysql_error(con));
        break;
      }
      status=0;
      break;

    case 5:
      if(myw(fd,(uint8_t *)"\x01\x03\x01\x0E",2)<0)break;
      ol=myr_ln(fd,1);
      if(ol==NULL){
        fprintf(stderr,"invalid LE1 energy reading\n");
        break;
      }
      snprintf(query,sizeof(query),
          "insert into energy_le1 (epoch,e) values(%ld,%f)",
          (long)t,(double)ol[0]/100.0);
      if(mysql_query(con,query)!=0){
        fprintf(stderr,"mysql query error: %s\n",mysql_error(con));
        break;
      }
      status=0;
      break;

    case 6:
      if(wj150_read_a0_b0(fd,&water_w,&traffic_p)!=0){
        fprintf(stderr,"invalid WJ150 reading\n");
        break;
      }
      snprintf(query,sizeof(query),
          "insert into water_cc (epoch,w) values(%ld,%d)",(long)t,water_w);
      if(mysql_query(con,query)!=0){
        fprintf(stderr,"mysql query error: %s\n",mysql_error(con));
        break;
      }
      snprintf(query,sizeof(query),
          "insert into traffic_cc (epoch,p) values(%ld,%d)",(long)t,traffic_p);
      if(mysql_query(con,query)!=0){
        fprintf(stderr,"mysql query error: %s\n",mysql_error(con));
        break;
      }
      status=0;
      break;
  }

cleanup:
  close(fd);
  mysql_close(con);
  return status;
}
