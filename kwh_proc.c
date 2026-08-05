// Gianluca Mazzini @2026- Version 1.9
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <mysql/mysql.h>
#include "/home/tools/setup_energy.c"

#define TOKEN_FILE "/home/www/data/google_access_token"
#define MAX_FILES 500

struct mem {
  char *ptr;
  size_t len;
};

typedef struct {
  char id[128];
  char name[256];
  char mime_type[128];
} DriveFile;

static void mem_init(struct mem *m) {
  m->len = 0;
  m->ptr = (char *)malloc(1);
  if (m->ptr != NULL) {
    m->ptr[0] = '\0';
  }
}

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

// Read access token from file
static int read_access_token(char *buf, size_t buflen) {
  FILE *fp;

  fp = NULL;

  fp = fopen(TOKEN_FILE, "r");
  if (fp == NULL) {
    fprintf(stderr, "Error: cannot open token file %s\n", TOKEN_FILE);
    return 0;
  }

  if (fgets(buf, (int)buflen, fp) == NULL) {
    fclose(fp);
    fprintf(stderr, "Error: cannot read token from %s\n", TOKEN_FILE);
    return 0;
  }

  fclose(fp);
  buf[strcspn(buf, "\r\n")] = '\0';

  if (buf[0] == '\0') {
    fprintf(stderr, "Error: empty token in %s\n", TOKEN_FILE);
    return 0;
  }

  return 1;
}

// Extract a JSON string value by key within a given range [start, end]
static int extract_json_value(const char *start, const char *end, const char *key, char *out_val, size_t max_len) {
  const char *p, *colon, *q1, *q2;
  size_t len;
  char search_key[128];

  p = NULL; colon = NULL; q1 = NULL; q2 = NULL;
  len = 0;

  snprintf(search_key, sizeof(search_key), "\"%s\"", key);

  p = strstr(start, search_key);
  if (p == NULL || p >= end) {
    return 0;
  }

  colon = strchr(p, ':');
  if (colon == NULL || colon >= end) {
    return 0;
  }

  q1 = strchr(colon, '"');
  if (q1 == NULL || q1 >= end) {
    return 0;
  }
  q1++;

  q2 = strchr(q1, '"');
  if (q2 == NULL || q2 >= end) {
    return 0;
  }

  len = (size_t)(q2 - q1);
  if (len >= max_len) {
    len = max_len - 1;
  }

  memcpy(out_val, q1, len);
  out_val[len] = '\0';

  return 1;
}

// Parse JSON response from Google Drive files.list
static int parse_drive_files(const char *json, DriveFile *out_files, int max_files) {
  const char *files_sec, *p, *obj_start, *obj_end;
  int count;

  files_sec = NULL; p = NULL; obj_start = NULL; obj_end = NULL;
  count = 0;

  files_sec = strstr(json, "\"files\"");
  if (files_sec == NULL) {
    return 0;
  }

  p = strstr(files_sec, "[");
  if (p == NULL) {
    return 0;
  }

  for (; count < max_files; p = obj_end + 1) {
    obj_start = strchr(p, '{');
    if (obj_start == NULL) {
      break;
    }

    obj_end = strchr(obj_start, '}');
    if (obj_end == NULL) {
      break;
    }

    if (extract_json_value(obj_start, obj_end, "id", out_files[count].id, sizeof(out_files[count].id)) &&
        extract_json_value(obj_start, obj_end, "name", out_files[count].name, sizeof(out_files[count].name))) {

      if (!extract_json_value(obj_start, obj_end, "mimeType", out_files[count].mime_type, sizeof(out_files[count].mime_type))) {
        strcpy(out_files[count].mime_type, "text/csv");
      }

      count++;
    }
  }

  return count;
}

// Strip quotes, spaces, newlines, and convert decimal commas to dots
static void clean_val_str(const char *src, char *dst, size_t max_len) {
  size_t i, j;
  char c;

  i = 0; j = 0;
  c = '\0';

  for (i = 0; src[i] != '\0' && j < max_len - 1; i++) {
    c = src[i];
    if (c == '"' || c == '\'' || c == ' ' || c == '\r' || c == '\n') {
      continue;
    }
    if (c == ',') {
      c = '.';
    }
    dst[j] = c;
    j++;
  }
  dst[j] = '\0';
}

// Tokenize line respecting quoted delimiters and auto-detecting separator (; or ,)
static int tokenize_line(char *line, char **tokens, int max_tokens) {
  char *ptr, *start;
  char sep;
  int count, in_quotes;

  ptr = NULL; start = NULL;
  sep = ';';
  count = 0; in_quotes = 0;

  if (strchr(line, ';') == NULL && strchr(line, ',') != NULL) {
    sep = ',';
  }

  ptr = line;
  start = ptr;

  for (; *ptr != '\0' && count < max_tokens; ptr++) {
    if (*ptr == '"') {
      in_quotes = !in_quotes;
    } else if (*ptr == sep && !in_quotes) {
      *ptr = '\0';
      tokens[count] = start;
      count++;
      start = ptr + 1;
    }
  }

  if (count < max_tokens) {
    tokens[count] = start;
    count++;
  }

  return count;
}

