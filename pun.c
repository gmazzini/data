// Gianluca Mazzini @2026- Version 1.22
// Processes PUN 15-minute prices and updates monthly and daily Google Sheets results

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <mysql/mysql.h>
#include "energy_config.h"

#define TOKEN_FILE "/home/www/data/google_access_token"
#define SPREADSHEET_ID "1RF4N-T2NR2UHai70AzTzwuLXowkLlOQWvFyb8AaE1xg"
#define SHEET_NAME_PUN "pun"
#define SHEET_NAME_H "h"
#define BASE_DATE "20251001"
#define WINDOW_SLOTS 12
#define DAY_SLOTS_MAX 100
#define HTTP_BUFFER_START 4096
#define TOKEN_SIZE 4096
#define AUTH_HEADER_SIZE 4352
#define URL_SIZE 768
#define JSON_SIZE 4096
#define QUERY_SIZE 512
#define MIN_EPSILON 0.00001

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

static int read_access_token(char *buf, size_t cap) {
  FILE *fp;

  fp=fopen(TOKEN_FILE,"r");
  if (fp==NULL) {
    fprintf(stderr,"Error: unable to open %s\n",TOKEN_FILE);
    return 0;
  }
  if (fgets(buf,(int)cap,fp)==NULL) {
    fclose(fp);
    fprintf(stderr,"Error: unable to read access token\n");
    return 0;
  }
  fclose(fp);

  buf[strcspn(buf,"\r\n")]='\0';
  if (*buf=='\0') {
    fprintf(stderr,"Error: empty access token\n");
    return 0;
  }
  return 1;
}

static int http_request(CURL *curl, const char *method, const char *url,
    struct curl_slist *headers, const char *payload, struct mem *body) {
  CURLcode rc;
  long status;
  size_t len;

  mem_reset(body);
  curl_easy_reset(curl);
  curl_easy_setopt(curl,CURLOPT_URL,url);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,body);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,60L);
  curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(curl,CURLOPT_USERAGENT,"pun/1.21");

  if (strcmp(method,"GET")==0) {
    curl_easy_setopt(curl,CURLOPT_HTTPGET,1L);
  } else if (strcmp(method,"POST")==0) {
    if (payload==NULL) {
      fprintf(stderr,"Error: missing HTTP payload\n");
      return 0;
    }
    len=strlen(payload);
    if (len>(size_t)LONG_MAX) {
      fprintf(stderr,"Error: HTTP payload too large\n");
      return 0;
    }
    curl_easy_setopt(curl,CURLOPT_POST,1L);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,payload);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)len);
  } else {
    fprintf(stderr,"Error: unsupported HTTP method\n");
    return 0;
  }

  rc=curl_easy_perform(curl);
  if (rc!=CURLE_OK) {
    fprintf(stderr,"Google Sheets curl error: %s\n",curl_easy_strerror(rc));
    return 0;
  }

  status=0;
  if (curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status)!=CURLE_OK) {
    fprintf(stderr,"Error: unable to read HTTP status\n");
    return 0;
  }
  if (status<200 || status>=300) {
    fprintf(stderr,"Google Sheets HTTP %ld: %.1000s\n",status,
        body->ptr!=NULL ? body->ptr : "");
    return 0;
  }
  return 1;
}

static int json_key_string(const char *p, const char *key, const char **value,
    size_t *len) {
  const char *q, *e;
  char pattern[64];
  int n;

  n=snprintf(pattern,sizeof(pattern),"\"%s\"",key);
  if (n<0 || n>=(int)sizeof(pattern)) {
    return 0;
  }
  q=strstr(p,pattern);
  if (q==NULL) {
    return 0;
  }
  q+=strlen(pattern);
  for (;*q!='\0' && isspace((unsigned char)*q);q++) {
  }
  if (*q!=':') {
    return 0;
  }
  q++;
  for (;*q!='\0' && isspace((unsigned char)*q);q++) {
  }
  if (*q!='\"') {
    return 0;
  }
  q++;
  e=strchr(q,'\"');
  if (e==NULL) {
    return 0;
  }

  *value=q;
  *len=(size_t)(e-q);
  return 1;
}

