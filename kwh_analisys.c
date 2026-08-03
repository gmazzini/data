// Standard system header files
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <mysql/mysql.h>

// Include local energy setup configuration (provides USER, PASSWORD, DB)
#include "/home/tools/setup_energy.c"

// Target configuration definitions
#define TOKEN_FILE "/home/www/data/google_access_token"
#define SPREADSHEET_ID "1fw-Nq7RPMs9JF4bb62LGrXuqg81v1bPJjupOmGsCCqg"

// Buffer structure for HTTP response body
typedef struct {
  char *data;
  size_t size;
} MemoryBuffer;

// Calculate Easter Monday date for a given year using Anonymous Gauss algorithm
static void get_easter_monday(int year, int *out_m, int *out_d) {
  int a, b, c, d, e, f, g, h, i, k, l, m;
  int month_e, day_e;

  a = 0; b = 0; c = 0; d = 0; e = 0; f = 0;
  g = 0; h = 0; i = 0; k = 0; l = 0; m = 0;
  month_e = 0; day_e = 0;

  a = year % 19;
  b = year / 100;
  c = year % 100;
  d = b / 4;
  e = b % 4;
  f = (b + 8) / 25;
  g = (b - f + 1) / 3;
  h = (19 * a + b - d - g + 15) % 30;
  i = c / 4;
  k = c % 4;
  l = (32 + 2 * e + 2 * i - h - k) % 7;
  m = (a + 11 * h + 22 * l) / 451;
  month_e = (h + l - 7 * m + 114) / 31;
  day_e = ((h + l - 7 * m + 114) % 31) + 1;

  if (month_e == 3) {
    if (day_e < 31) {
      *out_m = 3;
      *out_d = day_e + 1;
    } else {
      *out_m = 4;
      *out_d = 1;
    }
  } else {
    *out_m = 4;
    *out_d = day_e + 1;
  }
}

// Check if a date is an Italian national holiday
static int is_festivo(const struct tm *tm) {
  int m, d, y, em_m, em_d;

  m = 0; d = 0; y = 0;
  em_m = 0; em_d = 0;

  m = tm->tm_mon + 1;
  d = tm->tm_mday;
  y = tm->tm_year + 1900;

  if ((m == 1 && d == 1) || (m == 1 && d == 6) ||
      (m == 4 && d == 25) || (m == 5 && d == 1) ||
      (m == 6 && d == 2) || (m == 8 && d == 15) ||
      (m == 11 && d == 1) || (m == 12 && d == 8) ||
      (m == 12 && d == 25) || (m == 12 && d == 26)) {
    return 1;
  }

  get_easter_monday(y, &em_m, &em_d);
  if (m == em_m && d == em_d) {
    return 1;
  }

  return 0;
}

// Determine ARERA tariff band index (1 = F1, 2 = F2, 3 = F3)
static int get_band_index(const struct tm *t) {
  int wday, hour;

  wday = 0; hour = 0;

  wday = t->tm_wday;
  hour = t->tm_hour;

  if (wday == 0 || is_festivo(t)) {
    return 3;
  }

  if (wday == 6) {
    return (hour >= 7 && hour < 23) ? 2 : 3;
  }

  if (hour >= 8 && hour < 19) {
    return 1;
  }

  if ((hour >= 7 && hour < 8) || (hour >= 19 && hour < 23)) {
    return 2;
  }

  return 3;
}

