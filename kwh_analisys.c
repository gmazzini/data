#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mysql/mysql.h>
#include "/home/tools/setup_energy.c"

int main(int argc, char *argv[]) {
    // Controllo argomenti da riga di comando
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <NOME_TABELLA_MYSQL> <ANNO_AAAA>\n", argv[0]);
        fprintf(stderr, "Esempio: %s consumi_casa 2024\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *table_name = argv[1];
    int target_year = atoi(argv[2]);

    if (target_year < 1970 || target_year > 2100) {
        fprintf(stderr, "Errore: Anno '%s' non valido.\n", argv[2]);
        return EXIT_FAILURE;
    }

    // Imposta il fuso orario locale su Europe/Rome (gestione automatica CET/CEST)
    setenv("TZ", "Europe/Rome", 1);
    tzset();

    // Connessione a MySQL con le credenziali da setup_energy.c
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

    // Query per recuperare tutti i record di epoch e kwh
    char query[1024];
    snprintf(query, sizeof(query), "SELECT epoch, kwh FROM `%s`", table_name);

    if (mysql_query(conn, query)) {
        fprintf(stderr, "Errore query MySQL su tabella '%s': %s\n", table_name, mysql_error(conn));
        mysql_close(conn);
        return EXIT_FAILURE;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) {
        fprintf(stderr, "Errore recupero risultati: %s\n", mysql_error(conn));
        mysql_close(conn);
        return EXIT_FAILURE;
    }

    double hourly_kwh[24] = {0.0};
    long hourly_samples[24] = {0};
    double total_year_kwh = 0.0;
    long total_samples = 0;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        if (!row[0] || !row[1]) continue;

        time_t epoch = (time_t)atol(row[0]);
        double kwh = atof(row[1]);

        struct tm tm_info;
        if (localtime_r(&epoch, &tm_info) == NULL) continue;

        int row_year = tm_info.tm_year + 1900;
        if (row_year == target_year) {
            int hour = tm_info.tm_hour; // Ora locale 0..23
            if (hour >= 0 && hour < 24) {
                hourly_kwh[hour] += kwh;
                hourly_samples[hour]++;
                total_year_kwh += kwh;
                total_samples++;
            }
        }
    }

    mysql_free_result(result);
    mysql_close(conn);

    if (total_samples == 0) {
        printf("Nessun dato trovato per l'anno %d nella tabella '%s'.\n", target_year, table_name);
        return EXIT_SUCCESS;
    }

    // Stampa del Report Finale
    printf("\n");
    printf("=======================================================================\n");
    printf(" ANALISI CONSUMI ORARI ANNO %d (Tabella: %s)\n", target_year, table_name);
    printf(" Fuso Orario: Europe/Rome (Ora Locale)\n");
    printf("=======================================================================\n");
    printf(" Ora Locale   | Totale kWh     | Letture (15 min) | %% sul Totale Annuo\n");
    printf("--------------+----------------+------------------+--------------------\n");

    for (int h = 0; h < 24; h++) {
        double pct = (total_year_kwh > 0.0) ? (hourly_kwh[h] / total_year_kwh) * 100.0 : 0.0;
        printf("  %02d:00 - %02d:00 | %14.4f | %16ld | %17.2f%%\n",
               h, (h + 1) % 24, hourly_kwh[h], hourly_samples[h], pct);
    }

    printf("--------------+----------------+------------------+--------------------\n");
    printf("  TOTALE      | %14.4f | %16ld |             100.00%%\n", total_year_kwh, total_samples);
    printf("=======================================================================\n\n");

    return EXIT_SUCCESS;
}
