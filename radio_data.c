// Gianluca Mazzini @2022- Version 3.01

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "radio_data.h"

static int copy_field(char *dst,unsigned long cap,const char *src,unsigned long len) {
  if (cap==0 || len>=cap) return 0;
  memcpy(dst,src,len);
  dst[len]='\0';
  return 1;
}

int radio_adif_extract(const char **cursor,const char *const *names,int count,RadioAdif *out) {
  const char *p,*p1,*p2,*p3;
  unsigned long taglen,len;
  int i,nret;

  if (cursor==NULL || *cursor==NULL || names==NULL || out==NULL || count<1 || count>RADIO_ADIF_MAX_FIELDS) return -1;
  for (i=0;i<count;i++) out->value[i][0]='\0';

  p=*cursor;
  nret=0;
  for (;;) {
    p1=strchr(p,'<');
    if (p1==NULL) {
      *cursor=p+strlen(p);
      return nret;
    }
    p2=strchr(p1+1,'>');
    if (p2==NULL) {
      *cursor=p1;
      return nret;
    }
    if ((unsigned long)(p2-p1-1)>=3 && strncasecmp(p1+1,"EOR",3)==0) {
      *cursor=p2+1;
      return nret;
    }
    p3=memchr(p1+1,':',(size_t)(p2-p1-1));
    if (p3==NULL) {
      p=p2+1;
      continue;
    }
    taglen=(unsigned long)(p3-p1-1);
    len=strtoul(p3+1,NULL,10);
    if (len>=RADIO_ADIF_VALUE) {
      *cursor=p2+1;
      return -1;
    }
    if (strlen(p2+1)<len) {
      *cursor=p2+1;
      return -1;
    }
    for (i=0;i<count;i++) {
      if (strlen(names[i])==taglen && strncasecmp(names[i],p1+1,(size_t)taglen)==0) break;
    }
    if (i<count) {
      if (!copy_field(out->value[i],sizeof(out->value[i]),p2+1,len)) return -1;
      nret++;
    }
    p=p2+1+len;
  }
}

int radio_cty_lookup(MYSQL *con,const char *callsign,RadioCty *out) {
  MYSQL_RES *res;
  MYSQL_ROW row;
  char query[1024],call[64],escaped[128],*slash;
  const char *suffixes[]={"P","M","LH","MM","AM","A","B","QRP","0","1","2","3","4","5","6","7","8","9"};
  int i,n,len;

  if (con==NULL || callsign==NULL || out==NULL) return -1;
  memset(out,0,sizeof(*out));
  if (strlen(callsign)>=sizeof(call)) return -1;
  strcpy(call,callsign);

  slash=strrchr(call,'/');
  if (slash!=NULL) {
    n=(int)(sizeof(suffixes)/sizeof(suffixes[0]));
    for (i=0;i<n;i++) if (strcmp(slash+1,suffixes[i])==0) break;
    if (i<n) *slash='\0';
  }
  slash=strrchr(call,'/');
  if (slash!=NULL) {
    len=(int)strlen(call);
    if ((slash-call)<(len-(int)(slash-call)-1)) *slash='\0';
    else memmove(call,slash+1,strlen(slash+1)+1);
  }

  for (len=(int)strlen(call);len>0;len--) {
    call[len]='\0';
    mysql_real_escape_string(con,escaped,call,(unsigned long)strlen(call));
    snprintf(query,sizeof(query),"select base,name,dxcc,cont,cqzone,ituzone,latitude,longitude,gmtshift from cty where prefix='%s' limit 1",escaped);
    if (mysql_query(con,query)!=0) return -1;
    res=mysql_store_result(con);
    if (res==NULL) return -1;
    row=mysql_fetch_row(res);
    if (row!=NULL) {
      copy_field(out->base,sizeof(out->base),row[0],strlen(row[0]));
      copy_field(out->name,sizeof(out->name),row[1],strlen(row[1]));
      copy_field(out->dxcc,sizeof(out->dxcc),row[2],strlen(row[2]));
      copy_field(out->cont,sizeof(out->cont),row[3],strlen(row[3]));
      copy_field(out->cqzone,sizeof(out->cqzone),row[4],strlen(row[4]));
      copy_field(out->ituzone,sizeof(out->ituzone),row[5],strlen(row[5]));
      copy_field(out->latitude,sizeof(out->latitude),row[6],strlen(row[6]));
      copy_field(out->longitude,sizeof(out->longitude),row[7],strlen(row[7]));
      copy_field(out->gmtshift,sizeof(out->gmtshift),row[8],strlen(row[8]));
      mysql_free_result(res);
      return 1;
    }
    mysql_free_result(res);
  }
  return 0;
}

