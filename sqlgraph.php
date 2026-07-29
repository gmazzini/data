<?php
/**
 * sqlgraph.php - Rendering del grafico con cache flat in tmp/
 * Sfrutta tutte le variabili di configurazione grafiche e di autenticazione di local.php
 */

require_once __DIR__ . '/local.php';
require_once __DIR__ . '/sqlproc.php';

// ---------------------------------------------------------
// 1. AUTENTICAZIONE E SICUREZZA
// ---------------------------------------------------------
$req_auth = $_GET['auth'] ?? $_GET['key'] ?? '';
if (!empty($auth_key) && !empty($myauth)) {
    if ($req_auth !== $auth_key && $req_auth !== $myauth) {
        http_response_code(403);
        die("Accesso negato: chiave di autenticazione non valida.");
    }
}

// ---------------------------------------------------------
// 2. SANIFICAZIONE PARAMETRI TEMPORALI
// ---------------------------------------------------------
$hours = filter_input(INPUT_GET, 'hours', FILTER_VALIDATE_INT) ?: 24;
$raw_from = $_GET['from'] ?? "-{$hours} hours";
$raw_to   = $_GET['to']   ?? 'now';

$time_from = strtotime($raw_from);
$time_to   = strtotime($raw_to);

if ($time_from === false || $time_to === false) {
    http_response_code(400);
    die("Formato data/ora non valido.");
}

$from_str = date('Y-m-d H:i:s', $time_from);
$to_str   = date('Y-m-d H:i:s', $time_to);

// ---------------------------------------------------------
// 3. CACHE DETERMINISTICA IN tmp/ (Zero proliferazione file)
// ---------------------------------------------------------
$cache_dir_path = __DIR__ . '/' . trim($cache_dir, '/');
if (!is_dir($cache_dir_path)) {
    @mkdir($cache_dir_path, 0755, true);
}

// Hash univoco basato sull'intervallo richiesto e sui parametri grafici
$cache_hash = md5("{$from_str}_{$to_str}_" . serialize($labels));
$cache_file = "{$cache_dir_path}/g_{$cache_hash}.html";
$cache_ttl  = 300; // La cache dura 5 minuti (300 secondi)

// Se la cache esiste ed è ancora valida, la eroga subito
if (file_exists($cache_file) && (time() - filemtime($cache_file) < $cache_ttl)) {
    header('Content-Type: text/html; charset=utf-8');
    readfile($cache_file);
    exit;
}

// ---------------------------------------------------------
// 4. ESTRAZIONE DATI IN RAM
// ---------------------------------------------------------
$dataset = get_sensors_data($from_str, $to_str);

// ---------------------------------------------------------
// 5. RENDERING GRAFICO (Google Charts HTML)
// ---------------------------------------------------------
ob_start();
?>
<!DOCTYPE html>
<html lang="it">
<head>
    <meta charset="UTF-8">
    <title><?= htmlspecialchars($mytitle) ?></title>
    <script type="text/javascript" src="https://www.gstatic.com/charts/loader.js"></script>
    <script type="text/javascript">
      google.charts.load('current', {'packages':['corechart']});
      google.charts.setOnLoadCallback(drawChart);

      function drawChart() {
        var data = new google.visualization.DataTable();
        data.addColumn('datetime', 'Tempo');
        
        <?php foreach ($labels as $label): ?>
        data.addColumn('number', <?= json_encode($label) ?>);
        <?php endforeach; ?>

        data.addRows([
          <?php foreach ($dataset as $timestamp => $values): ?>
          [new Date("<?= date('c', strtotime($timestamp)) ?>"), <?= implode(',', array_map(fn($v) => $v === null ? 'null' : $v, $values)) ?>],
          <?php endforeach; ?>
        ]);

        var options = {
          title: <?= json_encode($mytitle) ?>,
          hAxis: { title: 'Data / Ora', format: 'dd/MM HH:mm' },
          vAxis: { 
            title: <?= json_encode($axisTitleLeft) ?>,
            minValue: <?= $yLeftMin0 ? '0' : 'null' ?>
          },
          series: <?= json_encode($seriesOpt) ?>,
          legend: { position: 'bottom' },
          chartArea: { width: '85%', height: '70%' },
          explorer: { actions: ['dragToPan', 'rightClickToReset'], keepInBounds: true }
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

// ---------------------------------------------------------
// 6. SOVRASCRITTURA CACHE ED EROGAZIONE
// ---------------------------------------------------------
// Scrive o sovrascrive l'unico file di cache corrispondente all'hash
file_put_contents($cache_file, $html_output, LOCK_EX);

header('Content-Type: text/html; charset=utf-8');
echo $html_output;
exit;
