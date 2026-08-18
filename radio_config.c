// Gianluca Mazzini @2022- Version 3.01

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio_config.h"

#define LINE_SIZE 2048

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

static int copy_value(char *dst,unsigned long cap,const char *src) {
  unsigned long n;

  n=(unsigned long)strlen(src);
  if (cap==0 || n>=cap) return 0;
  memcpy(dst,src,n+1);
  return 1;
}

static int parse_uint(const char *s,unsigned int min,unsigned int max,unsigned int *value) {
  char *end;
  unsigned long n;

  errno=0;
  n=strtoul(s,&end,10);
  if (errno!=0 || end==s || *end!='\0' || n<min || n>max) return 0;
  *value=(unsigned int)n;
  return 1;
}

static void set_error(char *err,unsigned long cap,const char *text,int line) {
  if (err==NULL || cap==0) return;
  if (line>0) snprintf(err,(size_t)cap,"%s at line %d",text,line);
  else snprintf(err,(size_t)cap,"%s",text);
}

int radio_config_load(RadioConfig *cfg,const char *path,char *err,unsigned long errcap) {
  FILE *fp;
  char line[LINE_SIZE],*key,*value,*eq;
  int lineno;

  if (cfg==NULL || path==NULL) {
    set_error(err,errcap,"invalid radio configuration arguments",0);
    return 0;
  }

  memset(cfg,0,sizeof(*cfg));
  copy_value(cfg->db_host,sizeof(cfg->db_host),"127.0.0.1");
  cfg->db_port=3306;
  cfg->adif_port=2333;
  copy_value(cfg->dxcluster_local_host,sizeof(cfg->dxcluster_local_host),"127.0.0.1");
  cfg->dxcluster_local_port=22222;
  cfg->dxcluster_timeout=300;
  cfg->dxcluster_reconnect=5;
  copy_value(cfg->callbook_local_host,sizeof(cfg->callbook_local_host),"127.0.0.1");
  cfg->callbook_local_port=22223;
  cfg->callbook_connect_timeout=5;
  cfg->callbook_http_timeout=15;
  copy_value(cfg->qrz_agent,sizeof(cfg->qrz_agent),"GM-radio-3.00");
  copy_value(cfg->qrz_ru_agent,sizeof(cfg->qrz_ru_agent),"GM-radio-3.00");
  cfg->qrz_ru_session_ttl=3600;
  cfg->qrz_ru_min_interval=3;

  fp=fopen(path,"r");
  if (fp==NULL) {
    set_error(err,errcap,"cannot open radio configuration",0);
    return 0;
  }

  lineno=0;
  while (fgets(line,sizeof(line),fp)!=NULL) {
    lineno++;
    key=trim(line);
    if (*key=='\0' || *key=='#') continue;
    eq=strchr(key,'=');
    if (eq==NULL) {
      fclose(fp);
      set_error(err,errcap,"invalid radio configuration line",lineno);
      return 0;
    }
    *eq='\0';
    value=trim(eq+1);
    key=trim(key);

    if (strcmp(key,"db_host")==0) {
      if (!copy_value(cfg->db_host,sizeof(cfg->db_host),value)) goto value_error;
    } else if (strcmp(key,"db_user")==0) {
      if (!copy_value(cfg->db_user,sizeof(cfg->db_user),value)) goto value_error;
    } else if (strcmp(key,"db_pass")==0) {
      if (!copy_value(cfg->db_pass,sizeof(cfg->db_pass),value)) goto value_error;
    } else if (strcmp(key,"db_name")==0) {
      if (!copy_value(cfg->db_name,sizeof(cfg->db_name),value)) goto value_error;
    } else if (strcmp(key,"db_port")==0) {
      if (!parse_uint(value,1,65535,&cfg->db_port)) goto value_error;
    } else if (strcmp(key,"adif_secret")==0) {
      if (!copy_value(cfg->adif_secret,sizeof(cfg->adif_secret),value)) goto value_error;
    } else if (strcmp(key,"adif_port")==0) {
      if (!parse_uint(value,1,65535,&cfg->adif_port)) goto value_error;
    } else if (strcmp(key,"dxcluster_host")==0) {
      if (!copy_value(cfg->dxcluster_host,sizeof(cfg->dxcluster_host),value)) goto value_error;
    } else if (strcmp(key,"dxcluster_port")==0) {
      if (!parse_uint(value,1,65535,&cfg->dxcluster_port)) goto value_error;
    } else if (strcmp(key,"dxcluster_callsign")==0) {
      if (!copy_value(cfg->dxcluster_callsign,sizeof(cfg->dxcluster_callsign),value)) goto value_error;
    } else if (strcmp(key,"dxcluster_local_host")==0) {
      if (!copy_value(cfg->dxcluster_local_host,sizeof(cfg->dxcluster_local_host),value)) goto value_error;
    } else if (strcmp(key,"dxcluster_local_port")==0) {
      if (!parse_uint(value,1,65535,&cfg->dxcluster_local_port)) goto value_error;
    } else if (strcmp(key,"dxcluster_timeout")==0) {
      if (!parse_uint(value,1,86400,&cfg->dxcluster_timeout)) goto value_error;
    } else if (strcmp(key,"dxcluster_reconnect")==0) {
      if (!parse_uint(value,1,3600,&cfg->dxcluster_reconnect)) goto value_error;
    } else if (strcmp(key,"callbook_local_host")==0) {
      if (!copy_value(cfg->callbook_local_host,sizeof(cfg->callbook_local_host),value)) goto value_error;
    } else if (strcmp(key,"callbook_local_port")==0) {
      if (!parse_uint(value,1,65535,&cfg->callbook_local_port)) goto value_error;
    } else if (strcmp(key,"callbook_connect_timeout")==0) {
      if (!parse_uint(value,1,300,&cfg->callbook_connect_timeout)) goto value_error;
    } else if (strcmp(key,"callbook_http_timeout")==0) {
      if (!parse_uint(value,1,600,&cfg->callbook_http_timeout)) goto value_error;
    } else if (strcmp(key,"qrz_user")==0) {
      if (!copy_value(cfg->qrz_user,sizeof(cfg->qrz_user),value)) goto value_error;
    } else if (strcmp(key,"qrz_pass")==0) {
      if (!copy_value(cfg->qrz_pass,sizeof(cfg->qrz_pass),value)) goto value_error;
    } else if (strcmp(key,"qrz_agent")==0) {
      if (!copy_value(cfg->qrz_agent,sizeof(cfg->qrz_agent),value)) goto value_error;
    } else if (strcmp(key,"qrz_ru_user")==0) {
      if (!copy_value(cfg->qrz_ru_user,sizeof(cfg->qrz_ru_user),value)) goto value_error;
    } else if (strcmp(key,"qrz_ru_pass")==0) {
      if (!copy_value(cfg->qrz_ru_pass,sizeof(cfg->qrz_ru_pass),value)) goto value_error;
    } else if (strcmp(key,"qrz_ru_agent")==0) {
      if (!copy_value(cfg->qrz_ru_agent,sizeof(cfg->qrz_ru_agent),value)) goto value_error;
    } else if (strcmp(key,"qrz_ru_session_ttl")==0) {
      if (!parse_uint(value,60,86400,&cfg->qrz_ru_session_ttl)) goto value_error;
    } else if (strcmp(key,"qrz_ru_min_interval")==0) {
      if (!parse_uint(value,1,60,&cfg->qrz_ru_min_interval)) goto value_error;
    } else {
      fclose(fp);
      set_error(err,errcap,"unknown radio configuration key",lineno);
      return 0;
    }
    continue;

value_error:
    fclose(fp);
    set_error(err,errcap,"invalid radio configuration value",lineno);
    return 0;
  }
  fclose(fp);

  return 1;
}
