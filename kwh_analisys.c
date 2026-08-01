#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Struttura per memorizzare le statistiche mensili delle 3 fasce
typedef struct {
    double f1;
    double f2;
    double f3;
} MeseStats;

// Funzione per verificare se un giorno e' un festivo nazionale italiano
int is_festivo_nazionale(int giorno, int mese) {
    if ((giorno == 1  && mese == 1)  || // Capodanno
        (giorno == 6  && mese == 1)  || // Epifania
        (giorno == 25 && mese == 4)  || // Liberazione
        (giorno == 1  && mese == 5)  || // Festa del Lavoro
        (giorno == 2  && mese == 6)  || // Festa della Repubblica
        (giorno == 15 && mese == 8)  || // Ferragosto
        (giorno == 1  && mese == 11) || // Tutti i Santi
        (giorno == 8  && mese == 12) || // Immacolata
        (giorno == 25 && mese == 12) || // Natale
        (giorno == 26 && mese == 12))   // Santo Stefano
    {
        return 1;
    }
    return 0;
}

// Determinazione della fascia oraria ARERA (1=F1, 2=F2, 3=F3)
int get_fascia_oraria(int day_of_week, int giorno, int mese, int ora) {
    // day_of_week: 0=Domenica, 1=Lunedì, ..., 6=Sabato
    if (day_of_week == 0 || is_festivo_nazionale(giorno, mese)) {
        return 3; // Domenica e Festivi sempre F3
    }
    if (day_of_week >= 1 && day_of_week <= 5) { // Lunedì - Venerdì
        if (ora >= 8 && ora < 19) {
            return 1; // F1 (08:00 - 19:00)
        } else if ((ora >= 7 && ora < 8) || (ora >= 19 && ora < 23)) {
            return 2; // F2 (07:00-08:00, 19:00-23:00)
        } else {
            return 3; // F3 (23:00 - 07:00)
        }
    } else { // Sabato
        if (ora >= 7 && ora < 23) {
            return 2; // F2 (07:00 - 23:00)
        } else {
            return 3; // F3
        }
    }
}

// Calcola il giorno della settimana (0 = Domenica, 1 = Lunedì, ..., 6 = Sabato)
int get_day_of_week(int giorno, int mese, int anno) {
    struct tm time_input = {0};
    time_input.tm_mday = giorno;
    time_input.tm_mon = mese - 1;
    time_input.tm_year = anno - 1900;
    mktime(&time_input);
    return time_input.tm_wday;
}

void elabora_csv(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("Errore: impossibile aprire il file %s\n", filename);
        return;
    }

    char buffer[16384];
    MeseStats mesi[13] = {0};     // Indici 1..12 per i mesi (Jan-Dec)
    double ore_annuali[24] = {0}; // Indici 0..23 per le ore (00:00 - 23:00)

    // Salta l'intestazione
    if (!fgets(buffer, sizeof(buffer), file)) {
        fclose(file);
        return;
    }

    while (fgets(buffer, sizeof(buffer), file)) {
        // Estrazione prima colonna: Data (DD/MM/YYYY)
        char *token = strtok(buffer, ",\r\n");
        if (!token) continue;

        char data_str[32];
        strncpy(data_str, token, sizeof(data_str) - 1);
        data_str[sizeof(data_str) - 1] = '\0';

        int giorno, mese, anno;
        if (sscanf(data_str, "%d/%d/%d", &giorno, &mese, &anno) != 3) {
            continue;
        }

        if (mese < 1 || mese > 12) continue;

        int dow = get_day_of_week(giorno, mese, anno);

        // Legge i 96 quarti d'ora (4 per ogni ora)
        for (int q = 0; q < 96; q++) {
            token = strtok(NULL, ",\r\n");
            if (!token) break;

            double val = atof(token);
            int ora = q / 4; // Ora corrispondente (0..23)

            // Accumula nel profilo orario annuo
            ore_annuali[ora] += val;

            // Determina la fascia e accumula per mese
            int fascia = get_fascia_oraria(dow, giorno, mese, ora);
            if (fascia == 1)      mesi[mese].f1 += val;
            else if (fascia == 2) mesi[mese].f2 += val;
            else if (fascia == 3) mesi[mese].f3 += val;
        }
    }

    fclose(file);

    // ==========================================
    // STAMPA RISULTATI
    // ==========================================
    printf("=====================================================\n");
    printf(" FILE: %s\n", filename);
    printf("=====================================================\n\n");

    // 1. ANALISI MENSILE F1, F2, F3 E TOTALE
    printf("--- ANALISI MENSILE CONSUMI (F1, F2, F3) ---\n");
    printf("%-6s | %-12s | %-12s | %-12s | %-12s\n", "Mese", "F1 (kWh)", "F2 (kWh)", "F3 (kWh)", "TOTALE (kWh)");
    printf("-------------------------------------------------------------\n");
    
    double tot_f1 = 0, tot_f2 = 0, tot_f3 = 0, tot_gen = 0;
    for (int m = 1; m <= 12; m++) {
        double tot_mese = mesi[m].f1 + mesi[m].f2 + mesi[m].f3;
        printf("%02d     | %12.2f | %12.2f | %12.2f | %12.2f\n",
               m, mesi[m].f1, mesi[m].f2, mesi[m].f3, tot_mese);
        
        tot_f1  += mesi[m].f1;
        tot_f2  += mesi[m].f2;
        tot_f3  += mesi[m].f3;
        tot_gen += tot_mese;
    }
    printf("-------------------------------------------------------------\n");
    printf("%-6s | %12.2f | %12.2f | %12.2f | %12.2f\n\n", "TOTALE", tot_f1, tot_f2, tot_f3, tot_gen);

    // 2. PROFILO DI CONSUMO ORARIO ANNUALE (SOLO INIZIO ORA)
    printf("--- PROFILO DI CONSUMO ORARIO ANNUALE ---\n");
    printf("%-8s | %-18s\n", "Orario", "Consumo Tot (kWh)");
    printf("------------------------------\n");
    for (int h = 0; h < 24; h++) {
        printf("%02d:00    | %18.2f\n", h, ore_annuali[h]);
    }
    printf("------------------------------\n\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Uso: %s <file_pulito_1.csv> [file_pulito_2.csv ...]\n", argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        elabora_csv(argv[i]);
    }
    return 0;
}
