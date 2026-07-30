<?php
// 1. Load local.php exclusively from current working directory
$local_config = getcwd() . "/local.php";

if (!file_exists($local_config)) {
    header('HTTP/1.1 500 Internal Server Error');
    die("Fatal Error: 'local.php' not found in current directory (" . getcwd() . ")." . PHP_EOL);
}

include $local_config;

// 2. Ensure Google Sheet ID and GID are defined
$sheet_id  = $sheet_id  ?? $pun_sheet_id  ?? null;
$sheet_gid = $sheet_gid ?? $pun_sheet_gid ?? null;

if (empty($sheet_id) || empty($sheet_gid)) {
    header('HTTP/1.1 500 Internal Server Error');
    die("Fatal Error: '\$sheet_id' or '\$sheet_gid' not configured in local.php." . PHP_EOL);
}

// 3. Manage Google access token
$token_file = "/home/www/data/google_access_token";

if (!file_exists($token_file)) {
    header('Content-Type: application/json');
    echo json_encode(array()) . PHP_EOL;
    exit(0);
}

$access_token = trim(file_get_contents($token_file));
if (empty($access_token)) {
    header('Content-Type: application/json');
    echo json_encode(array()) . PHP_EOL;
    exit(0);
}

// Helper to parse sqlgraph navigation parameters (?q=YYYYdDDD, ?q=YYYYwWW, ?q=YYYYmMM)
function parse_q_param($q) {
    $q = trim($q);
    if (preg_match('/^(\d{4})d(\d{1,3})$/', $q, $m)) {
        $year = intval($m[1]);
        $day_of_year = intval($m[2]);
        $dt = new DateTime();
        $dt->setDate($year, 1, 1)->add(new DateInterval("P" . ($day_of_year - 1) . "D"));
        $d = $dt->format('Y-m-d');
        return array($d, $d);
    }
    if (preg_match('/^(\d{4})w(\d{1,2})$/', $q, $m)) {
        $year = intval($m[1]);
        $week = intval($m[2]);
        $dt = new DateTime();
        $dt->setISODate($year, $week, 1);
        $start = $dt->format('Y-m-d');
        $dt->modify('+6 days');
        $end = $dt->format('Y-m-d');
        return array($start, $end);
    }
    if (preg_match('/^(\d{4})m(\d{1,2})$/', $q, $m)) {
        $year = intval($m[1]);
        $month = intval($m[2]);
        $start = sprintf("%04d-%02d-01", $year, $month);
        $end = date("Y-m-t", strtotime($start));
        return array($start, $end);
    }
    return false;
}

// Helper to parse input date strings into YYYY-MM-DD
function parse_to_ymd($input) {
    $input = trim($input);
    if (empty($input)) return false;

    if (is_numeric($input) && strlen($input) >= 9) {
        return date("Y-m-d", intval($input));
    }

    if (preg_match('/^(\d{1,2})[\/\-](\d{1,2})(?:[\/\-](\d{2,4}))?$/', $input, $m)) {
        $day   = intval($m[1]);
        $month = intval($m[2]);
        $year  = isset($m[3]) ? intval($m[3]) : intval(date("Y"));
        if ($year < 100) $year += 2000;
        return sprintf("%04d-%02d-%02d", $year, $month, $day);
    }

    $epoch = strtotime(str_replace('/', '-', $input));
    return $epoch ? date("Y-m-d", $epoch) : false;
}

// Determine target date range
$start_date = null;
$end_date   = null;

if (isset($_GET['q'])) {
    $range = parse_q_param($_GET['q']);
    if ($range) {
        list($start_date, $end_date) = $range;
    }
}

if (!$start_date) {
    $from_raw   = $_GET['from'] ?? $_GET['start'] ?? $_GET['date_from'] ?? $_GET['date'] ?? ($argv[1] ?? null);
    $to_raw     = $_GET['to']   ?? $_GET['end']   ?? $_GET['date_to']   ?? ($argv[2] ?? null);

    $start_date = $from_raw ? parse_to_ymd($from_raw) : date("Y-m-d");
    $end_date   = $to_raw   ? parse_to_ymd($to_raw)   : $start_date;
}

if ($start_date > $end_date) {
    $tmp = $start_date;
    $start_date = $end_date;
    $end_date = $tmp;
}

// Fetch CSV export from Google Sheets using parameters from local.php
$csv_url = "https://docs.google.com/spreadsheets/d/" . rawurlencode($sheet_id) . "/export?format=csv&gid=" . rawurlencode($sheet_gid);

