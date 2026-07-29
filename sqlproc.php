<?php
/**
 * sqlproc.php - Estrazione ed elaborazione dati sensori (100% RAM)
 * Sfrutta integralmente le definizioni contenute in local.php
 */

require_once __DIR__ . '/local.php';

/**
 * Connessione PDO al database MySQL (usa le variabili di local.php)
 */
function get_db(): PDO {
    static $pdo = null;
    if ($pdo === null) {
        global $db_host, $db_user, $db_pass, $db_name;
        $dsn = "mysql:host={$db_host};dbname={$db_name};charset=utf8mb4";
        $options = [
            PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
            PDO::ATTR_EMULATE_PREPARES   => false,
        ];
        try {
            $pdo = new PDO($dsn, $db_user, $db_pass, $options);
        } catch (PDOException $e) {
            http_response_code(500);
            die("Errore di connessione al database.");
        }
    }
    return $pdo;
}

/**
 * Estrae i dati per tutti i dispositivi definiti in $tab senza scrivere nulla su disco.
 */
function get_sensors_data(string $from, string $to): array {
    global $tab, $points;
    $pdo = get_db();

    // Nome del campo data/ora nella tabella MySQL ('timestamp' oppure 'date_time')
    $time_col = 'timestamp'; 

    $all_data = [];

    // Cicla sui dispositivi configurati nell'array $tab di local.php
    foreach ($tab as $index => $sensor) {
        $table  = preg_replace('/[^a-zA-Z0-9_]/', '', $sensor['table']); // Sanificazione tabella
        $device = $sensor['device'];
        $col    = preg_replace('/[^a-zA-Z0-9_]/', '', $sensor['cols'][0] ?? 'temperature');

        // Query preparata per massima sicurezza contro SQL Injection
        $sql = "SELECT `{$time_col}` AS t_time, `{$col}` AS t_val 
                FROM `{$table}` 
                WHERE device = :device 
                  AND `{$time_col}` BETWEEN :from AND :to 
                ORDER BY `{$time_col}` ASC";

        try {
            $stmt = $pdo->prepare($sql);
            $stmt->bindValue(':device', $device, PDO::PARAM_STR);
            $stmt->bindValue(':from', $from, PDO::PARAM_STR);
            $stmt->bindValue(':to', $to, PDO::PARAM_STR);
            $stmt->execute();
            $rows = $stmt->fetchAll();
        } catch (PDOException $e) {
            // Fallback nel caso la colonna nel DB si chiami 'date_time'
            $sql_fallback = "SELECT date_time AS t_time, `{$col}` AS t_val 
                             FROM `{$table}` 
                             WHERE device = :device 
                               AND date_time BETWEEN :from AND :to 
                             ORDER BY date_time ASC";
            $stmt = $pdo->prepare($sql_fallback);
            $stmt->bindValue(':device', $device, PDO::PARAM_STR);
            $stmt->bindValue(':from', $from, PDO::PARAM_STR);
            $stmt->bindValue(':to', $to, PDO::PARAM_STR);
            $stmt->execute();
            $rows = $stmt->fetchAll();
        }

        // Downsampling in RAM per rispettare il limite $points di local.php
        $count = count($rows);
        $step  = ($count > $points) ? ceil($count / $points) : 1;

        for ($i = 0; $i < $count; $i += $step) {
            $t = $rows[$i]['t_time'];
            $v = $rows[$i]['t_val'] !== null ? (float)$rows[$i]['t_val'] : null;

            if (!isset($all_data[$t])) {
                $all_data[$t] = array_fill(0, count($tab), null);
            }
            $all_data[$t][$index] = $v;
        }
    }

    // Ordina cronologicamente le letture
    ksort($all_data);

    return $all_data;
}
