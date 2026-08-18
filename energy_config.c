// Gianluca Mazzini @2026- Version 1.00

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "energy_config.h"

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

static int copy_value(char *dst, unsigned long cap, const char *src) {
  unsigned long n;

  n=(unsigned long)strlen(src);
  if (cap==0 || n>=cap) {
    return 0;
  }
  memcpy(dst,src,n+1);
  return 1;
}

static int parse_port(const char *s, unsigned int *port) {
  char *end;
  unsigned long n;

  errno=0;
  n=strtoul(s,&end,10);
  if (errno!=0 || end==s || *end!='\0' || n==0 || n>65535) {
    return 0;
  }
  *port=(unsigned int)n;
  return 1;
}

static void set_error(char *err, unsigned long cap, const char *text, int line) {
  if (err==NULL || cap==0) {
    return;
  }
  if (line>0) {
    snprintf(err,(size_t)cap,"%s at line %d",text,line);
  } else {
    snprintf(err,(size_t)cap,"%s",text);
  }
}

int energy_config_load(EnergyConfig *cfg, const char *path, char *err, unsigned long errcap) {
  FILE *fp;
  char line[LINE_SIZE];
  char *key, *value, *eq;
  int lineno;

  if (cfg==NULL || path==NULL) {
    set_error(err,errcap,"invalid energy configuration arguments",0);
    return 0;
  }

  memset(cfg,0,sizeof(*cfg));
  copy_value(cfg->db_host,sizeof(cfg->db_host),"127.0.0.1");
  cfg->db_port=3306;

  fp=fopen(path,"r");
  if (fp==NULL) {
    set_error(err,errcap,"cannot open energy configuration",0);
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
      fclose(fp);
      set_error(err,errcap,"invalid energy configuration line",lineno);
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
      if (!parse_port(value,&cfg->db_port)) goto value_error;
    } else if (strcmp(key,"so_host")==0) {
      if (!copy_value(cfg->so_host,sizeof(cfg->so_host),value)) goto value_error;
    } else if (strcmp(key,"so_port")==0) {
      if (!parse_port(value,&cfg->so_port)) goto value_error;
    } else if (strcmp(key,"cc1_host")==0) {
      if (!copy_value(cfg->cc1_host,sizeof(cfg->cc1_host),value)) goto value_error;
    } else if (strcmp(key,"cc1_port")==0) {
      if (!parse_port(value,&cfg->cc1_port)) goto value_error;
    } else if (strcmp(key,"cc2_host")==0) {
      if (!copy_value(cfg->cc2_host,sizeof(cfg->cc2_host),value)) goto value_error;
    } else if (strcmp(key,"cc2_port")==0) {
      if (!parse_port(value,&cfg->cc2_port)) goto value_error;
    } else if (strcmp(key,"cc3_host")==0) {
      if (!copy_value(cfg->cc3_host,sizeof(cfg->cc3_host),value)) goto value_error;
    } else if (strcmp(key,"cc3_port")==0) {
      if (!parse_port(value,&cfg->cc3_port)) goto value_error;
    } else {
      fclose(fp);
      set_error(err,errcap,"unknown energy configuration key",lineno);
      return 0;
    }
    continue;

value_error:
    fclose(fp);
    set_error(err,errcap,"invalid energy configuration value",lineno);
    return 0;
  }
  fclose(fp);

  if (*cfg->db_host=='\0' || *cfg->db_user=='\0' || *cfg->db_name=='\0' ||
      *cfg->so_host=='\0' || cfg->so_port==0 || *cfg->cc1_host=='\0' ||
      cfg->cc1_port==0 || *cfg->cc2_host=='\0' || cfg->cc2_port==0 ||
      *cfg->cc3_host=='\0' || cfg->cc3_port==0) {
    set_error(err,errcap,"missing required energy configuration value",0);
    return 0;
  }
  return 1;
}