// Generic HTTP GET request
static int http_get(CURL *curl, const char *url, struct curl_slist *headers, struct mem *out, long *http_code) {
  CURLcode res;

  res = CURLE_OK;

  curl_easy_reset(curl);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "cURL GET error: %s\n", curl_easy_strerror(res));
    return 0;
  }

  if (http_code != NULL) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
  }

  return 1;
}

// Delete file from Google Drive via API
static int delete_drive_file(CURL *curl, struct curl_slist *headers, const char *file_id) {
  struct mem body;
  CURLcode res;
  long http;
  char url[2048];

  body.ptr = NULL; body.len = 0;
  res = CURLE_OK;
  http = 0;

  mem_init(&body);

  snprintf(url, sizeof(url),
           "https://www.googleapis.com/drive/v3/files/%s?supportsAllDrives=true",
           file_id);

  curl_easy_reset(curl);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  res = curl_easy_perform(curl);
  if (res == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
  }

  free(body.ptr);

  if (res == CURLE_OK && (http == 200 || http == 204)) {
    return 1;
  }

  fprintf(stderr, "Error deleting file %s on Drive (HTTP %ld)\n", file_id, http);
  return 0;
}

// Parse CSV content and insert values into MySQL
static int process_and_insert_csv(MYSQL *conn, const char *table_name, const char *csv_data) {
  struct tm tm_info;
  time_t epoch;
  double val;
  int day, month, year, hour, minute;
  int is_second_line_of_day, token_count, i;
  char *data_copy, *saveptr, *line;
  char *tokens[120];
  char prev_date[32], date_raw[64], val_str[32], query[1024];

  data_copy = NULL; saveptr = NULL; line = NULL;
  epoch = 0;
  val = 0.0;
  day = 0; month = 0; year = 0; hour = 0; minute = 0;
  is_second_line_of_day = 0; token_count = 0; i = 0;
  memset(&tm_info, 0, sizeof(struct tm));
  memset(prev_date, 0, sizeof(prev_date));

  data_copy = strdup(csv_data);
  if (data_copy == NULL) {
    return 0;
  }

  // Begin transaction for batch insertion performance
  mysql_autocommit(conn, 0);

  // Skip CSV header line
  line = strtok_r(data_copy, "\r\n", &saveptr);
  if (line != NULL) {
    line = strtok_r(NULL, "\r\n", &saveptr);
  }

  for (; line != NULL; line = strtok_r(NULL, "\r\n", &saveptr)) {
    if (strlen(line) == 0) {
      continue;
    }

    token_count = tokenize_line(line, tokens, 120);

    if (token_count >= 2) {
      clean_val_str(tokens[0], date_raw, sizeof(date_raw));

      if (sscanf(date_raw, "%d/%d/%d", &day, &month, &year) == 3) {
        is_second_line_of_day = (strcmp(date_raw, prev_date) == 0);
        strncpy(prev_date, date_raw, sizeof(prev_date) - 1);

        for (i = 1; i < token_count && i <= 96; i++) {
          // Handle October DST transition
          if (is_second_line_of_day && i < 9) {
            continue;
          }

          clean_val_str(tokens[i], val_str, sizeof(val_str));

          if (!is_second_line_of_day && month == 10 && i > 12 && strcmp(val_str, "0") == 0) {
            continue;
          }

          val = atof(val_str);
          hour = (i - 1) / 4;
          minute = ((i - 1) % 4) * 15;

          memset(&tm_info, 0, sizeof(struct tm));
          tm_info.tm_year = year - 1900;
          tm_info.tm_mon = month - 1;
          tm_info.tm_mday = day;
          tm_info.tm_hour = hour;
          tm_info.tm_min = minute;
          tm_info.tm_sec = 0;

          if (is_second_line_of_day) {
            tm_info.tm_isdst = 0;
          } else if (month == 10 && day >= 25 && day <= 31 && hour < 3) {
            tm_info.tm_isdst = 1;
          } else {
            tm_info.tm_isdst = -1;
          }

          epoch = mktime(&tm_info);
          if (epoch == -1) {
            continue;
          }

          snprintf(query, sizeof(query),
            "INSERT INTO `%s` (epoch, kwh) "
            "VALUES (%ld, %.4f) "
            "ON DUPLICATE KEY UPDATE kwh=VALUES(kwh);",
            table_name, (long)epoch, val);

          if (mysql_query(conn, query) != 0) {
            fprintf(stderr, "MySQL query error on table %s: %s\n", table_name, mysql_error(conn));
            mysql_rollback(conn);
            mysql_autocommit(conn, 1);
            free(data_copy);
            return 0;
          }
        }
      }
    }
  }

  // Commit transaction
  mysql_commit(conn);
  mysql_autocommit(conn, 1);

  free(data_copy);
  return 1;
}

