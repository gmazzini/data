<?php
// Include configuration file containing $auth_key and database credentials
include "/home/www/sensori/local.php";

// Validate CLI arguments (expects device_eui and lookback window in seconds)
if ($argc < 3) exit(0);

$device_eui = $argv[1];
$seconds = intval($argv[2]);

if ($device_eui == "" || $seconds <= 0) exit(0);

// Convert 16-bit unsigned integer to signed 16-bit integer (two's complement)
function s16($v) {
    if ($v & 0x8000) $v -= 0x10000;
    return $v;
}

// Decode Dragino LHT65 hex payload to extract temperature and humidity
function lht65_temp_hum($hex, &$t, &$u) {
    $hex = preg_replace('/[^0-9a-fA-F]/', '', $hex);
    if (strlen($hex) < 12) return false;

    $b = array();
    for ($i = 0; $i < 12; $i += 2) {
        $b[] = hexdec(substr($hex, $i, 2));
    }

    // Byte 2 & 3: Temperature (°C)
    // Byte 4 & 5: Humidity (%)
    $t = s16(($b[2] << 8) | $b[3]) / 100;
    $u = ((($b[4] << 8) | $b[5]) & 0x0FFF) / 10;

    return true;
}

// Define the time window in UTC (required by RetePAIoT specifications)
$from = gmdate("Y-m-d H:i:s", time() - $seconds);
$to   = gmdate("Y-m-d H:i:s");
$url  = "https://api.retepaiot.it/payloaddata/" . rawurlencode($device_eui);

// Prepare JSON request body
$body = json_encode(array(
    "auth_key" => $auth_key,
    "from"     => $from,
    "to"       => $to
));

// Execute HTTP GET request with JSON body
$ch = curl_init($url);
curl_setopt($ch, CURLOPT_CUSTOMREQUEST, "GET");
curl_setopt($ch, CURLOPT_HTTPHEADER, array("Content-Type: application/json"));
curl_setopt($ch, CURLOPT_POSTFIELDS, $body);
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_CONNECTTIMEOUT, 5); // 5-second connection timeout
curl_setopt($ch, CURLOPT_TIMEOUT, 10);        // 10-second total execution timeout

$res = curl_exec($ch);
curl_close($ch);

if ($res === false || $res == "") exit(0);

// Validate and parse JSON response
$j = json_decode($res, true);
if (!is_array($j)) exit(0);
if (!isset($j["data"]) || !is_array($j["data"]) || empty($j["data"])) exit(0);

// Connect to MySQL database
$con = mysqli_connect($db_host, $db_user, $db_pass, $db_name);
if (!$con) exit(0);

$device = mysqli_real_escape_string($con, $device_eui);
$rows = array();

// Decode payloads and build batch query tuples
foreach ($j["data"] as $r) {
    if (!isset($r["timestamp"], $r["valore"])) continue;
    if (!lht65_temp_hum($r["valore"], $t, $u)) continue;

    $epoch = strtotime($r["timestamp"] . " UTC");
    if ($epoch <= 0) continue;

    $t_val = floatval($t);
    $u_val = floatval($u);

    $rows[] = "($epoch, '$device', $t_val, $u_val)";
}

// Execute single batch insert query to minimize database I/O
if (!empty($rows)) {
    $sql = "INSERT INTO ambient (epoch, device, temperature, humidity) VALUES " . implode(",", $rows) . " " .
           "ON DUPLICATE KEY UPDATE temperature=VALUES(temperature), humidity=VALUES(humidity)";
    mysqli_query($con, $sql);
}

mysqli_close($con);
?>
