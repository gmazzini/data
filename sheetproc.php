<?php
// Include local configuration file containing sheet parameters
include "/home/www/sensori/local.php";

// Path to access token managed by the external C pipeline
$token_file = "/home/www/data/google_access_token";

if (!file_exists($token_file)) {
    header('Content-Type: application/json');
    echo json_encode(array());
    exit(0);
}

$access_token = trim(file_get_contents($token_file));
if (empty($access_token)) {
    header('Content-Type: application/json');
    echo json_encode(array());
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

// Normalize various date formats (DD/MM/YYYY, DD/MM/YY, DD/MM, YYYY-MM-DD) to YYYY-MM-DD
function parse_date_to_ymd($date_str) {
    $date_str = trim($date_str);
    if (empty($date_str)) return false;

    if (preg_match('/^(\d{1,2})[\/\-](\d{1,2})(?:[\/\-](\d{2,4}))?$/', $date_str, $m)) {
        $day   = intval($m[1]);
        $month = intval($m[2]);
        if (isset($m[3])) {
            $year = intval($m[3]);
            if ($year < 100) $year += 2000;
        } else {
            $year = intval(date("Y"));
        }
        return sprintf("%04d-%02d-%02d", $year, $month, $day);
    }

    $epoch = strtotime(str_replace('/', '-', $date_str));
    if ($epoch) return date("Y-m-d", $epoch);

    return false;
}

// Fetch CSV export directly using gid to target the exact sheet tab with Bearer authorization
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
    echo json_encode(array());
    exit(0);
}

$lines = explode("\n", $csv_data);
$output = array();

// Parse CSV rows
foreach ($lines as $line) {
    $line = trim($line);
    if (empty($line)) continue;

    $row = str_getcsv($line);
    if (empty($row) || count($row) < 25) continue; // Requires Date + 24 hourly columns

    $row_date = parse_date_to_ymd($row[0]);
    if (!$row_date) continue;

    // Extract 24 hourly PUN values (columns B to Y) matching the target date
    if ($row_date === $target_date) {
        for ($h = 0; $h < 24; $h++) {
            $val_raw = trim($row[$h + 1]);
            if ($val_raw === "") continue;

            $val = floatval(str_replace(',', '.', $val_raw));
            $point_epoch = strtotime("$target_date $h:00:00 UTC");

            $output[] = array(
                "epoch" => $point_epoch,
                "value" => $val
            );
        }
        break;
    }
}

// Output JSON result
header('Content-Type: application/json');
echo json_encode($output);
?>
