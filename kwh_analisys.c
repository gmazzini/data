#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mysql/mysql.h>
#include <curl/curl.h>
#include "/home/tools/setup_energy.c"

#define SPREADSHEET_ID "1fw-Nq7RPMs9JF4bb62LGrXuqg81v1bPJjupOmGsCCqg"
#define MIN_YEAR 2015
#define MAX_YEARS 30

// Struttura memoria per libcurl (da sorgente 15)
struct mem {
    char *ptr;
    size_t len;
};

static void mem_init(struct mem *m) {
    m->len = 0;
    m->ptr = (char*)malloc(1);
    if (m->ptr) m->ptr[0] = '\0';
}

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb;
    struct mem *m = (struct mem*)userp;
    char *p = (char*)realloc(m->ptr, m->len + real_size + 1);
    if (!p) return 0;
    m->ptr = p;
    memcpy(m->ptr + m->len, contents, real_size);
    m->len += real_size;
    m->ptr[m->len] = '\0';
    return real_size;
}

// Verifica festività nazionali italiane (da sorgente 14)
int is_festivo_nazionale(int mday, int mon) {
    if ((mday == 1  && mon == 0)  || // Capodanno
        (mday == 6  && mon == 0)  || // Epifania
        (mday == 25 && mon == 3)  || // Liberazione
        (mday == 1  && mon == 4)  || // Festa del Lavoro
        (mday == 2  && mon == 5)  || // Festa della Repubblica
        (mday == 15 && mon == 7)  || // Ferragosto
        (mday == 1  && mon == 10) || // Tutti i Santi
        (mday == 8  && mon == 11) || // Immacolata
        (mday == 25 && mon == 11) || // Natale
        (mday == 26 && mon == 11))   // Santo Stefano
    {
        return 1;
    }
    return 0;
}

// Determinazione fascia oraria: 1=F1, 2=F2, 3=F3 (da sorgente 14)
int get_fascia_oraria(int wday, int mday, int mon, int hour) {
    if (wday == 0 || is_festivo_nazionale(mday, mon)) {
        return 3;
    }
    if (wday >= 1 && wday <= 5) { // Da Lunedì a Venerdì
        if (hour >= 8 && hour < 19) return 1;
        else if ((hour >= 7 && hour < 8) || (hour >= 19 && hour < 23)) return 2;
        else return 3;
    } else { // Sabato
        if (hour >= 7 && hour < 23) return 2;
        else return 3;
    }
}

// Funzione per inviare i dati JSON a Google Sheets API v4 tramite PUT (libcurl)
int update_google_sheet(const char *sheet_name, const char *json_payload, const char *access_token) {
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    struct mem response;
    mem_init(&response);

    curl = curl_easy_init();
    if (!curl) {
        free(response.ptr);
        return 0;
    }

    char url[1024];
    snprintf(url, sizeof(url),
             "https://sheets.googleapis.com/v4/spreadsheets/%s/values/%s!A1?valueInputOption=USER_ENTERED",
             SPREADSHEET_ID, sheet_name);

    char auth_header[2048];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", access_token);

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_payload));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        fprintf(stderr, "Errore aggiornamento foglio '%s' (HTTP %ld): %s\n", sheet_name, http_code, response.ptr);
        free(response.ptr);
        return 0;
    }

    printf("Foglio '%s' aggiornato con successo su Google Sheets!\n", sheet_name);
    free(response.ptr);
    return 1;
}

