// Gianluca Mazzini @2026- Version 1.14

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <zip.h>
#include <mysql/mysql.h>
#include "energy_config.h"

#define AUTH_URL "https://api.mercatoelettrico.org/request/api/v1/Auth"
#define DATA_URL "https://api.mercatoelettrico.org/request/api/v1/RequestData"
#define HTTP_BUFFER_START 4096
#define TOKEN_SIZE 4096
#define AUTH_POST_SIZE 512
#define REQUEST_POST_SIZE 512
#define AUTH_HEADER_SIZE 4200
#define PRICE_COUNT_MAX 100
#define NUMBER_SIZE 64
#define SQL_SIZE 8192

struct mem {
  char *ptr;
  size_t len;
  size_t cap;
};

static int mem_init(struct mem *m) {
  m->ptr=(char *)malloc(HTTP_BUFFER_START);
  m->len=0;
  m->cap=0;

  if (m->ptr==NULL) {
    return 0;
  }

  m->cap=HTTP_BUFFER_START;
  m->ptr[0]='\0';
  return 1;
}

static void mem_reset(struct mem *m) {
  m->len=0;
  if (m->ptr!=NULL) {
    m->ptr[0]='\0';
  }
}

static void mem_free(struct mem *m) {
  free(m->ptr);
  m->ptr=NULL;
  m->len=0;
  m->cap=0;
}

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
  struct mem *m;
  char *p;
  size_t real_size, needed, new_cap;

  m=(struct mem *)userp;

  if (nmemb!=0 && size>((size_t)-1)/nmemb) {
    return 0;
  }
  real_size=size*nmemb;
  if (real_size>((size_t)-1)-m->len-1) {
    return 0;
  }
  needed=m->len+real_size+1;

  if (needed>m->cap) {
    new_cap=m->cap;
    for (;new_cap<needed;) {
      if (new_cap>((size_t)-1)/2) {
        new_cap=needed;
        break;
      }
      new_cap*=2;
    }
    p=(char *)realloc(m->ptr,new_cap);
    if (p==NULL) {
      return 0;
    }
    m->ptr=p;
    m->cap=new_cap;
  }

  memcpy(m->ptr+m->len,contents,real_size);
  m->len+=real_size;
  m->ptr[m->len]='\0';
  return real_size;
}

static int curl_post(CURL *curl, const char *url, struct curl_slist *headers,
    const char *post, struct mem *body) {
  CURLcode rc;
  long status;
  size_t len;

  len=strlen(post);
  if (len>(size_t)LONG_MAX) {
    fprintf(stderr,"HTTP request too large\n");
    return 0;
  }

  mem_reset(body);
  curl_easy_setopt(curl,CURLOPT_URL,url);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,post);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)len);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,body);

  rc=curl_easy_perform(curl);
  if (rc!=CURLE_OK) {
    fprintf(stderr,"curl error: %s\n",curl_easy_strerror(rc));
    return 0;
  }

  status=0;
  if (curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status)!=CURLE_OK) {
    fprintf(stderr,"cannot read HTTP status\n");
    return 0;
  }
  if (status<200 || status>=300) {
    fprintf(stderr,"HTTP %ld: %.500s\n",status,body->ptr);
    return 0;
  }
  return 1;
}

static int json_string_span(const char *json, const char *key,
    const char **value, size_t *len) {
  char pattern[128];
  const char *p, *q;
  int n;

  n=snprintf(pattern,sizeof(pattern),"\"%s\"",key);
  if (n<0 || n>=(int)sizeof(pattern)) {
    return 0;
  }

  p=strstr(json,pattern);
  if (p==NULL) {
    return 0;
  }
  p+=strlen(pattern);
  for (;*p!='\0' && isspace((unsigned char)*p);p++) {
  }
  if (*p!=':') {
    return 0;
  }
  p++;
  for (;*p!='\0' && isspace((unsigned char)*p);p++) {
  }
  if (*p!='\"') {
    return 0;
  }
  p++;

  for (q=p;*q!='\0' && *q!='\"';q++) {
    if (*q=='\\') {
      return 0;
    }
  }
  if (*q!='\"') {
    return 0;
  }

  *value=p;
  *len=(size_t)(q-p);
  return 1;
}

static int json_copy_string(const char *json, const char *key, char *out,
    size_t cap) {
  const char *value;
  size_t len;

  if (!json_string_span(json,key,&value,&len) || len>=cap) {
    return 0;
  }
  memcpy(out,value,len);
  out[len]='\0';
  return 1;
}

