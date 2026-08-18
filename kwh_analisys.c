// Gianluca Mazzini @2026- Version 1.27
// Analyzes annual kWh data, PUN costs and ARERA bands, then updates Google Sheets

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <mysql/mysql.h>
#include "energy_config.h"

#define TOKEN_FILE "/home/www/data/google_access_token"
#define SPREADSHEET_ID "1fw-Nq7RPMs9JF4bb62LGrXuqg81v1bPJjupOmGsCCqg"
#define HTTP_BUFFER_START 4096
#define TOKEN_SIZE 4096
#define AUTH_HEADER_SIZE 4352
#define URL_SIZE 768
#define JSON_SIZE 65536
#define QUERY_SIZE 1024
#define MONTHS 12
#define DAYS 32
#define HOURS 24
#define BANDS 4
#define BOUNDARIES 13
#define BOUNDARY_WINDOW 3600

struct mem {
  char *ptr;
  size_t len;
  size_t cap;
};

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
  int wday, hour;

  wday=tm->tm_wday;
  hour=tm->tm_hour;
  if (wday==0 || is_festivo(tm)) {
    return 3;
  }
  if (wday==6) {
    return hour>=7 && hour<23 ? 2 : 3;
  }
  if (hour>=8 && hour<19) {
    return 1;
  }
  if ((hour>=7 && hour<8) || (hour>=19 && hour<23)) {
    return 2;
  }
  return 3;
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

static int read_access_token(char *token, size_t cap) {
  FILE *fp;

  fp=fopen(TOKEN_FILE,"r");
  if (fp==NULL) {
    fprintf(stderr,"Error: unable to open token file %s\n",TOKEN_FILE);
    return 0;
  }
  if (fgets(token,(int)cap,fp)==NULL) {
    fclose(fp);
    fprintf(stderr,"Error: unable to read access token\n");
    return 0;
  }
  fclose(fp);

  token[strcspn(token,"\r\n")]='\0';
  if (*token=='\0') {
    fprintf(stderr,"Error: empty access token\n");
    return 0;
  }
  return 1;
}

static int appendf(char *buf, size_t cap, size_t *used, const char *fmt, ...) {
  va_list ap;
  int n;

  if (*used>=cap) {
    return 0;
  }
  va_start(ap,fmt);
  n=vsnprintf(buf+*used,cap-*used,fmt,ap);
  va_end(ap);
  if (n<0 || (size_t)n>=cap-*used) {
    return 0;
  }
  *used+=(size_t)n;
  return 1;
}

