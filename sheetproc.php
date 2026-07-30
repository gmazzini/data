<?php
// Include local configuration file from /home/www/pun/local.php
include "/home/www/pun/local.php";

// Path to access token managed by the external C pipeline
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

// Convert input (String date OR numeric Unix Epoch) to YYYY-MM-DD
function parse_to_ymd($input) {
    $input = trim($input);
    if (empty($input)) return false;

    // If sqlgraph passed a numeric Unix Timestamp (e.g. 1785362400)
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

// Read parameters sent by sqlgraph or CLI
$from_raw = $_GET['from'] ?? $_GET['start'] ?? $_GET['date_from'] ?? $_GET['date'] ?? ($argv[1] ?? null);
$to_raw   = $_GET['to']   ?? $_GET['end']   ?? $_GET['date_to']   ?? ($argv[2] ?? null);

$start_date = $from_raw ? parse_to_ymd($from_raw) : date("Y-m-d");
$end_date   = $to_raw   ? parse_to_ymd($to_raw)   : $start_date;

if ($start_date > $end_date) {
    $tmp = $start_date;
    $start_date = $end_date;
    $end_date = $tmp;
}

// Epoch bounds for exact sqlgraph filtering
$min_epoch = (is_numeric($from_raw) && strlen($from_raw) >= 9) ? intval($from_raw) : strtotime("$start_date 00:00:00 UTC");
$max_epoch = (is_numeric($to_raw)   && strlen($to_raw)   >= 9) ? intval($to_raw)   : strtotime("$end_date 23:59:59 UTC");

// Fetch CSV export directly from Google Sheets
$csv_url = "https://docs.google.com/spreadsheets/d/" . rawurlencode($pun_sheet_id) . "/export?format=csv&gid=" . rawurlencode($pun_sheet_gid);

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

// Parse Header Row (Row 1) to build mapping: Column Index -> Local Time (HH:MM:SS)
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
$output = array();

// Iterate over data rows matching date range
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

                $point_epoch = $dt->getTimestamp();

                // Exact filter based on sqlgraph window bounds
                if ($point_epoch >= $min_epoch && $point_epoch <= $max_epoch) {
                    $output[] = array(
                        "epoch" => $point_epoch,
                        "value" => $val
                    );
                }
            } catch (Exception $e) {
                continue;
            }
        }
    }
}

// Sort all points chronologically by epoch
usort($output, function($a, $b) {
    return $a['epoch'] <=> $b['epoch'];
});

// Output clean JSON format
header('Content-Type: application/json');
echo json_encode($output) . PHP_EOL;
?>