static int find_sheet_id(const char *json, const char *title) {
  const char *p, *value, *scan, *last, *colon, *endptr;
  size_t len;
  long id;

  p=json;
  for (;json_key_string(p,"title",&value,&len);p=value+len+1) {
    if (strlen(title)!=len || strncmp(value,title,len)!=0) {
      continue;
    }

    scan=json;
    last=NULL;
    for (;scan<value;) {
      scan=strstr(scan,"\"sheetId\"");
      if (scan==NULL || scan>=value) {
        break;
      }
      last=scan;
      scan+=9;
    }
    if (last==NULL) {
      return -1;
    }

    colon=strchr(last,':');
    if (colon==NULL || colon>=value) {
      return -1;
    }
    colon++;
    for (;colon<value && isspace((unsigned char)*colon);colon++) {
    }
    errno=0;
    id=strtol(colon,(char **)&endptr,10);
    if (errno!=0 || endptr==colon || id<0 || id>INT_MAX) {
      return -1;
    }
    return (int)id;
  }
  return -1;
}

static int get_sheet_ids(CURL *curl, struct curl_slist *headers,
    struct mem *body, int *pun_id, int *h_id) {
  char url[URL_SIZE];
  int n;

  n=snprintf(url,sizeof(url),
      "https://sheets.googleapis.com/v4/spreadsheets/%s?fields=sheets.properties(sheetId,title)",
      SPREADSHEET_ID);
  if (n<0 || n>=(int)sizeof(url)) {
    return 0;
  }
  if (!http_request(curl,"GET",url,headers,NULL,body)) {
    return 0;
  }

  *pun_id=find_sheet_id(body->ptr,SHEET_NAME_PUN);
  *h_id=find_sheet_id(body->ptr,SHEET_NAME_H);
  if (*pun_id<0 || *h_id<0) {
    fprintf(stderr,"Error: unable to resolve sheet IDs for '%s' and '%s'\n",
        SHEET_NAME_PUN,SHEET_NAME_H);
    return 0;
  }
  return 1;
}

static int sort_google_sheets(CURL *curl, struct curl_slist *headers,
    struct mem *body, int pun_id, int h_id, int ascending) {
  char url[URL_SIZE], json[JSON_SIZE];
  const char *order;
  int n;

  order=ascending ? "ASCENDING" : "DESCENDING";
  n=snprintf(url,sizeof(url),
      "https://sheets.googleapis.com/v4/spreadsheets/%s:batchUpdate",
      SPREADSHEET_ID);
  if (n<0 || n>=(int)sizeof(url)) {
    return 0;
  }

  n=snprintf(json,sizeof(json),
      "{\"requests\":["
      "{\"sortRange\":{\"range\":{\"sheetId\":%d,\"startRowIndex\":1,"
      "\"startColumnIndex\":0,\"endColumnIndex\":10},"
      "\"sortSpecs\":[{\"dimensionIndex\":0,\"sortOrder\":\"%s\"}]}},"
      "{\"sortRange\":{\"range\":{\"sheetId\":%d,\"startRowIndex\":1,"
      "\"startColumnIndex\":0,\"endColumnIndex\":3},"
      "\"sortSpecs\":[{\"dimensionIndex\":0,\"sortOrder\":\"%s\"}]}}]}",
      pun_id,order,h_id,order);
  if (n<0 || n>=(int)sizeof(json)) {
    fprintf(stderr,"Error: sort JSON buffer too small\n");
    return 0;
  }

  return http_request(curl,"POST",url,headers,json,body);
}