static int update_google_sheets(const char *token, const char *hourly_tab,
    const char *hourly_range, const char *monthly_tab, const char *monthly_range,
    const char *compare_tab, const char *compare_range, int include_compare,
    const double hourly_sum[HOURS], double hourly_total,
    const double monthly_sum[MONTHS][BANDS],
    const double annual_totals[BANDS], const double energy_monthly[MONTHS],
    double energy_annual, const double monthly_cp[MONTHS],
    const double monthly_cf[MONTHS], const int monthly_sup[MONTHS],
    double total_cp, double total_cf, int total_sup) {
  CURL *curl;
  struct curl_slist *headers, *tmp_headers;
  struct mem body;
  CURLcode rc;
  char auth_header[AUTH_HEADER_SIZE], url[URL_SIZE];
  char *json;
  size_t used;
  long http_code;
  int h, m, n, ok;

  curl=NULL;
  headers=NULL;
  tmp_headers=NULL;
  body.ptr=NULL;
  body.len=0;
  body.cap=0;
  json=NULL;
  used=0;
  ok=0;

  if (!mem_init(&body)) {
    fprintf(stderr,"Error: HTTP memory allocation failed\n");
    return 0;
  }
  json=(char *)malloc(JSON_SIZE);
  if (json==NULL) {
    fprintf(stderr,"Error: JSON memory allocation failed\n");
    mem_free(&body);
    return 0;
  }

  n=snprintf(auth_header,sizeof(auth_header),"Authorization: Bearer %s",token);
  if (n<0 || n>=(int)sizeof(auth_header)) {
    fprintf(stderr,"Error: access token too long\n");
    goto cleanup;
  }
  n=snprintf(url,sizeof(url),
      "https://sheets.googleapis.com/v4/spreadsheets/%s/values:batchUpdate",
      SPREADSHEET_ID);
  if (n<0 || n>=(int)sizeof(url)) {
    goto cleanup;
  }

  if (!appendf(json,JSON_SIZE,&used,
      "{\"valueInputOption\":\"USER_ENTERED\",\"data\":["
      "{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[",
      hourly_tab,hourly_range)) {
    goto json_error;
  }
  for (h=0;h<HOURS;h++) {
    if (!appendf(json,JSON_SIZE,&used,"[%.5f],",hourly_sum[h])) {
      goto json_error;
    }
  }
  if (!appendf(json,JSON_SIZE,&used,"[%.5f]]},",hourly_total) ||
      !appendf(json,JSON_SIZE,&used,
      "{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[",
      monthly_tab,monthly_range)) {
    goto json_error;
  }
  for (m=0;m<MONTHS;m++) {
    if (!appendf(json,JSON_SIZE,&used,"[%.5f,%.5f,%.5f,%.5f],",
        monthly_sum[m][0],monthly_sum[m][1],monthly_sum[m][2],monthly_sum[m][3])) {
      goto json_error;
    }
  }
  if (!appendf(json,JSON_SIZE,&used,"[%.5f,%.5f,%.5f,%.5f]]}",
      annual_totals[0],annual_totals[1],annual_totals[2],annual_totals[3])) {
    goto json_error;
  }

  if (include_compare) {
    if (!appendf(json,JSON_SIZE,&used,
        ",{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[",
        compare_tab,compare_range)) {
      goto json_error;
    }
    for (m=0;m<MONTHS;m++) {
      if (!appendf(json,JSON_SIZE,&used,"[%.5f,%.5f,%.5f,%.5f,%d],",
          monthly_sum[m][3],energy_monthly[m],monthly_cp[m],monthly_cf[m],
          monthly_sup[m])) {
        goto json_error;
      }
    }
    if (!appendf(json,JSON_SIZE,&used,"[%.5f,%.5f,%.5f,%.5f,%d]]}",
        annual_totals[3],energy_annual,total_cp,total_cf,total_sup)) {
      goto json_error;
    }
  }
  if (!appendf(json,JSON_SIZE,&used,"]}")) {
    goto json_error;
  }

  curl=curl_easy_init();
  if (curl==NULL) {
    fprintf(stderr,"Error: curl_easy_init failed\n");
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

  curl_easy_setopt(curl,CURLOPT_URL,url);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,json);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)used);
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&body);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,60L);
  curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(curl,CURLOPT_USERAGENT,"kwh_analisys/1.26");

  rc=curl_easy_perform(curl);
  if (rc!=CURLE_OK) {
    fprintf(stderr,"Google Sheets curl error: %s\n",curl_easy_strerror(rc));
    goto cleanup;
  }
  http_code=0;
  if (curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http_code)!=CURLE_OK) {
    fprintf(stderr,"Error: unable to read Google HTTP status\n");
    goto cleanup;
  }
  if (http_code<200 || http_code>=300) {
    fprintf(stderr,"Google Sheets HTTP %ld: %.1000s\n",http_code,body.ptr);
    goto cleanup;
  }
  ok=1;
  goto cleanup;

json_error:
  fprintf(stderr,"Error: Google JSON buffer too small\n");

cleanup:
  curl_slist_free_all(headers);
  if (curl!=NULL) {
    curl_easy_cleanup(curl);
  }
  free(json);
  mem_free(&body);
  return ok;
}

static time_t month_boundary(int year, int month) {
  struct tm tm_target;

  memset(&tm_target,0,sizeof(tm_target));
  tm_target.tm_year=year-1900+month/12;
  tm_target.tm_mon=month%12;
  tm_target.tm_mday=1;
  tm_target.tm_isdst=-1;
  return mktime(&tm_target);
}

