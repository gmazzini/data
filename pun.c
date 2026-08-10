// Gianluca Mazzini @2026 - Version 1.19
// Writes PUN monthly results and daily 3-hour minimum window to Google Sheets
// Automatically resolves sheet IDs by name
// Sorts sheets ascending before update and descending after update
// Accepts input parameter in format YYYYMMDD (e.g. 20260810)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <float.h>
#include <curl/curl.h>
#include <mysql/mysql.h>
#include "/home/tools/setup_energy.c"

// Path to Google OAuth2 token file
#define TOKEN_FILE "/home/www/data/google_access_token"

// Google Spreadsheet ID and Tab names
#define SPREADSHEET_ID "1RF4N-T2NR2UHai70AzTzwuLXowkLlOQWvFyb8AaE1xg"
#define SHEET_NAME_PUN "pun"
#define SHEET_NAME_H "h"

// Max 15-minute intervals in a month and 3-hour window slots
#define MAX_RECORDS 3500
#define WINDOW_SLOTS 12

// Structure to store individual 15-minute records
typedef struct {
  time_t epoch;
  double price;
  int day;
  int month;
  int year;
  int band;
} Record;

// Struct for HTTP response body buffer
struct mem {
  char *ptr;
  size_t len;
};

// Initialize memory buffer
static void mem_init(struct mem *m) {
  m->len = 0;
  m->ptr = (char *)malloc(1);
  if (m->ptr != NULL) {
    m->ptr[0] = '\0';
  }
}

// Curl write callback to accumulate HTTP response body
static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
  struct mem *m;
  char *p;
  size_t realsize;

  m = NULL;
  p = NULL;
  realsize = 0;

  realsize = size * nmemb;
  m = (struct mem *)userp;
  p = (char *)realloc(m->ptr, m->len + realsize + 1);

  if (p == NULL) {
    return 0;
  }

  m->ptr = p;
  memcpy(&(m->ptr[m->len]), contents, realsize);
  m->len += realsize;
  m->ptr[m->len] = '\0';

  return realsize;
}

// Read access token from local file
static int read_access_token(char *buf, size_t buflen) {
  FILE *fp;

  fp = NULL;

  fp = fopen(TOKEN_FILE, "r");
  if (fp == NULL) {
    fprintf(stderr, "Error: unable to open %s\n", TOKEN_FILE);
    return 0;
  }

  if (fgets(buf, (int)buflen, fp) == NULL) {
    fclose(fp);
    fprintf(stderr, "Error: unable to read access token\n");
    return 0;
  }

  fclose(fp);
  buf[strcspn(buf, "\r\n")] = '\0';

  if (buf[0] == '\0') {
    fprintf(stderr, "Error: empty access token\n");
    return 0;
  }

  return 1;
}

// Dynamically resolve numeric sheetId from sheet title (e.g. "pun" or "h")
static int get_sheet_id_by_name(const char *token, const char *sheet_name) {
  CURL *curl;
  struct curl_slist *headers;
  struct mem body;
  CURLcode res;
  long http_code;
  char url[512], auth_header[1024], search_str[128];
  char *p, *id_ptr, *block_start;
  int sheet_id;

  curl = NULL;
  headers = NULL;
  body.ptr = NULL;
  body.len = 0;
  res = CURLE_OK;
  http_code = 0;
  sheet_id = -1;
  p = NULL;
  id_ptr = NULL;
  block_start = NULL;

  snprintf(url, sizeof(url),
    "https://sheets.googleapis.com/v4/spreadsheets/%s?fields=sheets.properties(sheetId,title)",
    SPREADSHEET_ID);

  snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);

  mem_init(&body);

  curl = curl_easy_init();
  if (curl == NULL) {
    free(body.ptr);
    return -1;
  }

  headers = curl_slist_append(headers, auth_header);

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "Google Sheets API (get_sheet_id) curl error: %s\n", curl_easy_strerror(res));
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.ptr);
    return -1;
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code < 200 || http_code >= 300) {
    fprintf(stderr, "Google Sheets API (get_sheet_id) HTTP %ld\n", http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.ptr);
    return -1;
  }

  snprintf(search_str, sizeof(search_str), "\"title\": \"%s\"", sheet_name);
  p = strstr(body.ptr, search_str);
  if (p == NULL) {
    snprintf(search_str, sizeof(search_str), "\"title\":\"%s\"", sheet_name);
    p = strstr(body.ptr, search_str);
  }

  if (p != NULL) {
    block_start = (p - 150 > body.ptr) ? (p - 150) : body.ptr;
    id_ptr = strstr(block_start, "\"sheetId\":");
    if (id_ptr != NULL) {
      if (sscanf(id_ptr, "\"sheetId\": %d", &sheet_id) != 1) {
        sscanf(id_ptr, "\"sheetId\":%d", &sheet_id);
      }
    }
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  free(body.ptr);

  if (sheet_id == -1) {
    fprintf(stderr, "Error: sheet name '%s' not found in spreadsheet\n", sheet_name);
  }

  return sheet_id;
}

