// Gianluca Mazzini @2026- Version 1.11
// Imports quarter-hour kWh CSV files from Google Drive into MySQL and deletes processed files

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <mysql/mysql.h>
#include "energy_config.h"

#define TOKEN_FILE "/home/www/data/google_access_token"
#define MAX_FILES 500
#define DRIVE_PAGE_SIZE 100
#define HTTP_BUFFER_START 4096
#define TOKEN_SIZE 4096
#define AUTH_HEADER_SIZE 4352
#define URL_SIZE 4096
#define PAGE_TOKEN_SIZE 2048
#define SQL_BUFFER_SIZE 65536
#define MAX_TOKENS 120
#define MAX_INTERVALS 96

typedef struct {
  char id[128];
  char name[256];
  char mime_type[128];
} DriveFile;

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
    fprintf(stderr,"Error: cannot open token file %s\n",TOKEN_FILE);
    return 0;
  }
  if (fgets(buf,(int)cap,fp)==NULL) {
    fclose(fp);
    fprintf(stderr,"Error: cannot read token from %s\n",TOKEN_FILE);
    return 0;
  }
  fclose(fp);

  buf[strcspn(buf,"\r\n")]='\0';
  if (*buf=='\0') {
    fprintf(stderr,"Error: empty token in %s\n",TOKEN_FILE);
    return 0;
  }
  return 1;
}

static int valid_table_name(const char *name) {
  const unsigned char *p;
  size_t len;

  if (name==NULL || *name=='\0') {
    return 0;
  }
  len=strlen(name);
  if (len>64) {
    return 0;
  }
  p=(const unsigned char *)name;
  if (!isalpha(*p) && *p!='_') {
    return 0;
  }
  for (p++;*p!='\0';p++) {
    if (!isalnum(*p) && *p!='_') {
      return 0;
    }
  }
  return 1;
}

static int valid_drive_id(const char *id) {
  const unsigned char *p;

  if (id==NULL || *id=='\0') {
    return 0;
  }
  for (p=(const unsigned char *)id;*p!='\0';p++) {
    if (!isalnum(*p) && *p!='_' && *p!='-') {
      return 0;
    }
  }
  return 1;
}

static int http_request(CURL *curl, const char *method, const char *url,
    struct curl_slist *headers, struct mem *body) {
  CURLcode rc;
  long http_code;

  mem_reset(body);
  curl_easy_reset(curl);
  curl_easy_setopt(curl,CURLOPT_URL,url);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,body);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);
  curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(curl,CURLOPT_USERAGENT,"kwh_proc/1.10");

  if (strcmp(method,"GET")==0) {
    curl_easy_setopt(curl,CURLOPT_HTTPGET,1L);
  } else if (strcmp(method,"DELETE")==0) {
    curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,"DELETE");
  } else {
    fprintf(stderr,"Error: unsupported HTTP method\n");
    return 0;
  }

  rc=curl_easy_perform(curl);
  if (rc!=CURLE_OK) {
    fprintf(stderr,"cURL error: %s\n",curl_easy_strerror(rc));
    return 0;
  }

  http_code=0;
  if (curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http_code)!=CURLE_OK) {
    fprintf(stderr,"Error: unable to read HTTP status\n");
    return 0;
  }
  if (http_code<200 || http_code>=300) {
    fprintf(stderr,"Google API HTTP %ld: %.1000s\n",http_code,
        body->ptr!=NULL ? body->ptr : "");
    return 0;
  }
  return 1;
}

static const char *json_object_end(const char *start) {
  const char *p;
  int quoted, escaped;

  quoted=0;
  escaped=0;
  for (p=start;*p!='\0';p++) {
    if (quoted) {
      if (escaped) {
        escaped=0;
      } else if (*p=='\\') {
        escaped=1;
      } else if (*p=='\"') {
        quoted=0;
      }
    } else if (*p=='\"') {
      quoted=1;
    } else if (*p=='}') {
      return p;
    }
  }
  return NULL;
}

