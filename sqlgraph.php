<?php
// sqlgraph.php - Graph rendering and cache delivery module

// Always load local.php from the current working directory
$local_config = getcwd() . '/local.php';
if (file_exists($local_config)) {
    require_once $local_config;
} else {
    http_response_code(500);
    die("Error: Unable to find local.php in current working directory (" . getcwd() . ")\n");
}

require_once __DIR__ . '/sqlproc.php';

// Validate authentication key if supplied
$req_auth = isset($_GET['auth']) ? $_GET['auth'] : (isset($_GET['key']) ? $_GET['key'] : '');
if (!empty($req_auth)) {
    if (isset($auth_key) && isset($myauth)) {
        if ($req_auth !== $auth_key && $req_auth !== $myauth) {
            http_response_code(403);
            die("Access denied: Invalid authentication key.\n");
        }
    }
}

// Parse date range parameters
$hours = isset($_GET['hours']) ? (int)$_GET['hours'] : 24;
$raw_from = isset($_GET['from']) ? $_GET['from'] : "-{$hours} hours";
$raw_to   = isset($_GET['to'])   ? $_GET['to']   : 'now';

$time_from = strtotime($raw_from);
$time_to   = strtotime($raw_to);

if ($time_from === false || $time_to === false) {
    http_response_code(400);
    die("Invalid date/time parameters.\n");
}

$from_str = date('Y-m-d H:i:s', $time_from);
$to_str   = date('Y-m-d H:i:s', $time_to);

// Configure flat cache directory inside current working directory
$cache_folder = isset($cache_dir) ? $cache_dir : 'tmp';
$cache_dir_path = getcwd() . '/' . trim($cache_folder, '/');

if (!is_dir($cache_dir_path)) {
    @mkdir($cache_dir_path, 0755, true);
}

// Generate single deterministic cache filename using MD5 hash
$cache_hash = md5("{$from_str}_{$to_str}_" . serialize(isset($labels) ? $labels : []));
$cache_file = "{$cache_dir_path}/g_{$cache_hash}.html";
$cache_ttl  = 300; // 5 minutes cache lifetime

// Deliver cached output if valid
if (file_exists($cache_file) && (time() - filemtime($cache_file) < $cache_ttl)) {
    header('Content-Type: text/html; charset=utf-8');
    readfile($cache_file);
    exit;
}

// Retrieve dataset in RAM
$dataset = get_sensors_data($from_str, $to_str);

// Render Google Charts HTML output
ob_start();
?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title><?= htmlspecialchars(isset($mytitle) ? $mytitle : 'Sensors Chart') ?></title>
    <script type="text/javascript" src="https://www.gstatic.com/charts/loader.js"></script>
    <script type="text/javascript">
      google.charts.load('current', {'packages':['corechart']});
      google.charts.setOnLoadCallback(drawChart);

      function drawChart() {
        var data = new google.visualization.DataTable();
        data.addColumn('datetime', 'Time');
        
        <?php if (isset($labels) && is_array($labels)): ?>
        <?php foreach ($labels as $label): ?>
        data.addColumn('number', <?= json_encode($label) ?>);
        <?php endforeach; ?>
        <?php endif; ?>

        data.addRows([
          <?php foreach ($dataset as $timestamp => $values): ?>
          [
            new Date("<?= date('c', strtotime($timestamp)) ?>"), 
            <?= implode(',', array_map(function($v) { return $v === null ? 'null' : $v; }, $values)) ?>
          ],
          <?php endforeach; ?>
        ]);

        var options = {
          title: <?= json_encode(isset($mytitle) ? $mytitle : '') ?>,
          hAxis: { title: 'Date / Time', format: 'dd/MM HH:mm' },
          vAxis: { 
            title: <?= json_encode(isset($axisTitleLeft) ? $axisTitleLeft : '') ?>,
            minValue: <?= (isset($yLeftMin0) && $yLeftMin0) ? '0' : 'null' ?>
          },
          series: <?= json_encode(isset($seriesOpt) ? $seriesOpt : []) ?>,
          legend: { position: 'bottom' },
          chartArea: { width: '85%', height: '70%' }
        };

        var chart = new google.visualization.LineChart(document.getElementById('chart_div'));
        chart.draw(data, options);
      }
    </script>
    <style>
        body { font-family: Arial, sans-serif; margin: 15px; background: #f9f9f9; }
        #chart_div { width: 100%; height: 550px; background: #fff; border: 1px solid #ccc; border-radius: 4px; }
    </style>
</head>
<body>
    <div id="chart_div"></div>
</body>
</html>
<?php
$html_output = ob_get_clean();

// Overwrite deterministic cache file
file_put_contents($cache_file, $html_output, LOCK_EX);

header('Content-Type: text/html; charset=utf-8');
echo $html_output;
exit;
