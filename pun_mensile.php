<?php
// pun_mensile.php
// Calcolo medie PUN per fasce orarie ARERA (F0, F1, F2, F3) con conversione Epoch GMT -> Ora Italiana

date_default_timezone_set('Europe/Rome');

// 1. Controllo argomenti CLI
if (!isset($argv[1]) || !isset($argv[2])) {
    echo "Errore: Parametri mancanti.\n";
    echo "Uso: php pun_mensile.php <ANNO> <MESE>\n";
    echo "Esempio: php pun_mensile.php 2026 07\n";
    exit(1);
}

$year  = (int)$argv[1];
$month = (int)$argv[2];

if ($year < 2000 || $year > 2100 || $month < 1 || $month > 12) {
    echo "Errore: Anno ($year) o Mese ($month) non validi.\n";
    exit(1);
}

// 2. Caricamento configurazione DB
$local_config = getcwd() . '/local.php';
if (file_exists($local_config)) {
    require_once $local_config;
} else {
    die("Errore: Impossibile trovare local.php nella directory corrente (" . getcwd() . ")\n");
}

// Nome tabella (predefinito 'gme' se non specificato diversamente)
$table_name = $db_table ?? 'gme';

// 3. Definizione range temporale in Ora Italiana (dal 1° del mese alle 00:00 al fine mese 23:59:59)
$tz = new DateTimeZone('Europe/Rome');
$dt_start = new DateTime(sprintf('%04d-%02d-01 00:00:00', $year, $month), $tz);
$dt_end   = new DateTime(sprintf('%04d-%02d-%02dT23:59:59', $year, $month, (int)$dt_start->format('t')), $tz);

$sds = $dt_start->getTimestamp(); // Inizio in Epoch GMT
$sde = $dt_end->getTimestamp();   // Fine in Epoch GMT

// 4. Connessione al database MySQL
$conn = mysqli_connect($db_host, $db_user, $db_pass, $db_name);
if (!$conn) {
    die("Errore di connessione al DB: " . mysqli_connect_error() . "\n");
}
@mysqli_set_charset($conn, "utf8mb4");

// 5. Query per estrarre le ore del mese
$sql = "SELECT epoch, pun FROM `$table_name` WHERE epoch BETWEEN $sds AND $sde ORDER BY epoch ASC";
$res = mysqli_query($conn, $sql);

// Fallback se il campo nel DB si chiama 'prezzo' anziché 'pun'
if (!$res) {
    $sql = "SELECT epoch, prezzo AS pun FROM `$table_name` WHERE epoch BETWEEN $sds AND $sde ORDER BY epoch ASC";
    $res = mysqli_query($conn, $sql);
}

if (!$res || mysqli_num_rows($res) === 0) {
    mysqli_close($conn);
    die("Nessun dato trovato per il periodo $year-" . sprintf("%02d", $month) . "\n");
}

$sums   = ['F0' => 0.0, 'F1' => 0.0, 'F2' => 0.0, 'F3' => 0.0];
$counts = ['F0' => 0,   'F1' => 0,   'F2' => 0,   'F3' => 0];

// 6. Ciclo di elaborazione record per record
while ($row = mysqli_fetch_assoc($res)) {
    $epoch = (int)$row['epoch'];
    $val   = (float)$row['pun'];

    // Conversione Epoch (GMT) -> Data/Ora Italiana
    $dt = new DateTime("@$epoch");
    $dt->setTimezone($tz);

    $fascia = getFasciaOraria($dt);

    $sums['F0'] += $val;
    $counts['F0']++;

    $sums[$fascia] += $val;
    $counts[$fascia]++;
}

mysqli_free_result($res);
mysqli_close($conn);

// 7. Calcolo medie
$f0 = $counts['F0'] > 0 ? $sums['F0'] / $counts['F0'] : 0.0;
$f1 = $counts['F1'] > 0 ? $sums['F1'] / $counts['F1'] : 0.0;
$f2 = $counts['F2'] > 0 ? $sums['F2'] / $counts['F2'] : 0.0;
$f3 = $counts['F3'] > 0 ? $sums['F3'] / $counts['F3'] : 0.0;

// 8. Output dei risultati (Formattati a 5 cifre decimali)
echo sprintf(
    "F0: %.5f | F1: %.5f | F2: %.5f | F3: %.5f\n",
    $f0, $f1, $f2, $f3
);


// --- FUNZIONI DI CALCOLO FESTIVITÀ E FASCE ARERA ---

/**
 * Verifica se un giorno è festivo secondo il calendario italiano (fissi + Pasquetta)
 */
function isFestivo(DateTime $dt): bool {
    $m = (int)$dt->format('m');
    $d = (int)$dt->format('d');
    $y = (int)$dt->format('Y');

    $fissi = [
        '01-01', // Capodanno
        '01-06', // Epifania
        '04-25', // Liberazione
        '05-01', // Festa del Lavoro
        '06-02', // Festa della Repubblica
        '08-15', // Ferragosto
        '11-01', // Tutti i Santi
        '12-08', // Immacolata
        '12-25', // Natale
        '12-26', // Santo Stefano
    ];

    if (in_array(sprintf('%02d-%02d', $m, $d), $fissi, true)) {
        return true;
    }

    // Calcolo Pasquetta (Lunedì dell'Angelo)
    $pasquetta = new DateTime("$y-03-21");
    $pasquetta->modify("+" . easter_days($y) . " days +1 day");

    return $dt->format('Y-m-d') === $pasquetta->format('Y-m-d');
}

/**
 * Riconosce la fascia oraria ARERA (F1, F2, F3) in ora locale italiana
 */
function getFasciaOraria(DateTime $dt): string {
    $dow = (int)$dt->format('N'); // 1 = Lunedì ... 7 = Domenica
    $h   = (int)$dt->format('H'); // Ora locale (0..23)

    // Domenica o Festivo -> Sempre F3
    if ($dow === 7 || isFestivo($dt)) {
        return 'F3';
    }

    // Sabato
    if ($dow === 6) {
        return ($h >= 7 && $h < 23) ? 'F2' : 'F3';
    }

    // Lunedì - Venerdì
    if ($h >= 8 && $h < 19) {
        return 'F1';
    }
    if (($h >= 7 && $h < 8) || ($h >= 19 && $h < 23)) {
        return 'F2';
    }

    return 'F3';
}
