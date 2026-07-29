<?php
// sqlproc.php - Data extraction and processing module

// Always load local.php from the current working directory
$local_config = getcwd() . '/local.php';
if (file_exists($local_config)) {
    require_once $local_config;
} else {
    http_response_code(500);
    die("Error: Unable to find local.php in current working directory (" . getcwd() . ")\n");
}

// PDO database connection singleton
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
            die("Database connection error: " . $e->getMessage() . "\n");
        }
    }
    return $pdo;
}

// Extract sensor data in memory without temporary files
function get_sensors_data(string $from, string $to): array {
    global $tab, $points;
    $pdo = get_db();

    $all_data = [];

    // Loop through sensors defined in local.php $tab array
    foreach ($tab as $index => $sensor) {
        $table  = preg_replace('/[^a-zA-Z0-9_]/', '', $sensor['table']);
        $device = $sensor['device'];
        $col    = preg_replace('/[^a-zA-Z0-9_]/', '', isset($sensor['cols'][0]) ? $sensor['cols'][0] : 'temperature');

        // Candidate timestamp column names
        $time_cols_to_try = ['timestamp', 'date_time', 'datetime', 'time'];
        $stmt = null;

        foreach ($time_cols_to_try as $t_col) {
            try {
                $sql = "SELECT `{$t_col}` AS t_time, `{$col}` AS t_val 
                        FROM `{$table}` 
                        WHERE device = :device 
                          AND `{$t_col}` BETWEEN :from AND :to 
                        ORDER BY `{$t_col}` ASC";

                $stmt = $pdo->prepare($sql);
                $stmt->bindValue(':device', $device, PDO::PARAM_STR);
                $stmt->bindValue(':from', $from, PDO::PARAM_STR);
                $stmt->bindValue(':to', $to, PDO::PARAM_STR);
                $stmt->execute();
                break;
            } catch (PDOException $e) {
                continue;
            }
        }

        if (!$stmt) {
            continue;
        }

        $rows = $stmt->fetchAll();
        $count = count($rows);
        $max_p = (isset($points) && $points > 0) ? $points : 1000;
        $step  = ($count > $max_p) ? ceil($count / $max_p) : 1;

        // Downsample data points in RAM to fit $points limit
        for ($i = 0; $i < $count; $i += $step) {
            $t = $rows[$i]['t_time'];
            $v = $rows[$i]['t_val'] !== null ? (float)$rows[$i]['t_val'] : null;

            if (!isset($all_data[$t])) {
                $all_data[$t] = array_fill(0, count($tab), null);
            }
            $all_data[$t][$index] = $v;
        }
    }

    ksort($all_data);
    return $all_data;
}
