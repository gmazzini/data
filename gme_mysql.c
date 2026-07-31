#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <zip.h>
#include <mysql/mysql.h>
#include "/home/tools/setup_energy.c"

struct mem {
  char *ptr;
  size_t len;
};

static void mem_init(struct mem *m) {
  m->len=0;
  m->ptr=(char*)malloc(1);
  if(m->ptr) m->ptr[0]='\0';
}

static size_t write_cb(void *contents,size_t size,size_t nmemb,void *userp) {
  size_t real_size;
  struct mem *m;
  char *p;

  real_size=size*nmemb;
  m=(struct mem*)userp;
  p=(char*)realloc(m->ptr,m->len+real_size+1);
  if(!p) return 0;
  m->ptr=p;
  memcpy(m->ptr+m->len,contents,real_size);
  m->len=m->len+real_size;
  m->ptr[m->len]='\0';
  return real_size;
}

int main(int argc,char *argv[]) {
  CURL *curl;
  struct curl_slist *headers;
  struct mem auth_body,req_body;
  MYSQL *con;
  zip_error_t zip_error;
  zip_source_t *zip_source;
  zip_t *zip_archive;
  zip_file_t *zip_file;
  zip_stat_t zip_stat;
  zip_int64_t entry_count;
  time_t E0,current_epoch;
  struct tm input_tm;
  char auth_post[1024],req_post[2048],auth_header[8192],gme_token[4096];
  char price_values[100][64],one_price[64],query[512];
  char *p,*q,*r,*s,*e,*b64,*json_buf;
  unsigned char *zip_buf,*file_buf;
  double price_val;
  int year,month,day,len,out_len,val,valb,c,i,j,count,err;

  // Check command line arguments
  if(argc!=4) {
    fprintf(stderr,"Usage: %s LOGIN PASSWORD YYYYMMDD\n",argv[0]);
    return 1;
  }

  if(strlen(argv[3])!=8) {
    fprintf(stderr,"date format must be YYYYMMDD\n");
    return 1;
  }

  // Parse input date YYYYMMDD
  year=(argv[3][0]-'0')*1000+(argv[3][1]-'0')*100+(argv[3][2]-'0')*10+(argv[3][3]-'0');
  month=(argv[3][4]-'0')*10+(argv[3][5]-'0');
  day=(argv[3][6]-'0')*10+(argv[3][7]-'0');

  // Set timezone to Europe/Rome for accurate midnight UTC epoch conversion
  setenv("TZ","Europe/Rome",1);
  tzset();

  memset(&input_tm,0,sizeof(input_tm));
  input_tm.tm_year=year-1900;
  input_tm.tm_mon=month-1;
  input_tm.tm_mday=day;
  input_tm.tm_hour=0;
  input_tm.tm_min=0;
  input_tm.tm_sec=0;
  input_tm.tm_isdst=-1;
  E0=mktime(&input_tm);
  if(E0==(time_t)-1) {
    fprintf(stderr,"date conversion error\n");
    return 1;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  mem_init(&auth_body);
  mem_init(&req_body);

  // Authenticate against GME API
  snprintf(auth_post,sizeof(auth_post),"{\"Login\":\"%s\",\"Password\":\"%s\"}",argv[1],argv[2]);

  curl=curl_easy_init();
  if(!curl) {
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  headers=NULL;
  headers=curl_slist_append(headers,"Content-Type: application/json");

  curl_easy_setopt(curl,CURLOPT_URL,"https://api.mercatoelettrico.org/request/api/v1/Auth");
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,auth_post);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(auth_post));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&auth_body);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);

  if(curl_easy_perform(curl)!=CURLE_OK) {
    fprintf(stderr,"auth curl error\n");
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  // Extract JWT token
  p=strstr(auth_body.ptr,"\"token\"");
  if(p==NULL) {
    printf("%s\n",auth_body.ptr);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  p=strchr(p,':');
  p=strchr(p,'"');
  p=p+1;
  q=strchr(p,'"');

  i=0;
  while(p<q&&i<(int)sizeof(gme_token)-1) {
    gme_token[i]=*p;
    i++;
    p++;
  }
  gme_token[i]=0;

  // Request price data from GME API
  snprintf(req_post,sizeof(req_post),
    "{\"Platform\":\"PublicMarketResults\","
    "\"Segment\":\"MGP\","
    "\"DataName\":\"ME_ZonalPrices\","
    "\"IntervalStart\":\"%s\","
    "\"IntervalEnd\":\"%s\","
    "\"Attributes\":{\"GranularityType\":\"PT15\"}}",
    argv[3],argv[3]);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  curl=curl_easy_init();
  if(!curl) {
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  headers=NULL;
  headers=curl_slist_append(headers,"Content-Type: application/json");
  snprintf(auth_header,sizeof(auth_header),"Authorization: Bearer %s",gme_token);
  headers=curl_slist_append(headers,auth_header);

  curl_easy_setopt(curl,CURLOPT_URL,"https://api.mercatoelettrico.org/request/api/v1/RequestData");
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,req_post);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(req_post));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&req_body);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);

  if(curl_easy_perform(curl)!=CURLE_OK) {
    fprintf(stderr,"request curl error\n");
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  // Extract Base64 response string
  p=strstr(req_body.ptr,"\"contentResponse\"");
  if(p==NULL) {
    printf("%s\n",req_body.ptr);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  p=strchr(p,':');
  p=strchr(p,'"');
  p=p+1;
  q=strchr(p,'"');

  len=(int)(q-p);
  b64=(char*)malloc(len+1);
  for(i=0;i<len;i++) b64[i]=p[i];
  b64[len]=0;

  // Decode Base64 buffer
  zip_buf=(unsigned char*)malloc((len*3)/4+4);
  val=0;
  valb=-8;
  out_len=0;

  for(i=0;i<len;i++) {
    c=b64[i];
    if(c>='A'&&c<='Z') c=c-'A';
    else if(c>='a'&&c<='z') c=c-'a'+26;
    else if(c>='0'&&c<='9') c=c-'0'+52;
    else if(c=='+') c=62;
    else if(c=='/') c=63;
    else if(c=='=') break;
    else continue;

    val=(val<<6)+c;
    valb=valb+6;

    if(valb>=0) {
      zip_buf[out_len]=(unsigned char)((val>>valb)&255);
      out_len++;
      valb=valb-8;
    }
  }

  // Extract zip archive in memory
  zip_error_init(&zip_error);
  zip_source=zip_source_buffer_create(zip_buf,out_len,0,&zip_error);
  if(zip_source==NULL) {
    fprintf(stderr,"zip source error\n");
    free(b64);
    free(zip_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  zip_archive=zip_open_from_source(zip_source,0,&zip_error);
  if(zip_archive==NULL) {
    fprintf(stderr,"zip open error\n");
    zip_source_free(zip_source);
    free(b64);
    free(zip_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  entry_count=zip_get_num_entries(zip_archive,0);
  if(entry_count<1) {
    fprintf(stderr,"zip empty\n");
    zip_close(zip_archive);
    free(b64);
    free(zip_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  zip_stat_init(&zip_stat);
  err=zip_stat_index(zip_archive,0,0,&zip_stat);
  if(err!=0) {
    fprintf(stderr,"zip stat error\n");
    zip_close(zip_archive);
    free(b64);
    free(zip_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  zip_file=zip_fopen_index(zip_archive,0,0);
  if(zip_file==NULL) {
    fprintf(stderr,"zip fopen error\n");
    zip_close(zip_archive);
    free(b64);
    free(zip_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  file_buf=(unsigned char*)malloc((size_t)zip_stat.size+1);
  zip_fread(zip_file,file_buf,zip_stat.size);
  file_buf[zip_stat.size]=0;
  zip_fclose(zip_file);
  zip_close(zip_archive);

  // Parse extracted JSON for PUN zone prices
  json_buf=(char*)file_buf;
  r=json_buf;
  count=0;

  while(1) {
    r=strstr(r,"\"Zone\":\"PUN\"");
    if(r==NULL) break;

    s=strstr(r,"\"Price\"");
    if(s==NULL) break;

    s=strchr(s,':');
    if(s==NULL) break;
    s=s+1;

    // Skip spaces and quotation marks before the numeric value
    while(*s==' '||*s=='"') s=s+1;

    e=s;
    // Find the end of the number (quote, comma, brace, bracket or whitespace)
    while(*e!='"'&&*e!=','&&*e!='}'&&*e!=']'&&*e!='\r'&&*e!='\n'&&*e!=0) e=e+1;

    j=0;
    while(s<e&&j<(int)sizeof(one_price)-1) {
      if(*s==',') one_price[j]='.';
      else one_price[j]=*s;
      j++;
      s=s+1;
    }
    one_price[j]=0;

    if(count<100) {
      strcpy(price_values[count],one_price);
    }
    count++;

    r=r+10;
  }

  // Validate record count (expects 92, 96 or 100 entries)
  if(count!=92&&count!=96&&count!=100) {
    fprintf(stderr,"found %d prices, expected 92, 96 or 100\n",count);
    free(b64);
    free(zip_buf);
    free(file_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  // Connect to MySQL database
  con=mysql_init(NULL);
  if(con==NULL) {
    fprintf(stderr,"mysql init error\n");
    free(b64);
    free(zip_buf);
    free(file_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  if(mysql_real_connect(con,"localhost",USER,PASSWORD,DB,0,NULL,0)==NULL) {
    fprintf(stderr,"mysql connect error: %s\n",mysql_error(con));
    mysql_close(con);
    free(b64);
    free(zip_buf);
    free(file_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    return 1;
  }

  // Insert or overwrite records into table pun_15m
  for(i=0;i<count;i++) {
    current_epoch=E0+(i*900);
    price_val=atof(price_values[i]);

    snprintf(query,sizeof(query),
      "insert into pun_15m (epoch,c) values(%ld,%.5f) on duplicate key update c=%.5f",
      (long)current_epoch,price_val,price_val);

    if(mysql_query(con,query)!=0) {
      fprintf(stderr,"mysql query error: %s\n",mysql_error(con));
    }
  }

  printf("COUNT=%d\n",count);

  // Cleanup resources
  mysql_close(con);
  free(b64);
  free(zip_buf);
  free(file_buf);
  free(auth_body.ptr);
  free(req_body.ptr);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  curl_global_cleanup();

  return 0;
}
