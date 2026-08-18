// Gianluca Mazzini @2022- Version 3.00

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <mysql/mysql.h>
#include "radio_config.h"

#define QRZ_URL "https://xmldata.qrz.com/xml/current/"
#define QRZ_RU_LOGIN_URL "https://api.qrz.ru/login"
#define QRZ_RU_CALL_URL "https://api.qrz.ru/callsign"
#define REQUEST_MAX 256
#define RESPONSE_MAX 512
#define SESSION_MAX 256
#define FIELD_MAX 512

#define LOOKUP_OK 1
#define LOOKUP_NOTFOUND 0
#define LOOKUP_ERROR -1
#define LOOKUP_AUTH -2

static volatile sig_atomic_t running=1;
static int listen_fd=-1;
static RadioConfig cfg;
static MYSQL *db=NULL;
static char qrz_key[SESSION_MAX];
static char qrz_ru_key[SESSION_MAX];
static time_t qrz_ru_login_time=0;
static time_t qrz_ru_last_request=0;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} HttpBuffer;

typedef struct {
  char callsign[21];
  char firstname[201];
  char lastname[201];
  char addr1[301];
  char addr2[201];
  char state[101];
  char zip[51];
  char country[46];
  char grid[7];
  char email[61];
  int cqzone;
  int ituzone;
  int born;
  char image[101];
  char src[11];
} WhoRecord;

static void on_signal(int sig) {
  (void)sig;
  running=0;
  if (listen_fd>=0) close(listen_fd);
  listen_fd=-1;
}

static void copy_text(char *dst,size_t cap,const char *src) {
  size_t n;

  if (dst==NULL || cap==0) return;
  if (src==NULL) {
    dst[0]='\0';
    return;
  }
  n=strlen(src);
  if (n>=cap) n=cap-1;
  memcpy(dst,src,n);
  dst[n]='\0';
}

static void upper_call(char *s) {
  unsigned char *p;

  p=(unsigned char *)s;
  for (;*p!='\0';p++) *p=(unsigned char)toupper(*p);
}

static int valid_callsign(const char *s) {
  size_t n,i;
  unsigned char c;

  if (s==NULL) return 0;
  n=strlen(s);
  if (n<2 || n>20) return 0;
  for (i=0;i<n;i++) {
    c=(unsigned char)s[i];
    if (!isalnum(c) && c!='/' && c!='-') return 0;
  }
  return 1;
}

static size_t http_write(void *ptr,size_t size,size_t nmemb,void *userdata) {
  HttpBuffer *b;
  size_t n,need,cap;
  char *p;

  b=(HttpBuffer *)userdata;
  n=size*nmemb;
  if (n==0) return 0;
  need=b->len+n+1;
  if (need>b->cap) {
    cap=b->cap==0?4096:b->cap;
    for (;cap<need;cap*=2) {
      if (cap>1024*1024) return 0;
    }
    p=(char *)realloc(b->data,cap);
    if (p==NULL) return 0;
    b->data=p;
    b->cap=cap;
  }
  memcpy(b->data+b->len,ptr,n);
  b->len+=n;
  b->data[b->len]='\0';
  return n;
}

static void http_buffer_free(HttpBuffer *b) {
  if (b->data!=NULL) free(b->data);
  b->data=NULL;
  b->len=0;
  b->cap=0;
}