static int base64_value(int c) {
  if (c>='A' && c<='Z') {
    return c-'A';
  }
  if (c>='a' && c<='z') {
    return c-'a'+26;
  }
  if (c>='0' && c<='9') {
    return c-'0'+52;
  }
  if (c=='+') {
    return 62;
  }
  if (c=='/') {
    return 63;
  }
  return -1;
}

static unsigned char *base64_decode(const char *src, size_t len,
    size_t *out_len) {
  unsigned char *out;
  size_t cap, i, n;
  unsigned int acc;
  int bits, v, c;

  cap=(len/4)*3+3;
  out=(unsigned char *)malloc(cap);
  if (out==NULL) {
    return NULL;
  }

  acc=0;
  bits=0;
  n=0;
  for (i=0;i<len;i++) {
    c=(unsigned char)src[i];
    if (c=='=') {
      break;
    }
    if (isspace(c)) {
      continue;
    }
    v=base64_value(c);
    if (v<0) {
      free(out);
      return NULL;
    }

    acc=(acc<<6)|(unsigned int)v;
    bits+=6;
    if (bits>=8) {
      bits-=8;
      out[n++]=(unsigned char)((acc>>bits)&255U);
      if (bits==0) {
        acc=0;
      } else {
        acc&=(1U<<bits)-1U;
      }
    }
  }

  *out_len=n;
  return out;
}

static unsigned char *unzip_data(const unsigned char *zip_buf, size_t zip_len,
    size_t *file_len) {
  zip_error_t error;
  zip_source_t *source;
  zip_t *archive;
  zip_file_t *file;
  zip_stat_t stat;
  zip_int64_t entries, i, got;
  zip_uint64_t offset;
  unsigned char *buf;
  const char *name;
  size_t name_len;

  source=NULL;
  archive=NULL;
  file=NULL;
  buf=NULL;
  zip_error_init(&error);

  source=zip_source_buffer_create(zip_buf,(zip_uint64_t)zip_len,0,&error);
  if (source==NULL) {
    fprintf(stderr,"zip source error: %s\n",zip_error_strerror(&error));
    zip_error_fini(&error);
    return NULL;
  }

  archive=zip_open_from_source(source,0,&error);
  if (archive==NULL) {
    fprintf(stderr,"zip open error: %s\n",zip_error_strerror(&error));
    zip_source_free(source);
    zip_error_fini(&error);
    return NULL;
  }
  zip_error_fini(&error);

  entries=zip_get_num_entries(archive,0);
  if (entries<1) {
    fprintf(stderr,"zip empty\n");
    zip_discard(archive);
    return NULL;
  }

  for (i=0;i<entries;i++) {
    zip_stat_init(&stat);
    if (zip_stat_index(archive,(zip_uint64_t)i,0,&stat)!=0) {
      continue;
    }
    name=stat.name;
    if (name==NULL) {
      continue;
    }
    name_len=strlen(name);
    if (name_len!=0 && name[name_len-1]=='/') {
      continue;
    }
    break;
  }
  if (i>=entries) {
    fprintf(stderr,"zip contains no regular file\n");
    zip_discard(archive);
    return NULL;
  }

  if (stat.size>(zip_uint64_t)((size_t)-1)-1) {
    fprintf(stderr,"zip entry too large\n");
    zip_discard(archive);
    return NULL;
  }

  file=zip_fopen_index(archive,(zip_uint64_t)i,0);
  if (file==NULL) {
    fprintf(stderr,"zip fopen error\n");
    zip_discard(archive);
    return NULL;
  }

  buf=(unsigned char *)malloc((size_t)stat.size+1);
  if (buf==NULL) {
    fprintf(stderr,"memory allocation error\n");
    zip_fclose(file);
    zip_discard(archive);
    return NULL;
  }

  offset=0;
  for (;offset<stat.size;) {
    got=zip_fread(file,buf+offset,stat.size-offset);
    if (got<=0) {
      fprintf(stderr,"zip read error\n");
      free(buf);
      zip_fclose(file);
      zip_discard(archive);
      return NULL;
    }
    offset+=(zip_uint64_t)got;
  }
  buf[stat.size]='\0';
  *file_len=(size_t)stat.size;

  zip_fclose(file);
  if (zip_close(archive)!=0) {
    zip_discard(archive);
  }
  return buf;
}

