// Standard system header files
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <mysql/mysql.h>

// Include local energy setup configuration (provides DB credentials and helper functions)
#include "/home/tools/setup_energy.c"

// Target configuration definitions
#define TOKEN_FILE "/home/www/data/google_access_token"
#define SPREADSHEET_ID "1fw-Nq7RPMs9JF4bb62LGrXuqg81v1bPJjupOmGsCCqg"
#define BASE_YEAR 2021

// Buffer structure for HTTP response body
typedef struct {
    char *data;
    size_t size;
} MemoryBuffer;

// Initialize dynamic memory buffer
static void init_memory_buffer(MemoryBuffer *mem) {
    mem->size = 0;
    mem->data = (char *)malloc(1);
    if (mem->data) {
        mem->data[0] = '\0';
    }
}

// Libcurl write callback to append received response bytes
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;

    char *ptr = (char *)realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

// Read access token from local security file
static int read_access_token(const char *filename, char *token, size_t token_size) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Error: Unable to open token file %s\n", filename);
        return 0;
    }
    if (!fgets(token, (int)token_size, fp)) {
        fclose(fp);
        fprintf(stderr, "Error: Unable to read access token\n");
        return 0;
    }
    fclose(fp);
    token[strcspn(token, "\r\n")] = '\0';
    if (token[0] == '\0') {
        fprintf(stderr, "Error: Token file is empty\n");
        return 0;
    }
    return 1;
}

// Convert 1-based column index to Excel column letters (1 -> A, 2 -> B, 6 -> F, etc.)
static void get_column_letter(int col_idx, char *out) {
    char temp[16];
    int i = 0;
    while (col_idx > 0) {
        int rem = (col_idx - 1) % 26;
        temp[i++] = (char)('A' + rem);
        col_idx = (col_idx - 1) / 26;
    }
    for (int j = 0; j < i; j++) {
        out[j] = temp[i - 1 - j];
    }
    out[i] = '\0';
}