static int update_google_sheets(CURL *curl, struct curl_slist *headers,
    struct mem *body, int row_pun, int row_h, const char *aaaamm,
    double f0, double f1, double f2, double f3, double min_val,
    double max_val, const char *bestday, const char *worstday,
    const char *minday, const char *date_str, const char *time_str,
    double avg_3h) {
  char url[URL_SIZE], json[JSON_SIZE];
  int n;

  n=snprintf(url,sizeof(url),
      "https://sheets.googleapis.com/v4/spreadsheets/%s/values:batchUpdate",
      SPREADSHEET_ID);
  if (n<0 || n>=(int)sizeof(url)) {
    return 0;
  }

  n=snprintf(json,sizeof(json),
      "{\"valueInputOption\":\"USER_ENTERED\",\"data\":["
      "{\"range\":\"%s!A%d:J%d\",\"majorDimension\":\"ROWS\","
      "\"values\":[[\"%s\",%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,"
      "\"%s\",\"%s\",\"%s\"]]},"
      "{\"range\":\"%s!A%d:C%d\",\"majorDimension\":\"ROWS\","
      "\"values\":[[\"%s\",\"%s\",%.5f]]}]}",
      SHEET_NAME_PUN,row_pun,row_pun,aaaamm,f0,f1,f2,f3,min_val,max_val,
      bestday,worstday,minday,SHEET_NAME_H,row_h,row_h,date_str,time_str,avg_3h);
  if (n<0 || n>=(int)sizeof(json)) {
    fprintf(stderr,"Error: values JSON buffer too small\n");
    return 0;
  }

  return http_request(curl,"POST",url,headers,json,body);
}

static void get_easter_monday(int year, int *out_m, int *out_d) {
  int a, b, c, d, e, f, g, h, i, k, l, m, month_e, day_e;

  a=year%19;
  b=year/100;
  c=year%100;
  d=b/4;
  e=b%4;
  f=(b+8)/25;
  g=(b-f+1)/3;
  h=(19*a+b-d-g+15)%30;
  i=c/4;
  k=c%4;
  l=(32+2*e+2*i-h-k)%7;
  m=(a+11*h+22*l)/451;
  month_e=(h+l-7*m+114)/31;
  day_e=((h+l-7*m+114)%31)+1;

  if (month_e==3 && day_e==31) {
    *out_m=4;
    *out_d=1;
  } else {
    *out_m=month_e;
    *out_d=day_e+1;
  }
}

static int is_festivo(const struct tm *tm) {
  int m, d, y, em_m, em_d;

  m=tm->tm_mon+1;
  d=tm->tm_mday;
  y=tm->tm_year+1900;
  if ((m==1 && d==1) || (m==1 && d==6) || (m==4 && d==25) ||
      (m==5 && d==1) || (m==6 && d==2) || (m==8 && d==15) ||
      (m==11 && d==1) || (m==12 && d==8) || (m==12 && d==25) ||
      (m==12 && d==26)) {
    return 1;
  }

  get_easter_monday(y,&em_m,&em_d);
  return m==em_m && d==em_d;
}

static int get_band_index(const struct tm *tm) {
  int dow, h;

  dow=tm->tm_wday;
  h=tm->tm_hour;
  if (dow==0 || is_festivo(tm)) {
    return 3;
  }
  if (dow==6) {
    return h>=7 && h<23 ? 2 : 3;
  }
  if (h>=8 && h<19) {
    return 1;
  }
  if ((h>=7 && h<8) || (h>=19 && h<23)) {
    return 2;
  }
  return 3;
}

static int parse_date(const char *text, int *year, int *month, int *day,
    time_t *noon_epoch) {
  struct tm t;
  struct tm *check;
  time_t value;
  int i;

  if (strlen(text)!=8) {
    return 0;
  }
  for (i=0;i<8;i++) {
    if (!isdigit((unsigned char)text[i])) {
      return 0;
    }
  }
  if (sscanf(text,"%4d%2d%2d",year,month,day)!=3) {
    return 0;
  }

  memset(&t,0,sizeof(t));
  t.tm_year=*year-1900;
  t.tm_mon=*month-1;
  t.tm_mday=*day;
  t.tm_hour=12;
  t.tm_isdst=-1;
  value=mktime(&t);
  if (value==(time_t)-1) {
    return 0;
  }

  check=localtime(&value);
  if (check==NULL || check->tm_year!=*year-1900 || check->tm_mon!=*month-1 ||
      check->tm_mday!=*day || check->tm_hour!=12) {
    return 0;
  }

  *noon_epoch=value;
  return 1;
}