static int http_post(const char *url,const char *fields,HttpBuffer *out,char *err,size_t errcap) {
  CURL *h;
  CURLcode rc;
  long status;
  char curlerr[CURL_ERROR_SIZE];

  h=curl_easy_init();
  if (h==NULL) {
    snprintf(err,errcap,"curl init failed");
    return 0;
  }
  curlerr[0]='\0';
  out->data=NULL;
  out->len=0;
  out->cap=0;
  curl_easy_setopt(h,CURLOPT_URL,url);
  curl_easy_setopt(h,CURLOPT_POST,1L);
  curl_easy_setopt(h,CURLOPT_POSTFIELDS,fields);
  curl_easy_setopt(h,CURLOPT_POSTFIELDSIZE,(long)strlen(fields));
  curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT,(long)cfg.callbook_connect_timeout);
  curl_easy_setopt(h,CURLOPT_TIMEOUT,(long)cfg.callbook_http_timeout);
  curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,1L);
  curl_easy_setopt(h,CURLOPT_MAXREDIRS,3L);
  curl_easy_setopt(h,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(h,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(h,CURLOPT_PROTOCOLS_STR,"https");
  curl_easy_setopt(h,CURLOPT_REDIR_PROTOCOLS_STR,"https");
  curl_easy_setopt(h,CURLOPT_USERAGENT,"GM-radio-callbookd/3.00");
  curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,http_write);
  curl_easy_setopt(h,CURLOPT_WRITEDATA,out);
  curl_easy_setopt(h,CURLOPT_ERRORBUFFER,curlerr);
  rc=curl_easy_perform(h);
  status=0;
  curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&status);
  curl_easy_cleanup(h);
  if (rc!=CURLE_OK) {
    snprintf(err,errcap,"HTTPS error: %s",curlerr[0]?curlerr:curl_easy_strerror(rc));
    http_buffer_free(out);
    return 0;
  }
  if (out->data==NULL || out->len==0) {
    if (status<200 || status>=300) snprintf(err,errcap,"HTTP status %ld",status);
    else snprintf(err,errcap,"empty HTTP response");
    http_buffer_free(out);
    return 0;
  }
  if (status<200 || status>=300) {
    if (strstr(out->data,"<QRZDatabase")==NULL && strstr(out->data,"<?xml")==NULL) {
      snprintf(err,errcap,"HTTP status %ld",status);
      http_buffer_free(out);
      return 0;
    }
  }
  return 1;
}

static int http_get(const char *url,HttpBuffer *out,char *err,size_t errcap) {
  CURL *h;
  CURLcode rc;
  long status;
  char curlerr[CURL_ERROR_SIZE];

  h=curl_easy_init();
  if (h==NULL) {
    snprintf(err,errcap,"curl init failed");
    return 0;
  }
  curlerr[0]='\0';
  out->data=NULL;
  out->len=0;
  out->cap=0;
  curl_easy_setopt(h,CURLOPT_URL,url);
  curl_easy_setopt(h,CURLOPT_HTTPGET,1L);
  curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT,(long)cfg.callbook_connect_timeout);
  curl_easy_setopt(h,CURLOPT_TIMEOUT,(long)cfg.callbook_http_timeout);
  curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,1L);
  curl_easy_setopt(h,CURLOPT_MAXREDIRS,3L);
  curl_easy_setopt(h,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(h,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(h,CURLOPT_PROTOCOLS_STR,"https");
  curl_easy_setopt(h,CURLOPT_REDIR_PROTOCOLS_STR,"https");
  curl_easy_setopt(h,CURLOPT_USERAGENT,"GM-radio-callbookd/3.00");
  curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,http_write);
  curl_easy_setopt(h,CURLOPT_WRITEDATA,out);
  curl_easy_setopt(h,CURLOPT_ERRORBUFFER,curlerr);
  rc=curl_easy_perform(h);
  status=0;
  curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&status);
  curl_easy_cleanup(h);
  if (rc!=CURLE_OK) {
    snprintf(err,errcap,"HTTPS error: %s",curlerr[0]?curlerr:curl_easy_strerror(rc));
    http_buffer_free(out);
    return 0;
  }
  if (out->data==NULL || out->len==0) {
    if (status<200 || status>=300) snprintf(err,errcap,"HTTP status %ld",status);
    else snprintf(err,errcap,"empty HTTP response");
    http_buffer_free(out);
    return 0;
  }
  if (status<200 || status>=300) {
    if (strstr(out->data,"<QRZDatabase")==NULL && strstr(out->data,"<?xml")==NULL) {
      snprintf(err,errcap,"HTTP status %ld",status);
      http_buffer_free(out);
      return 0;
    }
  }
  return 1;
}

static char *url_escape(const char *s) {
  char *p;

  p=curl_easy_escape(NULL,s,0);
  return p;
}

static xmlNode *xml_find(xmlNode *node,const char *name) {
  xmlNode *p,*q;

  for (p=node;p!=NULL;p=p->next) {
    if (p->type==XML_ELEMENT_NODE && strcasecmp((const char *)p->name,name)==0) return p;
    q=xml_find(p->children,name);
    if (q!=NULL) return q;
  }
  return NULL;
}

static int xml_value(xmlDoc *doc,const char *name,char *out,size_t cap) {
  xmlNode *node;
  xmlChar *value;

  out[0]='\0';
  node=xml_find(xmlDocGetRootElement(doc),name);
  if (node==NULL) return 0;
  value=xmlNodeGetContent(node);
  if (value==NULL) return 0;
  copy_text(out,cap,(const char *)value);
  xmlFree(value);
  return 1;
}