// Sort range in Google Sheet using batchUpdate API
static int sort_google_sheet_range(const char *token, int sheet_id,
                                   int num_cols, int ascending) {
  CURL *curl;
  struct curl_slist *headers;
  struct mem body;
  CURLcode res;
  long http_code;
  char url[512], json_payload[1024], auth_header[1024];

  curl = NULL;
  headers = NULL;
  body.ptr = NULL;
  body.len = 0;
  res = CURLE_OK;
  http_code = 0;

  snprintf(url, sizeof(url),
    "https://sheets.googleapis.com/v4/spreadsheets/%s:batchUpdate",
    SPREADSHEET_ID);

  snprintf(json_payload, sizeof(json_payload),
    "{"
    "\"requests\":[{"
      "\"sortRange\":{"
        "\"range\":{"
          "\"sheetId\":%d,"
          "\"startRowIndex\":1,"
          "\"startColumnIndex\":0,"
          "\"endColumnIndex\":%d"
        "},"
        "\"sortSpecs\":[{"
          "\"dimensionIndex\":0,"
          "\"sortOrder\":\"%s\""
        "}]"
      "}"
    "}]"
    "}",
    sheet_id, num_cols, ascending ? "ASCENDING" : "DESCENDING");

  snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);

  mem_init(&body);

  curl = curl_easy_init();
  if (curl == NULL) {
    free(body.ptr);
    return 0;
  }

  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, auth_header);

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_payload));

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "Google Sheets API (sort) curl error: %s\n", curl_easy_strerror(res));
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.ptr);
    return 0;
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code < 200 || http_code >= 300) {
    fprintf(stderr, "Google Sheets API (sort) HTTP %ld\n", http_code);
    fprintf(stderr, "Google response: %s\n", body.ptr ? body.ptr : "(null)");
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.ptr);
    return 0;
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  free(body.ptr);
  return 1;
}

// Calculate Easter Monday date for a given year using Anonymous Gauss algorithm
static void get_easter_monday(int year, int *out_m, int *out_d) {
  int a, b, c, d, e, f, g, h, i, k, l, m;
  int month_e, day_e;

  a = 0; b = 0; c = 0; d = 0; e = 0; f = 0;
  g = 0; h = 0; i = 0; k = 0; l = 0; m = 0;
  month_e = 0;
  day_e = 0;

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
static int is_festivo(struct tm *tm) {
  int m, d, y;
  int em_m, em_d;

  m = 0;
  d = 0;
  y = 0;
  em_m = 0;
  em_d = 0;

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

// Determine ARERA energy band index (1 = F1, 2 = F2, 3 = F3)
static int get_band_index(struct tm *tm) {
  int dow, h;

  dow = 0;
  h = 0;

  dow = tm->tm_wday;
  h = tm->tm_hour;

  if (dow == 0 || is_festivo(tm)) {
    return 3;
  }

  if (dow == 6) {
    return (h >= 7 && h < 23) ? 2 : 3;
  }

  if (h >= 8 && h < 19) {
    return 1;
  }

  if ((h >= 7 && h < 8) || (h >= 19 && h < 23)) {
    return 2;
  }

  return 3;
}

// Update Google Sheet row for monthly tab "pun" (Columns A to J)
static int update_google_sheet_pun(const char *token, const char *aaaamm, int row_index,
                                    double f0, double f1, double f2, double f3,
                                    double min_val, double max_val,
                                    const char *bestday, const char *worstday, const char *minday) {
  CURL *curl;
  struct curl_slist *headers;
  struct mem body;
  CURLcode res;
  long http_code;
  char url[512], json_payload[1024], auth_header[1024];

  curl = NULL;
  headers = NULL;
  body.ptr = NULL;
  body.len = 0;
  res = CURLE_OK;
  http_code = 0;

  snprintf(url, sizeof(url),
    "https://sheets.googleapis.com/v4/spreadsheets/%s/values/%s!A%d:J%d?valueInputOption=USER_ENTERED",
    SPREADSHEET_ID, SHEET_NAME_PUN, row_index, row_index);

  snprintf(json_payload, sizeof(json_payload),
    "{"
    "\"range\":\"%s!A%d:J%d\","
    "\"majorDimension\":\"ROWS\","
    "\"values\":[["
    "\"%s\",%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,\"%s\",\"%s\",\"%s\""
    "]]"
    "}",
    SHEET_NAME_PUN, row_index, row_index,
    aaaamm, f0, f1, f2, f3, min_val, max_val, bestday, worstday, minday);

  snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);

  mem_init(&body);

  curl = curl_easy_init();
  if (curl == NULL) {
    free(body.ptr);
    return 0;
  }

  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, auth_header);

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_payload));

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "Google Sheets API (tab pun) curl error: %s\n", curl_easy_strerror(res));
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.ptr);
    return 0;
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code < 200 || http_code >= 300) {
    fprintf(stderr, "Google Sheets API (tab pun) HTTP %ld\n", http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.ptr);
    return 0;
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  free(body.ptr);
  return 1;
}