time_t radio_adif_time(const char *date,const char *clock) {
  struct tm t;
  int y,m,d,hh,mm,ss,n;

  if (date==NULL || clock==NULL) return (time_t)-1;
  memset(&t,0,sizeof(t));
  y=m=d=hh=mm=ss=0;
  n=sscanf(date,"%4d%2d%2d",&y,&m,&d);
  if (n!=3) return (time_t)-1;
  n=sscanf(clock,"%2d%2d%2d",&hh,&mm,&ss);
  if (n<2) return (time_t)-1;
  if (y<1970 || m<1 || m>12 || d<1 || d>31 || hh<0 || hh>23 || mm<0 || mm>59 || ss<0 || ss>60) return (time_t)-1;
  t.tm_year=y-1900;
  t.tm_mon=m-1;
  t.tm_mday=d;
  t.tm_hour=hh;
  t.tm_min=mm;
  t.tm_sec=ss;
  t.tm_isdst=0;
  return timegm(&t);
}


int radio_locator_to_latlon(const char *locator,double *lat,double *lon) {
  char g[7];
  int n,i;
  double x,y,w,h;

  if (locator==NULL || lat==NULL || lon==NULL) return 0;
  n=(int)strlen(locator);
  if (n!=2 && n!=4 && n!=6) return 0;
  for (i=0;i<n;i++) g[i]=locator[i];
  g[n]='\0';

  if (g[0]>='a' && g[0]<='z') g[0]=(char)(g[0]-'a'+'A');
  if (g[1]>='a' && g[1]<='z') g[1]=(char)(g[1]-'a'+'A');
  if (g[0]<'A' || g[0]>'R' || g[1]<'A' || g[1]>'R') return 0;

  x=(double)(g[0]-'A')*20.0-180.0;
  y=(double)(g[1]-'A')*10.0-90.0;
  w=20.0;
  h=10.0;

  if (n>=4) {
    if (g[2]<'0' || g[2]>'9' || g[3]<'0' || g[3]>'9') return 0;
    x+=(double)(g[2]-'0')*2.0;
    y+=(double)(g[3]-'0');
    w=2.0;
    h=1.0;
  }

  if (n==6) {
    if (g[4]>='A' && g[4]<='X') g[4]=(char)(g[4]-'A'+'a');
    if (g[5]>='A' && g[5]<='X') g[5]=(char)(g[5]-'A'+'a');
    if (g[4]<'a' || g[4]>'x' || g[5]<'a' || g[5]>'x') return 0;
    x+=(double)(g[4]-'a')/12.0;
    y+=(double)(g[5]-'a')/24.0;
    w=1.0/12.0;
    h=1.0/24.0;
  }

  *lon=x+w/2.0;
  *lat=y+h/2.0;
  return 1;
}

double radio_distance_km(double lat1,double lon1,double lat2,double lon2) {
  double a,c,dlat,dlon,r1,r2;

  r1=lat1*3.14159265358979323846/180.0;
  r2=lat2*3.14159265358979323846/180.0;
  dlat=(lat2-lat1)*3.14159265358979323846/180.0;
  dlon=(lon2-lon1)*3.14159265358979323846/180.0;
  a=sin(dlat/2.0)*sin(dlat/2.0)+cos(r1)*cos(r2)*sin(dlon/2.0)*sin(dlon/2.0);
  if (a<0.0) a=0.0;
  if (a>1.0) a=1.0;
  c=2.0*atan2(sqrt(a),sqrt(1.0-a));
  return 6371.0*c;
}

double radio_bearing_deg(double lat1,double lon1,double lat2,double lon2) {
  double a,b,dlon,r1,r2;

  r1=lat1*3.14159265358979323846/180.0;
  r2=lat2*3.14159265358979323846/180.0;
  dlon=(lon2-lon1)*3.14159265358979323846/180.0;
  a=sin(dlon)*cos(r2);
  b=cos(r1)*sin(r2)-sin(r1)*cos(r2)*cos(dlon);
  a=atan2(a,b)*180.0/3.14159265358979323846;
  if (a<0.0) a+=360.0;
  if (a>=360.0) a-=360.0;
  return a;
}

int radio_locator_distance_bearing(const char *locator1,const char *locator2,double *distance_km,double *bearing_deg) {
  double lat1,lon1,lat2,lon2;

  if (distance_km==NULL || bearing_deg==NULL) return 0;
  if (!radio_locator_to_latlon(locator1,&lat1,&lon1)) return 0;
  if (!radio_locator_to_latlon(locator2,&lat2,&lon2)) return 0;
  *distance_km=radio_distance_km(lat1,lon1,lat2,lon2);
  *bearing_deg=radio_bearing_deg(lat1,lon1,lat2,lon2);
  return 1;
}
