// Gianluca Mazzini @2022- Version 3.00

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <mysql/mysql.h>
#include "radio_config.h"
#include "radio_data.h"

#define RXBUF 4096
#define QUERYBUF 8192
#define ESCBUF (RADIO_ADIF_VALUE*2+1)
#define ERRBUF 256

static volatile sig_atomic_t running=1;
static int listen_fd=-1;

static void stop_handler(int sig) {
  (void)sig;
  running=0;
  if (listen_fd>=0) close(listen_fd);
}

static MYSQL *db_connect(const RadioConfig *cfg) {
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

static int db_ready(MYSQL **pcon,const RadioConfig *cfg) {
  if (*pcon!=NULL && mysql_ping(*pcon)==0) return 1;
  if (*pcon!=NULL) {
    mysql_close(*pcon);
    *pcon=NULL;
  }
  *pcon=db_connect(cfg);
  return *pcon!=NULL;
}

static int escape_field(MYSQL *con,char *dst,unsigned long cap,const char *src) {
  unsigned long n;

  if (cap<3) return 0;
  n=mysql_real_escape_string(con,dst,src,(unsigned long)strlen(src));
  return n<cap;
}

static int open_listener(unsigned int port) {
  struct sockaddr_in addr;
  int fd,one;

  fd=socket(AF_INET,SOCK_DGRAM,0);
  if (fd<0) {
    fprintf(stderr,"socket error: %s\n",strerror(errno));
    return -1;
  }
  one=1;
  if (setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one))<0) {
    fprintf(stderr,"setsockopt error: %s\n",strerror(errno));
    close(fd);
    return -1;
  }
  memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET;
  addr.sin_addr.s_addr=INADDR_ANY;
  addr.sin_port=htons((unsigned short)port);
  if (bind(fd,(struct sockaddr *)&addr,sizeof(addr))<0) {
    fprintf(stderr,"bind port %u error: %s\n",port,strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

static int process_packet(MYSQL **pcon,const RadioConfig *cfg,char *packet) {
  static const char *names[]={
    "call","freq","freq_rx","rst_sent","rst_rcvd","mode","time_on","time_off",
    "stx_string","stx","srx_string","srx","contest_id","qso_date","qso_date_off",
    "comment","station_callsign"
  };
  RadioAdif adif;
  RadioCty cty;
  const char *cursor;
  char esc[10][ESCBUF];
  char query[QUERYBUF];
  const char *stx,*srx;
  time_t open_time,close_time;
  double freq,freq_rx;
  char *end;
  long dxcc;
  int fields,qlen,rc;

  cursor=packet;
  fields=radio_adif_extract(&cursor,names,17,&adif);
  if (fields<0) {
    fprintf(stderr,"invalid ADIF packet\n");
    return 0;
  }
  if (strcmp(adif.value[15],cfg->adif_secret)!=0) return 0;
  if (adif.value[0][0]=='\0' || adif.value[13][0]=='\0') {
    fprintf(stderr,"incomplete ADIF packet\n");
    return 0;
  }

  if (adif.value[14][0]=='\0') strcpy(adif.value[14],adif.value[13]);
  if (adif.value[7][0]=='\0') strcpy(adif.value[7],adif.value[6]);

  open_time=radio_adif_time(adif.value[13],adif.value[6]);
  close_time=radio_adif_time(adif.value[14],adif.value[7]);
  if (open_time==(time_t)-1 || close_time==(time_t)-1) {
    fprintf(stderr,"invalid ADIF date/time for %s\n",adif.value[0]);
    return 0;
  }

  end=NULL;
  freq=strtod(adif.value[1],&end);
  if (end==adif.value[1] || (end!=NULL && *end!='\0') || freq<0.0) {
    fprintf(stderr,"invalid ADIF frequency for %s\n",adif.value[0]);
    return 0;
  }
  if (adif.value[2][0]=='\0') freq_rx=0.0;
  else {
    end=NULL;
    freq_rx=strtod(adif.value[2],&end);
    if (end==adif.value[2] || (end!=NULL && *end!='\0') || freq_rx<0.0) {
      fprintf(stderr,"invalid ADIF RX frequency for %s\n",adif.value[0]);
      return 0;
    }
  }

  if (!db_ready(pcon,cfg)) return 0;
  rc=radio_cty_lookup(*pcon,adif.value[0],&cty);
  if (rc<0) {
    fprintf(stderr,"CTY lookup error: %s\n",mysql_error(*pcon));
    return 0;
  }
  dxcc=(rc>0)?strtol(cty.dxcc,NULL,10):0;

  stx=(adif.value[8][0]!='\0')?adif.value[8]:adif.value[9];
  srx=(adif.value[10][0]!='\0')?adif.value[10]:adif.value[11];

  if (!escape_field(*pcon,esc[0],sizeof(esc[0]),adif.value[16]) ||
      !escape_field(*pcon,esc[1],sizeof(esc[1]),adif.value[0]) ||
      !escape_field(*pcon,esc[2],sizeof(esc[2]),adif.value[5]) ||
      !escape_field(*pcon,esc[3],sizeof(esc[3]),adif.value[3]) ||
      !escape_field(*pcon,esc[4],sizeof(esc[4]),adif.value[4]) ||
      !escape_field(*pcon,esc[5],sizeof(esc[5]),stx) ||
      !escape_field(*pcon,esc[6],sizeof(esc[6]),srx) ||
      !escape_field(*pcon,esc[7],sizeof(esc[7]),adif.value[12])) {
    fprintf(stderr,"ADIF field escape overflow\n");
    return 0;
  }

  qlen=snprintf(query,sizeof(query),
    "insert ignore into log (mycall,callsign,mode,freqtx,freqrx,signaltx,signalrx,contesttx,contestrx,contest,dxcc,open,close) "
    "values('%s','%s','%s',%ld,%ld,'%s','%s','%s','%s','%s',%ld,%lld,%lld)",
    esc[0],esc[1],esc[2],(long)(freq*1000000.0),(long)(freq_rx*1000000.0),esc[3],esc[4],esc[5],esc[6],esc[7],dxcc,
    (long long)open_time,(long long)close_time);
  if (qlen<0 || (unsigned int)qlen>=sizeof(query)) {
    fprintf(stderr,"ADIF SQL query too long\n");
    return 0;
  }
  if (mysql_query(*pcon,query)!=0) {
    fprintf(stderr,"mysql insert error: %s\n",mysql_error(*pcon));
    return 0;
  }

  printf("saved %lld %s %s %.5f dxcc=%ld\n",(long long)open_time,adif.value[16],adif.value[0],freq,dxcc);
  fflush(stdout);
  return 1;
}

int main(int argc,char **argv) {
  RadioConfig cfg;
  MYSQL *con;
  char err[ERRBUF],buf[RXBUF];
  ssize_t n;

  if (argc>2 || (argc==2 && strcmp(argv[1],"--check")!=0)) {
    fprintf(stderr,"Usage: %s [--check]\n",argv[0]);
    return 1;
  }
  if (!radio_config_load(&cfg,RADIO_CONFIG_FILE,err,sizeof(err))) {
    fprintf(stderr,"config error: %s\n",err);
    return 1;
  }
  if (cfg.db_host[0]=='\0' || cfg.db_user[0]=='\0' || cfg.db_name[0]=='\0' ||
      cfg.adif_secret[0]=='\0' || cfg.adif_port==0) {
    fprintf(stderr,"config error: missing adif_rx configuration value\n");
    return 1;
  }

  con=db_connect(&cfg);
  if (con==NULL) return 1;
  if (argc==2) {
    printf("adif_rx check ok: database connected, UDP port %u\n",cfg.adif_port);
    mysql_close(con);
    return 0;
  }

  signal(SIGTERM,stop_handler);
  signal(SIGINT,stop_handler);
  listen_fd=open_listener(cfg.adif_port);
  if (listen_fd<0) {
    mysql_close(con);
    return 1;
  }
  printf("adif_rx listen UDP port %u\n",cfg.adif_port);
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
    buf[n]='\0';
    process_packet(&con,&cfg,buf);
  }

  if (listen_fd>=0) close(listen_fd);
  listen_fd=-1;
  if (con!=NULL) mysql_close(con);
  printf("adif_rx stopped\n");
  fflush(stdout);
  return 0;
}
