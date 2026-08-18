// Gianluca Mazzini @2022- Version 3.00

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include "radio_client.h"

static int send_all(int fd,const char *buf,unsigned long len) {
  ssize_t n;
  unsigned long done;

  done=0;
  for (;done<len;) {
    n=send(fd,buf+done,len-done,0);
    if (n<0) {
      if (errno==EINTR) continue;
      return 0;
    }
    if (n==0) return 0;
    done+=(unsigned long)n;
  }
  return 1;
}

int radio_callbook_lookup(const char *host,unsigned int port,int source,const char *callsign,unsigned int timeout,char *response,unsigned long response_cap) {
  struct addrinfo hints,*res,*rp;
  struct timeval tv;
  char service[16],req[128];
  ssize_t n;
  int fd,rc;
  const char *cmd;

  if (host==NULL || callsign==NULL || response==NULL || response_cap<2 || port==0 || timeout==0) return RADIO_CALLBOOK_ERROR;
  if (source==RADIO_CALLBOOK_QRZCOM) cmd="QRZCOM";
  else if (source==RADIO_CALLBOOK_QRZRU) cmd="QRZRU";
  else return RADIO_CALLBOOK_ERROR;
  snprintf(req,sizeof(req),"%s %s\n",cmd,callsign);
  snprintf(service,sizeof(service),"%u",port);
  memset(&hints,0,sizeof(hints));
  hints.ai_family=AF_UNSPEC;
  hints.ai_socktype=SOCK_STREAM;
  rc=getaddrinfo(host,service,&hints,&res);
  if (rc!=0) return RADIO_CALLBOOK_ERROR;
  fd=-1;
  for (rp=res;rp!=NULL;rp=rp->ai_next) {
    fd=socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
    if (fd<0) continue;
    tv.tv_sec=(time_t)timeout;
    tv.tv_usec=0;
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    if (connect(fd,rp->ai_addr,rp->ai_addrlen)==0) break;
    close(fd);
    fd=-1;
  }
  freeaddrinfo(res);
  if (fd<0) return RADIO_CALLBOOK_ERROR;
  if (!send_all(fd,req,(unsigned long)strlen(req))) {
    close(fd);
    return RADIO_CALLBOOK_ERROR;
  }
  n=recv(fd,response,response_cap-1,0);
  close(fd);
  if (n<=0) return RADIO_CALLBOOK_ERROR;
  response[n]='\0';
  if (strncmp(response,"OK ",3)==0) return RADIO_CALLBOOK_OK;
  if (strncmp(response,"NOTFOUND ",9)==0) return RADIO_CALLBOOK_NOTFOUND;
  return RADIO_CALLBOOK_ERROR;
}
