<?php
// Include local configuration
include "/home/www/sensori/local.php";

// Set default fallback values if not specified in local.php
if (!isset($pun_sheet_id))    $pun_sheet_id    = "1RF4N-T2NR2UHai70AzTzwuLXowkLlOQWvFyb8AaE1xg";
if (!isset($pun_sheet_range)) $pun_sheet_range = "A1:Z1000";

// Path to access token managed by the external pipeline
$token_file = "/home/www/data/google_access_token";

// Read access token directly from file
if (!file_exists($token_file)) exit(0);
$access_token = trim(file_get_contents($token_file));
if (empty($access_token)) exit(0);

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

// Fetch sheet data via Google Sheets REST API v4 using the Bearer token
$api_url = "https://sheets.googleapis.com/v4/spreadsheets/" . rawurlencode($pun_sheet_id) . "/values/" . rawurlencode($pun_sheet_range);

$ch = curl_init($api_url);
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_HTTPHEADER, array(
    "Authorization: Bearer " . $access_token,
    "Content-Type: application/json"
));
curl_setopt($ch, CURLOPT_CONNECTTIMEOUT, 5);
curl_setopt($ch, CURLOPT_TIMEOUT, 10);

$sheet_res = curl_exec($ch);
curl_close($ch);

if ($sheet_res === false || $sheet_res == "") exit(0);

$sheet_data = json_decode($sheet_res, true);
if (!isset($sheet_data["values"]) || !is_array($sheet_data["values"])) exit(0);

$output = array();

// Parse rows to extract matching date records
foreach ($sheet_data["values"] as $row) {
    if (empty($row) || count($row) < 2) continue;

    $row_date_str = trim($row[0]);
    $row_epoch = strtotime(str_replace('/', '-', $row_date_str));
    if (!$row_epoch) continue;

    $row_date = date("Y-m-d", $row_epoch);

    if ($row_date === $target_date) {

        // Vertical layout: [Date, Hour (1-24), Value]
        if (count($row) == 3 && is_numeric(trim($row[1]))) {
            $hour = intval($row[1]) - 1;
            $val  = floatval(str_replace(',', '.', trim($row[2])));

            $point_epoch = strtotime("$target_date $hour:00:00 UTC");
            $output[] = array(
                "epoch" => $point_epoch,
                "value" => $val
            );
        } 
        // Horizontal layout: [Date, H01, H02, ..., H24]
        elseif (count($row) >= 25) {
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
}

// Output formatted JSON array for sqldata
header('Content-Type: application/json');
echo json_encode($output);
?>