static xmlDoc *parse_xml(HttpBuffer *b,char *err,size_t errcap) {
  xmlDoc *doc;

  doc=xmlReadMemory(b->data,(int)b->len,"callbook.xml",NULL,XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING);
  if (doc==NULL) snprintf(err,errcap,"invalid XML response");
  return doc;
}

static int db_connect(char *err,size_t errcap) {
  if (db!=NULL) {
    if (mysql_ping(db)==0) return 1;
    mysql_close(db);
    db=NULL;
  }
  db=mysql_init(NULL);
  if (db==NULL) {
    snprintf(err,errcap,"mysql init failed");
    return 0;
  }
  if (mysql_real_connect(db,cfg.db_host,cfg.db_user,cfg.db_pass,cfg.db_name,cfg.db_port,NULL,0)==NULL) {
    snprintf(err,errcap,"database connect failed: %s",mysql_error(db));
    mysql_close(db);
    db=NULL;
    return 0;
  }
  if (mysql_set_character_set(db,"utf8mb4")!=0) {
    snprintf(err,errcap,"database charset failed: %s",mysql_error(db));
    mysql_close(db);
    db=NULL;
    return 0;
  }
  return 1;
}

static int bind_string(MYSQL_BIND *b,char *s,unsigned long *len) {
  memset(b,0,sizeof(*b));
  *len=(unsigned long)strlen(s);
  b->buffer_type=MYSQL_TYPE_STRING;
  b->buffer=s;
  b->buffer_length=*len+1;
  b->length=len;
  return 1;
}