static int parse_prices(const char *json, double *prices, int max_prices) {
  const char *r, *next, *s, *e, *endptr;
  char number[NUMBER_SIZE];
  double value;
  size_t len, i;
  int count, quoted;

  r=json;
  count=0;
  for (;(r=strstr(r,"\"Zone\":\"PUN\""))!=NULL;r+=12) {
    next=strstr(r+1,"\"Zone\":");
    s=strstr(r,"\"Price\"");
    if (s==NULL || (next!=NULL && s>next)) {
      continue;
    }
    s=strchr(s,':');
    if (s==NULL) {
      return -1;
    }
    s++;
    for (;*s!='\0' && isspace((unsigned char)*s);s++) {
    }

    quoted=0;
    if (*s=='\"') {
      quoted=1;
      s++;
    }

    if (quoted) {
      e=strchr(s,'\"');
      if (e==NULL) {
        return -1;
      }
    } else {
      for (e=s;*e!='\0' && *e!=',' && *e!='}' && *e!=']' &&
          *e!='\r' && *e!='\n';e++) {
      }
      for (;e>s && isspace((unsigned char)e[-1]);e--) {
      }
    }

    len=(size_t)(e-s);
    if (len==0 || len>=sizeof(number) || count>=max_prices) {
      return -1;
    }
    for (i=0;i<len;i++) {
      number[i]=s[i]==',' ? '.' : s[i];
    }
    number[len]='\0';

    errno=0;
    value=strtod(number,(char **)&endptr);
    if (errno!=0 || endptr==number || *endptr!='\0') {
      return -1;
    }
    prices[count++]=value;
  }
  return count;
}

static int valid_date(const char *text, time_t *epoch) {
  struct tm t;
  struct tm *check;
  time_t value;
  int year, month, day, i;

  if (strlen(text)!=8) {
    return 0;
  }
  for (i=0;i<8;i++) {
    if (!isdigit((unsigned char)text[i])) {
      return 0;
    }
  }
  if (sscanf(text,"%4d%2d%2d",&year,&month,&day)!=3) {
    return 0;
  }

  memset(&t,0,sizeof(t));
  t.tm_year=year-1900;
  t.tm_mon=month-1;
  t.tm_mday=day;
  t.tm_isdst=-1;
  value=mktime(&t);
  if (value==(time_t)-1) {
    return 0;
  }

  check=localtime(&value);
  if (check==NULL || check->tm_year!=year-1900 || check->tm_mon!=month-1 ||
      check->tm_mday!=day || check->tm_hour!=0 || check->tm_min!=0 ||
      check->tm_sec!=0) {
    return 0;
  }

  *epoch=value;
  return 1;
}

static int store_prices(time_t start, const double *prices, int count) {
  MYSQL *con;
  EnergyConfig cfg;
  char cfg_err[256];
  char query[SQL_SIZE];
  size_t used;
  time_t current;
  int i, n;

  if (!energy_config_load(&cfg,ENERGY_CONFIG_FILE,cfg_err,sizeof(cfg_err))) {
    fprintf(stderr,"energy config error: %s\n",cfg_err);
    return 0;
  }

  con=mysql_init(NULL);
  if (con==NULL) {
    fprintf(stderr,"mysql init error\n");
    return 0;
  }
  if (mysql_real_connect(con,cfg.db_host,cfg.db_user,cfg.db_pass,cfg.db_name,cfg.db_port,NULL,0)==NULL) {
    fprintf(stderr,"mysql connect error: %s\n",mysql_error(con));
    mysql_close(con);
    return 0;
  }

  n=snprintf(query,sizeof(query),"INSERT INTO pun_15m (epoch,c) VALUES ");
  if (n<0 || n>=(int)sizeof(query)) {
    mysql_close(con);
    return 0;
  }
  used=(size_t)n;

  for (i=0;i<count;i++) {
    current=start+(time_t)i*900;
    n=snprintf(query+used,sizeof(query)-used,"%s(%ld,%.5f)",
        i==0 ? "" : ",",(long)current,prices[i]);
    if (n<0 || (size_t)n>=sizeof(query)-used) {
      fprintf(stderr,"SQL buffer too small\n");
      mysql_close(con);
      return 0;
    }
    used+=(size_t)n;
  }

  n=snprintf(query+used,sizeof(query)-used,
      " ON DUPLICATE KEY UPDATE c=VALUES(c)");
  if (n<0 || (size_t)n>=sizeof(query)-used) {
    fprintf(stderr,"SQL buffer too small\n");
    mysql_close(con);
    return 0;
  }

  if (mysql_query(con,query)!=0) {
    fprintf(stderr,"mysql query error: %s\n",mysql_error(con));
    mysql_close(con);
    return 0;
  }

  mysql_close(con);
  return 1;
}

