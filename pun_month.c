// Program to compute monthly PUN band averages and metrics (MIN, MAX, BESTDAY, WORSTDAY, MINDAY)
// Writes results to Google Sheets from column A to J
// Accepts input parameter in format YYYYMM (e.g. 202607)

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

// Google Spreadsheet ID and Tab name
#define SPREADSHEET_ID "1RF4N-T2NR2UHai70AzTzwuLXowkLlOQWvFyb8AaE1xg"
#define SHEET_NAME "pun"

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
    m->ptr = (char*)malloc(1);
    if (m->ptr) m->ptr[0] = '\0';
}

// Curl write callback to accumulate HTTP response body
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

// Read access token from local file
static int read_access_token(char *buf, size_t buflen) {
    FILE *fp = fopen(TOKEN_FILE, "r");
    if (!fp) {
        fprintf(stderr, "Error: unable to open %s\n", TOKEN_FILE);
        return 0;
    }
    if (!fgets(buf, (int)buflen, fp)) {
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

// Calculate Easter Monday date for a given year using Anonymous Gauss algorithm
static void get_easter_monday(int year, int *out_m, int *out_d) {
    int a = year % 19;
    int b = year / 100;
    int c = year % 100;
    int d = b / 4;
    int e = b % 4;
    int f = (b + 8) / 25;
    int g = (b - f + 1) / 3;
    int h = (19 * a + b - d - g + 15) % 30;
    int i = c / 4;
    int k = c % 4;
    int l = (32 + 2 * e + 2 * i - h - k) % 7;
    int m = (a + 11 * h + 22 * l) / 451;
    int month_e = (h + l - 7 * m + 114) / 31;
    int day_e = ((h + l - 7 * m + 114) % 31) + 1;

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
    int m = tm->tm_mon + 1;
    int d = tm->tm_mday;
    int y = tm->tm_year + 1900;

    if ((m == 1 && d == 1) || (m == 1 && d == 6) ||
        (m == 4 && d == 25) || (m == 5 && d == 1) ||
        (m == 6 && d == 2) || (m == 8 && d == 15) ||
        (m == 11 && d == 1) || (m == 12 && d == 8) ||
        (m == 12 && d == 25) || (m == 12 && d == 26)) {
        return 1;
    }

    int em_m = 0, em_d = 0;
    get_easter_monday(y, &em_m, &em_d);
    if (m == em_m && d == em_d) {
        return 1;
    }

    return 0;
}

// Determine ARERA energy band index (1 = F1, 2 = F2, 3 = F3)
static int get_band_index(struct tm *tm) {
    int dow = tm->tm_wday;
    int h = tm->tm_hour;

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

// Update Google Sheet row using Google Sheets API v4 (Columns A to J)
static int update_google_sheet(const char *token, const char *aaaamm, int row_index,
                                double f0, double f1, double f2, double f3,
                                double min_val, double max_val,
                                const char *bestday, const char *worstday, const char *minday) {
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    char url[512];
    char json_payload[1024];
    char auth_header[1024];
    struct mem body;
    long http_code = 0;

    snprintf(url, sizeof(url),
        "https://sheets.googleapis.com/v4/spreadsheets/%s/values/%s!A%d:J%d?valueInputOption=USER_ENTERED",
        SPREADSHEET_ID, SHEET_NAME, row_index, row_index);

    snprintf(json_payload, sizeof(json_payload),
        "{"
        "\"range\":\"%s!A%d:J%d\","
        "\"majorDimension\":\"ROWS\","
        "\"values\":[["
        "\"%s\",%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,\"%s\",\"%s\",\"%s\""
        "]]"
        "}",
        SHEET_NAME, row_index, row_index,
        aaaamm, f0, f1, f2, f3, min_val, max_val, bestday, worstday, minday);

    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);

    mem_init(&body);

    curl = curl_easy_init();
    if (!curl) {
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

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Google Sheets API curl error: %s\n", curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(body.ptr);
        return 0;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        fprintf(stderr, "Google Sheets API HTTP %ld\n", http_code);
        fprintf(stderr, "Response: %s\n", body.ptr ? body.ptr : "(null)");
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
    if (argc != 2) {
        fprintf(stderr, "Usage: %s YYYYMM\n", argv[0]);
        fprintf(stderr, "Example: %s 202607\n", argv[0]);
        return 1;
    }

    if (strlen(argv[1]) != 6) {
        fprintf(stderr, "Error: parameter must be 6 digits (YYYYMM)\n");
        return 1;
    }

    int year = 0, month = 0;
    if (sscanf(argv[1], "%4d%2d", &year, &month) != 2 || month < 1 || month > 12) {
        fprintf(stderr, "Error: invalid year or month in %s\n", argv[1]);
        return 1;
    }

    int delta_months = (year - 2025) * 12 + (month - 10);
    int row_index = 2 + delta_months;

    if (row_index < 2) {
        fprintf(stderr, "Error: Date %s is before base date 202510\n", argv[1]);
        return 1;
    }

    setenv("TZ", "Europe/Rome", 1);
    tzset();

    struct tm start_tm;
    memset(&start_tm, 0, sizeof(struct tm));
    start_tm.tm_year = year - 1900;
    start_tm.tm_mon = month - 1;
    start_tm.tm_mday = 1;
    start_tm.tm_hour = 0;
    start_tm.tm_min = 0;
    start_tm.tm_sec = 0;
    start_tm.tm_isdst = -1;

    time_t sds = mktime(&start_tm);

    struct tm next_tm = start_tm;
    if (month == 12) {
        next_tm.tm_year += 1;
        next_tm.tm_mon = 0;
    } else {
        next_tm.tm_mon += 1;
    }
    next_tm.tm_isdst = -1;

    time_t sde = mktime(&next_tm) - 1;

    MYSQL *conn = mysql_init(NULL);
    if (!conn) {
        fprintf(stderr, "Error: mysql_init failed\n");
        return 1;
    }

    if (!mysql_real_connect(conn, "localhost", USER, PASSWORD, DB, 0, NULL, 0)) {
        fprintf(stderr, "MySQL connection error: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    char query[512];
    snprintf(query, sizeof(query),
        "SELECT epoch, c FROM pun_15m WHERE epoch BETWEEN %ld AND %ld ORDER BY epoch ASC",
        (long)sds, (long)sde);

    if (mysql_query(conn, query)) {
        fprintf(stderr, "MySQL query error: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) {
        fprintf(stderr, "MySQL store result error: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    static Record records[3500];
    int total_records = 0;

    double sums[4] = {0.0, 0.0, 0.0, 0.0};
    long counts[4] = {0, 0, 0, 0};

    double global_min = DBL_MAX;
    double global_max = -DBL_MAX;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        if (!row[0] || !row[1]) continue;

        long long epoch_val = atoll(row[0]);
        double c_val = atof(row[1]);

        time_t t_val = (time_t)epoch_val;
        struct tm local_tm;
        localtime_r(&t_val, &local_tm);

        int band = get_band_index(&local_tm);

        if (c_val < global_min) global_min = c_val;
        if (c_val > global_max) global_max = c_val;

        if (total_records < 3500) {
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

    double f0 = counts[0] > 0 ? sums[0] / counts[0] : 0.0;
    double f1 = counts[1] > 0 ? sums[1] / counts[1] : 0.0;
    double f2 = counts[2] > 0 ? sums[2] / counts[2] : 0.0;
    double f3 = counts[3] > 0 ? sums[3] / counts[3] : 0.0;

    // Calculate daily averages for BESTDAY and WORSTDAY
    double day_sums[32] = {0.0};
    long day_counts[32] = {0};

    for (int i = 0; i < total_records; i++) {
        int d = records[i].day;
        if (d >= 1 && d <= 31) {
            day_sums[d] += records[i].price;
            day_counts[d]++;
        }
    }

    int best_day = 1;
    int worst_day = 1;
    double min_day_avg = DBL_MAX;
    double max_day_avg = -DBL_MAX;

    for (int d = 1; d <= 31; d++) {
        if (day_counts[d] > 0) {
            double avg = day_sums[d] / day_counts[d];
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

    // Calculate MINDAY (Option A: Day with longest sequence of consecutive slots at global minimum)
    int max_consecutive_per_day[32] = {0};

    for (int d = 1; d <= 31; d++) {
        int current_run = 0;
        int max_run = 0;
        for (int i = 0; i < total_records; i++) {
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

    int min_day = best_day;
    int max_consec_run = -1;

    for (int d = 1; d <= 31; d++) {
        if (day_counts[d] > 0) {
            if (max_consecutive_per_day[d] > max_consec_run) {
                max_consec_run = max_consecutive_per_day[d];
                min_day = d;
            }
        }
    }

    char bestday_str[16], worstday_str[16], minday_str[16];
    snprintf(bestday_str, sizeof(bestday_str), "%04d-%02d-%02d", year, month, best_day);
    snprintf(worstday_str, sizeof(worstday_str), "%04d-%02d-%02d", year, month, worst_day);
    snprintf(minday_str, sizeof(minday_str), "%04d-%02d-%02d", year, month, min_day);

    char access_token[512];
    if (!read_access_token(access_token, sizeof(access_token))) {
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "Error: curl_global_init failed\n");
        return 1;
    }

    if (!update_google_sheet(access_token, argv[1], row_index,
                             f0, f1, f2, f3,
                             global_min, global_max,
                             bestday_str, worstday_str, minday_str)) {
        fprintf(stderr, "Error: failed to update Google Sheet\n");
        curl_global_cleanup();
        return 1;
    }

    printf("OK: Updated Sheet row %d for %s\n", row_index, argv[1]);
    printf("F0: %.5f | F1: %.5f | F2: %.5f | F3: %.5f\n", f0, f1, f2, f3);
    printf("MIN: %.5f | MAX: %.5f | BESTDAY: %s | WORSTDAY: %s | MINDAY: %s\n",
           global_min, global_max, bestday_str, worstday_str, minday_str);

    curl_global_cleanup();
    return 0;
}