static int parse_year(const char *text, int *year) {
  char *end;
  long value;

  if (text==NULL || *text=='\0') {
    return 0;
  }
  errno=0;
  value=strtol(text,&end,10);
  if (errno!=0 || end==text || *end!='\0' || value<1900 || value>2099) {
    return 0;
  }
  *year=(int)value;
  return 1;
}

static int parse_epoch(const char *text, time_t *value) {
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

static int parse_double(const char *text, double *value) {
  char *end;

  errno=0;
  *value=strtod(text,&end);
  return errno==0 && end!=text && *end=='\0';
}

static int get_column_letter(int col_idx, char *out, size_t cap) {
  char tmp[16];
  int i, j, rem;

  if (col_idx<=0) {
    return 0;
  }
  i=0;
  for (;col_idx>0;) {
    if (i>=(int)sizeof(tmp)-1) {
      return 0;
    }
    rem=(col_idx-1)%26;
    tmp[i++]=(char)('A'+rem);
    col_idx=(col_idx-1)/26;
  }
  if ((size_t)i+1>cap) {
    return 0;
  }
  for (j=0;j<i;j++) {
    out[j]=tmp[i-1-j];
  }
  out[i]='\0';
  return 1;
}

static long slot_index(time_t epoch, time_t start, size_t count) {
  time_t rounded;
  long long delta, slot;

  if (epoch<start) {
    return -1;
  }
  rounded=epoch-(epoch%900);
  if (rounded<start) {
    return -1;
  }
  delta=(long long)rounded-(long long)start;
  slot=delta/900;
  if (slot<0 || (unsigned long long)slot>=(unsigned long long)count) {
    return -1;
  }
  return (long)slot;
}

static int query_fiscal(MYSQL *conn, const char *table, time_t start,
    time_t end, double hourly[HOURS], double monthly[MONTHS][BANDS],
    double annual[BANDS], double *hourly_total) {
  MYSQL_RES *res;
  MYSQL_ROW row;
  struct tm *tm_ptr;
  time_t epoch;
  double value;
  char query[QUERY_SIZE];
  int hour, month, band, n;

  res=NULL;
  n=snprintf(query,sizeof(query),
      "SELECT epoch,kwh FROM %s WHERE epoch>=%ld AND epoch<%ld ORDER BY epoch",
      table,(long)start,(long)end);
  if (n<0 || n>=(int)sizeof(query) || mysql_query(conn,query)!=0) {
    fprintf(stderr,"MySQL fiscal query error: %s\n",mysql_error(conn));
    return 0;
  }
  res=mysql_use_result(conn);
  if (res==NULL) {
    fprintf(stderr,"MySQL fiscal result error: %s\n",mysql_error(conn));
    return 0;
  }

  for (row=mysql_fetch_row(res);row!=NULL;row=mysql_fetch_row(res)) {
    if (row[0]==NULL || row[1]==NULL || !parse_epoch(row[0],&epoch) ||
        !parse_double(row[1],&value)) {
      fprintf(stderr,"Error: invalid fiscal database row\n");
      mysql_free_result(res);
      return 0;
    }
    tm_ptr=localtime(&epoch);
    if (tm_ptr==NULL) {
      fprintf(stderr,"Error: fiscal localtime conversion failed\n");
      mysql_free_result(res);
      return 0;
    }
    hour=tm_ptr->tm_hour;
    month=tm_ptr->tm_mon;
    band=get_band_index(tm_ptr);

    hourly[hour]+=value;
    *hourly_total+=value;
    monthly[month][band-1]+=value;
    monthly[month][3]+=value;
    annual[band-1]+=value;
    annual[3]+=value;
  }
  if (mysql_errno(conn)!=0) {
    fprintf(stderr,"MySQL fiscal fetch error: %s\n",mysql_error(conn));
    mysql_free_result(res);
    return 0;
  }
  mysql_free_result(res);
  return 1;
}

static int query_pun(MYSQL *conn, time_t start, time_t end,
    double *lookup, unsigned char *valid, size_t slot_count,
    double sums[MONTHS][BANDS], unsigned long counts[MONTHS][BANDS],
    double averages[MONTHS][BANDS], unsigned long *loaded) {
  MYSQL_RES *res;
  MYSQL_ROW row;
  struct tm *tm_ptr;
  time_t epoch;
  double value;
  char query[QUERY_SIZE];
  long slot;
  int month, band, m, b, n;

  res=NULL;
  n=snprintf(query,sizeof(query),
      "SELECT epoch,c FROM pun_15m WHERE epoch>=%ld AND epoch<%ld ORDER BY epoch",
      (long)start,(long)end);
  if (n<0 || n>=(int)sizeof(query) || mysql_query(conn,query)!=0) {
    fprintf(stderr,"MySQL PUN query error: %s\n",mysql_error(conn));
    return 0;
  }
  res=mysql_use_result(conn);
  if (res==NULL) {
    fprintf(stderr,"MySQL PUN result error: %s\n",mysql_error(conn));
    return 0;
  }

  for (row=mysql_fetch_row(res);row!=NULL;row=mysql_fetch_row(res)) {
    if (row[0]==NULL || row[1]==NULL || !parse_epoch(row[0],&epoch) ||
        !parse_double(row[1],&value)) {
      fprintf(stderr,"Error: invalid PUN database row\n");
      mysql_free_result(res);
      return 0;
    }

    slot=slot_index(epoch,start,slot_count);
    if (slot>=0) {
      lookup[slot]=value;
      valid[slot]=1;
      (*loaded)++;
    }

    tm_ptr=localtime(&epoch);
    if (tm_ptr==NULL) {
      fprintf(stderr,"Error: PUN localtime conversion failed\n");
      mysql_free_result(res);
      return 0;
    }
    month=tm_ptr->tm_mon;
    band=get_band_index(tm_ptr);
    sums[month][band]+=value;
    counts[month][band]++;
  }
  if (mysql_errno(conn)!=0) {
    fprintf(stderr,"MySQL PUN fetch error: %s\n",mysql_error(conn));
    mysql_free_result(res);
    return 0;
  }
  mysql_free_result(res);

  for (m=0;m<MONTHS;m++) {
    for (b=1;b<=3;b++) {
      if (counts[m][b]>0) {
        averages[m][b]=sums[m][b]/(double)counts[m][b];
      }
    }
  }
  return 1;
}

static int query_energy(MYSQL *conn, const char *table, time_t start,
    time_t end, const time_t boundary[BOUNDARIES], double boundary_value[BOUNDARIES],
    unsigned char boundary_found[BOUNDARIES], double boundary_dist[BOUNDARIES],
    const double *pun_lookup, const unsigned char *pun_valid, size_t slot_count,
    const double f_monthly[MONTHS][BANDS],
    const unsigned long pun_counts[MONTHS][BANDS], double monthly_cp[MONTHS],
    double monthly_cf[MONTHS], double daily_cp[MONTHS][DAYS],
    double daily_cf[MONTHS][DAYS], unsigned long *cost_intervals,
    unsigned long *missing_pun) {
  MYSQL_RES *res;
  MYSQL_ROW row;
  struct tm *tm_ptr;
  time_t epoch, prev_epoch;
  double current, prev, delta, price, cost_p, cost_f, dist;
  char query[QUERY_SIZE];
  long slot;
  int i, month, day, band, n;

  res=NULL;
  prev=-1.0;
  prev_epoch=0;
  n=snprintf(query,sizeof(query),
      "SELECT epoch,(e1+e2+e3) FROM %s WHERE epoch BETWEEN %ld AND %ld ORDER BY epoch",
      table,(long)(boundary[0]-BOUNDARY_WINDOW),
      (long)(boundary[BOUNDARIES-1]+BOUNDARY_WINDOW));
  if (n<0 || n>=(int)sizeof(query) || mysql_query(conn,query)!=0) {
    fprintf(stderr,"MySQL energy query error: %s\n",mysql_error(conn));
    return 0;
  }
  res=mysql_use_result(conn);
  if (res==NULL) {
    fprintf(stderr,"MySQL energy result error: %s\n",mysql_error(conn));
    return 0;
  }

  for (row=mysql_fetch_row(res);row!=NULL;row=mysql_fetch_row(res)) {
    if (row[0]==NULL || row[1]==NULL || !parse_epoch(row[0],&epoch) ||
        !parse_double(row[1],&current)) {
      fprintf(stderr,"Error: invalid energy database row\n");
      mysql_free_result(res);
      return 0;
    }

    for (i=0;i<BOUNDARIES;i++) {
      dist=fabs(difftime(epoch,boundary[i]));
      if (dist<=(double)BOUNDARY_WINDOW && dist<boundary_dist[i]) {
        boundary_dist[i]=dist;
        boundary_value[i]=current;
        boundary_found[i]=1;
      }
    }

    if (epoch<start || epoch>=end) {
      continue;
    }

    tm_ptr=localtime(&epoch);
    if (tm_ptr==NULL) {
      fprintf(stderr,"Error: energy localtime conversion failed\n");
      mysql_free_result(res);
      return 0;
    }
    month=tm_ptr->tm_mon;
    day=tm_ptr->tm_mday;
    band=get_band_index(tm_ptr);

    if (prev>=0.0 && prev_epoch>0 && epoch-prev_epoch>=60 &&
        epoch-prev_epoch<=1200) {
      delta=current-prev;
      if (delta>0.0 && delta<100.0) {
        slot=slot_index(epoch,start,slot_count);
        if (slot<0 || !pun_valid[slot] || pun_counts[month][band]==0) {
          (*missing_pun)++;
        } else {
          price=pun_lookup[slot];
          cost_p=delta*(price/1000.0);
          cost_f=delta*(f_monthly[month][band]/1000.0);
          monthly_cp[month]+=cost_p;
          monthly_cf[month]+=cost_f;
          daily_cp[month][day]+=cost_p;
          daily_cf[month][day]+=cost_f;
          (*cost_intervals)++;
        }
      }
    }
    prev=current;
    prev_epoch=epoch;
  }
  if (mysql_errno(conn)!=0) {
    fprintf(stderr,"MySQL energy fetch error: %s\n",mysql_error(conn));
    mysql_free_result(res);
    return 0;
  }
  mysql_free_result(res);
  return 1;
}

int main(int argc, char *argv[]) {
  MYSQL *conn;
  EnergyConfig cfg;
  char cfg_err[256];
  const char *measure_type, *energy_table;
  double hourly_sum[HOURS], monthly_sum[MONTHS][BANDS];
  double annual_totals[BANDS], energy_monthly[MONTHS];
  double pun_sums[MONTHS][BANDS], f_monthly[MONTHS][BANDS];
  double monthly_cp[MONTHS], monthly_cf[MONTHS];
  double daily_cp[MONTHS][DAYS], daily_cf[MONTHS][DAYS];
  double boundary_value[BOUNDARIES], boundary_dist[BOUNDARIES];
  double *pun_lookup;
  unsigned long pun_counts[MONTHS][BANDS];
  unsigned char boundary_found[BOUNDARIES], *pun_valid;
  time_t boundary[BOUNDARIES], start_epoch, end_epoch;
  size_t slot_count;
  double hourly_total, energy_annual, total_cp, total_cf, diff;
  unsigned long pun_loaded, cost_intervals, missing_pun;
  int monthly_sup[MONTHS], total_sup;
  int target_year, is_so, base_year_hm, base_year_d;
  int hourly_col, start_m_col, end_m_col, start_d_col, end_d_col;
  int m, d, i, include_compare, status;
  char hourly_tab[16], monthly_tab[16], compare_tab[16];
  char hourly_letter[16], start_m_letter[16], end_m_letter[16];
  char start_d_letter[16], end_d_letter[16];
  char hourly_range[64], monthly_range[64], compare_range[64];
  char token[TOKEN_SIZE];

  conn=NULL;
  measure_type=NULL;
  energy_table=NULL;
  pun_lookup=NULL;
  pun_valid=NULL;
  memset(hourly_sum,0,sizeof(hourly_sum));
  memset(monthly_sum,0,sizeof(monthly_sum));
  memset(annual_totals,0,sizeof(annual_totals));
  memset(energy_monthly,0,sizeof(energy_monthly));
  memset(pun_sums,0,sizeof(pun_sums));
  memset(pun_counts,0,sizeof(pun_counts));
  memset(f_monthly,0,sizeof(f_monthly));
  memset(monthly_cp,0,sizeof(monthly_cp));
  memset(monthly_cf,0,sizeof(monthly_cf));
  memset(daily_cp,0,sizeof(daily_cp));
  memset(daily_cf,0,sizeof(daily_cf));
  memset(boundary_value,0,sizeof(boundary_value));
  memset(boundary_found,0,sizeof(boundary_found));
  memset(monthly_sup,0,sizeof(monthly_sup));
  for (i=0;i<BOUNDARIES;i++) {
    boundary_dist[i]=DBL_MAX;
  }
  hourly_total=0.0;
  energy_annual=0.0;
  total_cp=0.0;
  total_cf=0.0;
  pun_loaded=0;
  cost_intervals=0;
  missing_pun=0;
  total_sup=0;
  status=1;

  if (argc!=3) {
    fprintf(stderr,"Usage: %s <kwh_so|kwh_cc> <year>\n",argv[0]);
    return 1;
  }
  measure_type=argv[1];
  if (strcmp(measure_type,"kwh_so")!=0 && strcmp(measure_type,"kwh_cc")!=0) {
    fprintf(stderr,"Error: measure_type must be kwh_so or kwh_cc\n");
    return 1;
  }
  if (!parse_year(argv[2],&target_year)) {
    fprintf(stderr,"Error: invalid target year '%s'\n",argv[2]);
    return 1;
  }

  is_so=strcmp(measure_type,"kwh_so")==0;
  base_year_hm=is_so ? 2021 : 2024;
  base_year_d=2025;
  energy_table=is_so ? "energy_so" : "energy_cc";
  if (target_year<base_year_hm) {
    fprintf(stderr,"Error: target year %d is before base year %d\n",
        target_year,base_year_hm);
    return 1;
  }

  if (setenv("TZ","Europe/Rome",1)!=0) {
    fprintf(stderr,"Error: unable to set Europe/Rome timezone\n");
    return 1;
  }
  tzset();

  for (i=0;i<BOUNDARIES;i++) {
    boundary[i]=month_boundary(target_year,i);
    if (boundary[i]==(time_t)-1) {
      fprintf(stderr,"Error: unable to calculate month boundary\n");
      return 1;
    }
  }
  start_epoch=boundary[0];
  end_epoch=boundary[12];
  if (end_epoch<=start_epoch || (end_epoch-start_epoch)%900!=0) {
    fprintf(stderr,"Error: invalid annual epoch range\n");
    return 1;
  }
  slot_count=(size_t)((end_epoch-start_epoch)/900);
  pun_lookup=(double *)calloc(slot_count,sizeof(double));
  pun_valid=(unsigned char *)calloc(slot_count,sizeof(unsigned char));
  if (pun_lookup==NULL || pun_valid==NULL) {
    fprintf(stderr,"Error: unable to allocate PUN lookup (%lu slots)\n",
        (unsigned long)slot_count);
    goto cleanup;
  }

  if (!energy_config_load(&cfg,ENERGY_CONFIG_FILE,cfg_err,sizeof(cfg_err))) {
    fprintf(stderr,"Energy config error: %s\n",cfg_err);
    goto cleanup;
  }

  conn=mysql_init(NULL);
  if (conn==NULL) {
    fprintf(stderr,"Error: mysql_init failed\n");
    goto cleanup;
  }
  if (mysql_real_connect(conn,cfg.db_host,cfg.db_user,cfg.db_pass,cfg.db_name,cfg.db_port,NULL,0)==NULL) {
    fprintf(stderr,"MySQL connect error: %s\n",mysql_error(conn));
    goto cleanup;
  }

  if (!query_fiscal(conn,measure_type,start_epoch,end_epoch,hourly_sum,
      monthly_sum,annual_totals,&hourly_total)) {
    goto cleanup;
  }
  if (!query_pun(conn,start_epoch,end_epoch,pun_lookup,pun_valid,slot_count,
      pun_sums,pun_counts,f_monthly,&pun_loaded)) {
    goto cleanup;
  }
  if (!query_energy(conn,energy_table,start_epoch,end_epoch,boundary,
      boundary_value,boundary_found,boundary_dist,pun_lookup,pun_valid,
      slot_count,f_monthly,pun_counts,monthly_cp,monthly_cf,daily_cp,daily_cf,
      &cost_intervals,&missing_pun)) {
    goto cleanup;
  }

  for (m=0;m<MONTHS;m++) {
    if (boundary_found[m] && boundary_found[m+1]) {
      diff=boundary_value[m+1]-boundary_value[m];
      if (diff>0.0) {
        energy_monthly[m]=diff;
        energy_annual+=diff;
      }
    }

    for (d=1;d<DAYS;d++) {
      if (daily_cp[m][d]>0.0 && daily_cp[m][d]>daily_cf[m][d]) {
        monthly_sup[m]++;
      }
    }
    total_cp+=monthly_cp[m];
    total_cf+=monthly_cf[m];
    total_sup+=monthly_sup[m];
  }

  if (conn!=NULL) {
    mysql_close(conn);
    conn=NULL;
  }

  snprintf(hourly_tab,sizeof(hourly_tab),"%s",is_so ? "h_so" : "h_cc");
  snprintf(monthly_tab,sizeof(monthly_tab),"%s",is_so ? "m_so" : "m_cc");
  snprintf(compare_tab,sizeof(compare_tab),"%s",is_so ? "d_so" : "d_cc");

  hourly_col=2+(target_year-base_year_hm);
  start_m_col=2+(target_year-base_year_hm)*4;
  end_m_col=start_m_col+3;
  include_compare=target_year>=base_year_d;
  start_d_col=2+(target_year-base_year_d)*5;
  end_d_col=start_d_col+4;

  if (!get_column_letter(hourly_col,hourly_letter,sizeof(hourly_letter)) ||
      !get_column_letter(start_m_col,start_m_letter,sizeof(start_m_letter)) ||
      !get_column_letter(end_m_col,end_m_letter,sizeof(end_m_letter)) ||
      (include_compare &&
      (!get_column_letter(start_d_col,start_d_letter,sizeof(start_d_letter)) ||
      !get_column_letter(end_d_col,end_d_letter,sizeof(end_d_letter))))) {
    fprintf(stderr,"Error: unable to calculate Google Sheet columns\n");
    goto cleanup;
  }

  snprintf(hourly_range,sizeof(hourly_range),"%s2:%s26",hourly_letter,hourly_letter);
  snprintf(monthly_range,sizeof(monthly_range),"%s2:%s14",start_m_letter,end_m_letter);
  if (include_compare) {
    snprintf(compare_range,sizeof(compare_range),"%s2:%s14",start_d_letter,end_d_letter);
  } else {
    compare_range[0]='\0';
  }

  if (!read_access_token(token,sizeof(token))) {
    goto cleanup;
  }
  if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) {
    fprintf(stderr,"Error: curl_global_init failed\n");
    goto cleanup;
  }

  if (!update_google_sheets(token,hourly_tab,hourly_range,monthly_tab,
      monthly_range,compare_tab,compare_range,include_compare,hourly_sum,
      hourly_total,monthly_sum,annual_totals,energy_monthly,energy_annual,
      monthly_cp,monthly_cf,monthly_sup,total_cp,total_cf,total_sup)) {
    curl_global_cleanup();
    goto cleanup;
  }
  curl_global_cleanup();

  printf("Updated '%s' range %s for year %d\n",hourly_tab,hourly_range,target_year);
  printf("Updated '%s' range %s for year %d\n",monthly_tab,monthly_range,target_year);
  if (include_compare) {
    printf("Updated '%s' range %s for year %d\n",compare_tab,compare_range,target_year);
  }
  printf("PUN: %lu slots loaded, %lu cost intervals used, %lu intervals skipped for missing PUN\n",
      pun_loaded,cost_intervals,missing_pun);
  status=0;

cleanup:
  if (conn!=NULL) {
    mysql_close(conn);
  }
  free(pun_lookup);
  free(pun_valid);
  return status;
}
