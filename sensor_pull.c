// Gianluca Mazzini @2026- Version 1.00
// Pulls Dragino LHT65 payloads from RetePAIoT and stores temperature/humidity in MySQL

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <mysql/mysql.h>

#define CONFIG_FILE "/home/www/sensori/data.conf"
#define RETEPAIOT_URL "https://api.retepaiot.it/payloaddata/"
#define MAX_NAME 128
#define MAX_SECRET 512
#define MAX_LINE 2048
#define MAX_DEVICE 256
#define HTTP_BUFFER_START 4096
#define JSON_BODY_SIZE 2048
#define URL_SIZE 2048
#define SQL_SIZE 65536

typedef struct {
  char db_host[MAX_NAME];
  char db_user[MAX_NAME];
  char db_pass[MAX_SECRET];
  char db_name[MAX_NAME];
  char auth_key[MAX_SECRET];
  unsigned int db_port;
} Config;

struct mem {
  char *ptr;
  size_t len;
  size_t cap;
};

static char *trim(char *s) {
  char *e;

  for (;*s!='\0' && isspace((unsigned char)*s);s++) {
  }
  e=s+strlen(s);
  for (;e>s && isspace((unsigned char)e[-1]);e--) {
  }
  *e='\0';
  return s;
}

static void copy_text(char *dst, size_t cap, const char *src) {
  size_t n;

  if (cap==0) {
    return;
  }
  n=strlen(src);
  if (n>=cap) {
    n=cap-1;
  }
  memcpy(dst,src,n);
  dst[n]='\0';
}

static int parse_uint(const char *s, unsigned long *value) {
  char *end;
  unsigned long n;

  errno=0;
  n=strtoul(s,&end,10);
  if (errno!=0 || end==s || *end!='\0') {
    return 0;
  }
  *value=n;
  return 1;
}

static int load_config(Config *cfg) {
  FILE *fp;
  char line[MAX_LINE];
  char *key, *value, *eq;
  unsigned long n;
  int lineno;

  memset(cfg,0,sizeof(*cfg));
  copy_text(cfg->db_host,sizeof(cfg->db_host),"127.0.0.1");
  cfg->db_port=3306;

  fp=fopen(CONFIG_FILE,"r");
  if (fp==NULL) {
    fprintf(stderr,"sensor_pull: cannot open %s\n",CONFIG_FILE);
    return 0;
  }

  lineno=0;
  while (fgets(line,sizeof(line),fp)!=NULL) {
    lineno++;
    key=trim(line);
    if (*key=='\0' || *key=='#') {
      continue;
    }
    eq=strchr(key,'=');
    if (eq==NULL) {
      fprintf(stderr,"sensor_pull: invalid config line %d\n",lineno);
      fclose(fp);
      return 0;
    }
    *eq='\0';
    value=trim(eq+1);
    key=trim(key);

    if (strcmp(key,"db_host")==0) {
      copy_text(cfg->db_host,sizeof(cfg->db_host),value);
    } else if (strcmp(key,"db_user")==0) {
      copy_text(cfg->db_user,sizeof(cfg->db_user),value);
    } else if (strcmp(key,"db_pass")==0) {
      copy_text(cfg->db_pass,sizeof(cfg->db_pass),value);
    } else if (strcmp(key,"db_name")==0) {
      copy_text(cfg->db_name,sizeof(cfg->db_name),value);
    } else if (strcmp(key,"db_port")==0) {
      if (!parse_uint(value,&n) || n>65535) {
        fprintf(stderr,"sensor_pull: invalid db_port at line %d\n",lineno);
        fclose(fp);
        return 0;
      }
      cfg->db_port=(unsigned int)n;
    } else if (strcmp(key,"auth_key")==0) {
      copy_text(cfg->auth_key,sizeof(cfg->auth_key),value);
    }
  }
  fclose(fp);

  if (*cfg->db_user=='\0' || *cfg->db_name=='\0' || *cfg->auth_key=='\0') {
    fprintf(stderr,"sensor_pull: db_user, db_name and auth_key are required in %s\n",
        CONFIG_FILE);
    return 0;
  }
  return 1;
}

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

