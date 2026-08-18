#include "/home/tools/log/pfunc.c"
#include <sys/time.h>

static long ms(void){
  struct timeval tv;
  gettimeofday(&tv,NULL);
  return tv.tv_sec*1000L+tv.tv_usec/1000L;
}

int main(int argc,char **argv){
  int s,n,vv,cached,rows;
  long l1,l2,l3,t0,t1;
  char req[100],buf[65536],*p,*q,*p1,*p2,*p3,*p4,sql[1000];
  struct sockaddr_in a;
  MYSQL *con;
  MYSQL_RES *res;
  MYSQL_ROW row;
  if(argc!=2)return 1;
  s=socket(AF_INET,SOCK_STREAM,0);
  memset(&a,0,sizeof(a));
  a.sin_family=AF_INET; a.sin_port=htons(22222); inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
  if(connect(s,(struct sockaddr *)&a,sizeof(a))<0)return 2;
  sprintf(req,"30,%s\n",argv[1]); send(s,req,strlen(req),0);
  n=0; for(;;){int r=recv(s,buf+n,sizeof(buf)-1-n,0); if(r<=0)break; n+=r;} close(s); buf[n]='\0';
  con=mysql_init(NULL); if(!con)return 3;
  if(!mysql_real_connect(con,dbhost,dbuser,dbpassword,dbname,0,NULL,0))return 4;
  t0=ms(); rows=0; p=buf;
  for(;;){
    q=strchr(p,'\n'); if(!q)break; *q='\0';
    p1=strtok(p,","); p2=strtok(NULL,","); p3=strtok(NULL,","); p4=strtok(NULL,",");
    if(!p1||!p2||!p3||!p4)break;
    searchcty(con,p4); vv=atoi(mycty[2]);
    l1=l2=0; cached=0;
    sprintf(sql,"select qso,qsl,time from aux1 where mycall='IK4LZH' and dxcc=%d",vv);
    mysql_query(con,sql); res=mysql_store_result(con); row=mysql_fetch_row(res);
    if(row!=NULL && time(NULL)-atoll(row[2])<TIMEOUT_AUX1){cached=1; l1=atol(row[0]); l2=atol(row[1]);}
    mysql_free_result(res);
    if(!cached){
      sprintf(sql,"select count(*),sum(lotw)+sum(eqsl)+sum(qrz) from log where mycall='IK4LZH' and dxcc=%d",vv);
      mysql_query(con,sql); res=mysql_store_result(con); row=mysql_fetch_row(res); l1=atol(row[0]); l2=atol(row[1]); mysql_free_result(res);
      sprintf(sql,"replace into aux1 (qso,qsl,time,mycall,dxcc) values (%ld,%ld,%lld,'IK4LZH',%d)",l1,l2,(long long)time(NULL),vv); mysql_query(con,sql);
    }
    sprintf(sql,"select count(*),sum(lotw)+sum(eqsl)+sum(qrz),max(open) from log where mycall='IK4LZH' and callsign='%s'",p4);
    mysql_query(con,sql); res=mysql_store_result(con); row=mysql_fetch_row(res); l3=row&&row[0]?atol(row[0]):0; mysql_free_result(res);
    (void)l3;
    rows++; p=q+1;
  }
  t1=ms(); mysql_close(con);
  printf("filter=%s rows=%d ms=%ld\n",argv[1],rows,t1-t0);
  return 0;
}