static int month_range(int year, int month, time_t *start, time_t *end) {
  struct tm t;
  time_t first, next;

  memset(&t,0,sizeof(t));
  t.tm_year=year-1900;
  t.tm_mon=month-1;
  t.tm_mday=1;
  t.tm_isdst=-1;
  first=mktime(&t);
  if (first==(time_t)-1) {
    return 0;
  }

  t.tm_mon++;
  t.tm_isdst=-1;
  next=mktime(&t);
  if (next==(time_t)-1 || next<=first) {
    return 0;
  }

  *start=first;
  *end=next-1;
  return 1;
}

static int parse_db_epoch(const char *text, time_t *value) {
  char *end;
  long long n;

  errno=0;
  n=strtoll(text,&end,10);
  if (errno!=0 || end==text || *end!='\0') {
    return 0;
  }
  *value=(time_t)n;
  return (long long)*value==n;
}

static int parse_db_double(const char *text, double *value) {
  char *end;

  errno=0;
  *value=strtod(text,&end);
  return errno==0 && end!=text && *end=='\0';
}

static int find_min_window(const double *prices, const time_t *epochs, int count,
    int *best_index, double *best_avg) {
  double sum, min_sum;
  int i, k, valid, found;

  min_sum=DBL_MAX;
  *best_index=-1;
  found=0;

  for (i=0;i<=count-WINDOW_SLOTS;i++) {
    sum=prices[i];
    valid=1;
    for (k=1;k<WINDOW_SLOTS;k++) {
      if (epochs[i+k]!=epochs[i+k-1]+900) {
        valid=0;
        break;
      }
      sum+=prices[i+k];
    }
    if (valid && (!found || sum<min_sum)) {
      min_sum=sum;
      *best_index=i;
      found=1;
    }
  }

  if (!found) {
    return 0;
  }
  *best_avg=min_sum/(double)WINDOW_SLOTS;
  return 1;
}

