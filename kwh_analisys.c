// Standard system header files
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

// Filepath containing the Google OAuth2 access token
#define TOKEN_FILE "/home/www/data/google_access_token"

// Google Sheets configuration constants
#define SPREADSHEET_ID "1fw-Nq7RPMs9JF4bb62LGrXuqg81v1bPJjupOmGsCCqg
#define SHEET_NAME "pun"

// Buffer structure to hold API HTTP response body
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

// Curl write callback to append received data chunk to memory buffer
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;

    char *ptr = (char *)realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';

    return realsize;
}

// Read Bearer token from local security file
static int read_access_token(const char *filename, char *token, size_t token_size) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Error: Unable to open token file %s\n", filename);
        return 0;
    }

    if (!fgets(token, (int)token_size, fp)) {
        fclose(fp);
        fprintf(stderr, "Error: Unable to read token from file\n");
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

// Send HTTP PUT request to update specified row in Google Sheets API v4
static int update_google_sheet_row(const char *token, const char *spreadsheet_id,
                                   const char *sheet_name, int row_index,
                                   const char *date_code, double f0, double f1,
                                   double f2, double f3, double min_val,
                                   double max_val, const char *bestday,
                                   const char *worstday, const char *minday) {
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    char url[512];
    char json_payload[1024];
    char auth_header[1024];
    MemoryBuffer body;
    long http_code = 0;

    // Build Google Sheets REST API v4 endpoint URL with USER_ENTERED option
    snprintf(url, sizeof(url),
        "https://sheets.googleapis.com/v4/spreadsheets/%s/values/%s!A%d:J%d?valueInputOption=USER_ENTERED",
        spreadsheet_id, sheet_name, row_index, row_index);

    // Format JSON body matching Google Sheets API value update payload
    snprintf(json_payload, sizeof(json_payload),
        "{"
        "\"range\":\"%s!A%d:J%d\","
        "\"majorDimension\":\"ROWS\","
        "\"values\":[["
        "\"%s\",%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,\"%s\",\"%s\",\"%s\""
        "]]"
        "}",
        sheet_name, row_index, row_index,
        date_code, f0, f1, f2, f3, min_val, max_val, bestday, worstday, minday);

    // Prepare Authorization header with Bearer token
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);

    init_memory_buffer(&body);

    curl = curl_easy_init();
    if (!curl) {
        free(body.data);
        return 0;
    }

    // Set up HTTP request headers
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);

    // Set curl options for PUT REST request
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

    // Execute HTTP request
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Curl error: %s\n", curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(body.data);
        return 0;
    }

    // Verify HTTP status code response (2xx means success)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        fprintf(stderr, "Google Sheets API Error HTTP %ld\n", http_code);
        fprintf(stderr, "Response payload: %s\n", body.data ? body.data : "(null)");
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(body.data);
        return 0;
    }

    // Free allocated headers and memory resources
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.data);
    return 1;
}

int main(void) {
    char access_token[512];

    // Retrieve token from local file
    if (!read_access_token(TOKEN_FILE, access_token, sizeof(access_token))) {
        return 1;
    }

    // Initialize libcurl library
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "Failed to initialize libcurl\n");
        return 1;
    }

    // Example call to write values into row 2 (Columns A to J)
    int success = update_google_sheet_row(
        access_token, SPREADSHEET_ID, SHEET_NAME, 2,
        "202607", 112.50000, 120.10000, 115.30000, 98.40000,
        75.00000, 180.20000, "2026-07-12", "2026-07-28", "2026-07-05"
    );

    if (success) {
        printf("Successfully updated Google Sheet row!\n");
    } else {
        printf("Failed to update Google Sheet.\n");
    }

    // Clean up libcurl environment
    curl_global_cleanup();
    return 0;
}