static int who_store(WhoRecord *r,char *err,size_t errcap) {
  MYSQL_STMT *stmt;
  MYSQL_BIND b[16];
  unsigned long len[13];
  long long now;
  const char *sql;

  if (!db_connect(err,errcap)) return 0;
  sql="replace into who (callsign,firstname,lastname,addr1,addr2,state,zip,country,grid,email,cqzone,ituzone,born,image,time,src) values (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  stmt=mysql_stmt_init(db);
  if (stmt==NULL) {
    snprintf(err,errcap,"statement init failed");
    return 0;
  }
  if (mysql_stmt_prepare(stmt,sql,(unsigned long)strlen(sql))!=0) {
    snprintf(err,errcap,"statement prepare failed: %s",mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return 0;
  }
  memset(b,0,sizeof(b));
  bind_string(&b[0],r->callsign,&len[0]);
  bind_string(&b[1],r->firstname,&len[1]);
  bind_string(&b[2],r->lastname,&len[2]);
  bind_string(&b[3],r->addr1,&len[3]);
  bind_string(&b[4],r->addr2,&len[4]);
  bind_string(&b[5],r->state,&len[5]);
  bind_string(&b[6],r->zip,&len[6]);
  bind_string(&b[7],r->country,&len[7]);
  bind_string(&b[8],r->grid,&len[8]);
  bind_string(&b[9],r->email,&len[9]);
  b[10].buffer_type=MYSQL_TYPE_LONG;
  b[10].buffer=&r->cqzone;
  b[11].buffer_type=MYSQL_TYPE_LONG;
  b[11].buffer=&r->ituzone;
  b[12].buffer_type=MYSQL_TYPE_LONG;
  b[12].buffer=&r->born;
  bind_string(&b[13],r->image,&len[10]);
  now=(long long)time(NULL);
  b[14].buffer_type=MYSQL_TYPE_LONGLONG;
  b[14].buffer=&now;
  bind_string(&b[15],r->src,&len[11]);
  if (mysql_stmt_bind_param(stmt,b)!=0 || mysql_stmt_execute(stmt)!=0) {
    snprintf(err,errcap,"database write failed: %s",mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return 0;
  }
  mysql_stmt_close(stmt);
  return 1;
}

static int text_to_int(const char *s) {
  char *end;
  long n;

  if (s==NULL || *s=='\0') return 0;
  errno=0;
  n=strtol(s,&end,10);
  if (errno!=0 || end==s) return 0;
  return (int)n;
}

static int year_from_text(const char *s) {
  size_t i,n;
  int y;

  if (s==NULL) return 0;
  n=strlen(s);
  for (i=0;i+3<n;i++) {
    if (isdigit((unsigned char)s[i]) && isdigit((unsigned char)s[i+1]) && isdigit((unsigned char)s[i+2]) && isdigit((unsigned char)s[i+3])) {
      y=(s[i]-'0')*1000+(s[i+1]-'0')*100+(s[i+2]-'0')*10+(s[i+3]-'0');
      if (y>=1900 && y<=2100) return y;
    }
  }
  return 0;
}

static void translit_ru(const char *src,char *dst,size_t cap) {
  const char *cyr[]={"а","б","в","г","д","е","ё","ж","з","и","й","к","л","м","н","о","п","р","с","т","у","ф","х","ц","ч","ш","щ","ъ","ы","ь","э","ю","я","А","Б","В","Г","Д","Е","Ё","Ж","З","И","Й","К","Л","М","Н","О","П","Р","С","Т","У","Ф","Х","Ц","Ч","Ш","Щ","Ъ","Ы","Ь","Э","Ю","Я"};
  const char *lat[]={"a","b","v","g","d","e","e","zh","z","i","y","k","l","m","n","o","p","r","s","t","u","f","h","ts","ch","sh","sht","i","y","y","e","yu","ya","A","B","V","G","D","E","E","Zh","Z","I","Y","K","L","M","N","O","P","R","S","T","U","F","H","Ts","Ch","Sh","Sht","I","Y","Y","E","Yu","Ya"};
  size_t i,n,clen,used,add;
  int found;

  n=sizeof(cyr)/sizeof(cyr[0]);
  used=0;
  dst[0]='\0';
  for (;*src!='\0' && used+1<cap;) {
    found=0;
    for (i=0;i<n;i++) {
      clen=strlen(cyr[i]);
      if (strncmp(src,cyr[i],clen)==0) {
        add=strlen(lat[i]);
        if (used+add>=cap) return;
        memcpy(dst+used,lat[i],add);
        used+=add;
        dst[used]='\0';
        src+=clen;
        found=1;
        break;
      }
    }
    if (!found) {
      dst[used++]=*src++;
      dst[used]='\0';
    }
  }
}

static int qrz_login(char *err,size_t errcap) {
  HttpBuffer b;
  xmlDoc *doc;
  char *u,*p,*a,fields[2048],key[SESSION_MAX],xerr[FIELD_MAX];
  int ok;

  u=url_escape(cfg.qrz_user);
  p=url_escape(cfg.qrz_pass);
  a=url_escape(cfg.qrz_agent);
  if (u==NULL || p==NULL || a==NULL) {
    if (u!=NULL) curl_free(u);
    if (p!=NULL) curl_free(p);
    if (a!=NULL) curl_free(a);
    snprintf(err,errcap,"QRZ.com URL encoding failed");
    return 0;
  }
  snprintf(fields,sizeof(fields),"username=%s&password=%s&agent=%s",u,p,a);
  curl_free(u); curl_free(p); curl_free(a);
  if (!http_post(QRZ_URL,fields,&b,err,errcap)) return 0;
  doc=parse_xml(&b,err,errcap);
  if (doc==NULL) {
    http_buffer_free(&b);
    return 0;
  }
  key[0]=xerr[0]='\0';
  xml_value(doc,"Key",key,sizeof(key));
  xml_value(doc,"Error",xerr,sizeof(xerr));
  ok=key[0]!='\0';
  if (ok) copy_text(qrz_key,sizeof(qrz_key),key);
  else snprintf(err,errcap,"QRZ.com login failed: %s",xerr[0]?xerr:"no session key");
  xmlFreeDoc(doc);
  http_buffer_free(&b);
  return ok;
}

static int qrz_parse_call(xmlDoc *doc,const char *requested,WhoRecord *r,char *err,size_t errcap) {
  char tmp[FIELD_MAX],key[SESSION_MAX],xerr[FIELD_MAX];
  xmlNode *callnode;

  memset(r,0,sizeof(*r));
  key[0]=xerr[0]='\0';
  xml_value(doc,"Key",key,sizeof(key));
  xml_value(doc,"Error",xerr,sizeof(xerr));
  if (key[0]!='\0') copy_text(qrz_key,sizeof(qrz_key),key);
  callnode=xml_find(xmlDocGetRootElement(doc),"Callsign");
  if (callnode==NULL) {
    if (xerr[0]!='\0' && (strstr(xerr,"Not found")!=NULL || strstr(xerr,"not found")!=NULL)) return LOOKUP_NOTFOUND;
    if (key[0]=='\0') return LOOKUP_AUTH;
    snprintf(err,errcap,"QRZ.com lookup failed: %s",xerr[0]?xerr:"missing Callsign node");
    return LOOKUP_ERROR;
  }
  copy_text(r->callsign,sizeof(r->callsign),requested);
  upper_call(r->callsign);
  xml_value(doc,"fname",r->firstname,sizeof(r->firstname));
  xml_value(doc,"name",r->lastname,sizeof(r->lastname));
  xml_value(doc,"addr1",r->addr1,sizeof(r->addr1));
  xml_value(doc,"addr2",r->addr2,sizeof(r->addr2));
  xml_value(doc,"state",r->state,sizeof(r->state));
  xml_value(doc,"zip",r->zip,sizeof(r->zip));
  xml_value(doc,"country",r->country,sizeof(r->country));
  xml_value(doc,"grid",r->grid,sizeof(r->grid));
  xml_value(doc,"email",r->email,sizeof(r->email));
  tmp[0]='\0'; xml_value(doc,"cqzone",tmp,sizeof(tmp)); r->cqzone=text_to_int(tmp);
  tmp[0]='\0'; xml_value(doc,"ituzone",tmp,sizeof(tmp)); r->ituzone=text_to_int(tmp);
  tmp[0]='\0'; xml_value(doc,"born",tmp,sizeof(tmp)); r->born=text_to_int(tmp);
  xml_value(doc,"image",r->image,sizeof(r->image));
  copy_text(r->src,sizeof(r->src),"qrz.com");
  return LOOKUP_OK;
}

static int qrz_lookup_once(const char *call,WhoRecord *r,char *err,size_t errcap) {
  HttpBuffer b;
  xmlDoc *doc;
  char *k,*c,fields[1024];
  int rc;

  if (qrz_key[0]=='\0' && !qrz_login(err,errcap)) return LOOKUP_ERROR;
  k=url_escape(qrz_key);
  c=url_escape(call);
  if (k==NULL || c==NULL) {
    if (k!=NULL) curl_free(k);
    if (c!=NULL) curl_free(c);
    snprintf(err,errcap,"QRZ.com URL encoding failed");
    return LOOKUP_ERROR;
  }
  snprintf(fields,sizeof(fields),"s=%s&callsign=%s",k,c);
  curl_free(k); curl_free(c);
  if (!http_post(QRZ_URL,fields,&b,err,errcap)) return LOOKUP_ERROR;
  doc=parse_xml(&b,err,errcap);
  if (doc==NULL) {
    http_buffer_free(&b);
    return LOOKUP_ERROR;
  }
  rc=qrz_parse_call(doc,call,r,err,errcap);
  xmlFreeDoc(doc);
  http_buffer_free(&b);
  return rc;
}

static int qrz_lookup(const char *call,char *err,size_t errcap) {
  WhoRecord r;
  int rc;

  rc=qrz_lookup_once(call,&r,err,errcap);
  if (rc==LOOKUP_AUTH) {
    qrz_key[0]='\0';
    if (!qrz_login(err,errcap)) return LOOKUP_ERROR;
    rc=qrz_lookup_once(call,&r,err,errcap);
  }
  if (rc==LOOKUP_OK && !who_store(&r,err,errcap)) return LOOKUP_ERROR;
  return rc;
}

static void qrz_ru_wait(void) {
  time_t now,wait;

  if (qrz_ru_last_request==0) return;
  now=time(NULL);
  wait=(time_t)cfg.qrz_ru_min_interval-(now-qrz_ru_last_request);
  if (wait>0) sleep((unsigned int)wait);
}

static int qrz_ru_post(const char *url,const char *fields,HttpBuffer *b,char *err,size_t errcap) {
  int rc;

  qrz_ru_wait();
  rc=http_post(url,fields,b,err,errcap);
  qrz_ru_last_request=time(NULL);
  return rc;
}

static int qrz_ru_get(const char *url,HttpBuffer *b,char *err,size_t errcap) {
  int rc;

  qrz_ru_wait();
  rc=http_get(url,b,err,errcap);
  qrz_ru_last_request=time(NULL);
  return rc;
}

static int qrz_ru_login(char *err,size_t errcap) {
  HttpBuffer b;
  xmlDoc *doc;
  char *u,*p,*a,fields[2048],key[SESSION_MAX],xerr[FIELD_MAX];
  int ok;

  u=url_escape(cfg.qrz_ru_user);
  p=url_escape(cfg.qrz_ru_pass);
  a=url_escape(cfg.qrz_ru_agent);
  if (u==NULL || p==NULL || a==NULL) {
    if (u!=NULL) curl_free(u);
    if (p!=NULL) curl_free(p);
    if (a!=NULL) curl_free(a);
    snprintf(err,errcap,"QRZ.ru URL encoding failed");
    return 0;
  }
  snprintf(fields,sizeof(fields),"u=%s&p=%s&agent=%s",u,p,a);
  curl_free(u); curl_free(p); curl_free(a);
  if (!qrz_ru_post(QRZ_RU_LOGIN_URL,fields,&b,err,errcap)) return 0;
  doc=parse_xml(&b,err,errcap);
  if (doc==NULL) {
    http_buffer_free(&b);
    return 0;
  }
  key[0]=xerr[0]='\0';
  xml_value(doc,"session_id",key,sizeof(key));
  xml_value(doc,"error",xerr,sizeof(xerr));
  ok=key[0]!='\0';
  if (ok) {
    copy_text(qrz_ru_key,sizeof(qrz_ru_key),key);
    qrz_ru_login_time=time(NULL);
  } else snprintf(err,errcap,"QRZ.ru login failed: %s",xerr[0]?xerr:"no session id");
  xmlFreeDoc(doc);
  http_buffer_free(&b);
  return ok;
}

static int qrz_ru_session_valid(void) {
  if (qrz_ru_key[0]=='\0' || qrz_ru_login_time==0) return 0;
  if (time(NULL)-qrz_ru_login_time>=(time_t)cfg.qrz_ru_session_ttl) return 0;
  return 1;
}

static int qrz_ru_parse_call(xmlDoc *doc,const char *requested,WhoRecord *r,char *err,size_t errcap) {
  char raw[FIELD_MAX],tmp[FIELD_MAX],xerr[FIELD_MAX],code[64];
  xmlNode *callnode;

  memset(r,0,sizeof(*r));
  xerr[0]=code[0]='\0';
  xml_value(doc,"error",xerr,sizeof(xerr));
  xml_value(doc,"errorcode",code,sizeof(code));
  callnode=xml_find(xmlDocGetRootElement(doc),"Callsign");
  if (callnode==NULL) {
    if (strcmp(code,"404")==0 || strstr(xerr,"not found")!=NULL || strstr(xerr,"Not found")!=NULL) return LOOKUP_NOTFOUND;
    if (strcmp(code,"403")==0 || strstr(xerr,"expired")!=NULL || strstr(xerr,"Session")!=NULL) return LOOKUP_AUTH;
    snprintf(err,errcap,"QRZ.ru lookup failed: %s",xerr[0]?xerr:"missing Callsign node");
    return LOOKUP_ERROR;
  }
  copy_text(r->callsign,sizeof(r->callsign),requested);
  upper_call(r->callsign);
  raw[0]='\0'; xml_value(doc,"name",raw,sizeof(raw)); translit_ru(raw,r->firstname,sizeof(r->firstname));
  raw[0]='\0'; xml_value(doc,"surname",raw,sizeof(raw)); translit_ru(raw,r->lastname,sizeof(r->lastname));
  raw[0]='\0'; xml_value(doc,"street",raw,sizeof(raw)); translit_ru(raw,r->addr1,sizeof(r->addr1));
  raw[0]='\0'; xml_value(doc,"city",raw,sizeof(raw)); translit_ru(raw,r->addr2,sizeof(r->addr2));
  raw[0]='\0'; xml_value(doc,"state",raw,sizeof(raw)); translit_ru(raw,r->state,sizeof(r->state));
  raw[0]='\0'; xml_value(doc,"zip",raw,sizeof(raw)); translit_ru(raw,r->zip,sizeof(r->zip));
  raw[0]='\0'; xml_value(doc,"country",raw,sizeof(raw)); translit_ru(raw,r->country,sizeof(r->country));
  raw[0]='\0'; xml_value(doc,"qthloc",raw,sizeof(raw)); copy_text(r->grid,sizeof(r->grid),raw);
  tmp[0]='\0'; xml_value(doc,"cq_zone",tmp,sizeof(tmp)); r->cqzone=text_to_int(tmp);
  tmp[0]='\0'; xml_value(doc,"itu_zone",tmp,sizeof(tmp)); r->ituzone=text_to_int(tmp);
  tmp[0]='\0'; xml_value(doc,"birthday",tmp,sizeof(tmp)); r->born=year_from_text(tmp);
  raw[0]='\0'; xml_value(doc,"file",raw,sizeof(raw)); copy_text(r->image,sizeof(r->image),raw);
  r->email[0]='\0';
  copy_text(r->src,sizeof(r->src),"qrz.ru");
  return LOOKUP_OK;
}

static int qrz_ru_lookup_once(const char *call,WhoRecord *r,char *err,size_t errcap) {
  HttpBuffer b;
  xmlDoc *doc;
  char *k,*c,url[2048];
  int rc;

  if (!qrz_ru_session_valid()) {
    qrz_ru_key[0]='\0';
    if (!qrz_ru_login(err,errcap)) return LOOKUP_ERROR;
  }
  k=url_escape(qrz_ru_key);
  c=url_escape(call);
  if (k==NULL || c==NULL) {
    if (k!=NULL) curl_free(k);
    if (c!=NULL) curl_free(c);
    snprintf(err,errcap,"QRZ.ru URL encoding failed");
    return LOOKUP_ERROR;
  }
  snprintf(url,sizeof(url),"%s?id=%s&callsign=%s",QRZ_RU_CALL_URL,k,c);
  curl_free(k); curl_free(c);
  if (!qrz_ru_get(url,&b,err,errcap)) return LOOKUP_ERROR;
  doc=parse_xml(&b,err,errcap);
  if (doc==NULL) {
    http_buffer_free(&b);
    return LOOKUP_ERROR;
  }
  rc=qrz_ru_parse_call(doc,call,r,err,errcap);
  xmlFreeDoc(doc);
  http_buffer_free(&b);
  return rc;
}

static int qrz_ru_lookup(const char *call,char *err,size_t errcap) {
  WhoRecord r;
  int rc;

  rc=qrz_ru_lookup_once(call,&r,err,errcap);
  if (rc==LOOKUP_AUTH) {
    qrz_ru_key[0]='\0';
    qrz_ru_login_time=0;
    if (!qrz_ru_login(err,errcap)) return LOOKUP_ERROR;
    rc=qrz_ru_lookup_once(call,&r,err,errcap);
  }
  if (rc==LOOKUP_OK && !who_store(&r,err,errcap)) return LOOKUP_ERROR;
  return rc;
}

static int send_all(int fd,const char *buf,size_t len) {
  ssize_t n;
  size_t done;

  done=0;
  for (;done<len;) {
    n=send(fd,buf+done,len-done,0);
    if (n<0) {
      if (errno==EINTR) continue;
      return 0;
    }
    if (n==0) return 0;
    done+=(size_t)n;
  }
  return 1;
}

static int open_listener(void) {
  struct addrinfo hints,*res,*rp;
  char port[16];
  int fd,one,rc;

  memset(&hints,0,sizeof(hints));
  hints.ai_family=AF_UNSPEC;
  hints.ai_socktype=SOCK_STREAM;
  hints.ai_flags=AI_PASSIVE;
  snprintf(port,sizeof(port),"%u",cfg.callbook_local_port);
  rc=getaddrinfo(cfg.callbook_local_host,port,&hints,&res);
  if (rc!=0) return -1;
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
  return fd;
}

static void serve_client(int fd) {
  char req[REQUEST_MAX],cmd[32],call[32],resp[RESPONSE_MAX],err[FIELD_MAX];
  ssize_t n;
  int rc;

  n=recv(fd,req,sizeof(req)-1,0);
  if (n<=0) return;
  req[n]='\0';
  cmd[0]=call[0]='\0';
  if (sscanf(req,"%31s %31s",cmd,call)<1) return;
  if (strcasecmp(cmd,"PING")==0) {
    send_all(fd,"OK PONG\n",8);
    return;
  }
  if (strcasecmp(cmd,"STATUS")==0) {
    snprintf(resp,sizeof(resp),"OK qrz=%s qrzru=%s\n",qrz_key[0]?"session":"none",qrz_ru_session_valid()?"session":"none");
    send_all(fd,resp,strlen(resp));
    return;
  }
  if (!valid_callsign(call)) {
    send_all(fd,"ERROR INVALID CALLSIGN\n",23);
    return;
  }
  upper_call(call);
  err[0]='\0';
  if (strcasecmp(cmd,"QRZCOM")==0) rc=qrz_lookup(call,err,sizeof(err));
  else if (strcasecmp(cmd,"QRZRU")==0) rc=qrz_ru_lookup(call,err,sizeof(err));
  else {
    send_all(fd,"ERROR INVALID COMMAND\n",22);
    return;
  }
  if (rc==LOOKUP_OK) snprintf(resp,sizeof(resp),"OK %s %s\n",call,strcasecmp(cmd,"QRZCOM")==0?"qrz.com":"qrz.ru");
  else if (rc==LOOKUP_NOTFOUND) snprintf(resp,sizeof(resp),"NOTFOUND %s %s\n",call,strcasecmp(cmd,"QRZCOM")==0?"qrz.com":"qrz.ru");
  else snprintf(resp,sizeof(resp),"ERROR %s\n",err[0]?err:"lookup failed");
  send_all(fd,resp,strlen(resp));
}

static int config_valid(char *err,size_t errcap) {
  if (cfg.db_host[0]=='\0' || cfg.db_user[0]=='\0' || cfg.db_name[0]=='\0') {
    snprintf(err,errcap,"missing database configuration");
    return 0;
  }
  if (cfg.callbook_local_host[0]=='\0' || cfg.callbook_local_port==0 || cfg.callbook_connect_timeout==0 || cfg.callbook_http_timeout==0) {
    snprintf(err,errcap,"missing local callbook configuration");
    return 0;
  }
  if (cfg.qrz_user[0]=='\0' || cfg.qrz_pass[0]=='\0' || cfg.qrz_agent[0]=='\0') {
    snprintf(err,errcap,"missing QRZ.com configuration");
    return 0;
  }
  if (cfg.qrz_ru_user[0]=='\0' || cfg.qrz_ru_pass[0]=='\0' || cfg.qrz_ru_agent[0]=='\0' || cfg.qrz_ru_session_ttl==0 || cfg.qrz_ru_min_interval==0) {
    snprintf(err,errcap,"missing QRZ.ru configuration");
    return 0;
  }
  return 1;
}

int main(int argc,char **argv) {
  char err[FIELD_MAX];
  int cs,check;

  check=argc==2 && strcmp(argv[1],"--check")==0;
  if (argc>1 && !check) {
    fprintf(stderr,"usage: %s [--check]\n",argv[0]);
    return 2;
  }
  err[0]='\0';
  if (!radio_config_load(&cfg,RADIO_CONFIG_FILE,err,sizeof(err))) {
    fprintf(stderr,"config error: %s\n",err);
    return 1;
  }
  if (!config_valid(err,sizeof(err))) {
    fprintf(stderr,"config error: %s\n",err);
    return 1;
  }
  if (curl_global_init(CURL_GLOBAL_DEFAULT)!=0) {
    fprintf(stderr,"curl global init failed\n");
    return 1;
  }
  xmlInitParser();
  if (!db_connect(err,sizeof(err))) {
    fprintf(stderr,"%s\n",err);
    xmlCleanupParser();
    curl_global_cleanup();
    return 1;
  }
  if (check) {
    printf("callbookd check ok: database connected, local %s:%u\n",cfg.callbook_local_host,cfg.callbook_local_port);
    mysql_close(db);
    db=NULL;
    xmlCleanupParser();
    curl_global_cleanup();
    return 0;
  }
  signal(SIGINT,on_signal);
  signal(SIGTERM,on_signal);
  signal(SIGPIPE,SIG_IGN);
  listen_fd=open_listener();
  if (listen_fd<0) {
    fprintf(stderr,"cannot listen on %s:%u\n",cfg.callbook_local_host,cfg.callbook_local_port);
    mysql_close(db);
    db=NULL;
    xmlCleanupParser();
    curl_global_cleanup();
    return 1;
  }
  printf("callbookd local service %s:%u\n",cfg.callbook_local_host,cfg.callbook_local_port);
  fflush(stdout);
  for (;running;) {
    cs=accept(listen_fd,NULL,NULL);
    if (cs<0) {
      if (!running) break;
      if (errno==EINTR) continue;
      fprintf(stderr,"accept error: %s\n",strerror(errno));
      continue;
    }
    serve_client(cs);
    close(cs);
  }
  if (listen_fd>=0) close(listen_fd);
  if (db!=NULL) mysql_close(db);
  xmlCleanupParser();
  curl_global_cleanup();
  return 0;
}
