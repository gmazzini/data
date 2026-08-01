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
  m->ptr = (char*)malloc(1);
  if (m->ptr) m->ptr[0] = '\0';
}

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  struct mem *m = (struct mem*)userp;

  char *p = (char*)realloc(m->ptr, m->len + realsize + 1);
  if (!p) return 0;

  m->ptr = p;
  memcpy(&(m->ptr[m->len]), contents, realsize);
  m->len += realsize;
  m->ptr[m->len] = '\0';
  return realsize;
}

// Read access token from file
static int read_access_token(char *buf, size_t buflen) {
  FILE *fp = fopen(TOKEN_FILE, "r");
  if (!fp) {
    fprintf(stderr, "Error: cannot open token file %s\n", TOKEN_FILE);
    return 0;
  }
  if (!fgets(buf, (int)buflen, fp)) {
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
  char search_key[128];
  snprintf(search_key, sizeof(search_key), "\"%s\"", key);

  const char *p = strstr(start, search_key);
  if (!p || p >= end) return 0;

  const char *colon = strchr(p, ':');
  if (!colon || colon >= end) return 0;

  const char *q1 = strchr(colon, '"');
  if (!q1 || q1 >= end) return 0;
  q1++;

  const char *q2 = strchr(q1, '"');
  if (!q2 || q2 >= end) return 0;

  size_t len = q2 - q1;
  if (len >= max_len) len = max_len - 1;

  memcpy(out_val, q1, len);
  out_val[len] = '\0';
  return 1;
}

// Robust JSON parser for Drive files.list response
static int parse_drive_files(const char *json, DriveFile *out_files, int max_files) {
  int count = 0;
  const char *files_sec = strstr(json, "\"files\"");
  if (!files_sec) return 0;

  const char *p = strstr(files_sec, "[");
  if (!p) return 0;

  while (count < max_files) {
    const char *obj_start = strchr(p, '{');
    if (!obj_start) break;

    const char *obj_end = strchr(obj_start, '}');
    if (!obj_end) break;

    // Extract id and name key values independently
    if (extract_json_value(obj_start, obj_end, "id", out_files[count].id, sizeof(out_files[count].id)) &&
        extract_json_value(obj_start, obj_end, "name", out_files[count].name, sizeof(out_files[count].name))) {

      // Extract mimeType or set default if missing
      if (!extract_json_value(obj_start, obj_end, "mimeType", out_files[count].mime_type, sizeof(out_files[count].mime_type))) {
        strcpy(out_files[count].mime_type, "text/csv");
      }

      count++;
    }

    p = obj_end + 1;
  }
  return count;
}

// Generic HTTP GET request
static int http_get(CURL *curl, const char *url, struct curl_slist *headers, struct mem *out, long *http_code) {
  curl_easy_reset(curl);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "cURL GET error: %s\n", curl_easy_strerror(res));
    return 0;
  }
  if (http_code) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
  }
  return 1;
}

// Delete file from Google Drive via API
static int delete_drive_file(CURL *curl, struct curl_slist *headers, const char *file_id) {
  char url[2048];
  long http = 0;
  struct mem body;
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

  CURLcode res = curl_easy_perform(curl);
  if (res == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
  }
  free(body.ptr);

  if (res == CURLE_OK && (http == 200 || http == 204)) {
    return 1;
  } else {
    fprintf(stderr, "Error deleting file %s on Drive (HTTP %ld)\n", file_id, http);
    return 0;
  }
}