// Update Google Sheet row for daily window tab "h" (Columns A to C)
static int update_google_sheet_h(const char *token, int row_index,
                                 const char *date_str, const char *time_str,
                                 double avg_3h) {
  CURL *curl;
  struct curl_slist *headers;
  struct mem body;
  CURLcode res;
  long http_code;
  char url[512], json_payload[512], auth_header[1024];

  curl = NULL;
  headers = NULL;
  body.ptr = NULL;
  body.len = 0;
  res = CURLE_OK;
  http_code = 0;

  snprintf(url, sizeof(url),
    "https://sheets.googleapis.com/v4/spreadsheets/%s/values/%s!A%d:C%d?valueInputOption=USER_ENTERED",
    SPREADSHEET_ID, SHEET_NAME_H, row_index, row_index);

  snprintf(json_payload, sizeof(json_payload),
    "{"
    "\"range\":\"%s!A%d:C%d\","
    "\"majorDimension\":\"ROWS\","
    "\"values\":[["
    "\"%s\",\"%s\",%.5f"
    "]]"
    "}",
    SHEET_NAME_H, row_index, row_index,
    date_str, time_str, avg_3h);

  snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);

  mem_init(&body);

  curl = curl_easy_init();
  if (curl == NULL) {
    free(body.ptr);
    return 0;
  }

  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, auth_header);

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_payload));

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "Google Sheets API (tab h) curl error: %s\n", curl_easy_strerror(res));
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.ptr);
    return 0;
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code < 200 || http_code >= 300) {
    fprintf(stderr, "Google Sheets API (tab h) HTTP %ld\n", http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.ptr);
    return 0;
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  free(body.ptr);
  return 1;
}