static int append_json_string(char *dst, size_t cap, size_t *used,
    const char *src) {
  unsigned char c;
  int n;

  for (;*src!='\0';src++) {
    c=(unsigned char)*src;
    if (c=='\"' || c=='\\') {
      if (*used+2>=cap) {
        return 0;
      }
      dst[(*used)++]='\\';
      dst[(*used)++]=(char)c;
    } else if (c<32) {
      n=snprintf(dst+*used,cap-*used,"\\u%04x",(unsigned int)c);
      if (n<0 || (size_t)n>=cap-*used) {
        return 0;
      }
      *used+=(size_t)n;
    } else {
      if (*used+1>=cap) {
        return 0;
      }
      dst[(*used)++]=(char)c;
    }
  }
  dst[*used]='\0';
  return 1;
}

static int build_request_body(char *body, size_t cap, const char *auth_key,
    const char *from, const char *to) {
  size_t used;
  int n;

  n=snprintf(body,cap,"{\"auth_key\":\"");
  if (n<0 || (size_t)n>=cap) {
    return 0;
  }
  used=(size_t)n;
  if (!append_json_string(body,cap,&used,auth_key)) {
    return 0;
  }
  n=snprintf(body+used,cap-used,"\",\"from\":\"%s\",\"to\":\"%s\"}",
      from,to);
  return n>=0 && (size_t)n<cap-used;
}

static int fetch_payloads(const Config *cfg, const char *device,
    unsigned long seconds, struct mem *body) {
  CURL *curl;
  struct curl_slist *headers;
  CURLcode rc;
  char *escaped;
  char url[URL_SIZE], request[JSON_BODY_SIZE], from[32], to[32];
  struct tm tm_value;
  time_t now, from_epoch;
  long http_code;
  int n, ok;

  curl=NULL;
  headers=NULL;
  escaped=NULL;
  ok=0;
  now=time(NULL);
  if (now==(time_t)-1) {
    fprintf(stderr,"sensor_pull: cannot read current time\n");
    return 0;
  }
  from_epoch=seconds>(unsigned long)now ? 0 : now-(time_t)seconds;

  if (gmtime_r(&from_epoch,&tm_value)==NULL ||
      strftime(from,sizeof(from),"%Y-%m-%d %H:%M:%S",&tm_value)==0 ||
      gmtime_r(&now,&tm_value)==NULL ||
      strftime(to,sizeof(to),"%Y-%m-%d %H:%M:%S",&tm_value)==0) {
    fprintf(stderr,"sensor_pull: UTC time conversion failed\n");
    return 0;
  }
  if (!build_request_body(request,sizeof(request),cfg->auth_key,from,to)) {
    fprintf(stderr,"sensor_pull: request JSON too large\n");
    return 0;
  }

  curl=curl_easy_init();
  if (curl==NULL) {
    fprintf(stderr,"sensor_pull: curl_easy_init failed\n");
    return 0;
  }
  escaped=curl_easy_escape(curl,device,0);
  if (escaped==NULL) {
    goto cleanup;
  }
  n=snprintf(url,sizeof(url),"%s%s",RETEPAIOT_URL,escaped);
  curl_free(escaped);
  escaped=NULL;
  if (n<0 || n>=(int)sizeof(url)) {
    fprintf(stderr,"sensor_pull: URL too long\n");
    goto cleanup;
  }

  headers=curl_slist_append(NULL,"Content-Type: application/json");
  if (headers==NULL) {
    fprintf(stderr,"sensor_pull: curl header allocation failed\n");
    goto cleanup;
  }

  curl_easy_setopt(curl,CURLOPT_URL,url);
  curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,"GET");
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,request);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(request));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,body);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,5L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,10L);
  curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(curl,CURLOPT_USERAGENT,"sensor_pull/1.00");

  rc=curl_easy_perform(curl);
  if (rc!=CURLE_OK) {
    fprintf(stderr,"sensor_pull: RetePAIoT curl error: %s\n",curl_easy_strerror(rc));
    goto cleanup;
  }
  http_code=0;
  if (curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http_code)!=CURLE_OK ||
      http_code<200 || http_code>=300) {
    fprintf(stderr,"sensor_pull: RetePAIoT HTTP %ld\n",http_code);
    goto cleanup;
  }
  if (body->len==0) {
    fprintf(stderr,"sensor_pull: empty RetePAIoT response\n");
    goto cleanup;
  }
  ok=1;

