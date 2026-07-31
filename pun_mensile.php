<?php
// Set default timezone to Europe/Rome
date_default_timezone_set('Europe/Rome');

// Validate CLI input arguments
if (!isset($argv[1]) || !isset($argv[2])) {
    echo "Error: Missing parameters.\n";
    echo "Usage: php pun_mensile.php <YEAR> <MONTH>\n";
    echo "Example: php pun_mensile.php 2026 07\n";
    exit(1);
}

$year  = (int)$argv[1];
$month = (int)$argv[2];

// Validate year and month boundaries
if ($year < 2000 || $year > 2100 || $month < 1 || $month > 12) {
    echo "Error: Invalid year ($year) or month ($month).\n";
    exit(1);
}

// Load database configuration
$local_config = getcwd() . '/local.php';
if (file_exists($local_config)) {
    require_once $local_config;
} else {
    die("Error: Unable to find local.php in current working directory (" . getcwd() . ")\n");
}

// Set target table name
$table_name = 'pun_15m';

// Calculate start and end epoch timestamps in Italian local time
$tz = new DateTimeZone('Europe/Rome');
$dt_start = new DateTime(sprintf('%04d-%02d-01 00:00:00', $year, $month), $tz);
$dt_end   = new DateTime(sprintf('%04d-%02d-%02dT23:59:59', $year, $month, (int)$dt_start->format('t')), $tz);

$sds = $dt_start->getTimestamp();
$sde = $dt_end->getTimestamp();

// Open MySQL connection
$conn = mysqli_connect($db_host, $db_user, $db_pass, $db_name);
if (!$conn) {
    die("Database connection error: " . mysqli_connect_error() . "\n");
}
@mysqli_set_charset($conn, "utf8mb4");

// Query dataset for the given month interval
$sql = "SELECT epoch, pun FROM `$table_name` WHERE epoch BETWEEN $sds AND $sde ORDER BY epoch ASC";
$res = mysqli_query($conn, $sql);

// Fallback if column name is 'prezzo' instead of 'pun'
if (!$res) {
    $sql = "SELECT epoch, prezzo AS pun FROM `$table_name` WHERE epoch BETWEEN $sds AND $sde ORDER BY epoch ASC";
    $res = mysqli_query($conn, $sql);
}

if (!$res || mysqli_num_rows($res) === 0) {
    mysqli_close($conn);
    die("No data found for period $year-" . sprintf("%02d", $month) . "\n");
}

// Initialize accumulators for energy bands
$sums   = ['F0' => 0.0, 'F1' => 0.0, 'F2' => 0.0, 'F3' => 0.0];
$counts = ['F0' => 0,   'F1' => 0,   'F2' => 0,   'F3' => 0];

// Iterate through records
while ($row = mysqli_fetch_assoc($res)) {
    $epoch = (int)$row['epoch'];
    $val   = (float)$row['pun'];

    // Convert GMT epoch to Italian timezone DateTime
    $dt = new DateTime("@$epoch");
    $dt->setTimezone($tz);

    // Identify ARERA energy band
    $band = getFasciaOraria($dt);

    $sums['F0'] += $val;
    $counts['F0']++;

    $sums[$band] += $val;
    $counts[$band]++;
}

mysqli_free_result($res);
mysqli_close($conn);

// Compute average value for each band
$f0 = $counts['F0'] > 0 ? $sums['F0'] / $counts['F0'] : 0.0;
$f1 = $counts['F1'] > 0 ? $sums['F1'] / $counts['F1'] : 0.0;
$f2 = $counts['F2'] > 0 ? $sums['F2'] / $counts['F2'] : 0.0;
$f3 = $counts['F3'] > 0 ? $sums['F3'] / $counts['F3'] : 0.0;

// Print formatted monthly band averages
echo sprintf(
    "F0: %.5f | F1: %.5f | F2: %.5f | F3: %.5f\n",
    $f0, $f1, $f2, $f3
);


// Check if a date is an Italian national holiday
function isFestivo(DateTime $dt): bool {
    $m = (int)$dt->format('m');
    $d = (int)$dt->format('d');
    $y = (int)$dt->format('Y');

    // List of fixed Italian holidays
    $fissi = [
        '01-01',
        '01-06',
        '04-25',
        '05-01',
        '06-02',
        '08-15',
        '11-01',
        '12-08',
        '12-25',
        '12-26',
    ];

    if (in_array(sprintf('%02d-%02d', $m, $d), $fissi, true)) {
        return true;
    }

    // Dynamic calculation of Easter Monday
    $pasquetta = new DateTime("$y-03-21");
    $pasquetta->modify("+" . easter_days($y) . " days +1 day");

    return $dt->format('Y-m-d') === $pasquetta->format('Y-m-d');
}

// Determine ARERA energy band (F1, F2, F3) based on Italian local time
function getFasciaOraria(DateTime $dt): string {
    $dow = (int)$dt->format('N');
    $h   = (int)$dt->format('H');

    // Sunday or National Holiday is always F3
    if ($dow === 7 || isFestivo($dt)) {
        return 'F3';
    }

    // Saturday
    if ($dow === 6) {
        return ($h >= 7 && $h < 23) ? 'F2' : 'F3';
    }

    // Monday to Friday
    if ($h >= 8 && $h < 19) {
        return 'F1';
    }
    if (($h >= 7 && $h < 8) || ($h >= 19 && $h < 23)) {
        return 'F2';
    }

    return 'F3';
}
