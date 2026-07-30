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

// Determine target date from CLI argument or HTTP GET parameter
$target_date_raw = "";
if (isset($_GET['date'])) {
    $target_date_raw = trim($_GET['date']);
} elseif (isset($argv[1])) {
    $target_date_raw = trim($argv[1]);
}

if (empty($target_date_raw)) {
    $target_date = date("Y-m-d");
} else {
    $target_date = date("Y-m-d", strtotime($target_date_raw));
}

// Normalize date string to YYYY-MM-DD
function parse_date_to_ymd($date_str) {
    $date_str = trim($date_str);
    if (empty($date_str)) return false;

    if (preg_match('/^(\d{1,2})[\/\-](\d{1,2})(?:[\/\-](\d{2,4}))?$/', $date_str, $m)) {
        $day   = intval($m[1]);
        $month = intval($m[2]);
        $year  = isset($m[3]) ? intval($m[3]) : intval(date("Y"));
        if ($year < 100) $year += 2000;
        return sprintf("%04d-%02d-%02d", $year, $month, $day);
    }

    $epoch = strtotime(str_replace('/', '-', $date_str));
    return $epoch ? date("Y-m-d", $epoch) : false;
}

// Fetch CSV export directly from Google Sheets
$csv_url = "https://docs.google.com/spreadsheets/d/" . rawurlencode($pun_sheet_id) . "/export?format=csv&gid=" . rawurlencode($pun_sheet_gid);

$ch = curl_init($csv_url);
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_FOLLOWLOCATION, true);
curl_setopt($ch, CURLOPT_HTTPHEADER, array(
    "Authorization: Bearer " . $access_token
));
curl_setopt($ch, CURLOPT_CONNECTTIMEOUT, 5);
curl_setopt($ch, CURLOPT_TIMEOUT, 10);

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

// Parse Header Row (Row 1) to build a mapping of Column Index -> Local Time (HH:MM:SS)
$headers = str_getcsv($lines[0]);
$col_time_map = array();

foreach ($headers as $idx => $header_val) {
    if ($idx === 0) continue; // Column A is Date
    $h_clean = trim($header_val);

    // Matches formats like 01:00, 1:00, 02:15, 02:15:00
    if (preg_match('/^(\d{1,2}):(\d{2})(?::(\d{2}))?$/', $h_clean, $m)) {
        $sec = isset($m[3]) ? $m[3] : "00";
        $col_time_map[$idx] = sprintf("%02d:%02d:%02d", $m[1], $m[2], $sec);
    }
}

$tz_rome = new DateTimeZone('Europe/Rome');
$output = array();

// Iterate over data rows
for ($i = 1; $i < count($lines); $i++) {
    $line = trim($lines[$i]);
    if (empty($line)) continue;

    $row = str_getcsv($line);
    if (empty($row)) continue;

    $row_date = parse_date_to_ymd($row[0]);
    if ($row_date === $target_date) {
        foreach ($col_time_map as $col_idx => $time_str) {
            if (!isset($row[$col_idx])) continue;

            $val_raw = trim($row[$col_idx]);
            if ($val_raw === "" || !is_numeric(str_replace(',', '.', $val_raw))) continue;

            $val = floatval(str_replace(',', '.', $val_raw));

            // Convert Italian Local Time to UTC Unix Timestamp
            try {
                $dt = new DateTime("$target_date $time_str", $tz_rome);
                $epoch = $dt->getTimestamp();

                $output[] = array(
                    "epoch" => $epoch,
                    "value" => $val
                );
            } catch (Exception $e) {
                continue;
            }
        }
        break;
    }
}

// Sort points chronologically by epoch
usort($output, function($a, $b) {
    return $a['epoch'] <=> $b['epoch'];
});

// Output clean JSON format compatible with sqlgraph
header('Content-Type: application/json');
echo json_encode($output) . PHP_EOL;
?>