// Elaborazione di una singola tabella MySQL e invio sui due fogli (h_... e m_...)
int process_table_and_export(MYSQL *conn, const char *table_name, const char *target_h_sheet, const char *target_m_sheet, const char *access_token) {
    char query[512];
    snprintf(query, sizeof(query), "SELECT epoch, kwh FROM `%s`", table_name);

    if (mysql_query(conn, query)) {
        fprintf(stderr, "Errore query MySQL su '%s': %s\n", table_name, mysql_error(conn));
        return 0;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) {
        fprintf(stderr, "Errore recupero dati per '%s'\n", table_name);
        return 0;
    }

    // Strutture per l'aggregazione pluriannuale
    double hourly[MAX_YEARS][24] = {{0.0}};
    double monthly[MAX_YEARS][12][4] = {{{0.0}}}; // [0]=F1, [1]=F2, [2]=F3, [3]=TOT
    int year_present[MAX_YEARS] = {0};

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        if (!row[0] || !row[1]) continue;

        time_t epoch = (time_t)atol(row[0]);
        double kwh = atof(row[1]);

        struct tm tm_info;
        if (localtime_r(&epoch, &tm_info) == NULL) continue;

        int year = tm_info.tm_year + 1900;
        int y_idx = year - MIN_YEAR;

        if (y_idx >= 0 && y_idx < MAX_YEARS) {
            year_present[y_idx] = 1;

            int month = tm_info.tm_mon;   // 0..11
            int mday  = tm_info.tm_mday;  // 1..31
            int wday  = tm_info.tm_wday;  // 0..6
            int hour  = tm_info.tm_hour;  // 0..23

            if (month >= 0 && month < 12) {
                int fascia = get_fascia_oraria(wday, mday, month, hour);
                monthly[y_idx][month][fascia - 1] += kwh;
                monthly[y_idx][month][3] += kwh;
            }

            if (hour >= 0 && hour < 24) {
                hourly[y_idx][hour] += kwh;
            }
        }
    }
    mysql_free_result(result);

    // Identifica quali anni sono effettivamente presenti nel DB
    int valid_years[MAX_YEARS];
    int year_count = 0;
    for (int i = 0; i < MAX_YEARS; i++) {
        if (year_present[i]) {
            valid_years[year_count++] = MIN_YEAR + i;
        }
    }

    if (year_count == 0) {
        printf("Nessun dato trovato nella tabella '%s'.\n", table_name);
        return 1;
    }

    // -------------------------------------------------------------------------
    // 1. Costruzione JSON per Foglio ORARIO (h_so / h_cc)
    // -------------------------------------------------------------------------
    size_t json_alloc = 256 * 1024;
    char *h_json = (char*)malloc(json_alloc);
    h_json[0] = '\0';

    snprintf(h_json, json_alloc, "{\"range\":\"%s!A1\",\"majorDimension\":\"ROWS\",\"values\":[", target_h_sheet);

    // Intestazione
    strcat(h_json, "[\"Ora\"");
    for (int y = 0; y < year_count; y++) {
        char buf[32];
        snprintf(buf, sizeof(buf), ",\"%d\"", valid_years[y]);
        strcat(h_json, buf);
    }
    strcat(h_json, "]");

    // Righe ore (00:00 - 23:00)
    for (int h = 0; h < 24; h++) {
        char buf[64];
        snprintf(buf, sizeof(buf), ",[\"%02d:00\"", h);
        strcat(h_json, buf);
        for (int y = 0; y < year_count; y++) {
            int y_idx = valid_years[y] - MIN_YEAR;
            snprintf(buf, sizeof(buf), ",%.4f", hourly[y_idx][h]);
            strcat(h_json, buf);
        }
        strcat(h_json, "]");
    }

    // Riga TOTALE Orario
    strcat(h_json, ",[\"TOTALE\"");
    for (int y = 0; y < year_count; y++) {
        int y_idx = valid_years[y] - MIN_YEAR;
        double total_h = 0.0;
        for (int h = 0; h < 24; h++) total_h += hourly[y_idx][h];
        char buf[32];
        snprintf(buf, sizeof(buf), ",%.4f", total_h);
        strcat(h_json, buf);
    }
    strcat(h_json, "]");
    strcat(h_json, "]}");

    update_google_sheet(target_h_sheet, h_json, access_token);
    free(h_json);

    // -------------------------------------------------------------------------
    // 2. Costruzione JSON per Foglio MENSILE (m_so / m_cc)
    // -------------------------------------------------------------------------
    char *m_json = (char*)malloc(json_alloc);
    m_json[0] = '\0';

    snprintf(m_json, json_alloc, "{\"range\":\"%s!A1\",\"majorDimension\":\"ROWS\",\"values\":[", target_m_sheet);

    // Intestazione
    strcat(m_json, "[\"Mese\"");
    for (int y = 0; y < year_count; y++) {
        char buf[128];
        snprintf(buf, sizeof(buf), ",\"%d F1\",\"%d F2\",\"%d F3\",\"%d TOTALE\"", valid_years[y], valid_years[y], valid_years[y], valid_years[y]);
        strcat(m_json, buf);
    }
    strcat(m_json, "]");

    // Righe mesi (01 - 12)
    for (int m = 0; m < 12; m++) {
        char buf[64];
        snprintf(buf, sizeof(buf), ",[\"%02d\"", m + 1);
        strcat(m_json, buf);
        for (int y = 0; y < year_count; y++) {
            int y_idx = valid_years[y] - MIN_YEAR;
            snprintf(buf, sizeof(buf), ",%.2f,%.2f,%.2f,%.2f",
                     monthly[y_idx][m][0], monthly[y_idx][m][1], monthly[y_idx][m][2], monthly[y_idx][m][3]);
            strcat(m_json, buf);
        }
        strcat(m_json, "]");
    }

    // Riga TOTALE Mensile
    strcat(m_json, ",[\"TOTALE\"");
    for (int y = 0; y < year_count; y++) {
        int y_idx = valid_years[y] - MIN_YEAR;
        double tot_f1 = 0, tot_f2 = 0, tot_f3 = 0, tot_year = 0;
        for (int m = 0; m < 12; m++) {
            tot_f1 += monthly[y_idx][m][0];
            tot_f2 += monthly[y_idx][m][1];
            tot_f3 += monthly[y_idx][m][2];
            tot_year += monthly[y_idx][m][3];
        }
        char buf[128];
        snprintf(buf, sizeof(buf), ",%.2f,%.2f,%.2f,%.2f", tot_f1, tot_f2, tot_f3, tot_year);
        strcat(m_json, buf);
    }
    strcat(m_json, "]");
    strcat(m_json, "]}");

    update_google_sheet(target_m_sheet, m_json, access_token);
    free(m_json);

    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <GOOGLE_ACCESS_TOKEN>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *access_token = argv[1];

    // Imposta fuso orario italiano
    setenv("TZ", "Europe/Rome", 1);
    tzset();

    curl_global_init(CURL_GLOBAL_DEFAULT);

    MYSQL *conn = mysql_init(NULL);
    if (!conn) {
        fprintf(stderr, "Errore inizializzazione MySQL\n");
        return EXIT_FAILURE;
    }

    if (mysql_real_connect(conn, "localhost", USER, PASSWORD, DB, 0, NULL, 0) == NULL) {
        fprintf(stderr, "Errore connessione MySQL: %s\n", mysql_error(conn));
        mysql_close(conn);
        return EXIT_FAILURE;
    }

    printf("Connessione a MySQL completata. Avvio esportazione dati...\n\n");

    // Elabora sia kwh_so (fogli h_so e m_so) che kwh_cc (fogli h_cc e m_cc)
    process_table_and_export(conn, "kwh_so", "h_so", "m_so", access_token);
    process_table_and_export(conn, "kwh_cc", "h_cc", "m_cc", access_token);

    mysql_close(conn);
    curl_global_cleanup();

    printf("\nOperazione completata con successo!\n");
    return EXIT_SUCCESS;
}