static void init_memory_buffer(MemoryBuffer *mem) {
  mem->size = 0;
  mem->data = (char *)malloc(1);
  if (mem->data != NULL) {
    mem->data[0] = '\0';
  }
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
  MemoryBuffer *mem;
  char *ptr;
  size_t realsize;

  mem = NULL; ptr = NULL; realsize = 0;

  realsize = size * nmemb;
  mem = (MemoryBuffer *)userp;
  ptr = (char *)realloc(mem->data, mem->size + realsize + 1);

  if (ptr == NULL) {
    return 0;
  }

  mem->data = ptr;
  memcpy(&(mem->data[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->data[mem->size] = '\0';

  return realsize;
}

static int read_access_token(const char *filename, char *token, size_t token_size) {
  FILE *fp;

  fp = NULL;

  fp = fopen(filename, "r");
  if (fp == NULL) {
    fprintf(stderr, "Error: Unable to open token file %s\n", filename);
    return 0;
  }

  if (fgets(token, (int)token_size, fp) == NULL) {
    fclose(fp);
    fprintf(stderr, "Error: Unable to read access token\n");
    return 0;
  }

  fclose(fp);
  token[strcspn(token, "\r\n")] = '\0';

  if (token[0] == '\0') {
    fprintf(stderr, "Error: Empty access token\n");
    return 0;
  }

  return 1;
}

static void get_column_letter(int col_idx, char *out) {
  char temp[16];
  int i, j, rem;

  i = 0; j = 0; rem = 0;
  memset(temp, 0, sizeof(temp));

  for (; col_idx > 0; ) {
    rem = (col_idx - 1) % 26;
    temp[i] = (char)('A' + rem);
    i++;
    col_idx = (col_idx - 1) / 26;
  }

  for (j = 0; j < i; j++) {
    out[j] = temp[i - 1 - j];
  }
  out[i] = '\0';
}

static int put_google_sheet_range(const char *token, const char *sheet_name,
                                 const char *range_str, const char *json_payload) {
  CURL *curl;
  struct curl_slist *headers;
  MemoryBuffer body;
  CURLcode res;
  long http_code;
  char url[1024], auth_header[1024];

  curl = NULL; headers = NULL;
  body.data = NULL; body.size = 0;
  res = CURLE_OK; http_code = 0;

  snprintf(url, sizeof(url),
      "https://sheets.googleapis.com/v4/spreadsheets/%s/values/%s!%s?valueInputOption=USER_ENTERED",
      SPREADSHEET_ID, sheet_name, range_str);

  snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);
  init_memory_buffer(&body);

  curl = curl_easy_init();
  if (curl == NULL) {
    free(body.data);
    return 0;
  }

  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, auth_header);

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_payload));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "Curl error: %s\n", curl_easy_strerror(res));
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.data);
    return 0;
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code < 200 || http_code >= 300) {
    fprintf(stderr, "Google Sheets API Error HTTP %ld on tab %s (range %s)\n", http_code, sheet_name, range_str);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.data);
    return 0;
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  free(body.data);
  return 1;
}

// Generate epoch for the 1st day of the month
static time_t get_month_boundary_epoch(int year, int mon) {
  struct tm tm_target;

  memset(&tm_target, 0, sizeof(struct tm));
  tm_target.tm_year = year - 1900 + (mon / 12);
  tm_target.tm_mon = mon % 12;
  tm_target.tm_mday = 1;
  tm_target.tm_hour = 0;
  tm_target.tm_min = 0;
  tm_target.tm_sec = 0;
  tm_target.tm_isdst = -1;

  return mktime(&tm_target);
}

