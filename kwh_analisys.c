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
static int is_festivo(const struct tm *tm) {
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

// Determine ARERA tariff band index (1 = F1, 2 = F2, 3 = F3)
static int get_band_index(const struct tm *t) {
    int wday = t->tm_wday;
    int hour = t->tm_hour;

    if (wday == 0 || is_festivo(t)) return 3;

    if (wday == 6) {
        if (hour >= 7 && hour < 23) return 2;
        return 3;
    }

    if (hour >= 8 && hour < 19) return 1;
    if ((hour >= 7 && hour < 8) || (hour >= 19 && hour < 23)) return 2;
    return 3;
}

static void init_memory_buffer(MemoryBuffer *mem) {
    mem->size = 0;
    mem->data = (char *)malloc(1);
    if (mem->data) mem->data[0] = '\0';
}

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
    return (token[0] != '\0');
}

static void get_column_letter(int col_idx, char *out) {
    char temp[16];
    int i = 0;
    while (col_idx > 0) {
        int rem = (col_idx - 1) % 26;
        temp[i++] = (char)('A' + rem);
        col_idx = (col_idx - 1) / 26;
    }
    for (int j = 0; j < i; j++) out[j] = temp[i - 1 - j];
    out[i] = '\0';
}

static int put_google_sheet_range(const char *token, const char *sheet_name,
                                 const char *range_str, const char *json_payload) {
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    char url[1024], auth_header[1024];
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
        return 1;
    }

    const char *measure_type = argv[1];
    int target_year = atoi(argv[2]);

    if (strcmp(measure_type, "kwh_so") != 0 && strcmp(measure_type, "kwh_cc") != 0) {
        fprintf(stderr, "Error: Invalid measure_type '%s'. Must be 'kwh_so' or 'kwh_cc'.\n", measure_type);
        return 1;
    }

    int is_so = (strcmp(measure_type, "kwh_so") == 0);
    int base_year = is_so ? 2021 : 2024;
    const char *energy_table = is_so ? "energy_so" : "energy_cc";

    if (target_year < base_year || target_year > 2099) {
        fprintf(stderr, "Error: Invalid target year %d. Must be >= %d.\n", target_year, base_year);
        return 1;
    }

    setenv("TZ", "Europe/Rome", 1);
    tzset();

    char hourly_tab[16], monthly_tab[16], compare_tab[16];
    snprintf(hourly_tab, sizeof(hourly_tab), "%s", is_so ? "h_so" : "h_cc");
    snprintf(monthly_tab, sizeof(monthly_tab), "%s", is_so ? "m_so" : "m_cc");
    snprintf(compare_tab, sizeof(compare_tab), "%s", is_so ? "d_so" : "d_cc");

    char access_token[512];
    if (!read_access_token(TOKEN_FILE, access_token, sizeof(access_token))) return 1;

    MYSQL *conn = mysql_init(NULL);
    if (!conn) return 1;

    if (mysql_real_connect(conn, "localhost", USER, PASSWORD, DB, 0, NULL, 0) == NULL) {
        fprintf(stderr, "mysql connect error: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    // Intervallo temporale per il target_year
    struct tm start_tm = {0}, end_tm = {0};
    start_tm.tm_year = target_year - 1900; start_tm.tm_mon = 0; start_tm.tm_mday = 1; start_tm.tm_isdst = -1;
    end_tm.tm_year = target_year - 1900; end_tm.tm_mon = 11; end_tm.tm_mday = 31; end_tm.tm_hour = 23; end_tm.tm_min = 59; end_tm.tm_sec = 59; end_tm.tm_isdst = -1;

    time_t start_epoch = mktime(&start_tm);
    time_t end_epoch = mktime(&end_tm);

    // -------------------------------------------------------------
    // 1. ELABORAZIONE TABELLA FISCALE (kwh_so / kwh_cc)
    // -------------------------------------------------------------
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

    // -------------------------------------------------------------
    // 2. ELABORAZIONE MISURATORE TERZO (energy_so / energy_cc)
    // -------------------------------------------------------------
    // Query che legge da 7 giorni prima per trovare la lettura iniziale
    time_t margin_start = start_epoch - (7 * 86400);
    snprintf(query, sizeof(query),
             "SELECT epoch, (e1 + e2 + e3) AS total_e FROM %s WHERE epoch BETWEEN %ld AND %ld ORDER BY epoch ASC",
             energy_table, (long)margin_start, (long)end_epoch);

    double energy_monthly_sum[12] = {0.0};
    double energy_annual_total = 0.0;

    if (mysql_query(conn, query) == 0) {
        MYSQL_RES *res_e = mysql_store_result(conn);
        if (res_e) {
            double prev_val = -1.0;
            while ((row = mysql_fetch_row(res_e))) {
                if (!row[0] || !row[1]) continue;
                time_t ep = (time_t)atoll(row[0]);
                double curr_val = atof(row[1]);

                if (prev_val >= 0.0) {
                    double delta = curr_val - prev_val;
                    // Filtro per evitare valori negativi (reset contatore) o anomali
                    if (delta > 0.0 && ep >= start_epoch && ep <= end_epoch) {
                        struct tm *t = localtime(&ep);
                        if (t && t->tm_mon >= 0 && t->tm_mon < 12) {
                            energy_monthly_sum[t->tm_mon] += delta;
                            energy_annual_total += delta;
                        }
                    }
                }
                prev_val = curr_val;
            }
            mysql_free_result(res_e);
        }
    } else {
        fprintf(stderr, "Warning: unable to query %s: %s\n", energy_table, mysql_error(conn));
    }

    mysql_close(conn);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) return 1;

    // -------------------------------------------------------------
    // Google Sheets Update: 1. HOURLY TAB (h_so / h_cc)
    // -------------------------------------------------------------
    int hourly_col_idx = 2 + (target_year - base_year);
    char hourly_col_letter[16];
    get_column_letter(hourly_col_idx, hourly_col_letter);

    char hourly_range[64];
    snprintf(hourly_range, sizeof(hourly_range), "%s2:%s26", hourly_col_letter, hourly_col_letter);

    char *json_h = (char *)malloc(16384);
    if (json_h) {
        int off = snprintf(json_h, 16384, "{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[", hourly_tab, hourly_range);
        for (int h = 0; h < 24; h++) off += snprintf(json_h + off, 16384 - off, "[%.5f],", hourly_sum[h]);
        snprintf(json_h + off, 16384 - off, "[%.5f]]}", hourly_total);

        if (put_google_sheet_range(access_token, hourly_tab, hourly_range, json_h)) {
            printf("Updated '%s' range %s for year %d\n", hourly_tab, hourly_range, target_year);
        }
        free(json_h);
    }

    // -------------------------------------------------------------
    // Google Sheets Update: 2. MONTHLY TAB (m_so / m_cc)
    // -------------------------------------------------------------
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

    // -------------------------------------------------------------
    // Google Sheets Update: 3. COMPARISON TAB (d_so / d_cc)
    // -------------------------------------------------------------
    // Ogni anno occupa 2 colonne: Col 1 = kwh_*, Col 2 = energy_*
    int start_d_col = 2 + (target_year - base_year) * 2;
    int end_d_col = start_d_col + 1;

    char start_d_letter[16], end_d_letter[16];
    get_column_letter(start_d_col, start_d_letter);
    get_column_letter(end_d_col, end_d_letter);

    char compare_range[64];
    snprintf(compare_range, sizeof(compare_range), "%s2:%s14", start_d_letter, end_d_letter);

    char *json_d = (char *)malloc(16384);
    if (json_d) {
        int off = snprintf(json_d, 16384, "{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[", compare_tab, compare_range);
        for (int m = 0; m < 12; m++) {
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

    curl_global_cleanup();
    return 0;
}
