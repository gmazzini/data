// pproc.c log data processing by GM @2025 V 2.0
#include "/home/tools/log/pfunc.c"
#include "/home/tools/log/pscore.c"

int main(void){
  int c,act,vv,gg,s,mypage,f1,cached;
  char buf[1000],aux1[300],aux2[300],aux3[300],aux4[300],aux5[300],aux6[300],aux7[300],aux8[300],aux9[300],aux0[300],tok[13][100],mycall[16],*ff,*pp,*qq,*save1,*save2,*p1,*p2,*p3,*p4;
  struct tm ts,*tm_now;
  uint8_t in[4];
  uint32_t t;
  time_t epoch,td;
  long l1,l2,l3,l4,idx,suml[10],lff,nnn,ppp,qqq;
  long long ll1,ll2,ll3;
  MYSQL *con;
  MYSQL_RES *res;
  MYSQL_ROW row,row1;
  FILE *fp;
  double lat1,lat2,lon1,lon2,fx,f6,f7,f8;
  struct sockaddr_in a;
  const char *l11[]={"call","band","mode","lotw","eqsl","qrz"};
 
  data3=(struct data3 ***)malloc(TOT3*sizeof(struct data3 **)); ndata3=malloc(TOT3*sizeof(long *));
  for(l1=0;l1<TOT3;l1++){
    data3[l1]=(struct data3 **)malloc(TOTL2*sizeof(struct data3 *));
    ndata3[l1]=malloc(TOTL2*sizeof(long));
    for(l2=0;l2<TOTL2;l2++)data3[l1][l2]=(struct data3 *)malloc(TOTL3*sizeof(struct data3));
  }
  ff=(char *)malloc((MAXFF+1)*sizeof(char));
  // reading elements in csv with last file ff in base64 decoded with assuntion last quartet not usefull
  // 0:ota 1:btn.id 2:base 3:mypage 4:call 5:freq 6:mode 7:sigtx 8:sigrx 9:contest 10:contx 11:conrx 12:cluster|start 13:FILE
  for(vv=0,gg=0,lff=0;;){
    c=getchar();
    if(c==EOF)break;
    if(c==','){tok[vv][gg]='\0'; vv++; gg=0; continue;}
    if(vv<13)tok[vv][gg++]=(char)c;
    else {
      if(c=='=')break;
      in[gg]=c;
      if(gg==3){
        t=((uint32_t)B64DEC[in[0]] << 18) | ((uint32_t)B64DEC[in[1]] << 12) | ((uint32_t)B64DEC[in[2]] <<  6) | ((uint32_t)B64DEC[in[3]]);
        if(lff<MAXFF)ff[lff++]=(uint8_t)(t >> 16);
        if(lff<MAXFF)ff[lff++]=(uint8_t)(t >> 8);
        if(lff<MAXFF)ff[lff++]=(uint8_t)(t);
        gg=0;
      }
      else gg++;
    }
  }
  ff[lff]='\0';
  mypage=atoi(tok[3]);

  con=mysql_init(NULL);
  if(con==NULL)exit(1);
  if(mysql_real_connect(con,dbhost,dbuser,dbpassword,dbname,0,NULL,0)==NULL)exit(1);
  mysql_query(con,"SET time_zone='+00:00'");
  sprintf(buf,"select mycall from user where ota='%s' and lastota+durationota>%ld limit 1",tok[0],time(NULL));
  mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res);
  if(row==NULL){
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n");
    printf("<pre><b>Login expired</b>\nPlease login again\n</pre>");
    mysql_free_result(res);
    goto end;
  }
  else strcpy(mycall,row[0]);
  mysql_free_result(res);
  act=0; if(tok[1][0]=='a')act=atoi(tok[1]+1);

  if(act==5){ // Go button with date in call input and format YYYYMMDD
    printf("Content-Type: text/plain\r\n\r\n");
    sprintf(buf,"select count(*) from log where mycall='%s' and open>=%lld order by open",mycall,dt2e(tok[4],"00:00:00"));
    mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res);
    l1=atol(row[0]);
    mysql_free_result(res);
    printf("%ld\n",l1);
    goto end;
  }

  if((act>=1 && act<=8) || (act>=28 && act<=30)){ // List buttons(4: 1 2 3 4) and List Find buttons(3: 5 6 7) and List Contest buttons(3:28 29 30) 
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    printf("<pre>");
    printf("<p class=\"myh1\">%22s%5s %16s %10s %4s %5s %5s</p>","DATETIME","LEN","CALLSIGN","FREQ","MODE","SIGTX","SIGRX");
    if(act<=5)sprintf(buf,"select open,close,callsign,freqtx,freqrx,mode,signaltx,signalrx,lotw,eqsl,qrz,contesttx,contestrx,contest from log where mycall='%s' order by open desc, callsign desc limit %d offset %ld",mycall,mypage,atol(tok[2]));
    else if(act<=8)sprintf(buf,"select open,close,callsign,freqtx,freqrx,mode,signaltx,signalrx,lotw,eqsl,qrz,contesttx,contestrx,contest from log where callsign like '%s' and mycall='%s' order by open desc, callsign desc limit %d offset %ld",tok[4],mycall,mypage,atol(tok[2]));
    else sprintf(buf,"select open,close,callsign,freqtx,freqrx,mode,signaltx,signalrx,lotw,eqsl,qrz,contesttx,contestrx,contest from log where contest='%s' and mycall='%s' order by open desc, callsign desc limit %d offset %ld",tok[9],mycall,mypage,atol(tok[2]));
    mysql_query(con,buf);
    res=mysql_store_result(con);
    for(;;){
      row=mysql_fetch_row(res);
      if(row==NULL)break;
      aux1[0]='\0';
      if(atoi(row[8])==1)strcat(aux1,"L");
      if(atoi(row[9])==1)strcat(aux1,"E");
      if(atoi(row[10])==1)strcat(aux1,"Q");
      td=atoll(row[1])-atoll(row[0]);
      if(td==0)strcpy(aux2,"(0s)");
      else if(td<60)sprintf(aux2,"(%lds)",td);
      else if(td<3600)sprintf(aux2,"(%ldm)",td/60);
      else sprintf(aux2,"(%ldh)",td/3600);
      printf("<button type=\"button\" class=\"myb2\" onclick=\"cmd1(%lld,'%s')\"> </button> ",atoll(row[0]),row[2]);
      printf("%s%5s <b>%16s</b> %10.1f %4s %5s %5s %-3s ",e2dtc(atoll(row[0])),aux2,row[2],atol(row[3])/1000.0,row[5],row[6],row[7],aux1);
      if(row[13][0]!='\0')printf(" (%s,%s,%s)",row[13],row[11],row[12]);
      if(atol(row[4])>0&&atol(row[4])!=atol(row[3]))printf(" [%+.1f]",(atol(row[4])-atol(row[3]))/1000.0);
      printf("\n");
    }
    mysql_free_result(res);
    printf("</pre>");
    goto end;
  }
 
  if(act==9){ // dxcc and qrz solve unset button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    printf("<pre>");
    l1=l2=0;
    sprintf(buf,"select open,callsign from log where mycall='%s' and dxcc=0",mycall);
    mysql_query(con,buf);
    res=mysql_store_result(con);
    for(;;){
      row=mysql_fetch_row(res);
      if(row==NULL)break;
      searchcty(con,row[1]);
      if(mycty[0][0]!='\0'){
        sprintf(aux1,"Update log set dxcc=%d where mycall='%s' and open=%lld and callsign='%s' and dxcc=0",atoi(row1[2]),mycall,atoll(row[0]),row[1]);
        mysql_query(con,aux1);
        l1++;
      }
      else l2++;
    }
    mysql_free_result(res);
    printf("Set dxcc: %ld\nNot found dxcc: %ld\n",l1,l2);
    printf("</pre>");
    goto end;
  }

  if(act==10){ // Report button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    printf("<pre>");
    for(l1=0;l1<TOT3;l1++)for(l2=0;l2<TOTL2;l2++)ndata3[l1][l2]=0;
    sprintf(buf,"select callsign,freqtx,mode,lotw,eqsl,qrz,dxcc from log where mycall='%s'",mycall);
    mysql_query(con,buf);
    res=mysql_store_result(con);
    for(;;){
      row=mysql_fetch_row(res);
      if(row==NULL)break;
      c=(int)(atol(row[1])/1000000.0);
      if(c>433)continue;
      sprintf(aux1,"%04d%s",myband[c],mymode(row[2]));
      strcpy(aux2,wpx(row[0]));
      idx=incdata3(0,0,aux1,1,1);
      incdata3(1,idx,row[0],1,1);
      incdata3(1,TOTL2-1,row[0],1,1);
      incdata3(3,idx,aux2,1,1);
      incdata3(3,TOTL2-1,aux2,1,1);
      if(atoi(row[3])==1)incdata3(0,1,aux1,1,1);
      if(atoi(row[4])==1)incdata3(0,2,aux1,1,1);
      if(atoi(row[5])==1)incdata3(0,3,aux1,1,1);
      sprintf(aux1,"%03d",atoi(row[6]));
      idx=incdata3(0,4,aux1,1,1);
      incdata3(2,idx,row[0],1,1);
      incdata3(4,idx,aux2,1,1);
      if(atoi(row[3])==1)incdata3(0,5,aux1,1,1);
      if(atoi(row[4])==1)incdata3(0,6,aux1,1,1);
      if(atoi(row[5])==1)incdata3(0,7,aux1,1,1);
    }
    mysql_free_result(res);
    printf("<p class=\"myh1\">%6s %7s %8s %8s %8s %8s %8s</p>","B/Mode","QSO","QSO.uniq","QSO.wpx","QSL.LOTW","QSL.EQSL","QSL.QRZ");
    for(c=0;c<4;c++)for(suml[c]=0,l1=0;l1<ndata3[0][c];l1++)suml[c]+=data3[0][c][l1].num;
    printf("<p class=\"myh2\">%6s %7ld %8ld %8ld %8ld %8ld %8ld</p>","Tot",suml[0],ndata3[1][TOTL2-1],ndata3[3][TOTL2-1],suml[1],suml[2],suml[3]);
    for(l1=0;l1<ndata3[0][0];l1++)printf("%6s %7ld %8ld %8ld %8ld %8ld %8ld\n",data3[0][0][l1].lab,data3[0][0][l1].num,ndata3[1][data3[0][0][l1].idx],ndata3[2][data3[0][0][l1].idx],numdata3(0,1,data3[0][0][l1].lab),numdata3(0,2,data3[0][0][l1].lab),numdata3(0,3,data3[0][0][l1].lab));
    printf("\n");
    qsort(data3[0][4],ndata3[0][4],sizeof(struct data3),cmp3);
    printf("<p class=\"myh1\">%6s %7s %8s %8s %8s %8s %8s %s</p>","dxcc","QSO","QSO.uniq","QSO.wpx","QSL.LOTW","QSL.EQSL","QSL.QRZ","Country");
    printf("<p class=\"myh2\">%6s %7ld %8s %8s %8ld %8ld %8ld</p>","Tot",ndata3[0][4],"","",ndata3[0][5],ndata3[0][6],ndata3[0][7]);
    for(l1=0;l1<ndata3[0][4];l1++){
      printf("%6s %7ld %8ld %8ld %8ld %8ld %8ld",data3[0][4][l1].lab,data3[0][4][l1].num,ndata3[2][data3[0][4][l1].idx],ndata3[4][data3[0][4][l1].idx],numdata3(0,5,data3[0][4][l1].lab),numdata3(0,6,data3[0][4][l1].lab),numdata3(0,7,data3[0][4][l1].lab));
      sprintf(buf,"select name from cty where dxcc='%d' limit 1",atoi(data3[0][4][l1].lab));
      mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res);
      if(row!=NULL)printf(" %s",row[0]);
      mysql_free_result(res);
      printf("\n");
    }
    printf("</pre>");
    goto end;
  }

  if(act==11){ // Curio button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    for(l1=0;l1<TOT3;l1++)for(l2=0;l2<TOTL2;l2++)ndata3[l1][l2]=0;
    sprintf(buf,"select callsign,freqtx,mode,lotw,eqsl,qrz,dxcc from log where mycall='%s'",mycall);
    mysql_query(con,buf);
    res=mysql_store_result(con);
    for(;;){
      row=mysql_fetch_row(res);
      if(row==NULL)break;
      c=(int)(atol(row[1])/1000000.0);
      if(c>433)continue;
      incdata3(0,0,row[0],1,1);
      sprintf(aux1,"%04d",myband[c]);
      incdata3(0,1,aux1,1,1);
      incdata3(0,2,row[2],1,1);
      if(atoi(row[3])==1)incdata3(0,3,row[0],1,1); 
      if(atoi(row[4])==1)incdata3(0,4,row[0],1,1);
      if(atoi(row[5])==1)incdata3(0,5,row[0],1,1);
    }
    mysql_free_result(res);
    printf("<table>");
    for(c=0;c<6;c++){
      qsort(data3[0][c],ndata3[0][c],sizeof(struct data3),cmp3);
      printf("<td><pre><b>%7s     #</b>\n",l11[c]);
      for(l1=0,l2=min(ndata3[0][c],mypage);l1<l2;l1++)printf("%7.7s %6ld\n",data3[0][c][l1].lab,data3[0][c][l1].num);
      printf("</pre></td>");
    }
    printf("</table>");
    goto end;
  }

  if(act==12){ // Activity button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    printf("<pre>");
    for(l1=0;l1<TOT3;l1++)for(l2=0;l2<TOTL2;l2++)ndata3[l1][l2]=0;
    epoch=time(NULL); tm_now=gmtime(&epoch); ts=*tm_now;
    ts.tm_year-=2; timegm(&ts);
    strftime(aux3,sizeof(aux3),"%Y-%m",&ts);
    strftime(aux4,sizeof(aux4),"%Y-%m",tm_now);
    ts.tm_year+=2; ts.tm_mon-=1; timegm(&ts);
    strftime(aux5,sizeof(aux5),"%Y-%m-%d",&ts);
    strftime(aux6,sizeof(aux6),"%Y-%m-%d",tm_now);
    sprintf(buf,"select callsign,open,mode,lotw,eqsl,qrz,dxcc from log where mycall='%s'",mycall);
    mysql_query(con,buf);
    res=mysql_store_result(con);
    for(;;){
      row=mysql_fetch_row(res);
      if(row==NULL)break;
      strcpy(aux2,mymode(row[2]));
      sprintf(aux1,"%.4s",e2dtc(atoll(row[1])));
      idx=incdata3(0,0,aux1,1,1);
      incdata3(1,idx,row[0],1,1);
      incdata3(2,idx,wpx(row[0]),1,1);
      incdata3(3,idx,row[6],1,1);
      if(atoi(row[3])==1)incdata3(0,1,aux1,1,1);
      if(atoi(row[4])==1)incdata3(0,2,aux1,1,1);
      if(atoi(row[5])==1)incdata3(0,3,aux1,1,1);
      if(strcmp(aux2,"CW")==0)incdata3(0,4,aux1,1,1);
      if(strcmp(aux2,"DG")==0)incdata3(0,5,aux1,1,1);
      if(strcmp(aux2,"PH")==0)incdata3(0,6,aux1,1,1);
      sprintf(aux1,"%.7s",e2dtc(atoll(row[1])));
      if(strcmp(aux1,aux3)>=0 && strcmp(aux1,aux4)<=0){
        idx=incdata3(0,0,aux1,1,1);
        incdata3(1,idx,row[0],1,1);
        incdata3(2,idx,wpx(row[0]),1,1);
        incdata3(3,idx,row[6],1,1);
        if(atoi(row[3])==1)incdata3(0,1,aux1,1,1);
        if(atoi(row[4])==1)incdata3(0,2,aux1,1,1);
        if(atoi(row[5])==1)incdata3(0,3,aux1,1,1);
        if(strcmp(aux2,"CW")==0)incdata3(0,4,aux1,1,1);
        if(strcmp(aux2,"DG")==0)incdata3(0,5,aux1,1,1);
        if(strcmp(aux2,"PH")==0)incdata3(0,6,aux1,1,1);
      }
      sprintf(aux1,"%.10s",e2dtc(atoll(row[1])));
      if(strcmp(aux1,aux5)>=0 && strcmp(aux1,aux6)<=0){
        idx=incdata3(0,0,aux1,1,1);
        incdata3(1,idx,row[0],1,1);
        incdata3(2,idx,wpx(row[0]),1,1);
        incdata3(3,idx,row[6],1,1);
        if(atoi(row[3])==1)incdata3(0,1,aux1,1,1);
        if(atoi(row[4])==1)incdata3(0,2,aux1,1,1);
        if(atoi(row[5])==1)incdata3(0,3,aux1,1,1);
        if(strcmp(aux2,"CW")==0)incdata3(0,4,aux1,1,1);
        if(strcmp(aux2,"DG")==0)incdata3(0,5,aux1,1,1);
        if(strcmp(aux2,"PH")==0)incdata3(0,6,aux1,1,1);
      } 
    }
    mysql_free_result(res);
    suml[0]=4; suml[1]=7; suml[2]=10;
    strcpy(aux1,"YYYY-MM-DD");
    for(c=0;c<3;c++){
      printf("<p class=\"myh1\">%10.*s %8s %8s %8s %8s %8s %8s %8s %8s %8s %8s</p>",(int)suml[c],aux1,"QSO","QSO.cw","QSO.dg","QSO.ph","QSO.uniq","QSO.wpx","DXCC","QSL.LOTW","QSL.EQSL","QSL.QRZ");
      for(l1=ndata3[0][0]-1;l1>0;l1--){
        if(strlen(data3[0][0][l1].lab)==suml[c]){
          printf("%10s %8ld %8ld %8ld %8ld",data3[0][0][l1].lab,data3[0][0][l1].num,numdata3(0,4,data3[0][0][l1].lab),numdata3(0,5,data3[0][0][l1].lab),numdata3(0,6,data3[0][0][l1].lab));
          printf(" %8ld %8ld %8ld",ndata3[1][data3[0][0][l1].idx],ndata3[2][data3[0][0][l1].idx],ndata3[3][data3[0][0][l1].idx]);
          printf(" %8ld %8ld %8ld\n",numdata3(0,1,data3[0][0][l1].lab),numdata3(0,2,data3[0][0][l1].lab),numdata3(0,3,data3[0][0][l1].lab));
        }
      }
    }
    printf("</pre>");
    goto end;
  }

  if(act>=17 && act<=19){ // QSL.lotw QSL.eqsl QSL.qrz buttons
    strcpy(adif1[0],"CALL"); strcpy(adif1[1],"TIME_ON"); strcpy(adif1[2],"QSO_DATE");
    if(act==17){strcpy(adif1[3],"APP_LoTW_RXQSL"); strcpy(aux4,"lotw");}
    else if(act==18){strcpy(adif1[3],"EQSL_QSLRDATE"); strcpy(aux4,"eqsl");}
    else if(act==19){strcpy(adif1[3],"app_qrzlog_status"); strcpy(aux4,"qrz");}
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    printf("<pre>");
    vv=4; gg=adifextract(ff,vv);
    for(ppp=nnn=qqq=0;gg>0;){
      sscanf(adif[2],"%4ld%2ld%2ld",&l1,&l2,&l3); ts.tm_year=l1-1900; ts.tm_mon=l2-1; ts.tm_mday=l3;
      l3=0; sscanf(adif[1],"%2ld%2ld%2ld",&l1,&l2,&l3); ts.tm_hour=l1; ts.tm_min=l2; ts.tm_sec=l3;
      epoch=timegm(&ts);
      if(adif[3][0]!='\0'){
        sprintf(buf,"select %s from log where mycall='%s' and callsign='%s' and open>=%lld and open<=%lld",aux4,mycall,adif[0],epoch-QSLWIN,epoch+QSLWIN);
        mysql_query(con,buf); 
        res=mysql_store_result(con); 
        row=mysql_fetch_row(res); 
        if(row==NULL)c=-1; else c=atoi(row[0]); 
        mysql_free_result(res);
        ppp++;
        if(c==-1)qqq++;
        if(c==0){
          sprintf(buf,"update log set %s=1 where mycall='%s' and callsign='%s' and open>=%lld and open<=%lld",aux4,mycall,adif[0],epoch-QSLWIN,epoch+QSLWIN);
          mysql_query(con,buf);
          nnn++;
        }
      }
      gg=adifextract(NULL,vv);
    }
    printf("QSL %s Processed: %ld\nNew QSL %s Inserted: %ld\nQSO %s Missed: %ld\n",aux4,ppp,aux4,nnn,aux4,qqq);
    printf("</pre>");
    goto end;
  }

  if(act==15){ // adi in button
    strcpy(adif1[0],"call"); strcpy(adif1[1],"freq"); strcpy(adif1[2],"freq_rx"); strcpy(adif1[3],"rst_sent"); strcpy(adif1[4],"rst_rcvd"); strcpy(adif1[5],"mode");
    strcpy(adif1[6],"time_on"); strcpy(adif1[7],"time_off"); strcpy(adif1[8],"stx_string"); strcpy(adif1[9],"stx"); strcpy(adif1[10],"srx_string"); strcpy(adif1[11],"srx");
    strcpy(adif1[12],"contest_id"); strcpy(adif1[13],"qso_date"); strcpy(adif1[14],"qso_date_off");
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    printf("<pre>");
    vv=15; gg=adifextract(ff,vv);
    for(ppp=nnn=0;gg>0;){
      if(adif[6][4]=='\0'){adif[6][4]='0'; adif[6][5]='0'; adif[6][6]='\0';}
      if(adif[14][0]=='\0')strcpy(adif[14],adif[13]);
      if(adif[7][0]=='\0')strcpy(adif[7],adif[6]);
      if(adif[7][4]=='\0'){adif[7][4]='0'; adif[7][5]='0'; adif[7][6]='\0';}
      searchcty(con,adif[0]);
      sprintf(aux3,"('%s','%s','%s',%ld,%ld,'%s','%s','%s','%s','%s',%d,%lld,%lld)",mycall,adif[0],adif[5],(long)(atof(adif[1])*1000000.0),(long)(atof(adif[2])*1000000.0),adif[3],adif[4],(adif[8][0]=='\0')?adif[9]:adif[8],(adif[10][0]=='\0')?adif[11]:adif[10],adif[12],atoi(mycty[2]),dt2e(adif[13],adif[6]),dt2e(adif[14],adif[7]));
      sprintf(buf,"insert ignore into log (mycall,callsign,mode,freqtx,freqrx,signaltx,signalrx,contesttx,contestrx,contest,dxcc,open,close) value %s",aux3);
      mysql_query(con,buf);
      l1=mysql_affected_rows(con);
      if(l1>0){nnn+=l1; printf("%s\n",aux3);}
      ppp++;
      gg=adifextract(NULL,vv);
    }
    printf("QSO Processed: %ld\nNew QSO Inserted: %ld\n",ppp,nnn);
    printf("</pre>");
    goto end;
  }

   if(act==20){ // adi out button
     strcpy(adif1[0],"export_from"); strcpy(adif1[1],"export_to"); strcpy(adif1[2],"export_contest");
     printf("Status: 200 OK\r\n");
     printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
     vv=3; gg=adifextract(ff,vv);
     if(gg==0)goto end;
     srand((unsigned)time(NULL));
     sprintf(aux1,"%d%d%d%d.adi",rand(),rand(),rand(),rand());
     sprintf(aux2,"/home/www/log/files/%s",aux1);
     fp=fopen(aux2,"w");
     strcpy(aux3,"PROGRAMID"); fprintf(fp,"<LZHlogger:%d>%s\n",strlen(aux3),aux3);
     fprintf(fp,"<EOH>\n\n");
     if(adif[2][0]=='\0')sprintf(buf,"select open,callsign,freqtx,mode,signaltx,signalrx,close,freqrx,contesttx,contestrx,contest from log where mycall='%s' and open>=%lld and open<=%lld order by open",mycall,dtc2e(adif[0]),dtc2e(adif[1]));
     else sprintf(buf,"select open,callsign,freqtx,mode,signaltx,signalrx,close,freqrx,contesttx,contestrx,contest from log where mycall='%s' and contest='%s' order by open",mycall,adif[2]);
     mysql_query(con,buf);
     res=mysql_store_result(con);
     for(l1=0;;l1++){
       row=mysql_fetch_row(res);
       if(row==NULL)break;
       fprintf(fp,"<CALL:%d>%s\n",strlen(row[1]),row[1]);
       p1=e2dtc(atoll(row[0]));
       fprintf(fp,"<QSO_DATE:8>%.4s%.2s%.2s\n",p1,p1+5,p1+8);
       fprintf(fp,"<TIME_ON:6>%.2s%.2s%.2s\n",p1+11,p1+14,p1+17);
       p1=e2dtc(atoll(row[6]));
       fprintf(fp,"<QSO_DATE_OFF:8>%.4s%.2s%.2s\n",p1,p1+5,p1+8);
       fprintf(fp,"<TIME_OFF:6>%.2s%.2s%.2s\n",p1+11,p1+14,p1+17);
       sprintf(aux4,"%7.5f",atol(row[2])/1000000.0); fprintf(fp,"<FREQ:%d>%s\n",strlen(aux4),aux4);
       sprintf(aux4,"%7.5f",atol(row[7])/1000000.0); fprintf(fp,"<FREQ_RX:%d>%s\n",strlen(aux4),aux4);
       fprintf(fp,"<MODE:%d>%s\n",strlen(row[3]),row[3]);
       fprintf(fp,"<RST_SENT:%d>%s\n",strlen(row[4]),row[4]);
       fprintf(fp,"<RST_RCVD:%d>%s\n",strlen(row[5]),row[5]);
       fprintf(fp,"<STX_STRING:%d>%s\n",strlen(row[8]),row[8]);
       fprintf(fp,"<SRX_STRING:%d>%s\n",strlen(row[9]),row[9]);
       fprintf(fp,"<CONTEST_ID:%d>%s\n",strlen(row[10]),row[10]);
       fprintf(fp,"<EOR>\n\n");
     }
     res=mysql_store_result(con);
     fclose(fp);
     printf("<pre>");
     printf("<pre><a href='https://log.mazzini.org/files/%s' download>Download ADIF</a>\n",aux1);
     if(adif[2][0]=='\0')printf("from:%s to:%s\n",adif[0],adif[1]);
     else printf("contest:%s\n",adif[2]);
     printf("</pre>");
     goto end;
  }

  if(act==21){ // cbr out button
    strcpy(adif1[0],"export_from"); strcpy(adif1[1],"export_to"); strcpy(adif1[2],"export_contest");
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    vv=3; gg=adifextract(ff,vv);
    if(gg==0)goto end;
    srand((unsigned)time(NULL));
    sprintf(aux1,"%d%d%d%d.cbr",rand(),rand(),rand(),rand());
    sprintf(aux2,"/home/www/log/files/%s",aux1);
    fp=fopen(aux2,"w");
    fprintf(fp,"-OF-LOG: 3.0\nCREATED-BY: IK4LZH logger\n");
    fprintf(fp,"CONTEST: xxxxxx\nCALLSIGN: %s\nOPERATORS: %s\n",mycall,mycall);
    fprintf(fp,"CATEGORY-OPERATOR: SINGLE-OP\nCATEGORY-ASSISTED: ASSISTED\nCATEGORY-BAND: ALL\nCATEGORY-POWER: LOW\nCATEGORY-TRANSMITTER: ONE\n");    
    sprintf(buf,"select firstname,lastname,addr1,addr2,state,zip,country,email from who where callsign='%s'",mycall);
    mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res);
    fprintf(fp,"NAME: %s %s\n",row[0],row[1]);
    if(row[7][0]!='\0')fprintf(fp,"EMAIL: %s\n",row[7]);
    if(row[2][0]!='\0')fprintf(fp,"ADDRESS: %s\n",row[2]);
    if(row[3][0]!='\0')fprintf(fp,"ADDRESS-CITY: %s\n",row[3]);
    if(row[4][0]!='\0')fprintf(fp,"ADDRESS-STATE-PROVINCE: %s\n",row[4]);
    if(row[5][0]!='\0')fprintf(fp,"ADDRESS-POSTALCODE: %s\n",row[5]);
    if(row[6][0]!='\0')fprintf(fp,"ADDRESS-COUNTRY: %s\n",row[6]);
    fprintf(fp,"CLUB: Italian Contest Club\n");
    mysql_free_result(res);
    if(adif[2][0]=='\0')sprintf(buf,"select open,callsign,freqtx,mode,signaltx,signalrx,contesttx,contestrx from log where mycall='%s' and open>=%lld and open<=%lld order by open",mycall,dtc2e(adif[0]),dtc2e(adif[1]));
    else sprintf(buf,"select open,callsign,freqtx,mode,signaltx,signalrx,contesttx,contestrx from log where mycall='%s' and contest='%s' order by open",mycall,adif[2]);
    mysql_query(con,buf);
    res=mysql_store_result(con);
    for(l1=0;;l1++){
      row=mysql_fetch_row(res);
      if(row==NULL)break;
      p1=e2dtc(atoll(row[0]));
      fprintf(fp,"QSO: %5ld %2s %.4s-%.2s-%.2s %.2s%.2s",atol(row[2])/1000L,mymode(row[3]),p1,p1+5,p1+8,p1+11,p1+14);
      fprintf(fp," %-13s %3s %-6s %-13s %3s %-6s 0\n",mycall,row[4],row[6],row[1],row[5],row[7]);
    }
    res=mysql_store_result(con);
    fprintf(fp,"END-OF-LOG:\n");
    fclose(fp);
    printf("<pre>");
    printf("<pre><a href='https://log.mazzini.org/files/%s' download>Download Cabrillo</a>\n",aux1);
    if(adif[2][0]=='\0')printf("from:%s to:%s\n",adif[0],adif[1]);
    else printf("contest:%s\n",adif[2]);
    printf("</pre>");
    goto end;
  }
  
  if(act==16){ // lzh in button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    printf("<pre>");
    aux1[0]=aux2[0]=aux3[0]='\0';
    pp=strtok(ff,"\n");
    for(ppp=nnn=0;;){
      if(pp==NULL)break;
      if(pp[0]=='D')strcpy(aux1,pp+1);
      else if(pp[0]=='F')strcpy(aux2,pp+1);
      else if(pp[0]=='M')strcpy(aux3,pp+1);
      else if(pp[0]!='\0' && pp[0]!=' ' && aux1[0]!='\0' && aux2[0]!='\0' && aux3[0]!='\0'){
        aux7[0]=aux8[0]='\0'; sscanf(pp,"%s %s %s %s",aux5,aux6,aux7,aux8);
        for(qq=aux6;*qq!='\0';qq++)*qq=toupper(*qq);
        if(aux7[0]=='\0')strcpy(aux7,"59");
        if(aux8[0]=='\0')strcpy(aux8,"59");
        searchcty(con,aux6);
        strcat(aux5,":00"); 
        sprintf(aux9,"('%s','%s','%s',%ld,%ld,'%s','%s','','','',%d,%lld,%lld)",mycall,aux6,aux3,atol(aux2)*1000L,atol(aux2)*1000L,aux7,aux8,atoi(mycty[2]),dt2e(aux1,aux5),dt2e(aux1,aux5));
        sprintf(buf,"insert ignore into log (mycall,callsign,mode,freqtx,freqrx,signaltx,signalrx,contesttx,contestrx,contest,dxcc,open,close) value %s",aux9);
        mysql_query(con,buf);
        l1=mysql_affected_rows(con);
        if(l1>0){nnn+=l1; printf("%s\n",aux9);}
        ppp++;
      }
      pp=strtok(NULL,"\n");
    }
    printf("QSO Processed: %ld\nNew QSO Inserted: %ld\n",ppp,nnn);
    printf("</pre>");
    goto end;
  }
  
  if(act==22){ // cbr in button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    printf("<pre>");
    pp=strtok_r(ff,"\n",&save1);
    f1=1;
    for(ppp=nnn=0;;){
      if(pp==NULL)break;
      if(f1 && strncmp(pp,"CONTEST:",8)==0){strcpy(aux0,pp+9); aux0[strcspn(aux0,"\n")]='\0'; f1=0;}
      if(strncmp(pp,"QSO:",4)==0){
        c=nfields(pp);
        qq=strtok_r(pp," \t",&save2); // QSO
        qq=strtok_r(NULL," \t",&save2); strcpy(aux1,qq); // freq
        qq=strtok_r(NULL," \t",&save2); strcpy(aux2,qq); // mode
        qq=strtok_r(NULL," \t",&save2); strcpy(aux3,qq); // date
        qq=strtok_r(NULL," \t",&save2); strcpy(aux4,qq); strcat(aux4,":00"); // time
        qq=strtok_r(NULL," \t",&save2); // mycall
        if(c>10){qq=strtok_r(NULL," \t",&save2); strcpy(aux5,qq);} else aux5[0]='\0'; // signaltx
        qq=strtok_r(NULL," \t",&save2); strcpy(aux6,qq); // contesttx
        qq=strtok_r(NULL," \t",&save2); strcpy(aux7,qq); // callsign
        if(c>10){qq=strtok_r(NULL," \t",&save2); strcpy(aux8,qq);} else aux8[0]='\0'; // signalrx
        qq=strtok_r(NULL," \t",&save2); strcpy(aux9,qq); // contestrx
        searchcty(con,aux7);
        epoch=ddt2e(aux3,aux4);
        l1=atol(aux1)*1000L;        
        sprintf(buf,"select count(*),open from log where mycall='%s' and callsign='%s' and open>=%lld and open<=%lld and freqtx>=%ld and freqtx<=%ld limit 1",mycall,aux7,epoch-180,epoch+180,l1-1700000,l1+1700000);        
        mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res); gg=atoi(row[0]); if(gg>0)epoch=atoll(row[1]);
        mysql_free_result(res);
        if(gg==0){
          sprintf(buf,"insert into log (mycall,callsign,mode,freqtx,freqrx,signaltx,signalrx,contesttx,contestrx,contest,dxcc,open,close) value ('%s','%s','%s',%ld,%ld,'%s','%s','%s','%s','%s',%d,%lld,%lld)",mycall,aux7,aux2,l1,l1,aux5,aux8,aux6,aux9,aux0,atoi(mycty[2]),epoch,epoch);
          nnn++;
        }
        else sprintf(buf,"update log set contesttx='%s',contestrx='%s',contest='%s' where mycall='%s' and callsign='%s' and open=%lld",aux6,aux9,aux0,mycall,aux7,epoch);        
        mysql_query(con,buf);
        ppp++;
      }
      pp=strtok_r(NULL,"\n",&save1);
    }
    printf("QSO Processed: %ld\nNew QSO Inserted: %ld\n",ppp,nnn);
    printf("</pre>");
    goto end;
  }

  if(act==23){ // start button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    l1=(long)(atof(tok[5])*1000);
    if(strlen(tok[4])<3 || strlen(tok[6])<2 || strlen(tok[7])<2 || strlen(tok[8])<2 || l1==0)goto end;
    epoch=time(NULL); 
    printf("Start: %s\n",e2dtc(epoch));
    printf("<table><td>");
    searchcty(con,tok[4]);
    vv=atoi(mycty[2]); f6=atof(mycty[6]); f7=atof(mycty[7]); f8=atof(mycty[8]);
    if(mycty[0][0]!='\0')printf("<pre>base:%s\nname:%s\ndxcc:%s\ncont:%s\ncqzone:%s\nituzone:%s\nlatitude:%s\nlongitude:%s\ngmtshift:%s\n</pre>",mycty[0],mycty[1],mycty[2],mycty[3],mycty[4],mycty[5],mycty[6],mycty[7],mycty[8]);
    printf("</td><td>");    
    searchcty(con,mycall);
    printf("<pre>distance:%5.0f\nbearing:%5.0f\ndeltatime:%.0f\n</pre>",distance(f6,f7,atof(mycty[6]),atof(mycty[7])),bearing(f6,f7,atof(mycty[6]),atof(mycty[7])),atof(mycty[8])-f8);
    printf("</td><td>");    
    sprintf(buf,"select grid from who where callsign='%s'",tok[4]);
    mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res); if(row!=NULL)strcpy(aux1,row[0]); else aux1[0]='\0';
    mysql_free_result(res);
    sprintf(buf,"select grid from who where callsign='%s'",mycall);
    mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res); if(row!=NULL)strcpy(aux2,row[0]); else aux2[0]='\0';
    mysql_free_result(res);
    if(aux1!='\0'&&aux2[0]!='\0'){
      printf("<pre>gridyou:%s\ngridme:%s\n",aux1,aux2);      
      lat1=((aux1[1]-'A')*10.0+(aux1[3]-'0')+(aux1[5]-'a')/24.0+1.0/48.0-90.0);
      lat2=((aux2[1]-'A')*10.0+(aux2[3]-'0')+(aux2[5]-'a')/24.0+1.0/48.0-90.0);
      lon1=-((aux1[0]-'A')*20.0+(aux1[2]-'0')*2.0+(aux1[4]-'a')/12.0+1.0/24.0-180.0);
      lon2=-((aux2[0]-'A')*20.0+(aux2[2]-'0')*2.0+(aux2[4]-'a')/12.0+1.0/24.0-180.0);
      printf("distance:%5.0f\nbearing:%5.0f\n</pre>",distance(lat1,lon1,lat2,lon2),bearing(lat1,lon1,lat1,lon2));
    }
    printf("</td></table>");
    sprintf(buf,"select count(*) from log where mycall='%s' and dxcc=%d",mycall,vv);
    mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res); l1=atol(row[0]);
    mysql_free_result(res);
    printf("<pre>Records with same dxcc[%d]: %ld\n</pre>",vv,l1);
    for(l1=0;l1<TOT3;l1++)for(l2=0;l2<TOTL2;l2++)ndata3[l1][l2]=0;
    sprintf(buf,"select count(*) from who where callsign='%s'",tok[4]);
    mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res); c=atoi(row[0]);
    mysql_free_result(res);
    if(c==0)qrzcom(con,tok[4]);
    sprintf(buf,"select firstname,lastname,addr1,addr2,state,zip,country,grid,email,cqzone,ituzone,born,src,image,time from who where callsign='%s'",tok[4]);
    mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res);
    if(row!=NULL){
      printf("<table><td><pre>");
      printf("%s %s\n",row[0],row[1]);
      printf("%s\n%s\n",row[2],row[3]);
      printf("%s %s %s\n",row[4],row[5],row[6]);
      printf("%s\n%s\n",row[7],row[8]);
      printf("%s %s %s %s\n",row[9],row[10],row[11],row[12]);
    //  printf("%s\n",e2dtc(atoll(row[14])));
      printf("</pre></td>");
      if(row[13][0]!='\0')printf("<td><img src=\"%s\" width=\"200\" style=\"cursor:zoom-in\" onclick=\"openImgExact(this.src)\"></td>",row[13]);
      printf("</table>\n");
    }
    mysql_free_result(res);
    printf("<pre>");
    sprintf(buf,"select open,close,callsign,freqtx,freqrx,mode,signaltx,signalrx,lotw,eqsl,qrz,contesttx,contestrx,contest from log where callsign='%s' and mycall='%s' order by open desc",tok[4],mycall);
    mysql_query(con,buf);
    res=mysql_store_result(con);
    vv=0;
    for(;;){
      row=mysql_fetch_row(res);
      if(row==NULL)break;
      c=(int)(atol(row[3])/1000000.0);
      if(c>433)continue;
      sprintf(aux3,"%04d%s",myband[c],mymode(row[5]));
      incdata3(0,0,aux3,1,1);
      aux1[0]='\0';
      if(atoi(row[8])==1){strcat(aux1,"L"); incdata3(0,1,aux3,1,1);}
      if(atoi(row[9])==1){strcat(aux1,"E"); incdata3(0,2,aux3,1,1);}
      if(atoi(row[10])==1){strcat(aux1,"Q"); incdata3(0,3,aux3,1,1);}
      if(++vv<=5){
        td=atoll(row[1])-atoll(row[0]);
        if(td==0)strcpy(aux2,"(0s)");
        else if(td<60)sprintf(aux2,"(%lds)",td);
        else if(td<3600)sprintf(aux2,"(%ldm)",td/60);
        else sprintf(aux2,"(%ldh)",td/3600);
        printf("<button type=\"button\" class=\"myb2\" onclick=\"cmd1(%lld,'%s')\"> </button> ",atoll(row[0]),row[2]);
        printf("%s%5s %12s %7.1f %4s %5s %5s %-3s ",e2dtc(atoll(row[0])),aux2,row[2],atol(row[3])/1000.0,row[5],row[6],row[7],aux1);
        if(row[13][0]!='\0')printf(" (%s,%s,%s)",row[13],row[11],row[12]);
        if(atol(row[4])>0&&atol(row[4])!=atol(row[3]))printf(" [%+.1f]",(atol(row[4])-atol(row[3]))/1000.0);
        printf("\n");
      }
    }
    mysql_free_result(res);
    printf("<p class=\"myh1\">%6s %8s %8s %8s %8s</p>","B/Mode","QSO","QSL.LOTW","QSL.EQSL","QSL.QRZ");
    suml[1]=suml[2]=suml[3]=suml[4]=0;
    for(idx=0;idx<ndata3[0][0];idx++){
      l1=data3[0][0][idx].num; suml[1]+=l1;
      l2=numdata3(0,1,data3[0][0][idx].lab); suml[2]+=l2;
      l3=numdata3(0,2,data3[0][0][idx].lab); suml[3]+=l3;
      l4=numdata3(0,3,data3[0][0][idx].lab); suml[4]+=l4;
      printf("%6s %8ld %8ld %8ld %8ld\n",data3[0][0][idx].lab,l1,l2,l3,l4);
    }
    printf("<p class=\"myh2\">%6s %8ld %8ld %8ld %8ld</p>","ALL",suml[1],suml[2],suml[3],suml[4]);
    printf("</pre>");
    goto end;
  }

  if(act==26){ // end button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    l1=(long)(atof(tok[5])*1000);
    if(strlen(tok[4])<3 || strlen(tok[6])<2 || strlen(tok[7])<2 || strlen(tok[8])<2 || l1==0)goto end;
    if(tok[12][0]=='\0')goto end;
    if(tok[9][0]=='-')tok[9][0]='\0';
    if(tok[10][0]=='-')tok[10][0]='\0';
    if(tok[11][0]=='-')tok[11][0]='\0';
    searchcty(con,tok[4]);
    sprintf(buf,"insert into log (mycall,callsign,mode,freqtx,freqrx,signaltx,signalrx,contesttx,contestrx,contest,dxcc,open,close) value ('%s','%s','%s',%ld,%ld,'%s','%s','%s','%s','%s',%d,%lld,%lld)",mycall,tok[4],tok[6],l1,l1,tok[7],tok[8],tok[10],tok[11],tok[9],atoi(mycty[2]),dtc2e(tok[12]),time(NULL));
    mysql_query(con,buf);
    printf("%s inserted\n",tok[4]);
    goto end;
  }

  if(act==24){ // QRZ.com button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    qrzcom(con,tok[4]);
    goto end;
  }

  if(act==25){ // QRZ.ru button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    qrzru(con,tok[4]);
    goto end;
  }

  if(act==27){ // contest list button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    printf("<pre>");
    sprintf(buf,"select contest,min(open),max(open),count(callsign) from log where mycall='%s' and contest<>'' group by contest order by max(open) desc",mycall);
    mysql_query(con,buf);
    res=mysql_store_result(con);
    vv=sizeof(conid)/sizeof(conid[0]);
    for(;;){
      row=mysql_fetch_row(res);
      if(row==NULL)break;
      aux1[0]='\0';
      for(c=0;c<vv;c++)if(strncmp(row[0],conid[c],strlen(conid[c]))==0)break;
      if(c<vv)strcpy(aux1,"Scorable");
      printf("<button type=\"button\" class=\"myb2\" onclick=\"cmd2('%s')\">%20s</button>: [%4d] ",row[0],row[0],atoi(row[3]));
      printf("%s -> ",e2dtc(atoll(row[1])));
      printf("%s %s\n",e2dtc(atoll(row[2])),aux1);
    }
    mysql_free_result(res);
    printf("</pre>");
    goto end;
  }

  if(act==31){ // contest score button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    conscore_setup(con,tok,mycall);
    sprintf(buf,"select min(open),max(open) from log where mycall='%s' and contest='%s'",mycall,tok[9]);
    mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res); ll1=atoll(row[0]); ll2=atoll(row[1]);
    mysql_free_result(res);
    conscore(con,tok,mycall,ll1,ll2);
    printf("<pre>");
    printf("<p class=\"myh1\">%s</p>\n",tok[9]);
    gg=strlen(data3[0][4][0].lab);
    for(c=0;c<ndata3[0][4];c++){
      for(l1=0,idx=0;idx<ndata3[0][0];idx++)if(strncmp(data3[0][0][idx].lab,data3[0][4][c].lab,gg)==0)l1+=data3[0][0][idx].num;
      for(l2=0,idx=0;idx<ndata3[0][1];idx++)if(strncmp(data3[0][1][idx].lab,data3[0][4][c].lab,gg)==0)l2+=data3[0][1][idx].num;
      for(l3=0,idx=0;idx<ndata3[0][2];idx++)if(strncmp(data3[0][2][idx].lab,data3[0][4][c].lab,gg)==0)l3+=data3[0][2][idx].num;
      printf("%*s %5ld %8ld %4ld\n",gg,data3[0][4][c].lab,l1,l2,l3);
    }
    for(l1=0,idx=0;idx<ndata3[0][0];idx++)l1+=data3[0][0][idx].num;
    for(l2=0,idx=0;idx<ndata3[0][1];idx++)l2+=data3[0][1][idx].num;
    for(l3=0,idx=0;idx<ndata3[0][3];idx++)l3+=data3[0][3][idx].num;
    printf("<p class=\"myh1\">%*s %5ld %8ld %4ld</p>\n",gg,"ALL",l1,l2,l3);
    printf("<p class=\"myh2\">Score %9ld</p>\n",l2*l3);
    for(idx=0;idx<ndata3[0][3];idx++){
      printf("%s ",data3[0][3][idx].lab);
      if(idx>0 && idx%9==0)printf("\n");
    }
    printf("\n");
    printf("</pre>");
    goto end;
  }

  if(act==14){ // contest graph button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    conscore_setup(con,tok,mycall);
    sprintf(buf,"select min(open),max(open) from log where mycall='%s' and contest='%s'",mycall,tok[9]);
    mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res); ll1=atoll(row[0]); ll2=atoll(row[1]);
    mysql_free_result(res);
    printf("<div class=\"gchart\" data-rows='[ ");
    for(ll3=ll1;ll3<=ll2;ll3+=900){
      conscore(con,tok,mycall,ll3,ll3+899);
      for(l1=0,idx=0;idx<ndata3[0][0];idx++)l1+=data3[0][0][idx].num;
      for(l2=0,idx=0;idx<ndata3[0][1];idx++)l2+=data3[0][1][idx].num;
      for(l3=0,idx=0;idx<ndata3[0][3];idx++)l3+=data3[0][3][idx].num;
      printf("%c[ %lld,%ld,%ld,%ld,%ld ]\n",(ll3-ll1>0)?',':' ',ll3,l1,l2,l3,l2*l3);
    }
    printf("]' style=\"width:100%;height:520px\"></div>");
    goto end;
  }

  if(act==13){ // cluster button
    printf("Status: 200 OK\r\n");
    printf("Content-Type: text/html; charset=utf-8\r\n\r\n<!--%d-->",act);
    printf("<pre>");
    printf("<p class=\"myh1\">%22s <b>%16s</b> %10s %7s %7s %4s %4s %3s %s</p>","DATETIME","CALLSIGN","FREQ","QSODXCC","QSLDXCC","QSO","QSL","LAST","SPOTTER");
    s=socket(AF_INET,SOCK_STREAM,0);
    memset(&a,0,sizeof(a));
    a.sin_family=AF_INET; a.sin_port=htons(22222);
    inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
    connect(s,(struct sockaddr*)&a,sizeof(a));
    sprintf(aux1,"%d,%s\n",mypage,tok[12]);
    send(s,aux1,strlen(aux1),0);
    lff=0;
    for(;;){
      l1=recv(s,ff+lff,MAXFF-lff,0);
      if(l1<=0)break;
      lff+=l1;
      ff[lff]='\0';
      pp=ff;
      for(;;){
        qq=strpbrk(pp,"\n");
        if(!qq)break;
        *qq='\0';
        p1=strtok(pp,","); p2=strtok(NULL,","); p3=strtok(NULL,","); p4=strtok(NULL,",");
        l1=atol(p3); fx=l1/1000.0;
        pp=qq+1;
        searchcty(con,p4); vv=atoi(mycty[2]);

        l1=l2=0;
        cached=0;
        sprintf(buf,"select qso,qsl,time from aux1 where mycall='%s' and dxcc=%d",mycall,vv);
        mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res);
        if(row!=NULL && time(NULL)-atoll(row[2])<TIMEOUT_AUX1){cached=1; l1=atol(row[0]); l2=atol(row[1]);}
        mysql_free_result(res);
        if(cached==0){
          sprintf(buf,"select count(*),sum(lotw)+sum(eqsl)+sum(qrz) from log where mycall='%s' and dxcc=%d",mycall,vv);
          mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res); l1=atol(row[0]); l2=(row[1]==NULL)?0:atol(row[1]);
          mysql_free_result(res);
          sprintf(buf,"replace into aux1 (qso,qsl,time,mycall,dxcc) values (%ld,%ld,%lld,'%s',%d)",l1,l2,time(NULL),mycall,vv);
          mysql_query(con,buf);
        }
    
        sprintf(buf,"select count(*),sum(lotw)+sum(eqsl)+sum(qrz),max(open) from log where mycall='%s' and callsign='%s'",mycall,p4);
        mysql_query(con,buf); res=mysql_store_result(con); row=mysql_fetch_row(res);
        if(row[2]==NULL){l3=0; l4=0; strcpy(aux1,"   ");} else{l3=atol(row[0]); l4=atol(row[1]); strcpy(aux1,myts(time(NULL)-atoll(row[2])));}
        mysql_free_result(res);
        
        printf("<button type=\"button\" class=\"myb2\" onclick=\"cmd3('%s','%.1f')\"> </button> %s <b>%16s</b> %10.1f ",p4,fx,e2dtc(atoll(p1)),p4,fx);
        if(cached)printf("<span style=\"color: red;\">%7ld %7ld</span> ",l1,l2); else printf("%7ld %7ld ",l1,l2);
        printf("%4ld %4ld %3s %s\n",l3,l4,aux1,p2);
      }
      l1=lff-(pp-ff);
      if(l1>0)memmove(ff,pp,l1);
      lff=l1;
    }
    close(s);
    printf("</pre>");
    goto end;
  }
  
  end:
  mysql_close(con);
  return 0;
}