int main(int argc, char *argv[]) {
  MYSQL *conn;
  MYSQL_RES *result;
  MYSQL_ROW row;
  static Record records[MAX_RECORDS];
  struct tm start_tm, next_tm, local_tm, base_tm, target_tm;
  time_t sds, sde, t_val, base_epoch, target_epoch, day_epochs[96];
  double sums[4], day_sums[32], day_prices[96];
  double global_min, global_max, f0, f1, f2, f3;
  double min_day_avg, max_day_avg, avg, c_val;
  double window_sum, min_window_sum, min_window_avg;
  long counts[4], day_counts[32];
  long long epoch_val;
  int year, month, day, delta_months, days_diff;
  int row_index_pun, row_index_h, total_records, day_records_count;
  int best_day, worst_day, min_day, max_consec_run, current_run, max_run;
  int band, d, i, k, best_window_idx;
  int sheet_id_pun, sheet_id_h;
  int max_consecutive_per_day[32];
  char query[512], access_token[512], aaaamm[8];
  char bestday_str[16], worstday_str[16], minday_str[16];
  char date_str[16], time_str[16];

  conn = NULL;
  result = NULL;
  row = NULL;
  sds = 0;
  sde = 0;
  t_val = 0;
  base_epoch = 0;
  target_epoch = 0;
  global_min = DBL_MAX;
  global_max = -DBL_MAX;
  f0 = 0.0;
  f1 = 0.0;
  f2 = 0.0;
  f3 = 0.0;
  min_day_avg = DBL_MAX;
  max_day_avg = -DBL_MAX;
  avg = 0.0;
  c_val = 0.0;
  window_sum = 0.0;
  min_window_sum = DBL_MAX;
  min_window_avg = 0.0;
  epoch_val = 0;
  year = 0;
  month = 0;
  day = 0;
  delta_months = 0;
  days_diff = 0;
  row_index_pun = 0;
  row_index_h = 0;
  total_records = 0;
  day_records_count = 0;
  best_day = 1;
  worst_day = 1;
  min_day = 1;
  max_consec_run = -1;
  current_run = 0;
  max_run = 0;
  band = 0;
  d = 0;
  i = 0;
  k = 0;
  best_window_idx = 0;
  sheet_id_pun = -1;
  sheet_id_h = -1;

  memset(&start_tm, 0, sizeof(struct tm));
  memset(&next_tm, 0, sizeof(struct tm));
  memset(&local_tm, 0, sizeof(struct tm));
  memset(&base_tm, 0, sizeof(struct tm));
  memset(&target_tm, 0, sizeof(struct tm));
  memset(sums, 0, sizeof(sums));
  memset(day_sums, 0, sizeof(day_sums));
  memset(day_prices, 0, sizeof(day_prices));
  memset(day_epochs, 0, sizeof(day_epochs));
  memset(counts, 0, sizeof(counts));
  memset(day_counts, 0, sizeof(day_counts));
  memset(max_consecutive_per_day, 0, sizeof(max_consecutive_per_day));
  memset(query, 0, sizeof(query));
  memset(access_token, 0, sizeof(access_token));
  memset(aaaamm, 0, sizeof(aaaamm));
  memset(bestday_str, 0, sizeof(bestday_str));
  memset(worstday_str, 0, sizeof(worstday_str));
  memset(minday_str, 0, sizeof(minday_str));
  memset(date_str, 0, sizeof(date_str));
  memset(time_str, 0, sizeof(time_str));

  if (argc != 2) {
    fprintf(stderr, "Usage: %s YYYYMMDD\n", argv[0]);
    fprintf(stderr, "Example: %s 20260810\n", argv[0]);
    return 1;
  }

  if (strlen(argv[1]) != 8) {
    fprintf(stderr, "Error: parameter must be 8 digits (YYYYMMDD)\n");
    return 1;
  }

  if (sscanf(argv[1], "%4d%2d%2d", &year, &month, &day) != 3 ||
      month < 1 || month > 12 || day < 1 || day > 31) {
    fprintf(stderr, "Error: invalid year, month or day in %s\n", argv[1]);
    return 1;
  }

  snprintf(aaaamm, sizeof(aaaamm), "%04d%02d", year, month);
  snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", year, month, day);

  delta_months = (year - 2025) * 12 + (month - 10);
  row_index_pun = 2 + delta_months;

  if (row_index_pun < 2) {
    fprintf(stderr, "Error: Date %s is before base date 20251001\n", argv[1]);
    return 1;
  }

  base_tm.tm_year = 2025 - 1900;
  base_tm.tm_mon = 10 - 1;
  base_tm.tm_mday = 1;
  base_tm.tm_hour = 12;
  base_tm.tm_min = 0;
  base_tm.tm_sec = 0;
  base_tm.tm_isdst = -1;
  base_epoch = mktime(&base_tm);

  target_tm.tm_year = year - 1900;
  target_tm.tm_mon = month - 1;
  target_tm.tm_mday = day;
  target_tm.tm_hour = 12;
  target_tm.tm_min = 0;
  target_tm.tm_sec = 0;
  target_tm.tm_isdst = -1;
  target_epoch = mktime(&target_tm);

  days_diff = (int)floor(difftime(target_epoch, base_epoch) / 86400.0 + 0.5);
  row_index_h = 2 + days_diff;

  if (row_index_h < 2) {
    fprintf(stderr, "Error: Date %s is before base date 20251001\n", argv[1]);
    return 1;
  }

  setenv("TZ", "Europe/Rome", 1);
  tzset();

  start_tm.tm_year = year - 1900;
  start_tm.tm_mon = month - 1;
  start_tm.tm_mday = 1;
  start_tm.tm_hour = 0;
  start_tm.tm_min = 0;
  start_tm.tm_sec = 0;
  start_tm.tm_isdst = -1;

  sds = mktime(&start_tm);

  next_tm = start_tm;
  if (month == 12) {
    next_tm.tm_year += 1;
    next_tm.tm_mon = 0;
  } else {
    next_tm.tm_mon += 1;
  }
  next_tm.tm_isdst = -1;

  sde = mktime(&next_tm) - 1;

  conn = mysql_init(NULL);
  if (conn == NULL) {
    fprintf(stderr, "Error: mysql_init failed\n");
    return 1;
  }

  if (mysql_real_connect(conn, "localhost", USER, PASSWORD, DB, 0, NULL, 0) == NULL) {
    fprintf(stderr, "MySQL connection error: %s\n", mysql_error(conn));
    mysql_close(conn);
    return 1;
  }

  snprintf(query, sizeof(query),
    "SELECT epoch, c FROM pun_15m WHERE epoch BETWEEN %ld AND %ld ORDER BY epoch ASC",
    (long)sds, (long)sde);

  if (mysql_query(conn, query) != 0) {
    fprintf(stderr, "MySQL query error: %s\n", mysql_error(conn));
    mysql_close(conn);
    return 1;
  }

  result = mysql_store_result(conn);
  if (result == NULL) {
    fprintf(stderr, "MySQL store result error: %s\n", mysql_error(conn));
    mysql_close(conn);
    return 1;
  }

  for (row = mysql_fetch_row(result); row != NULL; row = mysql_fetch_row(result)) {
    if (row[0] == NULL || row[1] == NULL) {
      continue;
    }

    epoch_val = atoll(row[0]);
    c_val = atof(row[1]);

    t_val = (time_t)epoch_val;
    localtime_r(&t_val, &local_tm);

    band = get_band_index(&local_tm);

    if (c_val < global_min) {
      global_min = c_val;
    }
    if (c_val > global_max) {
      global_max = c_val;
    }

    if (total_records < MAX_RECORDS) {
      records[total_records].epoch = t_val;
      records[total_records].price = c_val;
      records[total_records].day = local_tm.tm_mday;
      records[total_records].month = local_tm.tm_mon + 1;
      records[total_records].year = local_tm.tm_year + 1900;
      records[total_records].band = band;
      total_records++;
    }

    sums[0] += c_val;
    counts[0]++;

    sums[band] += c_val;
    counts[band]++;
  }

  mysql_free_result(result);
  mysql_close(conn);

  if (counts[0] == 0) {
    fprintf(stderr, "Error: No data found for period %s\n", argv[1]);
    return 1;
  }

  f0 = counts[0] > 0 ? sums[0] / counts[0] : 0.0;
  f1 = counts[1] > 0 ? sums[1] / counts[1] : 0.0;
  f2 = counts[2] > 0 ? sums[2] / counts[2] : 0.0;
  f3 = counts[3] > 0 ? sums[3] / counts[3] : 0.0;

  for (i = 0; i < total_records; i++) {
    d = records[i].day;
    if (d >= 1 && d <= 31) {
      day_sums[d] += records[i].price;
      day_counts[d]++;
    }
  }

  for (d = 1; d <= 31; d++) {
    if (day_counts[d] > 0) {
      avg = day_sums[d] / day_counts[d];
      if (avg < min_day_avg) {
        min_day_avg = avg;
        best_day = d;
      }
      if (avg > max_day_avg) {
        max_day_avg = avg;
        worst_day = d;
      }
    }
  }

  for (d = 1; d <= 31; d++) {
    current_run = 0;
    max_run = 0;
    for (i = 0; i < total_records; i++) {
      if (records[i].day == d) {
        if (fabs(records[i].price - global_min) < 0.00001) {
          current_run++;
          if (current_run > max_run) {
            max_run = current_run;
          }
        } else {
          current_run = 0;
        }
      }
    }
    max_consecutive_per_day[d] = max_run;
  }

  min_day = best_day;

  for (d = 1; d <= 31; d++) {
    if (day_counts[d] > 0) {
      if (max_consecutive_per_day[d] > max_consec_run) {
        max_consec_run = max_consecutive_per_day[d];
        min_day = d;
      }
    }
  }

  snprintf(bestday_str, sizeof(bestday_str), "%04d-%02d-%02d", year, month, best_day);
  snprintf(worstday_str, sizeof(worstday_str), "%04d-%02d-%02d", year, month, worst_day);
  snprintf(minday_str, sizeof(minday_str), "%04d-%02d-%02d", year, month, min_day);

  day_records_count = 0;
  for (i = 0; i < total_records; i++) {
    if (records[i].day == day) {
      if (day_records_count < 96) {
        day_prices[day_records_count] = records[i].price;
        day_epochs[day_records_count] = records[i].epoch;
        day_records_count++;
      }
    }
  }

  best_window_idx = 0;
  min_window_sum = DBL_MAX;

  if (day_records_count >= WINDOW_SLOTS) {
    for (i = 0; i <= day_records_count - WINDOW_SLOTS; i++) {
      window_sum = 0.0;
      for (k = 0; k < WINDOW_SLOTS; k++) {
        window_sum += day_prices[i + k];
      }
      if (window_sum < min_window_sum) {
        min_window_sum = window_sum;
        best_window_idx = i;
      }
    }
    min_window_avg = min_window_sum / (double)WINDOW_SLOTS;
    localtime_r(&day_epochs[best_window_idx], &local_tm);
    snprintf(time_str, sizeof(time_str), "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
  } else {
    snprintf(time_str, sizeof(time_str), "00:00");
    min_window_avg = 0.0;
  }

  if (!read_access_token(access_token, sizeof(access_token))) {
    return 1;
  }

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
    fprintf(stderr, "Error: curl_global_init failed\n");
    return 1;
  }

  // Resolving sheet IDs dynamically
  sheet_id_pun = get_sheet_id_by_name(access_token, SHEET_NAME_PUN);
  sheet_id_h = get_sheet_id_by_name(access_token, SHEET_NAME_H);

  if (sheet_id_pun == -1 || sheet_id_h == -1) {
    fprintf(stderr, "Error: failed to resolve sheet IDs from names\n");
    curl_global_cleanup();
    return 1;
  }

  // STEP 1: Sort sheets ASCENDING
  if (!sort_google_sheet_range(access_token, sheet_id_pun, 10, 1) ||
      !sort_google_sheet_range(access_token, sheet_id_h, 3, 1)) {
    fprintf(stderr, "Error: failed to sort sheets ASCENDING\n");
    curl_global_cleanup();
    return 1;
  }

  // STEP 2: Write target rows
  if (!update_google_sheet_pun(access_token, aaaamm, row_index_pun,
                               f0, f1, f2, f3,
                               global_min, global_max,
                               bestday_str, worstday_str, minday_str)) {
    fprintf(stderr, "Error: failed to update Google Sheet tab pun\n");
    curl_global_cleanup();
    return 1;
  }

  if (!update_google_sheet_h(access_token, row_index_h,
                             date_str, time_str, min_window_avg)) {
    fprintf(stderr, "Error: failed to update Google Sheet tab h\n");
    curl_global_cleanup();
    return 1;
  }

  // STEP 3: Sort sheets DESCENDING
  if (!sort_google_sheet_range(access_token, sheet_id_pun, 10, 0) ||
      !sort_google_sheet_range(access_token, sheet_id_h, 3, 0)) {
    fprintf(stderr, "Error: failed to sort sheets DESCENDING\n");
    curl_global_cleanup();
    return 1;
  }

  printf("OK: Updated Sheet 'pun' row %d for month %s\n", row_index_pun, aaaamm);
  printf("F0: %.5f | F1: %.5f | F2: %.5f | F3: %.5f\n", f0, f1, f2, f3);
  printf("MIN: %.5f | MAX: %.5f | BESTDAY: %s | WORSTDAY: %s | MINDAY: %s\n",
         global_min, global_max, bestday_str, worstday_str, minday_str);
  printf("OK: Updated Sheet 'h' row %d for date %s -> Start: %s | 3h Avg: %.5f €/MWh\n",
         row_index_h, date_str, time_str, min_window_avg);

  curl_global_cleanup();
  return 0;
}