int main(int argc, char *argv[]) {
  CURL *curl;
  struct curl_slist *headers, *tmp_headers;
  struct mem body;
  const char *content;
  unsigned char *zip_buf, *file_buf;
  double prices[PRICE_COUNT_MAX];
  char auth_post[AUTH_POST_SIZE], req_post[REQUEST_POST_SIZE];
  char auth_header[AUTH_HEADER_SIZE], token[TOKEN_SIZE];
  size_t content_len, zip_len, file_len;
  time_t start;
  int n, count, status;

  curl=NULL;
  headers=NULL;
  tmp_headers=NULL;
  body.ptr=NULL;
  body.len=0;
  body.cap=0;
  content=NULL;
  zip_buf=NULL;
  file_buf=NULL;
  content_len=0;
  zip_len=0;
  file_len=0;
  start=0;
  count=0;
  status=1;

  if (argc!=4) {
    fprintf(stderr,"Usage: %s LOGIN PASSWORD YYYYMMDD\n",argv[0]);
    return 1;
  }

  if (setenv("TZ","Europe/Rome",1)!=0) {
    fprintf(stderr,"cannot set timezone\n");
    return 1;
  }
  tzset();
  if (!valid_date(argv[3],&start)) {
    fprintf(stderr,"date format must be a valid YYYYMMDD date\n");
    return 1;
  }

  if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) {
    fprintf(stderr,"curl global init error\n");
    return 1;
  }
  if (!mem_init(&body)) {
    fprintf(stderr,"memory allocation error\n");
    goto cleanup;
  }

  curl=curl_easy_init();
  if (curl==NULL) {
    fprintf(stderr,"curl init error\n");
    goto cleanup;
  }

  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);
  curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(curl,CURLOPT_USERAGENT,"gme_mysql/1.13");

  n=snprintf(auth_post,sizeof(auth_post),
      "{\"Login\":\"%s\",\"Password\":\"%s\"}",argv[1],argv[2]);
  if (n<0 || n>=(int)sizeof(auth_post)) {
    fprintf(stderr,"login or password too long\n");
    goto cleanup;
  }

  headers=curl_slist_append(NULL,"Content-Type: application/json");
  if (headers==NULL) {
    fprintf(stderr,"curl header allocation error\n");
    goto cleanup;
  }
  if (!curl_post(curl,AUTH_URL,headers,auth_post,&body)) {
    goto cleanup;
  }
  if (!json_copy_string(body.ptr,"token",token,sizeof(token))) {
    fprintf(stderr,"token not found or too long: %.500s\n",body.ptr);
    goto cleanup;
  }

  n=snprintf(req_post,sizeof(req_post),
      "{\"Platform\":\"PublicMarketResults\","
      "\"Segment\":\"MGP\","
      "\"DataName\":\"ME_ZonalPrices\","
      "\"IntervalStart\":\"%s\","
      "\"IntervalEnd\":\"%s\","
      "\"Attributes\":{\"GranularityType\":\"PT15\"}}",
      argv[3],argv[3]);
  if (n<0 || n>=(int)sizeof(req_post)) {
    fprintf(stderr,"request buffer too small\n");
    goto cleanup;
  }

  n=snprintf(auth_header,sizeof(auth_header),"Authorization: Bearer %s",token);
  if (n<0 || n>=(int)sizeof(auth_header)) {
    fprintf(stderr,"authorization token too long\n");
    goto cleanup;
  }
  tmp_headers=curl_slist_append(headers,auth_header);
  if (tmp_headers==NULL) {
    fprintf(stderr,"curl header allocation error\n");
    goto cleanup;
  }
  headers=tmp_headers;
  tmp_headers=NULL;

  if (!curl_post(curl,DATA_URL,headers,req_post,&body)) {
    goto cleanup;
  }
  if (!json_string_span(body.ptr,"contentResponse",&content,&content_len)) {
    fprintf(stderr,"contentResponse not found: %.500s\n",body.ptr);
    goto cleanup;
  }

  zip_buf=base64_decode(content,content_len,&zip_len);
  if (zip_buf==NULL || zip_len==0) {
    fprintf(stderr,"base64 decode error\n");
    goto cleanup;
  }

  mem_free(&body);
  file_buf=unzip_data(zip_buf,zip_len,&file_len);
  if (file_buf==NULL || file_len==0) {
    goto cleanup;
  }
  free(zip_buf);
  zip_buf=NULL;

  count=parse_prices((char *)file_buf,prices,PRICE_COUNT_MAX);
  if (count!=92 && count!=96 && count!=100) {
    fprintf(stderr,"found %d prices, expected 92, 96 or 100\n",count);
    goto cleanup;
  }

  if (!store_prices(start,prices,count)) {
    goto cleanup;
  }

  printf("COUNT=%d\n",count);
  status=0;

cleanup:
  free(zip_buf);
  free(file_buf);
  mem_free(&body);
  curl_slist_free_all(headers);
  if (curl!=NULL) {
    curl_easy_cleanup(curl);
  }
  curl_global_cleanup();
  return status;
}