cleanup:
  if (escaped!=NULL) {
    curl_free(escaped);
  }
  curl_slist_free_all(headers);
  if (curl!=NULL) {
    curl_easy_cleanup(curl);
  }
  return ok;
}

static const char *matching(const char *open, char left, char right) {
  const char *p;
  int depth, quoted, escaped;

  depth=0;
  quoted=0;
  escaped=0;
  for (p=open;*p!='\0';p++) {
    if (quoted) {
      if (escaped) {
        escaped=0;
      } else if (*p=='\\') {
        escaped=1;
      } else if (*p=='\"') {
        quoted=0;
      }
      continue;
    }
    if (*p=='\"') {
      quoted=1;
    } else if (*p==left) {
      depth++;
    } else if (*p==right) {
      depth--;
      if (depth==0) {
        return p;
      }
    }
  }
  return NULL;
}

static int json_string(const char *start, const char *end, const char *key,
    char *out, size_t cap) {
  char pattern[64];
  const char *p, *q;
  size_t used;
  int n;

  n=snprintf(pattern,sizeof(pattern),"\"%s\"",key);
  if (n<0 || n>=(int)sizeof(pattern)) {
    return 0;
  }
  p=start;
  for (;;) {
    p=strstr(p,pattern);
    if (p==NULL || p>=end) {
      return 0;
    }
    p+=strlen(pattern);
    for (;p<end && isspace((unsigned char)*p);p++) {
    }
    if (p<end && *p==':') {
      break;
    }
  }
  p++;
  for (;p<end && isspace((unsigned char)*p);p++) {
  }
  if (p>=end || *p!='\"') {
    return 0;
  }
  p++;

  used=0;
  for (q=p;q<end && *q!='\"';q++) {
    if (*q=='\\') {
      q++;
      if (q>=end || (*q!='\"' && *q!='\\' && *q!='/')) {
        return 0;
      }
    }
    if (used+1>=cap) {
      return 0;
    }
    out[used++]=*q;
  }
  if (q>=end || *q!='\"') {
    return 0;
  }
  out[used]='\0';
  return 1;
}

static int hex_value(int c) {
  if (c>='0' && c<='9') {
    return c-'0';
  }
  if (c>='a' && c<='f') {
    return c-'a'+10;
  }
  if (c>='A' && c<='F') {
    return c-'A'+10;
  }
  return -1;
}

static int decode_lht65(const char *hex, double *temperature, double *humidity) {
  unsigned char b[6];
  int high, nibble, count, raw_temp, raw_hum;

  count=0;
  high=-1;
  for (;*hex!='\0' && count<6;hex++) {
    nibble=hex_value((unsigned char)*hex);
    if (nibble<0) {
      continue;
    }
    if (high<0) {
      high=nibble;
    } else {
      b[count++]=(unsigned char)((high<<4)|nibble);
      high=-1;
    }
  }
  if (count<6) {
    return 0;
  }

  raw_temp=((int)b[2]<<8)|b[3];
  if (raw_temp&0x8000) {
    raw_temp-=0x10000;
  }
  raw_hum=(((int)b[4]<<8)|b[5])&0x0fff;
  *temperature=(double)raw_temp/100.0;
  *humidity=(double)raw_hum/10.0;
  return 1;
}