$ch = curl_init($csv_url);
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_FOLLOWLOCATION, true);
curl_setopt($ch, CURLOPT_HTTPHEADER, array(
    "Authorization: Bearer " . $access_token
));
curl_setopt($ch, CURLOPT_CONNECTTIMEOUT, 5);
curl_setopt($ch, CURLOPT_TIMEOUT, 15);

$csv_data = curl_exec($ch);
curl_close($ch);

if ($csv_data === false || empty($csv_data) || strpos($csv_data, "<!DOCTYPE html") !== false) {
    header('Content-Type: application/json');
    echo json_encode(array()) . PHP_EOL;
    exit(0);
}

$lines = explode("\n", $csv_data);
if (count($lines) < 2) {
    header('Content-Type: application/json');
    echo json_encode(array()) . PHP_EOL;
    exit(0);
}

// Map CSV header columns to time strings
$headers = str_getcsv($lines[0]);
$col_time_map = array();

foreach ($headers as $idx => $header_val) {
    if ($idx === 0) continue;
    $h_clean = trim($header_val);

    if (preg_match('/^(\d{1,2}):(\d{2})(?::(\d{2}))?$/', $h_clean, $m)) {
        $sec = isset($m[3]) ? $m[3] : "00";
        $col_time_map[$idx] = sprintf("%02d:%02d:%02d", $m[1], $m[2], $sec);
    }
}

$tz_rome = new DateTimeZone('Europe/Rome');
$tab_data  = array();
$json_data = array();

// Extract matching data points within date range
for ($i = 1; $i < count($lines); $i++) {
    $line = trim($lines[$i]);
    if (empty($line)) continue;

    $row = str_getcsv($line);
    if (empty($row)) continue;

    $row_date = parse_to_ymd($row[0]);
    if (!$row_date) continue;

    if ($row_date >= $start_date && $row_date <= $end_date) {
        
        $dt_start = new DateTime("$row_date 00:00:00", $tz_rome);
        $dt_end   = new DateTime("$row_date 23:00:00", $tz_rome);
        $is_dst_fallback_day = ($dt_start->format('I') == 1 && $dt_end->format('I') == 0);

        foreach ($col_time_map as $col_idx => $time_str) {
            if (!isset($row[$col_idx])) continue;

            $val_raw = trim($row[$col_idx]);
            if ($val_raw === "" || !is_numeric(str_replace(',', '.', $val_raw))) continue;

            $val = floatval(str_replace(',', '.', $val_raw));

            try {
                if ($is_dst_fallback_day && strpos($time_str, '02:') === 0) {
                    $dt = new DateTime("$row_date $time_str" . ($col_idx < 97 ? "+02:00" : "+01:00"));
                } else {
                    $dt = new DateTime("$row_date $time_str", $tz_rome);
                }

                $epoch = $dt->getTimestamp();

                // Format for sqlgraph.php engine
                $tab_data[$epoch] = array($val);

                // Format for standalone JSON output
                $json_data[] = array(
                    "epoch" => $epoch,
                    "value" => $val
                );
            } catch (Exception $e) {
                continue;
            }
        }
    }
}

// Sort chronologically by timestamp key
ksort($tab_data);

// Export series definitions and data into $GLOBALS for sqlgraph.php
$col_def = array("PUN");

$GLOBALS['title']  = $GLOBALS['title']  ?? $title  ?? "Prezzo Unico Nazionale (PUN)";
$GLOBALS['unit']   = $GLOBALS['unit']   ?? $unit   ?? "€/MWh";
$GLOBALS['vlabel'] = $GLOBALS['vlabel'] ?? $vlabel ?? "€/MWh";

$GLOBALS['names']  = $GLOBALS['names']  ?? $names  ?? $col_def;
$GLOBALS['vars']   = $GLOBALS['vars']   ?? $vars   ?? $col_def;
$GLOBALS['cols']   = $GLOBALS['cols']   ?? $cols   ?? $col_def;
$GLOBALS['series'] = $GLOBALS['series'] ?? $series ?? $col_def;
$GLOBALS['legend'] = $GLOBALS['legend'] ?? $legend ?? $col_def;

$GLOBALS['tab']    = $tab_data;
$GLOBALS['points'] = $tab_data;

$tab    = $tab_data;
$points = $tab_data;

// Output JSON when invoked directly via CLI or standalone HTTP request
if (basename($_SERVER['SCRIPT_FILENAME'] ?? '') === basename(__FILE__)) {
    header('Content-Type: application/json');

    usort($json_data, function($a, $b) {
        return $a['epoch'] <=> $b['epoch'];
    });

    echo json_encode($json_data) . PHP_EOL;
}
?>