static int extract_json_value(const char *start, const char *end,
    const char *key, char *out, size_t cap) {
  const char *p, *colon, *q, *e;
  char search[128];
  size_t len;
  int n;

  n=snprintf(search,sizeof(search),"\"%s\"",key);
  if (n<0 || n>=(int)sizeof(search)) {
    return 0;
  }

  p=strstr(start,search);
  if (p==NULL || (end!=NULL && p>=end)) {
    return 0;
  }
  colon=strchr(p,':');
  if (colon==NULL || (end!=NULL && colon>=end)) {
    return 0;
  }
  q=colon+1;
  for (;*q!='\0' && isspace((unsigned char)*q);q++) {
  }
  if (*q!='\"') {
    return 0;
  }
  q++;

  e=q;
  for (;*e!='\0';e++) {
    if (*e=='\\' && e[1]!='\0') {
      e++;
      continue;
    }
    if (*e=='\"') {
      break;
    }
  }
  if (*e!='\"' || (end!=NULL && e>end)) {
    return 0;
  }

  len=(size_t)(e-q);
  if (len>=cap) {
    return 0;
  }
  memcpy(out,q,len);
  out[len]='\0';
  return 1;
}

static int parse_drive_files(const char *json, DriveFile *files, int capacity,
    int *count) {
  const char *files_sec, *array, *p, *obj_start, *obj_end, *array_end;
  DriveFile *file;

  files_sec=strstr(json,"\"files\"");
  if (files_sec==NULL) {
    return 0;
  }
  array=strchr(files_sec,'[');
  if (array==NULL) {
    return 0;
  }
  array_end=strchr(array,']');
  if (array_end==NULL) {
    return 0;
  }

  p=array+1;
  for (;;) {
    obj_start=strchr(p,'{');
    if (obj_start==NULL || obj_start>=array_end) {
      break;
    }
    obj_end=json_object_end(obj_start+1);
    if (obj_end==NULL || obj_end>array_end) {
      return 0;
    }
    if (*count>=capacity) {
      fprintf(stderr,"Error: more than %d Drive files found\n",capacity);
      return 0;
    }

    file=&files[*count];
    memset(file,0,sizeof(*file));
    if (extract_json_value(obj_start,obj_end,"id",file->id,sizeof(file->id)) &&
        extract_json_value(obj_start,obj_end,"name",file->name,sizeof(file->name))) {
      if (!extract_json_value(obj_start,obj_end,"mimeType",file->mime_type,
          sizeof(file->mime_type))) {
        strcpy(file->mime_type,"text/csv");
      }
      (*count)++;
    }
    p=obj_end+1;
  }
  return 1;
}

static int list_drive_files(CURL *curl, struct curl_slist *headers,
    struct mem *body, const char *folder_id, DriveFile *files, int capacity,
    int *count) {
  char url[URL_SIZE], page_token[PAGE_TOKEN_SIZE];
  char *escaped;
  int n;

  *count=0;
  page_token[0]='\0';
  for (;;) {
    escaped=NULL;
    if (*page_token!='\0') {
      escaped=curl_easy_escape(curl,page_token,0);
      if (escaped==NULL) {
        fprintf(stderr,"Error: unable to encode Drive page token\n");
        return 0;
      }
    }

    n=snprintf(url,sizeof(url),
        "https://www.googleapis.com/drive/v3/files"
        "?q='%s'+in+parents+and+trashed=false"
        "&fields=nextPageToken,files(id,name,mimeType)"
        "&pageSize=%d&supportsAllDrives=true&includeItemsFromAllDrives=true%s%s",
        folder_id,DRIVE_PAGE_SIZE,
        escaped!=NULL ? "&pageToken=" : "",escaped!=NULL ? escaped : "");
    if (escaped!=NULL) {
      curl_free(escaped);
    }
    if (n<0 || n>=(int)sizeof(url)) {
      fprintf(stderr,"Error: Drive list URL too long\n");
      return 0;
    }

    if (!http_request(curl,"GET",url,headers,body)) {
      return 0;
    }
    if (!parse_drive_files(body->ptr,files,capacity,count)) {
      fprintf(stderr,"Error: invalid Drive file list response\n");
      return 0;
    }

    page_token[0]='\0';
    if (!extract_json_value(body->ptr,NULL,"nextPageToken",page_token,
        sizeof(page_token))) {
      break;
    }
    if (*page_token=='\0') {
      break;
    }
  }
  return 1;
}

static int delete_drive_file(CURL *curl, struct curl_slist *headers,
    struct mem *body, const char *file_id) {
  char url[URL_SIZE];
  int n;

  n=snprintf(url,sizeof(url),
      "https://www.googleapis.com/drive/v3/files/%s?supportsAllDrives=true",
      file_id);
  if (n<0 || n>=(int)sizeof(url)) {
    return 0;
  }
  return http_request(curl,"DELETE",url,headers,body);
}