int main(int argc, char *argv[]) {
  CURL *curl;
  struct curl_slist *headers;
  MYSQL *conn;
  struct mem list_body, file_body;
  DriveFile files[MAX_FILES];
  long http_code;
  int file_count, i;
  const char *folder_id, *table_name;
  char access_token[512], auth_header[1024], url[4096];

  curl = NULL;
  headers = NULL;
  conn = NULL;
  list_body.ptr = NULL; list_body.len = 0;
  file_body.ptr = NULL; file_body.len = 0;
  http_code = 0;
  file_count = 0; i = 0;
  folder_id = NULL; table_name = NULL;

  // Check command line arguments
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <GOOGLE_FOLDER_ID> <MYSQL_TABLE_NAME>\n", argv[0]);
    return EXIT_FAILURE;
  }

  folder_id = argv[1];
  table_name = argv[2];

  // Set local timezone to Europe/Rome
  setenv("TZ", "Europe/Rome", 1);
  tzset();

  if (!read_access_token(access_token, sizeof(access_token))) {
    return EXIT_FAILURE;
  }

  // Connect to MySQL database
  conn = mysql_init(NULL);
  if (conn == NULL) {
    fprintf(stderr, "MySQL init error\n");
    return EXIT_FAILURE;
  }

  if (mysql_real_connect(conn, "localhost", USER, PASSWORD, DB, 0, NULL, 0) == NULL) {
    fprintf(stderr, "MySQL connect error: %s\n", mysql_error(conn));
    mysql_close(conn);
    return EXIT_FAILURE;
  }

  // Initialize cURL library
  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl = curl_easy_init();
  if (curl == NULL) {
    fprintf(stderr, "cURL easy init error\n");
    mysql_close(conn);
    curl_global_cleanup();
    return EXIT_FAILURE;
  }

  snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", access_token);
  headers = curl_slist_append(headers, auth_header);

  // Retrieve file list from Google Drive folder
  snprintf(url, sizeof(url),
           "https://www.googleapis.com/drive/v3/files"
           "?q='%s'+in+parents+and+trashed=false"
           "&fields=files(id,name,mimeType)"
           "&pageSize=100"
           "&supportsAllDrives=true"
           "&includeItemsFromAllDrives=true",
           folder_id);

  mem_init(&list_body);

  if (!http_get(curl, url, headers, &list_body, &http_code) || http_code != 200) {
    fprintf(stderr, "Error fetching file list (HTTP %ld)\n", http_code);
    free(list_body.ptr);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    mysql_close(conn);
    curl_global_cleanup();
    return EXIT_FAILURE;
  }

  file_count = parse_drive_files(list_body.ptr, files, MAX_FILES);
  free(list_body.ptr);

  printf("Found %d file(s) to process in folder %s.\n", file_count, folder_id);

  // Download, insert into MySQL and delete each file from Drive
  for (i = 0; i < file_count; i++) {
    printf("[%d/%d] Downloading '%s' (ID: %s, MIME: %s)...\n",
           i + 1, file_count, files[i].name, files[i].id, files[i].mime_type);

    if (strcmp(files[i].mime_type, "application/vnd.google-apps.spreadsheet") == 0) {
      snprintf(url, sizeof(url),
               "https://www.googleapis.com/drive/v3/files/%s/export?mimeType=text/csv&supportsAllDrives=true",
               files[i].id);
    } else {
      snprintf(url, sizeof(url),
               "https://www.googleapis.com/drive/v3/files/%s?alt=media&supportsAllDrives=true",
               files[i].id);
    }

    mem_init(&file_body);

    if (http_get(curl, url, headers, &file_body, &http_code) && http_code == 200) {
      if (process_and_insert_csv(conn, table_name, file_body.ptr)) {
        printf("  -> Successfully inserted data into table '%s'.\n", table_name);

        if (delete_drive_file(curl, headers, files[i].id)) {
          printf("  -> Successfully deleted '%s' from Google Drive.\n", files[i].name);
        } else {
          fprintf(stderr, "  -> Failed to delete '%s' from Google Drive.\n", files[i].name);
        }
      } else {
        fprintf(stderr, "  -> DB insertion failed for '%s'. File not deleted.\n", files[i].name);
      }
    } else {
      fprintf(stderr, "  -> Download failed for '%s' (HTTP %ld)\n", files[i].name, http_code);
    }

    free(file_body.ptr);
    file_body.ptr = NULL;
    file_body.len = 0;
  }

  // Cleanup resources
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  curl_global_cleanup();
  mysql_close(conn);

  printf("Processing completed.\n");
  return EXIT_SUCCESS;
}