int main(int argc, char *argv[]) {
  MYSQL *conn;
  EnergyConfig cfg;
  char cfg_err[256];
  MYSQL_RES *result;
  MYSQL_ROW row;
  CURL *curl;
  struct curl_slist *headers, *tmp_headers;
  struct mem body;
  struct tm base_tm, local_tm;
  struct tm *local_ptr;
  time_t sds, sde, target_noon, base_noon, t_val, prev_min_epoch;
  time_t day_epochs[DAY_SLOTS_MAX];
  double sums[4], day_sums[32], day_prices[DAY_SLOTS_MAX];
  double global_min, global_max, f0, f1, f2, f3, c_val;
  double min_day_avg, max_day_avg, avg, min_window_avg;
  long counts[4], day_counts[32];
  int max_min_run[32];
  int year, month, day, delta_months, days_diff;
  int row_pun, row_h, day_count, best_day, worst_day, min_day;
  int max_run, run_len, run_day, band, d, best_window_idx;
  int sheet_id_pun, sheet_id_h, sorted_ascending, status;
  char query[QUERY_SIZE], token[TOKEN_SIZE], auth_header[AUTH_HEADER_SIZE];
  char aaaamm[8], bestday[16], worstday[16], minday[16];
  char date_str[16], time_str[16];
  int n;

  conn=NULL;
  result=NULL;
  row=NULL;
  curl=NULL;
  headers=NULL;
  tmp_headers=NULL;
  body.ptr=NULL;
  body.len=0;
  body.cap=0;
  memset(&base_tm,0,sizeof(base_tm));
  memset(&local_tm,0,sizeof(local_tm));
  sds=0;
  sde=0;
  target_noon=0;
  base_noon=0;
  prev_min_epoch=0;
  memset(day_epochs,0,sizeof(day_epochs));
  memset(sums,0,sizeof(sums));
  memset(day_sums,0,sizeof(day_sums));
  memset(day_prices,0,sizeof(day_prices));
  memset(counts,0,sizeof(counts));
  memset(day_counts,0,sizeof(day_counts));
  memset(max_min_run,0,sizeof(max_min_run));
  global_min=DBL_MAX;
  global_max=-DBL_MAX;
  f0=0.0;
  f1=0.0;
  f2=0.0;
  f3=0.0;
  min_day_avg=DBL_MAX;
  max_day_avg=-DBL_MAX;
  min_window_avg=0.0;
  year=0;
  month=0;
  day=0;
  delta_months=0;
  days_diff=0;
  row_pun=0;
  row_h=0;
  day_count=0;
  best_day=0;
  worst_day=0;
  min_day=0;
  max_run=-1;
  run_len=0;
  run_day=0;
  best_window_idx=-1;
  sheet_id_pun=-1;
  sheet_id_h=-1;
  sorted_ascending=0;
  status=1;

  if (argc!=2) {
    fprintf(stderr,"Usage: %s YYYYMMDD\n",argv[0]);
    fprintf(stderr,"Example: %s 20260810\n",argv[0]);
    return 1;
  }

  if (setenv("TZ","Europe/Rome",1)!=0) {
    fprintf(stderr,"Error: unable to set Europe/Rome timezone\n");
    return 1;
  }
  tzset();

  if (!parse_date(argv[1],&year,&month,&day,&target_noon)) {
    fprintf(stderr,"Error: parameter must be a valid date in YYYYMMDD format\n");
    return 1;
  }
  if (strcmp(argv[1],BASE_DATE)<0) {
    fprintf(stderr,"Error: date %s is before base date %s\n",argv[1],BASE_DATE);
    return 1;
  }

  n=snprintf(aaaamm,sizeof(aaaamm),"%04d%02d",year,month);
  if (n<0 || n>=(int)sizeof(aaaamm)) {
    return 1;
  }
  n=snprintf(date_str,sizeof(date_str),"%04d-%02d-%02d",year,month,day);
  if (n<0 || n>=(int)sizeof(date_str)) {
    return 1;
  }

  delta_months=(year-2025)*12+(month-10);
  row_pun=2+delta_months;

  base_tm.tm_year=2025-1900;
  base_tm.tm_mon=10-1;
  base_tm.tm_mday=1;
  base_tm.tm_hour=12;
  base_tm.tm_isdst=-1;
  base_noon=mktime(&base_tm);
  if (base_noon==(time_t)-1) {
    fprintf(stderr,"Error: unable to calculate base date\n");
    return 1;
  }
  days_diff=(int)(difftime(target_noon,base_noon)/86400.0+0.5);
  row_h=2+days_diff;

  if (!month_range(year,month,&sds,&sde)) {
    fprintf(stderr,"Error: unable to calculate month range\n");
    return 1;
  }

  if (!energy_config_load(&cfg,ENERGY_CONFIG_FILE,cfg_err,sizeof(cfg_err))) {
    fprintf(stderr,"Energy config error: %s\n",cfg_err);
    return 1;
  }

  conn=mysql_init(NULL);
  if (conn==NULL) {
    fprintf(stderr,"Error: mysql_init failed\n");
    return 1;
  }
  if (mysql_real_connect(conn,cfg.db_host,cfg.db_user,cfg.db_pass,cfg.db_name,cfg.db_port,NULL,0)==NULL) {
    fprintf(stderr,"MySQL connection error: %s\n",mysql_error(conn));
    mysql_close(conn);
    return 1;
  }

  n=snprintf(query,sizeof(query),
      "SELECT epoch,c FROM pun_15m WHERE epoch BETWEEN %ld AND %ld ORDER BY epoch",
      (long)sds,(long)sde);
  if (n<0 || n>=(int)sizeof(query) || mysql_query(conn,query)!=0) {
    fprintf(stderr,"MySQL query error: %s\n",mysql_error(conn));
    mysql_close(conn);
    return 1;
  }

  result=mysql_use_result(conn);
  if (result==NULL) {
    fprintf(stderr,"MySQL result error: %s\n",mysql_error(conn));
    mysql_close(conn);
    return 1;
  }

  for (row=mysql_fetch_row(result);row!=NULL;row=mysql_fetch_row(result)) {
    if (row[0]==NULL || row[1]==NULL) {
      continue;
    }
    if (!parse_db_epoch(row[0],&t_val) || !parse_db_double(row[1],&c_val)) {
      fprintf(stderr,"Error: invalid database value\n");
      mysql_free_result(result);
      mysql_close(conn);
      return 1;
    }

    local_ptr=localtime(&t_val);
    if (local_ptr==NULL) {
      fprintf(stderr,"Error: localtime conversion failed\n");
      mysql_free_result(result);
      mysql_close(conn);
      return 1;
    }
    local_tm=*local_ptr;
    d=local_tm.tm_mday;
    if (d<1 || d>31) {
      continue;
    }

    band=get_band_index(&local_tm);
    sums[0]+=c_val;
    counts[0]++;
    sums[band]+=c_val;
    counts[band]++;
    day_sums[d]+=c_val;
    day_counts[d]++;

    if (c_val>global_max) {
      global_max=c_val;
    }
    if (c_val<global_min-MIN_EPSILON) {
      global_min=c_val;
      memset(max_min_run,0,sizeof(max_min_run));
      run_day=d;
      run_len=1;
      prev_min_epoch=t_val;
      max_min_run[d]=1;
    } else if (fabs(c_val-global_min)<MIN_EPSILON) {
      if (run_day==d && t_val==prev_min_epoch+900) {
        run_len++;
      } else {
        run_day=d;
        run_len=1;
      }
      prev_min_epoch=t_val;
      if (run_len>max_min_run[d]) {
        max_min_run[d]=run_len;
      }
    } else {
      run_day=0;
      run_len=0;
      prev_min_epoch=0;
    }

    if (d==day) {
      if (day_count>=DAY_SLOTS_MAX) {
        fprintf(stderr,"Error: more than %d records found for target day\n",DAY_SLOTS_MAX);
        mysql_free_result(result);
        mysql_close(conn);
        return 1;
      }
      day_prices[day_count]=c_val;
      day_epochs[day_count]=t_val;
      day_count++;
    }
  }

  if (mysql_errno(conn)!=0) {
    fprintf(stderr,"MySQL fetch error: %s\n",mysql_error(conn));
    mysql_free_result(result);
    mysql_close(conn);
    return 1;
  }
  mysql_free_result(result);
  mysql_close(conn);
  conn=NULL;

  if (counts[0]==0) {
    fprintf(stderr,"Error: no data found for month %s\n",aaaamm);
    return 1;
  }
  if (day_count<WINDOW_SLOTS ||
      !find_min_window(day_prices,day_epochs,day_count,&best_window_idx,&min_window_avg)) {
    fprintf(stderr,"Error: target day %s has no complete 3-hour window\n",date_str);
    return 1;
  }

  f0=sums[0]/(double)counts[0];
  f1=counts[1]>0 ? sums[1]/(double)counts[1] : 0.0;
  f2=counts[2]>0 ? sums[2]/(double)counts[2] : 0.0;
  f3=counts[3]>0 ? sums[3]/(double)counts[3] : 0.0;

  for (d=1;d<=31;d++) {
    if (day_counts[d]==0) {
      continue;
    }
    avg=day_sums[d]/(double)day_counts[d];
    if (best_day==0 || avg<min_day_avg) {
      min_day_avg=avg;
      best_day=d;
    }
    if (worst_day==0 || avg>max_day_avg) {
      max_day_avg=avg;
      worst_day=d;
    }
    if (max_min_run[d]>max_run) {
      max_run=max_min_run[d];
      min_day=d;
    }
  }

  n=snprintf(bestday,sizeof(bestday),"%04d-%02d-%02d",year,month,best_day);
  n|=snprintf(worstday,sizeof(worstday),"%04d-%02d-%02d",year,month,worst_day);
  n|=snprintf(minday,sizeof(minday),"%04d-%02d-%02d",year,month,min_day);
  if (n<0) {
    return 1;
  }

  local_ptr=localtime(&day_epochs[best_window_idx]);
  if (local_ptr==NULL) {
    fprintf(stderr,"Error: localtime conversion failed\n");
    return 1;
  }
  n=snprintf(time_str,sizeof(time_str),"%02d:%02d",local_ptr->tm_hour,local_ptr->tm_min);
  if (n<0 || n>=(int)sizeof(time_str)) {
    return 1;
  }

  if (!read_access_token(token,sizeof(token))) {
    return 1;
  }
  if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) {
    fprintf(stderr,"Error: curl_global_init failed\n");
    return 1;
  }
  if (!mem_init(&body)) {
    fprintf(stderr,"Error: memory allocation failed\n");
    curl_global_cleanup();
    return 1;
  }

  curl=curl_easy_init();
  if (curl==NULL) {
    fprintf(stderr,"Error: curl_easy_init failed\n");
    goto cleanup;
  }

  n=snprintf(auth_header,sizeof(auth_header),"Authorization: Bearer %s",token);
  if (n<0 || n>=(int)sizeof(auth_header)) {
    fprintf(stderr,"Error: access token too long\n");
    goto cleanup;
  }
  headers=curl_slist_append(NULL,"Content-Type: application/json");
  if (headers==NULL) {
    fprintf(stderr,"Error: curl header allocation failed\n");
    goto cleanup;
  }
  tmp_headers=curl_slist_append(headers,auth_header);
  if (tmp_headers==NULL) {
    fprintf(stderr,"Error: curl header allocation failed\n");
    goto cleanup;
  }
  headers=tmp_headers;
  tmp_headers=NULL;

  if (!get_sheet_ids(curl,headers,&body,&sheet_id_pun,&sheet_id_h)) {
    goto cleanup;
  }
  if (!sort_google_sheets(curl,headers,&body,sheet_id_pun,sheet_id_h,1)) {
    fprintf(stderr,"Error: failed to sort sheets ascending\n");
    goto cleanup;
  }
  sorted_ascending=1;

  if (!update_google_sheets(curl,headers,&body,row_pun,row_h,aaaamm,
      f0,f1,f2,f3,global_min,global_max,bestday,worstday,minday,
      date_str,time_str,min_window_avg)) {
    fprintf(stderr,"Error: failed to update Google Sheets\n");
    goto cleanup;
  }

  if (!sort_google_sheets(curl,headers,&body,sheet_id_pun,sheet_id_h,0)) {
    fprintf(stderr,"Error: failed to sort sheets descending\n");
    goto cleanup;
  }
  sorted_ascending=0;

  printf("OK: Updated Sheet '%s' row %d for month %s\n",SHEET_NAME_PUN,row_pun,aaaamm);
  printf("F0: %.5f | F1: %.5f | F2: %.5f | F3: %.5f\n",f0,f1,f2,f3);
  printf("MIN: %.5f | MAX: %.5f | BESTDAY: %s | WORSTDAY: %s | MINDAY: %s\n",
      global_min,global_max,bestday,worstday,minday);
  printf("OK: Updated Sheet '%s' row %d for date %s -> Start: %s | 3h Avg: %.5f EUR/MWh\n",
      SHEET_NAME_H,row_h,date_str,time_str,min_window_avg);
  status=0;

cleanup:
  if (sorted_ascending && curl!=NULL && sheet_id_pun>=0 && sheet_id_h>=0) {
    if (!sort_google_sheets(curl,headers,&body,sheet_id_pun,sheet_id_h,0)) {
      fprintf(stderr,"Warning: unable to restore descending sheet order\n");
    }
  }
  curl_slist_free_all(headers);
  if (curl!=NULL) {
    curl_easy_cleanup(curl);
  }
  mem_free(&body);
  curl_global_cleanup();
  return status;
}