// Parse CSV content and insert values into MySQL
static int process_and_insert_csv(MYSQL *conn, const char *table_name, const char *csv_data) {
  char *data_copy = strdup(csv_data);
  if (!data_copy) return 0;

  char *saveptr = NULL;
  char *line = strtok_r(data_copy, "\r\n", &saveptr);

  // Skip CSV header row
  if (line) {
    line = strtok_r(NULL, "\r\n", &saveptr);
  }

  char prev_date[32] = "";

  while (line) {
    if (strlen(line) == 0) {
      line = strtok_r(NULL, "\r\n", &saveptr);
      continue;
    }

    // Tokenize line by semicolon
    char *tokens[120];
    int token_count = 0;
    char *ptr = line;
    char *start = ptr;

    while (*ptr) {
      if (*ptr == ';') {
        *ptr = '\0';
        tokens[token_count++] = start;
        start = ptr + 1;
      }
      ptr++;
    }
    tokens[token_count++] = start;

    if (token_count >= 2) {
      char date_str[32] = "";
      if (sscanf(tokens[0], "\"%[^\"]\"", date_str) != 1) {
        strncpy(date_str, tokens[0], sizeof(date_str) - 1);
      }

      int day, month, year;
      if (sscanf(date_str, "%d/%d/%d", &day, &month, &year) == 3) {

        int is_second_line_of_day = (strcmp(date_str, prev_date) == 0);
        strncpy(prev_date, date_str, sizeof(prev_date) - 1);

        for (int i = 1; i < token_count && i <= 96; i++) {

          // Handle October DST transition (25-hour day split)
          if (is_second_line_of_day && i < 9) continue;
          if (!is_second_line_of_day && month == 10 && i > 12 && strcmp(tokens[i], "0") == 0) continue;

          // Convert decimal comma to dot
          char val_str[32];
          strncpy(val_str, tokens[i], sizeof(val_str) - 1);
          val_str[sizeof(val_str) - 1] = '\0';
          for (char *c = val_str; *c; c++) {
            if (*c == ',') *c = '.';
          }
          double val = atof(val_str);

          int hour = (i - 1) / 4;
          int minute = ((i - 1) % 4) * 15;

          struct tm tm_info;
          memset(&tm_info, 0, sizeof(struct tm));
          tm_info.tm_year = year - 1900;
          tm_info.tm_mon  = month - 1;
          tm_info.tm_mday = day;
          tm_info.tm_hour = hour;
          tm_info.tm_min  = minute;
          tm_info.tm_sec  = 0;

          if (is_second_line_of_day) {
            tm_info.tm_isdst = 0;  // Second row on DST change day -> Standard Time (CET)
          } else if (month == 10 && day >= 25 && day <= 31 && hour < 3) {
            tm_info.tm_isdst = 1;  // First row on DST change day -> Daylight Saving Time (CEST)
          } else {
            tm_info.tm_isdst = -1; // Default automatic DST resolution
          }

          time_t epoch = mktime(&tm_info);
          if (epoch == -1) continue;

          // Insert query matching epoch and kwh columns
          char query[1024];
          snprintf(query, sizeof(query),
            "INSERT INTO `%s` (epoch, kwh) "
            "VALUES (%ld, %.4f) "
            "ON DUPLICATE KEY UPDATE kwh=VALUES(kwh);",
            table_name, (long)epoch, val);

          if (mysql_query(conn, query)) {
            fprintf(stderr, "MySQL query error on table %s: %s\n", table_name, mysql_error(conn));
            free(data_copy);
            return 0;
          }
        }
      }
    }
    line = strtok_r(NULL, "\r\n", &saveptr);
  }

  free(data_copy);
  return 1;
}

int main(int argc, char *argv[]) {
  // Check command line arguments
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <GOOGLE_FOLDER_ID> <MYSQL_TABLE_NAME>\n", argv[0]);
    return EXIT_FAILURE;
  }

  const char *folder_id = argv[1];
  const char *table_name = argv[2];

  // Set local timezone to Europe/Rome
  setenv("TZ", "Europe/Rome", 1);
  tzset();

  char access_token[512];
  if (!read_access_token(access_token, sizeof(access_token))) {
    return EXIT_FAILURE;
  }

  // Connect to MySQL database using credentials from /home/tools/setup_energy.c
  MYSQL *conn = mysql_init(NULL);
  if (!conn) {
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
  CURL *curl = curl_easy_init();
  if (!curl) {
    fprintf(stderr, "cURL easy init error\n");
    mysql_close(conn);
    curl_global_cleanup();
    return EXIT_FAILURE;
  }

  char auth_header[1024];
  snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", access_token);
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, auth_header);

  // Retrieve file list with mimeType from Google Drive folder
  char url[4096];
  snprintf(url, sizeof(url),
           "https://www.googleapis.com/drive/v3/files"
           "?q='%s'+in+parents+and+trashed=false"
           "&fields=files(id,name,mimeType)"
           "&pageSize=100"
           "&supportsAllDrives=true"
           "&includeItemsFromAllDrives=true",
           folder_id);

  struct mem list_body;
  mem_init(&list_body);
  long http_code = 0;

  if (!http_get(curl, url, headers, &list_body, &http_code) || http_code != 200) {
    fprintf(stderr, "Error fetching file list (HTTP %ld)\n", http_code);
    free(list_body.ptr);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    mysql_close(conn);
    curl_global_cleanup();
    return EXIT_FAILURE;
  }

  DriveFile files[MAX_FILES];
  int file_count = parse_drive_files(list_body.ptr, files, MAX_FILES);
  free(list_body.ptr);

  printf("Found %d file(s) to process in folder %s.\n", file_count, folder_id);

  // Download, insert into MySQL and delete each file from Drive
  for (int i = 0; i < file_count; i++) {
    printf("[%d/%d] Downloading '%s' (ID: %s, MIME: %s)...\n", 
           i + 1, file_count, files[i].name, files[i].id, files[i].mime_type);

    // Handle Google Sheets export vs standard file download
    if (strcmp(files[i].mime_type, "application/vnd.google-apps.spreadsheet") == 0) {
      snprintf(url, sizeof(url),
               "https://www.googleapis.com/drive/v3/files/%s/export?mimeType=text/csv&supportsAllDrives=true",
               files[i].id);
    } else {
      snprintf(url, sizeof(url),
               "https://www.googleapis.com/drive/v3/files/%s?alt=media&supportsAllDrives=true",
               files[i].id);
    }

    struct mem file_body;
    mem_init(&file_body);

    if (http_get(curl, url, headers, &file_body, &http_code) && http_code == 200) {

      // Insert CSV data into MySQL table
      if (process_and_insert_csv(conn, table_name, file_body.ptr)) {
        printf("  -> Successfully inserted data into table '%s'.\n", table_name);

        // Delete file from Google Drive upon successful insertion
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
  }

  // Cleanup resources
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  curl_global_cleanup();
  mysql_close(conn);

  printf("Processing completed.\n");
  return EXIT_SUCCESS;
}