static int utc_timestamp(const char *text, time_t *epoch) {
  struct tm tm_value, check;
  time_t value;
  char sep;
  int year, month, day, hour, minute, second;

  if (sscanf(text,"%4d-%2d-%2d%c%2d:%2d:%2d",
      &year,&month,&day,&sep,&hour,&minute,&second)!=7 ||
      (sep!=' ' && sep!='T')) {
    return 0;
  }
  memset(&tm_value,0,sizeof(tm_value));
  tm_value.tm_year=year-1900;
  tm_value.tm_mon=month-1;
  tm_value.tm_mday=day;
  tm_value.tm_hour=hour;
  tm_value.tm_min=minute;
  tm_value.tm_sec=second;

  value=timegm(&tm_value);
  if (value==(time_t)-1 || gmtime_r(&value,&check)==NULL ||
      check.tm_year!=year-1900 || check.tm_mon!=month-1 ||
      check.tm_mday!=day || check.tm_hour!=hour || check.tm_min!=minute ||
      check.tm_sec!=second) {
    return 0;
  }
  *epoch=value;
  return 1;
}

static int sql_start(char *query, size_t cap, size_t *used) {
  int n;

  n=snprintf(query,cap,
      "INSERT INTO ambient (epoch,device,temperature,humidity) VALUES ");
  if (n<0 || (size_t)n>=cap) {
    return 0;
  }
  *used=(size_t)n;
  return 1;
}

static int sql_flush(MYSQL *con, char *query, size_t cap, size_t *used,
    int *tuples) {
  const char *tail;
  size_t len;

  if (*tuples==0) {
    return 1;
  }
  tail=" ON DUPLICATE KEY UPDATE temperature=VALUES(temperature),humidity=VALUES(humidity)";
  len=strlen(tail);
  if (*used+len+1>cap) {
    return 0;
  }
  memcpy(query+*used,tail,len+1);
  if (mysql_query(con,query)!=0) {
    fprintf(stderr,"sensor_pull: MySQL insert error: %s\n",mysql_error(con));
    return 0;
  }
  *tuples=0;
  return sql_start(query,cap,used);
}

static int sql_add(MYSQL *con, char *query, size_t cap, size_t *used,
    int *tuples, time_t epoch, const char *device, double temp, double hum) {
  char tuple[1024];
  const char *tail;
  size_t len, reserve;
  int n;

  tail=" ON DUPLICATE KEY UPDATE temperature=VALUES(temperature),humidity=VALUES(humidity)";
  n=snprintf(tuple,sizeof(tuple),"%s(%ld,'%s',%.4f,%.4f)",
      *tuples==0 ? "" : ",",(long)epoch,device,temp,hum);
  if (n<0 || n>=(int)sizeof(tuple)) {
    return 0;
  }
  len=(size_t)n;
  reserve=strlen(tail)+1;

  if (*used+len+reserve>cap) {
    if (!sql_flush(con,query,cap,used,tuples)) {
      return 0;
    }
    n=snprintf(tuple,sizeof(tuple),"(%ld,'%s',%.4f,%.4f)",
        (long)epoch,device,temp,hum);
    if (n<0 || n>=(int)sizeof(tuple)) {
      return 0;
    }
    len=(size_t)n;
  }

  memcpy(query+*used,tuple,len);
  *used+=len;
  query[*used]='\0';
  (*tuples)++;
  return 1;
}

