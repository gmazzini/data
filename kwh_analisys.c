// Standard system header files
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

// Path to Google OAuth2 token file
#define TOKEN_FILE "/home/www/data/google_access_token"

// Google Spreadsheet ID target
#define SPREADSHEET_ID "1fw-Nq7RPMs9JF4bb62LGrXuqg81v1bPJjupOmGsCCqg"

// Maximum number of supported years (e.g. 2021 to 2030)
#define MAX_YEARS 10

// Memory buffer structure for HTTP response body
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

// Libcurl write callback to accumulate response data
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

// Read access token from local file
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
        fprintf(stderr, "Error: Access token file is empty\n");
        return 0;
    }
    return 1;
}

// Convert zero-based column index to Excel column letters (0 -> A, 1 -> B, 25 -> Z, 26 -> AA)
static void get_column_letter(int col_idx, char *out) {
    char temp[16];
    int i = 0;
    col_idx++;
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

// Execute HTTP PUT request to Google Sheets API v4
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
        fprintf(stderr, "Google Sheets API Error HTTP %ld on tab %s\n", http_code, sheet_name);
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

// Write hourly table (h_so or h_cc) from Row 1 to 26 across all years
static int update_hourly_sheet(const char *token, const char *tab_name,
                               int start_year, int num_years,
                               double hourly_data[24][MAX_YEARS], double year_totals[MAX_YEARS]) {
    char end_col_letter[16];
    get_column_letter(num_years, end_col_letter);

    char range_str[64];
    snprintf(range_str, sizeof(range_str), "A1:%s26", end_col_letter);

    char *json = (char *)malloc(65536);
    if (!json) return 0;

    int offset = snprintf(json, 65536, "{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[", tab_name, range_str);

    // Header row: "Ora", "2021", "2022", ...
    offset += snprintf(json + offset, 65536 - offset, "[\"Ora\"");
    for (int y = 0; y < num_years; y++) {
        offset += snprintf(json + offset, 65536 - offset, ",\"%d\"", start_year + y);
    }
    offset += snprintf(json + offset, 65536 - offset, "]");

    // Hourly rows: 00:00 to 23:00 (Rows 2 to 25)
    for (int h = 0; h < 24; h++) {
        offset += snprintf(json + offset, 65536 - offset, ",[\"%02d:00\"", h);
        for (int y = 0; y < num_years; y++) {
            offset += snprintf(json + offset, 65536 - offset, ",%.5f", hourly_data[h][y]);
        }
        offset += snprintf(json + offset, 65536 - offset, "]");
    }

    // Totals row (Row 26)
    offset += snprintf(json + offset, 65536 - offset, ",[\"Totale\"");
    for (int y = 0; y < num_years; y++) {
        offset += snprintf(json + offset, 65536 - offset, ",%.5f", year_totals[y]);
    }
    offset += snprintf(json + offset, 65536 - offset, "]]}");

    int res = put_google_sheet_range(token, tab_name, range_str, json);
    free(json);
    return res;
}

// Write monthly table (m_so or m_cc) from Row 1 to 14 with 4 columns per year (F1, F2, F3, Totale)
static int update_monthly_sheet(const char *token, const char *tab_name,
                                int start_year, int num_years,
                                double monthly_data[12][MAX_YEARS][4], double annual_totals[MAX_YEARS][4]) {
    int total_cols = 1 + num_years * 4;
    char end_col_letter[16];
    get_column_letter(total_cols - 1, end_col_letter);

    char range_str[64];
    snprintf(range_str, sizeof(range_str), "A1:%s14", end_col_letter);

    char *json = (char *)malloc(131072);
    if (!json) return 0;

    int offset = snprintf(json, 131072, "{\"range\":\"%s!%s\",\"majorDimension\":\"ROWS\",\"values\":[", tab_name, range_str);

    // Header row: "Mese", "2021 F1", "2021 F2", "2021 F3", "2021 Totale", ...
    offset += snprintf(json + offset, 131072 - offset, "[\"Mese\"");
    for (int y = 0; y < num_years; y++) {
        int yr = start_year + y;
        offset += snprintf(json + offset, 131072 - offset, ",\"%d F1\",\"%d F2\",\"%d F3\",\"%d Totale\"", yr, yr, yr, yr);
    }
    offset += snprintf(json + offset, 131072 - offset, "]");

    // Monthly rows: 01 to 12 (Rows 2 to 13)
    for (int m = 0; m < 12; m++) {
        offset += snprintf(json + offset, 131072 - offset, ",[\"%02d\"", m + 1);
        for (int y = 0; y < num_years; y++) {
            offset += snprintf(json + offset, 131072 - offset, ",%.5f,%.5f,%.5f,%.5f",
                               monthly_data[m][y][0],
                               monthly_data[m][y][1],
                               monthly_data[m][y][2],
                               monthly_data[m][y][3]);
        }
        offset += snprintf(json + offset, 131072 - offset, "]");
    }

    // Totals row (Row 14)
    offset += snprintf(json + offset, 131072 - offset, ",[\"Totale\"");
    for (int y = 0; y < num_years; y++) {
        offset += snprintf(json + offset, 131072 - offset, ",%.5f,%.5f,%.5f,%.5f",
                           annual_totals[y][0],
                           annual_totals[y][1],
                           annual_totals[y][2],
                           annual_totals[y][3]);
    }
    offset += snprintf(json + offset, 131072 - offset, "]]}");

    int res = put_google_sheet_range(token, tab_name, range_str, json);
    free(json);
    return res;
}

int main(void) {
    char access_token[512];

    if (!read_access_token(TOKEN_FILE, access_token, sizeof(access_token))) {
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "Failed to initialize libcurl\n");
        return 1;
    }

    // Sample dimensions (e.g., 2021 to 2025 -> 5 years)
    int start_year = 2021;
    int num_years = 5;

    // Dummy data structures to demonstrate memory layout
    static double h_so_data[24][MAX_YEARS];
    static double h_so_totals[MAX_YEARS];
    
    static double m_so_data[12][MAX_YEARS][4];
    static double m_so_totals[MAX_YEARS][4];

    // 1. Update Hourly Tab h_so
    if (update_hourly_sheet(access_token, "h_so", start_year, num_years, h_so_data, h_so_totals)) {
        printf("Successfully updated 'h_so'\n");
    }

    // 2. Update Hourly Tab h_cc
    if (update_hourly_sheet(access_token, "h_cc", start_year, num_years, h_so_data, h_so_totals)) {
        printf("Successfully updated 'h_cc'\n");
    }

    // 3. Update Monthly Tab m_so
    if (update_monthly_sheet(access_token, "m_so", start_year, num_years, m_so_data, m_so_totals)) {
        printf("Successfully updated 'm_so'\n");
    }

    // 4. Update Monthly Tab m_cc
    if (update_monthly_sheet(access_token, "m_cc", start_year, num_years, m_so_data, m_so_totals)) {
        printf("Successfully updated 'm_cc'\n");
    }

    curl_global_cleanup();
    return 0;
}
