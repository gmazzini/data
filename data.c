// Gianluca Mazzini @2024- Version 2.07

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <mysql.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_FILE "data.conf"
#define MAX_SERIES 64
#define MAX_SOURCES 64
#define MAX_NAME 128
#define MAX_EXPR 512
#define MAX_LABEL 256
#define MAX_QUERY 262144
#define MAX_LINE 2048
#define MAX_AXIS_FORMAT 32
#define MAX_PATH_LEN 4096
#define SVG_WIDTH 1200
#define SVG_HEIGHT 700
#define SVG_LEFT 82
#define SVG_RIGHT 82
#define SVG_TOP 88
#define SVG_BOTTOM 92

#define MODE_VALUE 0
#define MODE_DELTA 1
#define MODE_TOTAL 2

typedef struct {
  char table[MAX_NAME];
  char device[MAX_NAME];
  int delta;
} SourceConfig;

typedef struct {
  char table[MAX_NAME];
  char device[MAX_NAME];
  char expr[MAX_EXPR];
  char label[MAX_LABEL];
  int axis;
  int delta;
  int source;
} SeriesConfig;

typedef struct {
  char db_host[MAX_NAME];
  char db_user[MAX_NAME];
  char db_pass[MAX_LABEL];
  char db_name[MAX_NAME];
  char auth_key[MAX_LABEL];
  char title[MAX_LABEL];
  char axis_left[MAX_LABEL];
  char axis_right[MAX_LABEL];
  char axis_left_format[MAX_AXIS_FORMAT];
  char axis_right_format[MAX_AXIS_FORMAT];
  unsigned int db_port;
  int y_left_min0;
  int y_right_min0;
  unsigned long points;
  int series_count;
  int source_count;
  SeriesConfig series[MAX_SERIES];
  SourceConfig source[MAX_SOURCES];
} Config;

typedef struct {
  double acc;
  double last;
  double prev;
  double base;
  unsigned long count;
  int has_last;
  int has_prev;
  int has_base;
} SeriesState;

typedef struct {
  double min;
  double max;
  int valid;
} AxisStats;

typedef struct {
  time_t *epoch;
  double *value;
  unsigned char *valid;
  unsigned long count;
  unsigned long capacity;
  int series_count;
} OutputData;

static const char *graph_colors[] = {
  "#0d6efd", "#dc3545", "#198754", "#fd7e14",
  "#6f42c1", "#20c997", "#d63384", "#6c757d",
  "#0dcaf0", "#ffc107", "#6610f2", "#495057"
};

