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

// Determine ARERA tariff band index (1 = F1, 2 = F2, 3 = F3)
static int get_band_index(const struct tm *t) {
    int wday = t->tm_wday; // 0 = Sunday, 1 = Monday, ..., 6 = Saturday
    int hour = t->tm_hour; // 0 .. 23

    // Sunday is always F3
    if (wday == 0) return 3;

    // Saturday: 07:00-23:00 -> F2, rest -> F3
    if (wday == 6) {
        if (hour >= 7 && hour < 23) return 2;
        return 3;
    }

    // Monday to Friday: 08:00-19:00 -> F1, 07:00-08:00 & 19:00-23:00 -> F2, night -> F3
    if (hour >= 8 && hour < 19) return 1;
    if ((hour >= 7 && hour < 8) || (hour >= 19 && hour < 23)) return 2;
    return 3;
}

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

    // Impostazione dell'anno di partenza dinamico
    int base_year = (strcmp(measure_type, "kwh_so") == 0) ? 2021 : 2024;

    if (target_year < base_year || target_year > 2099) {
        fprintf(stderr, "Error: Invalid target year %d for %s. Must be >= %d.\n", target_year, measure_type, base_year);
        return 1;
    }

    // Set timezone to Europe/Rome for accurate timestamp breakdown
    setenv("TZ", "Europe/Rome", 1);
    tzset();

    char hourly_tab[16], monthly_tab[16];
    if (strcmp(measure_type, "kwh_so") == 0) {
        strcpy(hourly_tab, "h_so");
        strcpy(monthly_tab, "m_so");
    } else {
        strcpy(hourly_tab, "h_cc");
        strcpy(monthly_tab, "m_cc");
    }

    char access_token[512];
    if (!read_access_token(TOKEN_FILE, access_token, sizeof(access_token))) {
        return 1;
    }

    // Connect to MySQL database using USER, PASSWORD, DB from setup_energy.c
    MYSQL *conn = mysql_init(NULL);
    if (!conn) {
        fprintf(stderr, "mysql init error\n");
        return 1;
    }

    if (mysql_real_connect(conn, "localhost", USER, PASSWORD, DB, 0, NULL, 0) == NULL) {
        fprintf(stderr, "mysql connect error: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    struct tm start_tm = {0}, end_tm = {0};
    start_tm.tm_year = target_year - 1900;
    start_tm.tm_mon = 0;
    start_tm.tm_mday = 1;
    start_tm.tm_hour = 0;
    start_tm.tm_min = 0;
    start_tm.tm_sec = 0;
    start_tm.tm_isdst = -1;

    end_tm.tm_year = target_year - 1900;
    end_tm.tm_mon = 11;
    end_tm.tm_mday = 31;
    end_tm.tm_hour = 23;
    end_tm.tm_min = 59;
    end_tm.tm_sec = 59;
    end_tm.tm_isdst = -1;

    time_t start_epoch = mktime(&start_tm);
    time_t end_epoch = mktime(&end_tm);

    // Query per la tabella selezionata (kwh_so oppure kwh_cc)
    char query[1024];
    snprintf(query, sizeof(query),
             "SELECT epoch, kwh FROM %s WHERE epoch BETWEEN %ld AND %ld ORDER BY epoch ASC",
             measure_type, (long)start_epoch, (long)end_epoch);

    if (mysql_query(conn, query)) {
        fprintf(stderr, "mysql query error: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        fprintf(stderr, "mysql store result error: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    double hourly_sum[24] = {0.0};
    double hourly_total = 0.0;
    
    double monthly_sum[12][4] = {{0.0}};
    double monthly_annual_totals[4] = {0.0};

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!row[0] || !row[1]) continue;

        time_t ep = (time_t)atoll(row[0]);
        double val = atof(row[1]);

        struct tm *t = localtime(&ep);
        if (!t) continue;

        int hour = t->tm_hour;
        int month = t->tm_mon;

        int band = get_band_index(t);

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
    mysql_close(conn);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "Failed to initialize libcurl\n");
        return 1;
    }

    // 1. UPDATE HOURLY TAB
    // SO: Colonna B (idx 2) = 2021 | CC: Colonna B (idx 2) = 2024
    int hourly_col_idx = 2 + (target_year - base_year);
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

    // 2. UPDATE MONTHLY TAB
    // SO: B..E (2021), F..I (2022) | CC: B..E (2024), F..I (2025)
    int start_m_col = 2 + (target_year - base_year) * 4;
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