static int store_payloads(const Config *cfg, const char *device,
    const char *json, int *inserted) {
  MYSQL *con;
  const char *data_key, *array, *array_end, *p, *obj, *obj_end;
  char timestamp[128], payload[512], escaped_device[MAX_DEVICE*2+1];
  char query[SQL_SIZE];
  unsigned long escaped_len;
  size_t used;
  time_t epoch;
  double temperature, humidity;
  int tuples, rows, ok;

  con=NULL;
  tuples=0;
  rows=0;
  ok=0;

  data_key=strstr(json,"\"data\"");
  if (data_key==NULL) {
    if (strstr(json,"Nessun payload disponibile")!=NULL) {
      *inserted=0;
      return 1;
    }
    fprintf(stderr,"sensor_pull: response has no data field\n");
    return 0;
  }
  array=strchr(data_key,'[');
  if (array==NULL) {
    fprintf(stderr,"sensor_pull: invalid data array\n");
    return 0;
  }
  array_end=matching(array,'[',']');
  if (array_end==NULL) {
    fprintf(stderr,"sensor_pull: unterminated data array\n");
    return 0;
  }

  con=mysql_init(NULL);
  if (con==NULL) {
    fprintf(stderr,"sensor_pull: mysql_init failed\n");
    return 0;
  }
  if (mysql_real_connect(con,cfg->db_host,cfg->db_user,cfg->db_pass,cfg->db_name,
      cfg->db_port,NULL,0)==NULL) {
    fprintf(stderr,"sensor_pull: MySQL connect error: %s\n",mysql_error(con));
    goto cleanup;
  }

  if (strlen(device)>=(sizeof(escaped_device)-1)/2) {
    fprintf(stderr,"sensor_pull: device identifier too long\n");
    goto cleanup;
  }
  escaped_len=mysql_real_escape_string(con,escaped_device,device,
      (unsigned long)strlen(device));
  escaped_device[escaped_len]='\0';

  if (mysql_autocommit(con,0)!=0 || !sql_start(query,sizeof(query),&used)) {
    fprintf(stderr,"sensor_pull: cannot start MySQL transaction\n");
    goto cleanup;
  }

  p=array+1;
  for (;;) {
    obj=strchr(p,'{');
    if (obj==NULL || obj>=array_end) {
      break;
    }
    obj_end=matching(obj,'{','}');
    if (obj_end==NULL || obj_end>array_end) {
      fprintf(stderr,"sensor_pull: malformed data object\n");
      mysql_rollback(con);
      mysql_autocommit(con,1);
      goto cleanup;
    }

    if (json_string(obj,obj_end,"timestamp",timestamp,sizeof(timestamp)) &&
        json_string(obj,obj_end,"valore",payload,sizeof(payload)) &&
        decode_lht65(payload,&temperature,&humidity) &&
        utc_timestamp(timestamp,&epoch)) {
      if (!sql_add(con,query,sizeof(query),&used,&tuples,epoch,escaped_device,
          temperature,humidity)) {
        mysql_rollback(con);
        mysql_autocommit(con,1);
        goto cleanup;
      }
      rows++;
    }
    p=obj_end+1;
  }

  if (!sql_flush(con,query,sizeof(query),&used,&tuples)) {
    mysql_rollback(con);
    mysql_autocommit(con,1);
    goto cleanup;
  }
  if (mysql_commit(con)!=0) {
    fprintf(stderr,"sensor_pull: MySQL commit error: %s\n",mysql_error(con));
    mysql_rollback(con);
    mysql_autocommit(con,1);
    goto cleanup;
  }
  if (mysql_autocommit(con,1)!=0) {
    fprintf(stderr,"sensor_pull: cannot restore MySQL autocommit\n");
    goto cleanup;
  }

  *inserted=rows;
  ok=1;

cleanup:
  if (con!=NULL) {
    mysql_close(con);
  }
  return ok;
}

int main(int argc, char *argv[]) {
  Config cfg;
  struct mem body;
  char *end;
  unsigned long seconds;
  int inserted, status;

  body.ptr=NULL;
  body.len=0;
  body.cap=0;
  inserted=0;
  status=1;

  if (argc!=3) {
    fprintf(stderr,"Usage: %s DEVICE_EUI SECONDS\n",argv[0]);
    return 1;
  }
  if (*argv[1]=='\0' || strlen(argv[1])>=MAX_DEVICE) {
    fprintf(stderr,"sensor_pull: invalid device EUI\n");
    return 1;
  }
  errno=0;
  seconds=strtoul(argv[2],&end,10);
  if (errno!=0 || end==argv[2] || *end!='\0' || seconds==0) {
    fprintf(stderr,"sensor_pull: invalid lookback seconds\n");
    return 1;
  }

  if (!load_config(&cfg)) {
    return 1;
  }
  if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) {
    fprintf(stderr,"sensor_pull: curl_global_init failed\n");
    return 1;
  }
  if (!mem_init(&body)) {
    fprintf(stderr,"sensor_pull: memory allocation failed\n");
    curl_global_cleanup();
    return 1;
  }

  if (fetch_payloads(&cfg,argv[1],seconds,&body) &&
      store_payloads(&cfg,argv[1],body.ptr,&inserted)) {
    printf("INSERTED=%d\n",inserted);
    status=0;
  }

  mem_free(&body);
  curl_global_cleanup();
  return status;
}