int main(int argc, char *argv[]) {
  MYSQL *conn;
  MYSQL_RES *res, *res_e;
  MYSQL_ROW row, row_e;
  struct tm start_tm, end_tm, *t_ptr;
  time_t start_epoch, end_epoch, ep, t_b;
  double hourly_sum[24], monthly_sum[12][4], monthly_annual_totals[4];
  double E_boundary[13], energy_monthly_sum[12];
  double hourly_total, energy_annual_total, val, diff;
  int E_found[13];
  int target_year, is_so, base_year_h_m, base_year_d;
  int hour, month, band, i, m, h, off;
  int hourly_col_idx, start_m_col, end_m_col, start_d_col, end_d_col;
  const char *measure_type, *energy_table;
  char hourly_tab[16], monthly_tab[16], compare_tab[16];
  char access_token[512], query[1024];
  char hourly_col_letter[16], hourly_range[64];
  char start_m_letter[16], end_m_letter[16], monthly_range[64];
  char start_d_letter[16], end_d_letter[16], compare_range[64];
  char *json_h, *json_m, *json_d;

  conn = NULL; res = NULL; res_e = NULL; row = NULL; row_e = NULL;
  t_ptr = NULL;
  start_epoch = 0; end_epoch = 0; ep = 0; t_b = 0;
  hourly_total = 0.0; energy_annual_total = 0.0; val = 0.0; diff = 0.0;
  target_year = 0; is_so = 0; base_year_h_m = 0; base_year_d = 2025;
  hour = 0; month = 0; band = 0; i = 0; m = 0; h = 0; off = 0;
  hourly_col_idx = 0; start_m_col = 0; end_m_col = 0; start_d_col = 0; end_d_col = 0;
  measure_type = NULL; energy_table = NULL;
  json_h = NULL; json_m = NULL; json_d = NULL;

  memset(&start_tm, 0, sizeof(struct tm));
  memset(&end_tm, 0, sizeof(struct tm));
  memset(hourly_sum, 0, sizeof(hourly_sum));
  memset(monthly_sum, 0, sizeof(monthly_sum));
  memset(monthly_annual_totals, 0, sizeof(monthly_annual_totals));
  memset(E_boundary, 0, sizeof(E_boundary));
  memset(energy_monthly_sum, 0, sizeof(energy_monthly_sum));
  memset(E_found, 0, sizeof(E_found));

  if (argc < 3) {
    fprintf(stderr, "Usage: %s <kwh_so|kwh_cc> <year>\n", argv[0]);
    return 1;
  }

  measure_type = argv[1];
  target_year = atoi(argv[2]);

  if (strcmp(measure_type, "kwh_so") != 0 && strcmp(measure_type, "kwh_cc") != 0) {
    fprintf(stderr, "Error: Invalid measure_type '%s'. Must be 'kwh_so' or 'kwh_cc'.\n", measure_type);
    return 1;
  }

  is_so = (strcmp(measure_type, "kwh_so") == 0);
  base_year_h_m = is_so ? 2021 : 2024;
  energy_table = is_so ? "energy_so" : "energy_cc";

  if (target_year < base_year_h_m || target_year > 2099) {
    fprintf(stderr, "Error: Invalid target year %d.\n", target_year);
    return 1;
  }

  setenv("TZ", "Europe/Rome", 1);
  tzset();

  snprintf(hourly_tab, sizeof(hourly_tab), "%s", is_so ? "h_so" : "h_cc");
  snprintf(monthly_tab, sizeof(monthly_tab), "%s", is_so ? "m_so" : "m_cc");
  snprintf(compare_tab, sizeof(compare_tab), "%s", is_so ? "d_so" : "d_cc");

  if (!read_access_token(TOKEN_FILE, access_token, sizeof(access_token))) {
    return 1;
  }

  conn = mysql_init(NULL);
  if (conn == NULL) {
    return 1;
  }

  if (mysql_real_connect(conn, "localhost", USER, PASSWORD, DB, 0, NULL, 0) == NULL) {
    fprintf(stderr, "mysql connect error: %s\n", mysql_error(conn));
    mysql_close(conn);
    return 1;
  }

  // Time interval for target_year
  start_tm.tm_year = target_year - 1900;
  start_tm.tm_mon = 0;
  start_tm.tm_mday = 1;
  start_tm.tm_isdst = -1;

  end_tm.tm_year = target_year - 1900;
  end_tm.tm_mon = 11;
  end_tm.tm_mday = 31;
  end_tm.tm_hour = 23;
  end_tm.tm_min = 59;
  end_tm.tm_sec = 59;
  end_tm.tm_isdst = -1;

  start_epoch = mktime(&start_tm);
  end_epoch = mktime(&end_tm);

  // 1. Fiscal Table Processing
  snprintf(query, sizeof(query),
           "SELECT epoch, kwh FROM %s WHERE epoch BETWEEN %ld AND %ld ORDER BY epoch ASC",
           measure_type, (long)start_epoch, (long)end_epoch);

  if (mysql_query(conn, query) != 0) {
    fprintf(stderr, "mysql query error: %s\n", mysql_error(conn));
    mysql_close(conn);
    return 1;
  }

  res = mysql_store_result(conn);
  if (res == NULL) {
    mysql_close(conn);
    return 1;
  }

  for (row = mysql_fetch_row(res); row != NULL; row = mysql_fetch_row(res)) {
    if (row[0] == NULL || row[1] == NULL) {
      continue;
    }

    ep = (time_t)atoll(row[0]);
    val = atof(row[1]);

    t_ptr = localtime(&ep);
    if (t_ptr == NULL) {
      continue;
    }

    hour = t_ptr->tm_hour;
    month = t_ptr->tm_mon;
    band = get_band_index(t_ptr);

    if (hour >= 0 && hour < 24) {
      hourly_sum[hour] += val;
      hourly_total += val;
    }

    if (month >= 0 && month < 12 && band >= 1 && band <= 3) {
      monthly_sum[month][band - 1] += val;
      monthly_sum[month][3] += val;
      monthly_annual_totals[band - 1] += val;
      monthly_annual_totals[3] += val;
    }
  }
  mysql_free_result(res);

  // 2. Third-party Meter Processing
  for (i = 0; i <= 12; i++) {
    t_b = get_month_boundary_epoch(target_year, i);
    snprintf(query, sizeof(query),
             "SELECT (e1 + e2 + e3) FROM %s ORDER BY ABS(CAST(epoch AS SIGNED) - %ld) ASC LIMIT 1",
             energy_table, (long)t_b);

    if (mysql_query(conn, query) == 0) {
      res_e = mysql_store_result(conn);
      if (res_e != NULL) {
        row_e = mysql_fetch_row(res_e);
        if (row_e != NULL && row_e[0] != NULL) {
          E_boundary[i] = atof(row_e[0]);
          E_found[i] = 1;
        }
        mysql_free_result(res_e);
      }
    }
  }

  for (m = 0; m < 12; m++) {
    if (E_found[m] && E_found[m + 1]) {
      diff = E_boundary[m + 1] - E_boundary[m];
      if (diff > 0.0) {
        energy_monthly_sum[m] = diff;
        energy_annual_total += diff;
      }
    }
  }

  mysql_close(conn);

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
    return 1;
  }

  // Google Sheets Update: 1. HOURLY TAB
  hourly_col_idx = 2 + (target_year - base_year_h_m);
  get_column_letter(hourly_col_idx, hourly_col_letter);

  snprintf(hourly_range, sizeof(hourly_range), "%s2:%s26", hourly_col_letter, hourly_col_letter);

  json_h = (char *)malloc(16384);
  if (json_h != NULL) {
    off = snprintf(json_h, 16384, "{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[", hourly_tab, hourly_range);
    for (h = 0; h < 24; h++) {
      off += snprintf(json_h + off, 16384 - off, "[%.5f],", hourly_sum[h]);
    }
    snprintf(json_h + off, 16384 - off, "[%.5f]]}", hourly_total);

    if (put_google_sheet_range(access_token, hourly_tab, hourly_range, json_h)) {
      printf("Updated '%s' range %s for year %d\n", hourly_tab, hourly_range, target_year);
    }
    free(json_h);
  }

  // Google Sheets Update: 2. MONTHLY TAB
  start_m_col = 2 + (target_year - base_year_h_m) * 4;
  end_m_col = start_m_col + 3;

  get_column_letter(start_m_col, start_m_letter);
  get_column_letter(end_m_col, end_m_letter);

  snprintf(monthly_range, sizeof(monthly_range), "%s2:%s14", start_m_letter, end_m_letter);

  json_m = (char *)malloc(32768);
  if (json_m != NULL) {
    off = snprintf(json_m, 32768, "{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[", monthly_tab, monthly_range);
    for (m = 0; m < 12; m++) {
      off += snprintf(json_m + off, 32768 - off, "[%.5f,%.5f,%.5f,%.5f],",
                      monthly_sum[m][0], monthly_sum[m][1],
                      monthly_sum[m][2], monthly_sum[m][3]);
    }
    snprintf(json_m + off, 32768 - off, "[%.5f,%.5f,%.5f,%.5f]]}",
             monthly_annual_totals[0], monthly_annual_totals[1],
             monthly_annual_totals[2], monthly_annual_totals[3]);

    if (put_google_sheet_range(access_token, monthly_tab, monthly_range, json_m)) {
      printf("Updated '%s' range %s for year %d\n", monthly_tab, monthly_range, target_year);
    }
    free(json_m);
  }

  // Google Sheets Update: 3. COMPARISON TAB
  if (target_year >= base_year_d) {
    start_d_col = 2 + (target_year - base_year_d) * 2;
    end_d_col = start_d_col + 1;

    get_column_letter(start_d_col, start_d_letter);
    get_column_letter(end_d_col, end_d_letter);

    snprintf(compare_range, sizeof(compare_range), "%s2:%s14", start_d_letter, end_d_letter);

    json_d = (char *)malloc(16384);
    if (json_d != NULL) {
      off = snprintf(json_d, 16384, "{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[", compare_tab, compare_range);
      for (m = 0; m < 12; m++) {
        off += snprintf(json_d + off, 16384 - off, "[%.5f,%.5f],",
                        monthly_sum[m][3], energy_monthly_sum[m]);
      }
      snprintf(json_d + off, 16384 - off, "[%.5f,%.5f]]}",
               monthly_annual_totals[3], energy_annual_total);

      if (put_google_sheet_range(access_token, compare_tab, compare_range, json_d)) {
        printf("Updated '%s' range %s for year %d\n", compare_tab, compare_range, target_year);
      }
      free(json_d);
    }
  }

  curl_global_cleanup();
  return 0;
}
