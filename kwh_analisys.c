// Include standard C libraries and MySQL header
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mysql/mysql.h>
#include "/home/tools/setup_energy.c"

// Month names in Italian for table output
static const char *MONTH_NAMES[] = {
    "Gennaio", "Febbraio", "Marzo", "Aprile", "Maggio", "Giugno",
    "Luglio", "Agosto", "Settembre", "Ottobre", "Novembre", "Dicembre"
};

// Check national holidays in Italy
int is_festivo_nazionale(int mday, int mon) {
    if ((mday == 1  && mon == 0)  || // New Year
        (mday == 6  && mon == 0)  || // Epiphany
        (mday == 25 && mon == 3)  || // Liberation Day
        (mday == 1  && mon == 4)  || // Labor Day
        (mday == 2  && mon == 5)  || // Republic Day
        (mday == 15 && mon == 7)  || // Ferragosto
        (mday == 1  && mon == 10) || // All Saints
        (mday == 8  && mon == 11) || // Immaculate Conception
        (mday == 25 && mon == 11) || // Christmas
        (mday == 26 && mon == 11))   // St. Stephen
    {
        return 1;
    }
    return 0;
}

// Determine tariff band (1 = F1, 2 = F2, 3 = F3)
int get_fascia_oraria(int wday, int mday, int mon, int hour) {
    if (wday == 0 || is_festivo_nazionale(mday, mon)) {
        return 3;
    }
    if (wday >= 1 && wday <= 5) { // Monday to Friday
        if (hour >= 8 && hour < 19) return 1;
        else if ((hour >= 7 && hour < 8) || (hour >= 19 && hour < 23)) return 2;
        else return 3;
    } else { // Saturday
        if (hour >= 7 && hour < 23) return 2;
        else return 3;
    }
}

int main(int argc, char *argv[]) {
    // Check command line arguments
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <NOME_TABELLA_MYSQL> <ANNO_AAAA>\n", argv[0]);
        fprintf(stderr, "Esempio: %s kwh_so 2024\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *table_name = argv[1];
    int target_year = atoi(argv[2]);

    if (target_year < 1970 || target_year > 2100) {
        fprintf(stderr, "Errore: Anno '%s' non valido.\n", argv[2]);
        return EXIT_FAILURE;
    }

    // Set local timezone to Europe/Rome
    setenv("TZ", "Europe/Rome", 1);
    tzset();

    // Initialize MySQL connection
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

    // Execute query to retrieve epoch and kwh
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

    // Accumulation arrays
    double monthly_kwh[12] = {0.0};
    double monthly_f1[12]  = {0.0};
    double monthly_f2[12]  = {0.0};
    double monthly_f3[12]  = {0.0};

    double hourly_kwh[24] = {0.0};

    double total_year_kwh = 0.0;
    double total_year_f1  = 0.0;
    double total_year_f2  = 0.0;
    double total_year_f3  = 0.0;
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
            int month = tm_info.tm_mon;   // 0..11
            int mday  = tm_info.tm_mday;  // 1..31
            int wday  = tm_info.tm_wday;  // 0..6
            int hour  = tm_info.tm_hour;  // 0..23

            if (month >= 0 && month < 12) {
                int fascia = get_fascia_oraria(wday, mday, month, hour);
                if (fascia == 1)      monthly_f1[month] += kwh;
                else if (fascia == 2) monthly_f2[month] += kwh;
                else if (fascia == 3) monthly_f3[month] += kwh;

                monthly_kwh[month] += kwh;
            }

            if (hour >= 0 && hour < 24) {
                hourly_kwh[hour] += kwh;
            }

            total_year_kwh += kwh;
            total_samples++;
        }
    }

    // Aggregate annual band totals
    for (int m = 0; m < 12; m++) {
        total_year_f1 += monthly_f1[m];
        total_year_f2 += monthly_f2[m];
        total_year_f3 += monthly_f3[m];
    }

    mysql_free_result(result);
    mysql_close(conn);

    if (total_samples == 0) {
        printf("\nNessun dato trovato per l'anno %d nella tabella '%s'.\n\n", target_year, table_name);
        return EXIT_SUCCESS;
    }

    // Table 1: Monthly summary with F1, F2, F3 breakdown
    printf("\n");
    printf("===================================================================\n");
    printf(" 1. RIEPILOGO MENSILE E FASCE ORARIE (ANNO %d)\n", target_year);
    printf(" Tabella DB: %s | Fuso Orario: Europe/Rome\n", table_name);
    printf("===================================================================\n");
    printf(" Mese        | F1 (kWh)     | F2 (kWh)     | F3 (kWh)     | TOTALE (kWh)\n");
    printf("-------------+--------------+--------------+--------------+--------------\n");

    for (int m = 0; m < 12; m++) {
        printf(" %-11s | %12.2f | %12.2f | %12.2f | %12.2f\n",
               MONTH_NAMES[m], monthly_f1[m], monthly_f2[m], monthly_f3[m], monthly_kwh[m]);
    }

    printf("-------------+--------------+--------------+--------------+--------------\n");
    printf(" TOTALE ANNO | %12.2f | %12.2f | %12.2f | %12.2f\n", 
           total_year_f1, total_year_f2, total_year_f3, total_year_kwh);
    printf("===================================================================\n");

    // Table 2: Hourly profile displaying only start hour
    printf("\n");
    printf("=======================================\n");
    printf(" 2. PROFILO DI CONSUMO ORARIO ANNO %d\n", target_year);
    printf("=======================================\n");
    printf(" Ora   | Totale kWh\n");
    printf("-------+-------------------------------\n");

    for (int h = 0; h < 24; h++) {
        printf(" %02d:00 | %14.4f\n", h, hourly_kwh[h]);
    }

    printf("-------+-------------------------------\n");
    printf(" TOTALE| %14.4f\n", total_year_kwh);
    printf("=======================================\n\n");

    return EXIT_SUCCESS;
}