static void clean_val_str(const char *src, char *dst, size_t cap) {
  size_t i, j;
  char c;

  j=0;
  for (i=0;src[i]!='\0' && j+1<cap;i++) {
    c=src[i];
    if (c=='\"' || c=='\'' || c==' ' || c=='\r' || c=='\n' || c=='\t') {
      continue;
    }
    dst[j++]=c==',' ? '.' : c;
  }
  dst[j]='\0';
}

static int tokenize_line(char *line, char **tokens, int max_tokens) {
  char *p, *start;
  char sep;
  int count, quoted;

  sep=';';
  if (strchr(line,';')==NULL && strchr(line,',')!=NULL) {
    sep=',';
  }

  count=0;
  quoted=0;
  start=line;
  for (p=line;*p!='\0' && count<max_tokens;p++) {
    if (*p=='\"') {
      quoted=!quoted;
    } else if (*p==sep && !quoted) {
      *p='\0';
      tokens[count++]=start;
      start=p+1;
    }
  }
  if (count<max_tokens) {
    tokens[count++]=start;
  }
  return count;
}

static int parse_value(const char *text, double *value) {
  char *end;

  if (*text=='\0') {
    return 0;
  }
  errno=0;
  *value=strtod(text,&end);
  return errno==0 && end!=text && *end=='\0';
}

static int sql_start(char *query, size_t cap, const char *table, size_t *used) {
  int n;

  n=snprintf(query,cap,"INSERT INTO `%s` (epoch,kwh) VALUES ",table);
  if (n<0 || (size_t)n>=cap) {
    return 0;
  }
  *used=(size_t)n;
  return 1;
}

static int sql_flush(MYSQL *conn, char *query, size_t cap, const char *table,
    size_t *used, int *tuples) {
  const char *tail;
  size_t tail_len;

  if (*tuples==0) {
    return 1;
  }
  tail=" ON DUPLICATE KEY UPDATE kwh=VALUES(kwh)";
  tail_len=strlen(tail);
  if (*used+tail_len+1>cap) {
    return 0;
  }
  memcpy(query+*used,tail,tail_len+1);
  if (mysql_query(conn,query)!=0) {
    fprintf(stderr,"MySQL query error on table %s: %s\n",table,mysql_error(conn));
    return 0;
  }
  *tuples=0;
  return sql_start(query,cap,table,used);
}