// Execute HTTP PUT request to Google Sheets API v4 for a specific range
static int put_google_sheet_range(const char *token, const char *sheet_name,
                                 const char *range_str, const char *json_payload) {
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    char url[1024];
    char auth_header[1024];
    MemoryBuffer body;
    long http_code = 0;

    snprintf(url, sizeof(url),
        "https://sheets.googleapis.com/v4/spreadsheets/%s/values/%s!%s?valueInputOption=USER_ENTERED",
        SPREADSHEET_ID, sheet_name, range_str);

    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);

    init_memory_buffer(&body);

    curl = curl_easy_init();
    if (!curl) {
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

    CURLcode res = curl_easy_perform(curl);
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
        fprintf(stderr, "Response: %s\n", body.data ? body.data : "(null)");
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

int main(int argc, char *argv[]) {
    // Validate command-line arguments
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <kwh_so|kwh_cc> <year>\n", argv[0]);
        fprintf(stderr, "Example: %s kwh_so 2022\n", argv[0]);
        return 1;
    }

    const char *measure_type = argv[1];
    int target_year = atoi(argv[2]);

    if (strcmp(measure_type, "kwh_so") != 0 && strcmp(measure_type, "kwh_cc") != 0) {
        fprintf(stderr, "Error: Invalid measure_type '%s'. Must be 'kwh_so' or 'kwh_cc'.\n", measure_type);
        return 1;
    }

    if (target_year < BASE_YEAR || target_year > 2099) {
        fprintf(stderr, "Error: Invalid target year %d. Must be >= %d.\n", target_year, BASE_YEAR);
        return 1;
    }

    // Determine target Google Sheets tabs based on measure type
    char hourly_tab[16], monthly_tab[16];
    if (strcmp(measure_type, "kwh_so") == 0) {
        strcpy(hourly_tab, "h_so");
        strcpy(monthly_tab, "m_so");
    } else {
        strcpy(hourly_tab, "h_cc");
        strcpy(monthly_tab, "m_cc");
    }

    // Read Google OAuth2 Bearer token
    char access_token[512];
    if (!read_access_token(TOKEN_FILE, access_token, sizeof(access_token))) {
        return 1;
    }

    // Connect to MySQL database using parameters from setup_energy.c
    MYSQL *conn = mysql_init(NULL);
    if (!conn) {
        fprintf(stderr, "MySQL init failed\n");
        return 1;
    }

    if (!mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASS, DB_NAME, 0, NULL, 0)) {
        fprintf(stderr, "MySQL connection error: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    // Prepare Unix timestamps for target year limits in Europe/Rome
    struct tm start_tm = {0}, end_tm = {0};
    start_tm.tm_year = target_year - 1900;
    start_tm.tm_mon = 0;
    start_tm.tm_mday = 1;
    
    end_tm.tm_year = target_year - 1900;
    end_tm.tm_mon = 11;
    end_tm.tm_mday = 31;
    end_tm.tm_hour = 23;
    end_tm.tm_min = 59;
    end_tm.tm_sec = 59;

    time_t start_epoch = mktime(&start_tm);
    time_t end_epoch = mktime(&end_tm);

    // Query MySQL table (energy_15m or pun_15m depending on system schema)
    char query[1024];
    snprintf(query, sizeof(query),
             "SELECT epoch, %s FROM energy_15m WHERE epoch BETWEEN %ld AND %ld ORDER BY epoch ASC",
             measure_type, (long)start_epoch, (long)end_epoch);

    if (mysql_query(conn, query)) {
        // Fallback to pun_15m table if energy_15m is not found
        snprintf(query, sizeof(query),
                 "SELECT epoch, %s FROM pun_15m WHERE epoch BETWEEN %ld AND %ld ORDER BY epoch ASC",
                 measure_type, (long)start_epoch, (long)end_epoch);
        if (mysql_query(conn, query)) {
            fprintf(stderr, "MySQL query failed: %s\n", mysql_error(conn));
            mysql_close(conn);
            return 1;
        }
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        fprintf(stderr, "MySQL store result error: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    // Initialize accumulation metrics
    double hourly_sum[24] = {0.0};
    double hourly_total = 0.0;
    
    double monthly_sum[12][4] = {{0.0}};      // [month][0:F1, 1:F2, 2:F3, 3:Totale]
    double monthly_annual_totals[4] = {0.0};  // [0:F1, 1:F2, 2:F3, 3:Totale]

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!row[0] || !row[1]) continue;

        time_t ep = (time_t)atoll(row[0]);
        double val = atof(row[1]);

        struct tm *t = localtime(&ep);
        if (!t) continue;

        int hour = t->tm_hour;   // 0 .. 23
        int month = t->tm_mon;   // 0 .. 11

        // Determine ARERA tariff band index (1 for F1, 2 for F2, 3 for F3)
        int band = get_band_index(t);

        // Accumulate hourly values
        if (hour >= 0 && hour < 24) {
            hourly_sum[hour] += val;
            hourly_total += val;
        }

        // Accumulate monthly values
        if (month >= 0 && month < 12 && band >= 1 && band <= 3) {
            monthly_sum[month][band - 1] += val; // Band-specific sum
            monthly_sum[month][3] += val;        // Monthly total sum
            
            monthly_annual_totals[band - 1] += val; // Annual band total
            monthly_annual_totals[3] += val;        // Annual grand total
        }
    }

    mysql_free_result(res);
    mysql_close(conn);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "Failed to initialize libcurl\n");
        return 1;
    }

    // -------------------------------------------------------------
    // 1. UPDATE HOURLY SHEET (h_so / h_cc)
    // -------------------------------------------------------------
    int hourly_col_idx = 2 + (target_year - BASE_YEAR); // Col B=2021, C=2022...
    char hourly_col_letter[16];
    get_column_letter(hourly_col_idx, hourly_col_letter);

    char hourly_range[64];
    snprintf(hourly_range, sizeof(hourly_range), "%s2:%s26", hourly_col_letter, hourly_col_letter);

    char *json_h = (char *)malloc(16384);
    if (json_h) {
        int off = snprintf(json_h, 16384, "{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[", hourly_tab, hourly_range);
        for (int h = 0; h < 24; h++) {
            off += snprintf(json_h + off, 16384 - off, "[%.5f],", hourly_sum[h]);
        }
        snprintf(json_h + off, 16384 - off, "[%.5f]]}", hourly_total);

        if (put_google_sheet_range(access_token, hourly_tab, hourly_range, json_h)) {
            printf("Updated '%s' range %s for year %d\n", hourly_tab, hourly_range, target_year);
        }
        free(json_h);
    }

    // -------------------------------------------------------------
    // 2. UPDATE MONTHLY SHEET (m_so / m_cc)
    // -------------------------------------------------------------
    int start_m_col = 2 + (target_year - BASE_YEAR) * 4; // 2021=B(2), 2022=F(6)...
    int end_m_col = start_m_col + 3;

    char start_m_letter[16], end_m_letter[16];
    get_column_letter(start_m_col, start_m_letter);
    get_column_letter(end_m_col, end_m_letter);

    char monthly_range[64];
    snprintf(monthly_range, sizeof(monthly_range), "%s2:%s14", start_m_letter, end_m_letter);

    char *json_m = (char *)malloc(32768);
    if (json_m) {
        int off = snprintf(json_m, 32768, "{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[", monthly_tab, monthly_range);
        for (int m = 0; m < 12; m++) {
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

    curl_global_cleanup();
    return 0;
}