static char *trim(char *s) {
  char *e;

  while (*s != '\0' && isspace((unsigned char)*s)) {
    s++;
  }
  e=s+strlen(s);
  while (e>s && isspace((unsigned char)e[-1])) {
    e--;
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

static int parse_uint(const char *s, unsigned long *v) {
  char *e;
  unsigned long n;

  errno=0;
  n=strtoul(s,&e,10);
  if (errno!=0 || e==s || *trim(e)!='\0') {
    return 0;
  }
  *v=n;
  return 1;
}

static int valid_table_name(const char *s) {
  const unsigned char *p;

  if (*s=='\0') {
    return 0;
  }
  for (p=(const unsigned char *)s; *p!='\0'; p++) {
    if (!isalnum(*p) && *p!='_') {
      return 0;
    }
  }
  return 1;
}

static int split_series(char *s, char **part, int count) {
  char *p;
  int i;

  p=s;
  for (i=0;i<count;i++) {
    part[i]=p;
    if (i==count-1) {
      break;
    }
    p=strchr(p,'|');
    if (p==NULL) {
      return 0;
    }
    *p='\0';
    p++;
  }
  if (strchr(part[count-1],'|')!=NULL) {
    return 0;
  }
  for (i=0;i<count;i++) {
    part[i]=trim(part[i]);
  }
  return 1;
}

static int find_source(Config *cfg, const char *table, const char *device, int delta) {
  int i;

  for (i=0;i<cfg->source_count;i++) {
    if (cfg->source[i].delta==delta && strcmp(cfg->source[i].table,table)==0 &&
        strcmp(cfg->source[i].device,device)==0) {
      return i;
    }
  }
  if (cfg->source_count>=MAX_SOURCES) {
    return -1;
  }
  i=cfg->source_count++;
  copy_text(cfg->source[i].table,sizeof(cfg->source[i].table),table);
  copy_text(cfg->source[i].device,sizeof(cfg->source[i].device),device);
  cfg->source[i].delta=delta;
  return i;
}

static int valid_axis_format(const char *s) {
  const unsigned char *p;
  unsigned int width;
  unsigned int precision;
  int digits;

  if (s==NULL || *s!='%') {
    return 0;
  }
  p=(const unsigned char *)s+1;
  width=0;
  digits=0;
  for (; isdigit(*p); p++) {
    width=width*10+(unsigned int)(*p-'0');
    digits++;
    if (digits>2 || width>20) {
      return 0;
    }
  }
  if (*p=='.') {
    p++;
    precision=0;
    digits=0;
    for (; isdigit(*p); p++) {
      precision=precision*10+(unsigned int)(*p-'0');
      digits++;
      if (digits>2 || precision>10) {
        return 0;
      }
    }
    if (digits==0) {
      return 0;
    }
  }
  if (*p!='f' && *p!='F' && *p!='e' && *p!='E' && *p!='g' && *p!='G') {
    return 0;
  }
  return p[1]=='\0';
}

static int load_config(const char *path, Config *cfg, char *err, size_t errcap) {
  FILE *fp;
  char line[MAX_LINE];
  char *p;
  char *eq;
  char *part[6];
  unsigned long n;
  int lineno;
  int axis;
  int delta;
  int source;
  int axis_used[2];
  SeriesConfig *s;

  memset(cfg,0,sizeof(*cfg));
  axis_used[0]=0;
  axis_used[1]=0;
  copy_text(cfg->db_host,sizeof(cfg->db_host),"127.0.0.1");
  cfg->db_port=3306;
  cfg->points=1000;
  copy_text(cfg->title,sizeof(cfg->title),"Data");
  fp=fopen(path,"r");
  if (fp==NULL) {
    sprintf(err,"cannot open configuration file %s",path);
    return 0;
  }
  lineno=0;
  while (fgets(line,sizeof(line),fp)!=NULL) {
    lineno++;
    p=trim(line);
    if (*p=='\0' || *p=='#') {
      continue;
    }
    eq=strchr(p,'=');
    if (eq==NULL) {
      sprintf(err,"invalid configuration line %d",lineno);
      fclose(fp);
      return 0;
    }
    *eq='\0';
    eq=trim(eq+1);
    p=trim(p);
    if (strcmp(p,"db_host")==0) {
      copy_text(cfg->db_host,sizeof(cfg->db_host),eq);
    } else if (strcmp(p,"db_user")==0) {
      copy_text(cfg->db_user,sizeof(cfg->db_user),eq);
    } else if (strcmp(p,"db_pass")==0) {
      copy_text(cfg->db_pass,sizeof(cfg->db_pass),eq);
    } else if (strcmp(p,"db_name")==0) {
      copy_text(cfg->db_name,sizeof(cfg->db_name),eq);
    } else if (strcmp(p,"auth_key")==0) {
      copy_text(cfg->auth_key,sizeof(cfg->auth_key),eq);
    } else if (strcmp(p,"db_port")==0) {
      if (!parse_uint(eq,&n) || n>65535) {
        sprintf(err,"invalid db_port at line %d",lineno);
        fclose(fp);
        return 0;
      }
      cfg->db_port=(unsigned int)n;
    } else if (strcmp(p,"points")==0) {
      if (!parse_uint(eq,&n) || n==0) {
        sprintf(err,"invalid points at line %d",lineno);
        fclose(fp);
        return 0;
      }
      cfg->points=n;
    } else if (strcmp(p,"title")==0) {
      copy_text(cfg->title,sizeof(cfg->title),eq);
    } else if (strcmp(p,"axis_left")==0) {
      copy_text(cfg->axis_left,sizeof(cfg->axis_left),eq);
    } else if (strcmp(p,"axis_right")==0) {
      copy_text(cfg->axis_right,sizeof(cfg->axis_right),eq);
    } else if (strcmp(p,"axis_left_format")==0) {
      if (!valid_axis_format(eq)) {
        sprintf(err,"invalid axis_left_format at line %d",lineno);
        fclose(fp);
        return 0;
      }
      copy_text(cfg->axis_left_format,sizeof(cfg->axis_left_format),eq);
    } else if (strcmp(p,"axis_right_format")==0) {
      if (!valid_axis_format(eq)) {
        sprintf(err,"invalid axis_right_format at line %d",lineno);
        fclose(fp);
        return 0;
      }
      copy_text(cfg->axis_right_format,sizeof(cfg->axis_right_format),eq);
    } else if (strcmp(p,"y_left_min0")==0) {
      cfg->y_left_min0=atoi(eq)!=0;
    } else if (strcmp(p,"y_right_min0")==0) {
      cfg->y_right_min0=atoi(eq)!=0;
    } else if (strcmp(p,"series")==0) {
      if (cfg->series_count>=MAX_SERIES || !split_series(eq,part,6)) {
        sprintf(err,"invalid series at line %d",lineno);
        fclose(fp);
        return 0;
      }
      if (!valid_table_name(part[0]) || *part[2]=='\0' || *part[3]=='\0') {
        sprintf(err,"invalid series fields at line %d",lineno);
        fclose(fp);
        return 0;
      }
      axis=atoi(part[4]);
      delta=atoi(part[5]);
      if ((axis!=0 && axis!=1) || delta<0 || delta>2) {
        sprintf(err,"invalid axis or delta at line %d",lineno);
        fclose(fp);
        return 0;
      }
      source=find_source(cfg,part[0],part[1],delta);
      if (source<0) {
        sprintf(err,"too many sources at line %d",lineno);
        fclose(fp);
        return 0;
      }
      s=&cfg->series[cfg->series_count++];
      copy_text(s->table,sizeof(s->table),part[0]);
      copy_text(s->device,sizeof(s->device),part[1]);
      copy_text(s->expr,sizeof(s->expr),part[2]);
      copy_text(s->label,sizeof(s->label),part[3]);
      s->axis=axis;
      axis_used[axis]=1;
      s->delta=delta;
      s->source=source;
    } else {
      sprintf(err,"unknown configuration key '%s' at line %d",p,lineno);
      fclose(fp);
      return 0;
    }
  }
  fclose(fp);
  if (*cfg->db_user=='\0' || *cfg->db_name=='\0') {
    copy_text(err,errcap,"db_user and db_name are required");
    return 0;
  }
  if (cfg->series_count==0) {
    copy_text(err,errcap,"at least one series is required");
    return 0;
  }
  if (axis_used[0] && *cfg->axis_left_format=='\0') {
    copy_text(err,errcap,"axis_left_format is required for axis 0");
    return 0;
  }
  if (axis_used[1] && *cfg->axis_right_format=='\0') {
    copy_text(err,errcap,"axis_right_format is required for axis 1");
    return 0;
  }
  return 1;
}

static int buf_add(char *dst, size_t cap, const char *src) {
  size_t a;
  size_t b;

  a=strlen(dst);
  b=strlen(src);
  if (a+b>=cap) {
    return 0;
  }
  memcpy(dst+a,src,b+1);
  return 1;
}

static int buf_add_num(char *dst, size_t cap, long n) {
  char tmp[64];

  sprintf(tmp,"%ld",n);
  return buf_add(dst,cap,tmp);
}

static int word_equal(const char *a, size_t n, const char *b) {
  size_t i;

  if (strlen(b)!=n) {
    return 0;
  }
  for (i=0;i<n;i++) {
    if (tolower((unsigned char)a[i])!=tolower((unsigned char)b[i])) {
      return 0;
    }
  }
  return 1;
}

static int skip_sql_word(const char *s, size_t n) {
  static const char *words[] = {
    "null", "true", "false", "abs", "avg", "count", "sum", "min", "max",
    "if", "ifnull", "coalesce", "nullif", "round", "floor", "ceil", "date",
    "now", "unix_timestamp", "case", "when", "then", "else", "end", "over",
    "partition", "by", "rows", "range", "groups", "current_row", "unbounded",
    "preceding", "following", "and", "or", "not", "as", "distinct", "mod"
  };
  size_t i;

  for (i=0;i<sizeof(words)/sizeof(words[0]);i++) {
    if (word_equal(s,n,words[i])) {
      return 1;
    }
  }
  return 0;
}

static int qualify_expr(const char *expr, const char *alias, char *out, size_t cap) {
  const char *p;
  const char *q;
  char word[MAX_NAME];
  char one[2];
  size_t n;
  int quote;

  out[0]='\0';
  p=expr;
  quote=0;
  one[1]='\0';
  while (*p!='\0') {
    if (quote!=0) {
      one[0]=*p;
      if (!buf_add(out,cap,one)) {
        return 0;
      }
      if (*p==quote && (p==expr || p[-1]!='\\')) {
        quote=0;
      }
      p++;
      continue;
    }
    if (*p=='\'' || *p=='\"' || *p=='`') {
      quote=(unsigned char)*p;
      one[0]=*p++;
      if (!buf_add(out,cap,one)) {
        return 0;
      }
      continue;
    }
    if (isalpha((unsigned char)*p) || *p=='_') {
      q=p+1;
      while (isalnum((unsigned char)*q) || *q=='_') {
        q++;
      }
      n=(size_t)(q-p);
      if (n>=sizeof(word)) {
        return 0;
      }
      memcpy(word,p,n);
      word[n]='\0';
      if (!skip_sql_word(p,n)) {
        if (!buf_add(out,cap,alias) || !buf_add(out,cap,".`") ||
            !buf_add(out,cap,word) || !buf_add(out,cap,"`")) {
          return 0;
        }
      } else if (!buf_add(out,cap,word)) {
        return 0;
      }
      p=q;
      continue;
    }
    one[0]=*p++;
    if (!buf_add(out,cap,one)) {
      return 0;
    }
  }
  return 1;
}

static int append_device_where(MYSQL *db, char *query, size_t cap, const char *device) {
  char escaped[MAX_NAME*2+1];
  unsigned long n;

  if (*device=='\0') {
    return 1;
  }
  n=mysql_real_escape_string(db,escaped,device,(unsigned long)strlen(device));
  escaped[n]='\0';
  return buf_add(query,cap," AND device='") && buf_add(query,cap,escaped) &&
         buf_add(query,cap,"'");
}

static MYSQL *open_db(const Config *cfg, char *err, size_t errcap) {
  MYSQL *db;

  db=mysql_init(NULL);
  if (db==NULL) {
    copy_text(err,errcap,"mysql_init failed");
    return NULL;
  }
  if (mysql_real_connect(db,cfg->db_host,cfg->db_user,cfg->db_pass,cfg->db_name,
      cfg->db_port,NULL,0)==NULL) {
    copy_text(err,errcap,mysql_error(db));
    mysql_close(db);
    return NULL;
  }
  mysql_set_character_set(db,"utf8mb4");
  return db;
}

static int build_union(MYSQL *db, const Config *cfg, time_t start, time_t end,
    char *out, size_t cap) {
  char part[2048];
  int i;

  out[0]='\0';
  for (i=0;i<cfg->source_count;i++) {
    part[0]='\0';
    if (i>0 && !buf_add(out,cap," UNION ")) {
      return 0;
    }
    if (!buf_add(part,sizeof(part),"SELECT epoch FROM `") ||
        !buf_add(part,sizeof(part),cfg->source[i].table) ||
        !buf_add(part,sizeof(part),"` WHERE epoch BETWEEN ") ||
        !buf_add_num(part,sizeof(part),(long)start) ||
        !buf_add(part,sizeof(part)," AND ") ||
        !buf_add_num(part,sizeof(part),(long)end) ||
        !append_device_where(db,part,sizeof(part),cfg->source[i].device) ||
        !buf_add(out,cap,part)) {
      return 0;
    }
  }
  return 1;
}

static int query_max_epoch(MYSQL *db, const Config *cfg, time_t *max_epoch) {
  MYSQL_RES *res;
  MYSQL_ROW row;
  char query[MAX_QUERY];
  char part[2048];
  int i;

  query[0]='\0';
  if (!buf_add(query,sizeof(query),"SELECT MAX(max_e) FROM (")) {
    return 0;
  }
  for (i=0;i<cfg->source_count;i++) {
    if (i>0 && !buf_add(query,sizeof(query)," UNION ALL ")) {
      return 0;
    }
    part[0]='\0';
    if (!buf_add(part,sizeof(part),"SELECT MAX(epoch) AS max_e FROM `") ||
        !buf_add(part,sizeof(part),cfg->source[i].table) ||
        !buf_add(part,sizeof(part),"` WHERE 1=1") ||
        !append_device_where(db,part,sizeof(part),cfg->source[i].device) ||
        !buf_add(query,sizeof(query),part)) {
      return 0;
    }
  }
  if (!buf_add(query,sizeof(query),") x")) {
    return 0;
  }
  if (mysql_query(db,query)!=0) {
    return 0;
  }
  res=mysql_store_result(db);
  if (res==NULL) {
    return 0;
  }
  row=mysql_fetch_row(res);
  if (row!=NULL && row[0]!=NULL) {
    *max_epoch=(time_t)strtol(row[0],NULL,10);
  }
  mysql_free_result(res);
  return 1;
}

static int leap_year(int year) {
  return (year%4==0 && (year%100!=0 || year%400==0));
}

static time_t day_start(int year, int yday) {
  struct tm t;

  memset(&t,0,sizeof(t));
  t.tm_year=year-1900;
  t.tm_mon=0;
  t.tm_mday=1+yday;
  t.tm_isdst=-1;
  return mktime(&t);
}

static time_t month_start(int year, int month) {
  struct tm t;

  memset(&t,0,sizeof(t));
  t.tm_year=year-1900;
  t.tm_mon=month-1;
  t.tm_mday=1;
  t.tm_isdst=-1;
  return mktime(&t);
}

static time_t iso_week_start(int year, int week) {
  struct tm t;
  time_t jan4;
  int iso_wday;

  memset(&t,0,sizeof(t));
  t.tm_year=year-1900;
  t.tm_mon=0;
  t.tm_mday=4;
  t.tm_isdst=-1;
  jan4=mktime(&t);
  t=*localtime(&jan4);
  iso_wday=t.tm_wday==0 ? 7 : t.tm_wday;
  t.tm_mday-=iso_wday-1;
  t.tm_mday+=(week-1)*7;
  t.tm_hour=0;
  t.tm_min=0;
  t.tm_sec=0;
  t.tm_isdst=-1;
  return mktime(&t);
}

static time_t end_of_day(time_t start) {
  struct tm t;
  time_t next;

  t=*localtime(&start);
  t.tm_mday++;
  t.tm_hour=0;
  t.tm_min=0;
  t.tm_sec=0;
  t.tm_isdst=-1;
  next=mktime(&t);
  return next-1;
}

static time_t add_days(time_t value, int days) {
  struct tm t;

  t=*localtime(&value);
  t.tm_mday+=days;
  t.tm_isdst=-1;
  return mktime(&t);
}

static time_t add_months(time_t value, int months) {
  struct tm t;

  t=*localtime(&value);
  t.tm_mon+=months;
  t.tm_isdst=-1;
  return mktime(&t);
}

static int parse_q(const char *q, time_t fallback, char *mode, time_t *start, time_t *end) {
  int year;
  int num;
  char type;
  struct tm t;
  time_t s;
  time_t next;

  if (q==NULL || sscanf(q,"%4d%c%d",&year,&type,&num)!=3 ||
      (type!='d' && type!='w' && type!='m')) {
    t=*localtime(&fallback);
    year=t.tm_year+1900;
    num=t.tm_yday;
    type='d';
  }
  if (year<1970 || year>2100) {
    return 0;
  }
  if (type=='d') {
    if (num<0 || num>=(leap_year(year) ? 366 : 365)) {
      return 0;
    }
    s=day_start(year,num);
    next=add_days(s,1);
  } else if (type=='w') {
    if (num<1 || num>53) {
      return 0;
    }
    s=iso_week_start(year,num);
    next=add_days(s,7);
  } else {
    if (num<1 || num>12) {
      return 0;
    }
    s=month_start(year,num);
    next=add_months(s,1);
  }
  *mode=type;
  *start=s;
  *end=next-1;
  return 1;
}

static void query_q(char *out, size_t cap) {
  const char *qs;
  const char *p;
  const char *e;
  size_t n;

  out[0]='\0';
  qs=getenv("QUERY_STRING");
  if (qs==NULL) {
    return;
  }
  p=qs;
  while (*p!='\0') {
    if ((p==qs || p[-1]=='&') && p[0]=='q' && p[1]=='=') {
      p+=2;
      e=strchr(p,'&');
      n=e==NULL ? strlen(p) : (size_t)(e-p);
      if (n>=cap) {
        n=cap-1;
      }
      memcpy(out,p,n);
      out[n]='\0';
      return;
    }
    p=strchr(p,'&');
    if (p==NULL) {
      return;
    }
    p++;
  }
}

static int iso_week_number(time_t value) {
  char tmp[8];
  struct tm *t;

  t=localtime(&value);
  if (strftime(tmp,sizeof(tmp),"%V",t)==0) {
    return 1;
  }
  return atoi(tmp);
}

static void build_q(time_t value, char mode, char *out) {
  struct tm *t;
  int year;
  int num;

  t=localtime(&value);
  year=t->tm_year+1900;
  if (mode=='w') {
    num=iso_week_number(value);
    sprintf(out,"%04dw%02d",year,num);
  } else if (mode=='m') {
    sprintf(out,"%04dm%02d",year,t->tm_mon+1);
  } else {
    sprintf(out,"%04dd%03d",year,t->tm_yday);
  }
}

static time_t period_limit(time_t max_epoch, char mode) {
  struct tm t;

  t=*localtime(&max_epoch);
  if (mode=='w') {
    return iso_week_start(t.tm_year+1900,iso_week_number(max_epoch));
  }
  if (mode=='m') {
    return month_start(t.tm_year+1900,t.tm_mon+1);
  }
  return day_start(t.tm_year+1900,t.tm_yday);
}

static time_t period_shift(time_t start, char mode, int direction) {
  if (mode=='w') {
    return add_days(start,7*direction);
  }
  if (mode=='m') {
    return add_months(start,direction);
  }
  return add_days(start,direction);
}

static int query_count(MYSQL *db, const Config *cfg, time_t start, time_t end,
    unsigned long *count) {
  MYSQL_RES *res;
  MYSQL_ROW row;
  char uni[MAX_QUERY];
  char query[MAX_QUERY];

  if (!build_union(db,cfg,start,end,uni,sizeof(uni))) {
    return 0;
  }
  query[0]='\0';
  if (!buf_add(query,sizeof(query),"SELECT COUNT(*) FROM (") ||
      !buf_add(query,sizeof(query),uni) || !buf_add(query,sizeof(query),") e")) {
    return 0;
  }
  if (mysql_query(db,query)!=0) {
    return 0;
  }
  res=mysql_store_result(db);
  if (res==NULL) {
    return 0;
  }
  row=mysql_fetch_row(res);
  if (row==NULL || row[0]==NULL) {
    mysql_free_result(res);
    return 0;
  }
  *count=strtoul(row[0],NULL,10);
  mysql_free_result(res);
  return 1;
}

static int init_states(MYSQL *db, const Config *cfg, time_t start, SeriesState *state) {
  MYSQL_RES *res;
  MYSQL_ROW row;
  char query[MAX_QUERY];
  int source;
  int i;
  int first;
  double v;

  for (source=0;source<cfg->source_count;source++) {
    query[0]='\0';
    if (!buf_add(query,sizeof(query),"SELECT ")) {
      return 0;
    }
    first=1;
    for (i=0;i<cfg->series_count;i++) {
      if (cfg->series[i].source!=source) {
        continue;
      }
      if (!first && !buf_add(query,sizeof(query),",")) {
        return 0;
      }
      if (!buf_add(query,sizeof(query),"(") ||
          !buf_add(query,sizeof(query),cfg->series[i].expr) ||
          !buf_add(query,sizeof(query),")")) {
        return 0;
      }
      first=0;
    }
    if (!buf_add(query,sizeof(query)," FROM `") ||
        !buf_add(query,sizeof(query),cfg->source[source].table) ||
        !buf_add(query,sizeof(query),"` WHERE epoch < ") ||
        !buf_add_num(query,sizeof(query),(long)start) ||
        !append_device_where(db,query,sizeof(query),cfg->source[source].device) ||
        !buf_add(query,sizeof(query)," ORDER BY epoch DESC LIMIT 1")) {
      return 0;
    }
    if (mysql_query(db,query)!=0) {
      return 0;
    }
    res=mysql_store_result(db);
    if (res==NULL) {
      return 0;
    }
    row=mysql_fetch_row(res);
    if (row!=NULL) {
      first=0;
      for (i=0;i<cfg->series_count;i++) {
        if (cfg->series[i].source!=source) {
          continue;
        }
        if (row[first]!=NULL) {
          v=strtod(row[first],NULL);
          if (cfg->series[i].delta==MODE_DELTA) {
            state[i].prev=v;
            state[i].has_prev=1;
          } else if (cfg->series[i].delta==MODE_TOTAL) {
            state[i].base=v;
            state[i].prev=v;
            state[i].last=0.0;
            state[i].has_base=1;
            state[i].has_prev=1;
            state[i].has_last=1;
          } else {
            state[i].last=v;
            state[i].has_last=1;
          }
        }
        first++;
      }
    }
    mysql_free_result(res);
  }
  return 1;
}

static int build_data_query(MYSQL *db, const Config *cfg, time_t start, time_t end,
    char *query, size_t cap) {
  char uni[MAX_QUERY];
  char alias[32];
  char expr[MAX_EXPR*2];
  int i;
  int source;

  if (!build_union(db,cfg,start,end,uni,sizeof(uni))) {
    return 0;
  }
  query[0]='\0';
  if (!buf_add(query,cap,"SELECT e.epoch")) {
    return 0;
  }
  for (i=0;i<cfg->series_count;i++) {
    sprintf(alias,"t%d",cfg->series[i].source);
    if (!qualify_expr(cfg->series[i].expr,alias,expr,sizeof(expr)) ||
        !buf_add(query,cap,",(") || !buf_add(query,cap,expr) ||
        !buf_add(query,cap,")")) {
      return 0;
    }
  }
  if (!buf_add(query,cap," FROM (") || !buf_add(query,cap,uni) ||
      !buf_add(query,cap,") e ")) {
    return 0;
  }
  for (source=0;source<cfg->source_count;source++) {
    sprintf(alias,"t%d",source);
    if (!buf_add(query,cap,"LEFT JOIN `") ||
        !buf_add(query,cap,cfg->source[source].table) ||
        !buf_add(query,cap,"` ") || !buf_add(query,cap,alias) ||
        !buf_add(query,cap," ON ") || !buf_add(query,cap,alias) ||
        !buf_add(query,cap,".epoch=e.epoch")) {
      return 0;
    }
    if (*cfg->source[source].device!='\0') {
      char escaped[MAX_NAME*2+1];
      unsigned long n;

      n=mysql_real_escape_string(db,escaped,cfg->source[source].device,
          (unsigned long)strlen(cfg->source[source].device));
      escaped[n]='\0';
      if (!buf_add(query,cap," AND ") || !buf_add(query,cap,alias) ||
          !buf_add(query,cap,".device='") || !buf_add(query,cap,escaped) ||
          !buf_add(query,cap,"'")) {
        return 0;
      }
    }
    if (!buf_add(query,cap," ")) {
      return 0;
    }
  }
  return buf_add(query,cap,"ORDER BY e.epoch");
}

static int alloc_output(OutputData *out, unsigned long capacity, int series_count) {
  size_t cells;

  memset(out,0,sizeof(*out));
  if (capacity==0) {
    return 1;
  }
  cells=(size_t)capacity*(size_t)series_count;
  if (series_count<=0 || cells/(size_t)series_count!=(size_t)capacity) {
    return 0;
  }
  out->epoch=(time_t *)malloc(sizeof(time_t)*(size_t)capacity);
  out->value=(double *)malloc(sizeof(double)*cells);
  out->valid=(unsigned char *)malloc(cells);
  if (out->epoch==NULL || out->value==NULL || out->valid==NULL) {
    free(out->epoch);
    free(out->value);
    free(out->valid);
    memset(out,0,sizeof(*out));
    return 0;
  }
  out->capacity=capacity;
  out->series_count=series_count;
  return 1;
}

static void free_output(OutputData *out) {
  free(out->epoch);
  free(out->value);
  free(out->valid);
  memset(out,0,sizeof(*out));
}

static int emit_bucket(const Config *cfg, SeriesState *state, AxisStats *axis,
    OutputData *out, time_t epoch) {
  unsigned long pos;
  double m;
  int i;
  int valid;

  if (out->count>=out->capacity) {
    return 0;
  }
  pos=out->count++;
  out->epoch[pos]=epoch;
  for (i=0;i<cfg->series_count;i++) {
    valid=1;
    if (state[i].count>0) {
      if (cfg->series[i].delta==MODE_VALUE) {
        m=state[i].acc/(double)state[i].count;
      } else {
        m=state[i].acc;
      }
      state[i].last=m;
      state[i].has_last=1;
    } else if (state[i].has_last) {
      m=state[i].last;
    } else if (cfg->series[i].delta==MODE_DELTA) {
      m=0.0;
    } else {
      m=0.0;
      valid=0;
    }
    out->valid[pos*(unsigned long)cfg->series_count+(unsigned long)i]=(unsigned char)valid;
    out->value[pos*(unsigned long)cfg->series_count+(unsigned long)i]=m;
    if (valid) {
      if (!axis[cfg->series[i].axis].valid) {
        axis[cfg->series[i].axis].min=m;
        axis[cfg->series[i].axis].max=m;
        axis[cfg->series[i].axis].valid=1;
      } else {
        if (m<axis[cfg->series[i].axis].min) {
          axis[cfg->series[i].axis].min=m;
        }
        if (m>axis[cfg->series[i].axis].max) {
          axis[cfg->series[i].axis].max=m;
        }
      }
    }
    state[i].acc=0.0;
    state[i].count=0;
  }
  return 1;
}

static int load_data(MYSQL *db, const Config *cfg, time_t start, time_t end,
    unsigned long raw_count, OutputData *out, AxisStats *axis) {
  MYSQL_RES *res;
  MYSQL_ROW row;
  SeriesState state[MAX_SERIES];
  char query[MAX_QUERY];
  unsigned long agg;
  unsigned long in_bucket;
  unsigned long capacity;
  time_t epoch;
  time_t last_epoch;
  double curr;
  double delta;
  int i;

  memset(state,0,sizeof(state));
  memset(axis,0,sizeof(AxisStats)*2);
  capacity=raw_count<cfg->points ? raw_count : cfg->points;
  if (!alloc_output(out,capacity,cfg->series_count)) {
    return 0;
  }
  if (raw_count==0) {
    return 1;
  }
  if (!init_states(db,cfg,start,state)) {
    free_output(out);
    return 0;
  }
  agg=(raw_count+cfg->points-1)/cfg->points;
  if (agg==0) {
    agg=1;
  }
  if (!build_data_query(db,cfg,start,end,query,sizeof(query)) || mysql_query(db,query)!=0) {
    free_output(out);
    return 0;
  }
  res=mysql_use_result(db);
  if (res==NULL) {
    free_output(out);
    return 0;
  }
  in_bucket=0;
  last_epoch=0;
  while ((row=mysql_fetch_row(res))!=NULL) {
    epoch=(time_t)strtol(row[0],NULL,10);
    last_epoch=epoch;
    for (i=0;i<cfg->series_count;i++) {
      if (row[i+1]==NULL) {
        continue;
      }
      curr=strtod(row[i+1],NULL);
      if (cfg->series[i].delta==MODE_DELTA) {
        if (state[i].has_prev) {
          delta=curr-state[i].prev;
          if (delta<0.0) {
            delta=curr;
          }
          state[i].acc+=delta;
          state[i].count++;
        }
        state[i].prev=curr;
        state[i].has_prev=1;
      } else if (cfg->series[i].delta==MODE_TOTAL) {
        if (!state[i].has_base) {
          state[i].base=curr;
          state[i].has_base=1;
        }
        state[i].acc=curr-state[i].base;
        state[i].count=1;
        state[i].prev=curr;
        state[i].has_prev=1;
      } else {
        state[i].acc+=curr;
        state[i].count++;
      }
    }
    in_bucket++;
    if (in_bucket>=agg) {
      if (!emit_bucket(cfg,state,axis,out,last_epoch)) {
        mysql_free_result(res);
        free_output(out);
        return 0;
      }
      in_bucket=0;
    }
  }
  mysql_free_result(res);
  if (in_bucket>0 && last_epoch!=0) {
    if (!emit_bucket(cfg,state,axis,out,last_epoch)) {
      free_output(out);
      return 0;
    }
  }
  for (i=0;i<2;i++) {
    if (axis[i].valid) {
      if ((i==0 && cfg->y_left_min0) || (i==1 && cfg->y_right_min0)) {
        if (axis[i].min>0.0) {
          axis[i].min=0.0;
        }
      }
      if (axis[i].min==axis[i].max) {
        axis[i].min-=1.0;
        axis[i].max+=1.0;
      }
    }
  }
  return 1;
}

static void html_escape(const char *s) {
  const unsigned char *p;

  for (p=(const unsigned char *)s; *p!='\0'; p++) {
    if (*p=='&') {
      fputs("&amp;",stdout);
    } else if (*p=='<') {
      fputs("&lt;",stdout);
    } else if (*p=='>') {
      fputs("&gt;",stdout);
    } else if (*p=='\"') {
      fputs("&quot;",stdout);
    } else if (*p=='\'') {
      fputs("&#39;",stdout);
    } else {
      fputc(*p,stdout);
    }
  }
}

static void format_time(time_t value, const char *fmt, char *out, size_t cap) {
  struct tm *t;

  t=localtime(&value);
  if (t==NULL || strftime(out,cap,fmt,t)==0) {
    out[0]='\0';
  }
}

static void period_label(time_t start, char mode, char *out) {
  char a[8];
  char b[8];
  time_t end;
  struct tm *t;

  t=localtime(&start);
  if (mode=='w') {
    end=add_days(start,6);
    format_time(start,"%d/%m",a,sizeof(a));
    format_time(end,"%d/%m",b,sizeof(b));
    sprintf(out,"Week %02d/%04d (%s - %s)",iso_week_number(start),t->tm_year+1900,a,b);
  } else if (mode=='m') {
    format_time(start,"%m/%Y",a,sizeof(a));
    sprintf(out,"Month %s",a);
  } else {
    format_time(start,"%d/%m/%Y",out,64);
  }
}

static double y_position(double value, const AxisStats *axis) {
  double height;

  height=(double)(SVG_HEIGHT-SVG_TOP-SVG_BOTTOM);
  return SVG_TOP+(axis->max-value)*height/(axis->max-axis->min);
}

static double x_position(unsigned long index, unsigned long count) {
  double width;

  width=(double)(SVG_WIDTH-SVG_LEFT-SVG_RIGHT);
  if (count<=1) {
    return SVG_LEFT+width*0.5;
  }
  return SVG_LEFT+width*(double)index/(double)(count-1);
}

static void draw_svg(const Config *cfg, const OutputData *out, const AxisStats *axis) {
  unsigned long i;
  unsigned long j;
  unsigned long idx;
  unsigned long ticks;
  unsigned long k;
  double x;
  double y;
  double value;
  double tick_value;
  char label[64];
  int s;
  int a;
  int segment;
  int mobile_hide;
  const char *color;

  printf("<svg class=\"graph\" viewBox=\"0 0 %d %d\" role=\"img\">\n",SVG_WIDTH,SVG_HEIGHT);
  printf("<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"white\" stroke=\"#adb5bd\"/>\n",
      SVG_LEFT,SVG_TOP,SVG_WIDTH-SVG_LEFT-SVG_RIGHT,SVG_HEIGHT-SVG_TOP-SVG_BOTTOM);
  for (j=0;j<=5;j++) {
    y=SVG_TOP+(double)(SVG_HEIGHT-SVG_TOP-SVG_BOTTOM)*(double)j/5.0;
    printf("<line x1=\"%d\" y1=\"%.2f\" x2=\"%d\" y2=\"%.2f\" stroke=\"#e9ecef\"/>\n",
        SVG_LEFT,y,SVG_WIDTH-SVG_RIGHT,y);
    for (a=0;a<2;a++) {
      if (!axis[a].valid) {
        continue;
      }
      tick_value=axis[a].max-(axis[a].max-axis[a].min)*(double)j/5.0;
      if (a==0) {
        printf("<text x=\"%d\" y=\"%.2f\" text-anchor=\"end\" dominant-baseline=\"middle\" class=\"axis-text\">",
            SVG_LEFT-8,y);
        printf(cfg->axis_left_format,tick_value);
        puts("</text>");
      } else {
        printf("<text x=\"%d\" y=\"%.2f\" text-anchor=\"start\" dominant-baseline=\"middle\" class=\"axis-text\">",
            SVG_WIDTH-SVG_RIGHT+8,y);
        printf(cfg->axis_right_format,tick_value);
        puts("</text>");
      }
    }
  }
  if (*cfg->axis_left!='\0') {
    printf("<text transform=\"translate(18 %d) rotate(-90)\" text-anchor=\"middle\" class=\"axis-title\">",SVG_HEIGHT/2);
    html_escape(cfg->axis_left);
    puts("</text>");
  }
  if (*cfg->axis_right!='\0') {
    printf("<text transform=\"translate(%d %d) rotate(90)\" text-anchor=\"middle\" class=\"axis-title\">",SVG_WIDTH-18,SVG_HEIGHT/2);
    html_escape(cfg->axis_right);
    puts("</text>");
  }
  if (out->count>0) {
    ticks=out->count<24 ? out->count : 24;
    for (j=0;j<ticks;j++) {
      if (ticks<=1) {
        i=0;
      } else {
        i=j*(out->count-1)/(ticks-1);
      }
      mobile_hide=0;
      if (ticks>12) {
        mobile_hide=1;
        for (k=0;k<12;k++) {
          if (j==k*(ticks-1)/11) {
            mobile_hide=0;
            break;
          }
        }
      }
      x=x_position(i,out->count);
      format_time(out->epoch[i],"%d/%m %H:%M",label,sizeof(label));
      printf("<line x1=\"%.2f\" y1=\"%d\" x2=\"%.2f\" y2=\"%d\" stroke=\"#dee2e6\"/>\n",
          x,SVG_HEIGHT-SVG_BOTTOM,x,SVG_HEIGHT-SVG_BOTTOM+5);
      printf("<text x=\"%.2f\" y=\"%d\" text-anchor=\"end\" transform=\"rotate(-50 %.2f %d)\" class=\"axis-text%s\">",
          x,SVG_HEIGHT-SVG_BOTTOM+18,x,SVG_HEIGHT-SVG_BOTTOM+18,
          mobile_hide ? " x-mobile-hide" : "");
      html_escape(label);
      puts("</text>");
    }
  }
  for (s=0;s<cfg->series_count;s++) {
    if (!axis[cfg->series[s].axis].valid) {
      continue;
    }
    color=graph_colors[(unsigned int)s%(sizeof(graph_colors)/sizeof(graph_colors[0]))];
    segment=0;
    for (i=0;i<out->count;i++) {
      idx=i*(unsigned long)cfg->series_count+(unsigned long)s;
      if (!out->valid[idx]) {
        if (segment) {
          puts("\"/>");
          segment=0;
        }
        continue;
      }
      value=out->value[idx];
      x=x_position(i,out->count);
      y=y_position(value,&axis[cfg->series[s].axis]);
      if (!segment) {
        printf("<polyline fill=\"none\" stroke=\"%s\" stroke-width=\"2\" vector-effect=\"non-scaling-stroke\" points=\"",color);
        segment=1;
      }
      printf("%.2f,%.2f ",x,y);
    }
    if (segment) {
      puts("\"/>");
    }
  }
  puts("</svg>");
}

static void draw_legend(const Config *cfg) {
  int i;
  const char *color;

  puts("<div class=\"legend\">");
  for (i=0;i<cfg->series_count;i++) {
    color=graph_colors[(unsigned int)i%(sizeof(graph_colors)/sizeof(graph_colors[0]))];
    printf("<span><i style=\"background:%s\"></i>",color);
    html_escape(cfg->series[i].label);
    printf("%s</span>\n",cfg->series[i].axis==0 ? "" : " (R)");
  }
  puts("</div>");
}

static void render_page(const Config *cfg, const OutputData *out, const AxisStats *axis,
    time_t start, time_t max_epoch, char mode, unsigned long raw_count) {
  time_t prev;
  time_t next;
  time_t limit;
  time_t today;
  time_t now;
  time_t mode_start;
  struct tm t;
  char q_prev[32];
  char q_next[32];
  char q_today[32];
  char q_day[32];
  char q_week[32];
  char q_month[32];
  char label[64];
  int future;

  limit=period_limit(max_epoch,mode);
  prev=period_shift(start,mode,-1);
  next=period_shift(start,mode,1);
  future=next>limit;
  now=time(NULL);
  t=*localtime(&now);
  today=day_start(t.tm_year+1900,t.tm_yday);
  build_q(prev,mode,q_prev);
  build_q(next,mode,q_next);
  build_q(today,mode,q_today);
  mode_start=start;
  build_q(mode_start,'d',q_day);
  build_q(mode_start,'w',q_week);
  build_q(mode_start,'m',q_month);
  period_label(start,mode,label);

  fputs("Content-Type: text/html; charset=utf-8\r\n\r\n",stdout);
  puts("<!doctype html><html lang=\"it\"><head><meta charset=\"utf-8\">");
  puts("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  printf("<title>");
  html_escape(cfg->title);
  printf(" - ");
  html_escape(label);
  puts("</title>");
  puts("<style>");
  puts("html,body{margin:0;padding:0;font-family:Arial,sans-serif;background:#fff;color:#212529}");
  puts("body{padding:4px}.toolbar{display:flex;flex-wrap:wrap;align-items:center;gap:4px;background:#f1f3f5;padding:5px 7px;border:1px solid #ced4da;border-radius:4px}");
  puts(".btn{display:inline-block;padding:4px 8px;background:#fff;color:#333;text-decoration:none;border:1px solid #adb5bd;border-radius:3px;font-weight:bold;font-size:13px}.btn.active{background:#0d6efd;color:#fff;border-color:#0d6efd}.btn.today{background:#198754;color:#fff;border-color:#198754}.btn.disabled{opacity:.4;pointer-events:none}.date{font-weight:bold;margin-left:4px}.stats{margin-left:auto;font-size:12px;color:#6c757d}.legend{display:flex;flex-wrap:wrap;justify-content:center;gap:12px;margin:8px 4px 0;font-size:12px}.legend i{display:inline-block;width:18px;height:3px;vertical-align:middle;margin-right:5px}.graph{display:block;width:100%;height:calc(100vh - 105px);min-height:420px}.axis-text{font:11px Arial,sans-serif;fill:#495057}.axis-title{font:bold 12px Arial,sans-serif;fill:#343a40}@media(max-width:600px){.x-mobile-hide{display:none}.stats{width:100%;margin-left:0}.graph{height:calc(100vh - 140px)}}");
  puts("</style></head><body>");
  puts("<div class=\"toolbar\">");
  printf("<a class=\"btn\" href=\"?q=%s\">&laquo;</a>\n",q_prev);
  if (future) {
    puts("<span class=\"btn disabled\">&raquo;</span>");
  } else {
    printf("<a class=\"btn\" href=\"?q=%s\">&raquo;</a>\n",q_next);
  }
  printf("<a class=\"btn today\" href=\"?q=%s\">TODAY</a>\n",q_today);
  printf("<a class=\"btn%s\" href=\"?q=%s\">DAY</a>\n",mode=='d' ? " active" : "",q_day);
  printf("<a class=\"btn%s\" href=\"?q=%s\">WEEK</a>\n",mode=='w' ? " active" : "",q_week);
  printf("<a class=\"btn%s\" href=\"?q=%s\">MONTH</a>\n",mode=='m' ? " active" : "",q_month);
  printf("<a class=\"btn\" href=\"?q=");
  if (mode=='d') {
    fputs(q_day,stdout);
  } else if (mode=='w') {
    fputs(q_week,stdout);
  } else {
    fputs(q_month,stdout);
  }
  puts("\">&#128259;</a>");
  printf("<span class=\"date\">");
  html_escape(label);
  printf("</span><span class=\"stats\">raw %lu &middot; plotted %lu &middot; target %lu</span>\n",
      raw_count,out->count,cfg->points);
  puts("</div>");
  printf("<h3 style=\"text-align:center;margin:8px 0 0\">");
  html_escape(cfg->title);
  puts("</h3>");
  draw_legend(cfg);
  draw_svg(cfg,out,axis);
  puts("</body></html>");
}

static void render_error(const char *message) {
  fputs("Status: 500 Internal Server Error\r\n",stdout);
  fputs("Content-Type: text/plain; charset=utf-8\r\n\r\n",stdout);
  fputs("data: ",stdout);
  fputs(message,stdout);
  fputc('\n',stdout);
}

static int build_config_path(char *out, size_t cap, const char *argv0) {
  const char *root;
  const char *name;
  const char *file;
  const char *slash;
  size_t n;

  root=getenv("DOCUMENT_ROOT");
  name=getenv("SCRIPT_NAME");
  if (root!=NULL && *root!='\0' && name!=NULL && *name=='/') {
    slash=strrchr(name,'/');
    n=(size_t)(slash-name);
    if (strlen(root)+n+strlen("/data.conf")+1>cap) {
      return 0;
    }
    strcpy(out,root);
    strncat(out,name,n);
    strcat(out,"/data.conf");
    return 1;
  }
  file=getenv("SCRIPT_FILENAME");
  if (file==NULL || *file=='\0') {
    file=argv0;
  }
  if (file==NULL || *file=='\0') {
    return 0;
  }
  slash=strrchr(file,'/');
  if (slash==NULL) {
    copy_text(out,cap,CONFIG_FILE);
    return 1;
  }
  n=(size_t)(slash-file)+1;
  if (n+strlen(CONFIG_FILE)+1>cap) {
    return 0;
  }
  memcpy(out,file,n);
  strcpy(out+n,CONFIG_FILE);
  return 1;
}

int main(int argc, char **argv) {
  Config cfg;
  OutputData out;
  AxisStats axis[2];
  MYSQL *db;
  char config_path[MAX_PATH_LEN];
  char err[512];
  char q[64];
  char mode;
  time_t max_epoch;
  time_t start;
  time_t end;
  time_t limit;
  unsigned long raw_count;

  putenv("TZ=Europe/Rome");
  tzset();
  (void)argc;
  if (!build_config_path(config_path,sizeof(config_path),argv[0])) {
    render_error("cannot determine data.conf path");
    return 1;
  }
  if (!load_config(config_path,&cfg,err,sizeof(err))) {
    render_error(err);
    return 1;
  }
  db=open_db(&cfg,err,sizeof(err));
  if (db==NULL) {
    render_error(err);
    return 1;
  }
  max_epoch=time(NULL);
  if (!query_max_epoch(db,&cfg,&max_epoch)) {
    copy_text(err,sizeof(err),mysql_error(db));
    mysql_close(db);
    render_error(*err!='\0' ? err : "cannot determine latest epoch");
    return 1;
  }
  query_q(q,sizeof(q));
  if (!parse_q(*q=='\0' ? NULL : q,max_epoch,&mode,&start,&end)) {
    mysql_close(db);
    render_error("invalid q parameter");
    return 1;
  }
  limit=period_limit(max_epoch,mode);
  if (start>limit) {
    start=limit;
    if (mode=='w') {
      end=add_days(start,7)-1;
    } else if (mode=='m') {
      end=add_months(start,1)-1;
    } else {
      end=end_of_day(start);
    }
  }
  raw_count=0;
  if (!query_count(db,&cfg,start,end,&raw_count)) {
    copy_text(err,sizeof(err),mysql_error(db));
    mysql_close(db);
    render_error(*err!='\0' ? err : "count query failed");
    return 1;
  }
  if (!load_data(db,&cfg,start,end,raw_count,&out,axis)) {
    copy_text(err,sizeof(err),mysql_error(db));
    mysql_close(db);
    render_error(*err!='\0' ? err : "data query failed");
    return 1;
  }
  mysql_close(db);
  render_page(&cfg,&out,axis,start,max_epoch,mode,raw_count);
  free_output(&out);
  return 0;
}