static int sql_add(MYSQL *conn, char *query, size_t cap, const char *table,
    size_t *used, int *tuples, time_t epoch, double value) {
  char tuple[96];
  size_t len, reserve;
  int n;

  n=snprintf(tuple,sizeof(tuple),"%s(%ld,%.4f)",*tuples==0 ? "" : ",",
      (long)epoch,value);
  if (n<0 || n>=(int)sizeof(tuple)) {
    return 0;
  }
  len=(size_t)n;
  reserve=strlen(" ON DUPLICATE KEY UPDATE kwh=VALUES(kwh)")+1;
  if (*used+len+reserve>cap) {
    if (!sql_flush(conn,query,cap,table,used,tuples)) {
      return 0;
    }
    n=snprintf(tuple,sizeof(tuple),"(%ld,%.4f)",(long)epoch,value);
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

static int process_and_insert_csv(MYSQL *conn, const char *table,
    const char *csv_data) {
  struct tm tm_info;
  time_t epoch;
  double value;
  char *data_copy, *line;
  char *tokens[MAX_TOKENS];
  char prev_date[32], date_raw[64], val_str[32];
  char query[SQL_BUFFER_SIZE];
  size_t query_used, date_len;
  int day, month, year, hour, minute, second_line;
  int token_count, i, tuples, inserted, skipped;

  data_copy=NULL;
  line=NULL;
  memset(prev_date,0,sizeof(prev_date));
  query_used=0;
  tuples=0;
  inserted=0;
  skipped=0;

  data_copy=(char *)malloc(strlen(csv_data)+1);
  if (data_copy==NULL) {
    fprintf(stderr,"Memory allocation error while processing CSV\n");
    return 0;
  }
  strcpy(data_copy,csv_data);

  if (mysql_autocommit(conn,0)!=0) {
    fprintf(stderr,"MySQL autocommit error: %s\n",mysql_error(conn));
    free(data_copy);
    return 0;
  }
  if (!sql_start(query,sizeof(query),table,&query_used)) {
    mysql_rollback(conn);
    mysql_autocommit(conn,1);
    free(data_copy);
    return 0;
  }

  line=strtok(data_copy,"\r\n");
  if (line!=NULL) {
    line=strtok(NULL,"\r\n");
  }

  for (;line!=NULL;line=strtok(NULL,"\r\n")) {
    if (*line=='\0') {
      continue;
    }
    token_count=tokenize_line(line,tokens,MAX_TOKENS);
    if (token_count<2) {
      continue;
    }

    clean_val_str(tokens[0],date_raw,sizeof(date_raw));
    if (sscanf(date_raw,"%d/%d/%d",&day,&month,&year)!=3 ||
        month<1 || month>12 || day<1 || day>31) {
      skipped++;
      continue;
    }

    date_len=strlen(date_raw);
    if (date_len>=sizeof(prev_date)) {
      skipped++;
      continue;
    }
    second_line=strcmp(date_raw,prev_date)==0;
    memcpy(prev_date,date_raw,date_len+1);

    for (i=1;i<token_count && i<=MAX_INTERVALS;i++) {
      if (second_line && i<9) {
        continue;
      }

      clean_val_str(tokens[i],val_str,sizeof(val_str));
      if (!second_line && month==10 && i>12 && strcmp(val_str,"0")==0) {
        continue;
      }
      if (!parse_value(val_str,&value)) {
        skipped++;
        continue;
      }

      hour=(i-1)/4;
      minute=((i-1)%4)*15;
      memset(&tm_info,0,sizeof(tm_info));
      tm_info.tm_year=year-1900;
      tm_info.tm_mon=month-1;
      tm_info.tm_mday=day;
      tm_info.tm_hour=hour;
      tm_info.tm_min=minute;
      if (second_line) {
        tm_info.tm_isdst=0;
      } else if (month==10 && day>=25 && day<=31 && hour<3) {
        tm_info.tm_isdst=1;
      } else {
        tm_info.tm_isdst=-1;
      }

      epoch=mktime(&tm_info);
      if (epoch==(time_t)-1) {
        skipped++;
        continue;
      }
      if (!sql_add(conn,query,sizeof(query),table,&query_used,&tuples,epoch,value)) {
        mysql_rollback(conn);
        mysql_autocommit(conn,1);
        free(data_copy);
        return 0;
      }
      inserted++;
    }
  }

  if (inserted==0 || !sql_flush(conn,query,sizeof(query),table,&query_used,&tuples)) {
    if (inserted==0) {
      fprintf(stderr,"Error: CSV contains no valid data rows\n");
    }
    mysql_rollback(conn);
    mysql_autocommit(conn,1);
    free(data_copy);
    return 0;
  }
  if (mysql_commit(conn)!=0) {
    fprintf(stderr,"MySQL commit error: %s\n",mysql_error(conn));
    mysql_rollback(conn);
    mysql_autocommit(conn,1);
    free(data_copy);
    return 0;
  }
  if (mysql_autocommit(conn,1)!=0) {
    fprintf(stderr,"MySQL autocommit restore error: %s\n",mysql_error(conn));
    free(data_copy);
    return 0;
  }

  if (skipped>0) {
    printf("  -> Inserted %d values, skipped %d invalid/empty values.\n",inserted,skipped);
  } else {
    printf("  -> Inserted %d values.\n",inserted);
  }
  free(data_copy);
  return 1;
}

int main(int argc, char *argv[]) {
  CURL *curl;
  EnergyConfig cfg;
  char cfg_err[256];
  struct curl_slist *headers;
  MYSQL *conn;
  struct mem body;
  DriveFile files[MAX_FILES];
  const char *folder_id, *table_name;
  char access_token[TOKEN_SIZE], auth_header[AUTH_HEADER_SIZE], url[URL_SIZE];
  int file_count, i, n, failures, status;

  curl=NULL;
  headers=NULL;
  conn=NULL;
  body.ptr=NULL;
  body.len=0;
  body.cap=0;
  folder_id=NULL;
  table_name=NULL;
  file_count=0;
  failures=0;
  status=EXIT_FAILURE;

  if (argc!=3) {
    fprintf(stderr,"Usage: %s GOOGLE_FOLDER_ID MYSQL_TABLE_NAME\n",argv[0]);
    return EXIT_FAILURE;
  }
  folder_id=argv[1];
  table_name=argv[2];

  if (!valid_drive_id(folder_id)) {
    fprintf(stderr,"Error: invalid Google Drive folder ID\n");
    return EXIT_FAILURE;
  }
  if (!valid_table_name(table_name)) {
    fprintf(stderr,"Error: invalid MySQL table name\n");
    return EXIT_FAILURE;
  }
  if (setenv("TZ","Europe/Rome",1)!=0) {
    fprintf(stderr,"Error: unable to set Europe/Rome timezone\n");
    return EXIT_FAILURE;
  }
  tzset();

  if (!read_access_token(access_token,sizeof(access_token))) {
    return EXIT_FAILURE;
  }

  if (!energy_config_load(&cfg,ENERGY_CONFIG_FILE,cfg_err,sizeof(cfg_err))) {
    fprintf(stderr,"Energy config error: %s\n",cfg_err);
    return EXIT_FAILURE;
  }

  conn=mysql_init(NULL);
  if (conn==NULL) {
    fprintf(stderr,"MySQL init error\n");
    return EXIT_FAILURE;
  }
  if (mysql_real_connect(conn,cfg.db_host,cfg.db_user,cfg.db_pass,cfg.db_name,cfg.db_port,NULL,0)==NULL) {
    fprintf(stderr,"MySQL connect error: %s\n",mysql_error(conn));
    mysql_close(conn);
    return EXIT_FAILURE;
  }

  if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) {
    fprintf(stderr,"cURL global init error\n");
    mysql_close(conn);
    return EXIT_FAILURE;
  }
  if (!mem_init(&body)) {
    fprintf(stderr,"Memory allocation error\n");
    goto cleanup;
  }
  curl=curl_easy_init();
  if (curl==NULL) {
    fprintf(stderr,"cURL easy init error\n");
    goto cleanup;
  }

  n=snprintf(auth_header,sizeof(auth_header),"Authorization: Bearer %s",access_token);
  if (n<0 || n>=(int)sizeof(auth_header)) {
    fprintf(stderr,"Error: access token too long\n");
    goto cleanup;
  }
  headers=curl_slist_append(NULL,auth_header);
  if (headers==NULL) {
    fprintf(stderr,"cURL header allocation error\n");
    goto cleanup;
  }

  if (!list_drive_files(curl,headers,&body,folder_id,files,MAX_FILES,&file_count)) {
    goto cleanup;
  }
  printf("Found %d file(s) to process in folder %s.\n",file_count,folder_id);

  for (i=0;i<file_count;i++) {
    printf("[%d/%d] Downloading '%s' (ID: %s, MIME: %s)...\n",
        i+1,file_count,files[i].name,files[i].id,files[i].mime_type);

    if (strcmp(files[i].mime_type,"application/vnd.google-apps.spreadsheet")==0) {
      n=snprintf(url,sizeof(url),
          "https://www.googleapis.com/drive/v3/files/%s/export?mimeType=text/csv&supportsAllDrives=true",
          files[i].id);
    } else {
      n=snprintf(url,sizeof(url),
          "https://www.googleapis.com/drive/v3/files/%s?alt=media&supportsAllDrives=true",
          files[i].id);
    }
    if (n<0 || n>=(int)sizeof(url)) {
      fprintf(stderr,"  -> Download URL too long for '%s'.\n",files[i].name);
      failures++;
      continue;
    }

    if (!http_request(curl,"GET",url,headers,&body)) {
      fprintf(stderr,"  -> Download failed for '%s'.\n",files[i].name);
      failures++;
      continue;
    }
    if (!process_and_insert_csv(conn,table_name,body.ptr)) {
      fprintf(stderr,"  -> DB insertion failed for '%s'. File not deleted.\n",files[i].name);
      failures++;
      continue;
    }
    if (!delete_drive_file(curl,headers,&body,files[i].id)) {
      fprintf(stderr,"  -> Failed to delete '%s' from Google Drive.\n",files[i].name);
      failures++;
      continue;
    }
    printf("  -> Successfully imported and deleted '%s'.\n",files[i].name);
  }

  if (failures==0) {
    printf("Processing completed: %d file(s), no errors.\n",file_count);
    status=EXIT_SUCCESS;
  } else {
    fprintf(stderr,"Processing completed with %d failure(s) out of %d file(s).\n",
        failures,file_count);
  }

cleanup:
  curl_slist_free_all(headers);
  if (curl!=NULL) {
    curl_easy_cleanup(curl);
  }
  mem_free(&body);
  curl_global_cleanup();
  if (conn!=NULL) {
    mysql_close(conn);
  }
  return status;
}
