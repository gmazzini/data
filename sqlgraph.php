<?php
// sqlgraph.php - Responsive chart rendering script

date_default_timezone_set('Europe/Rome');

$local_config = getcwd() . '/local.php';
if (file_exists($local_config)) {
    require_once $local_config;
} else {
    http_response_code(500);
    die("Error: Unable to find local.php in current working directory (" . getcwd() . ")\n");
}

$q_raw = $_GET['q'] ?? '';

// Navigation helper function to build q query parameter
function build_q_param(DateTime $dt, string $mode): string {
    $y = $dt->format('Y');
    if ($mode === 'w') {
        return sprintf("%04dw%02d", $y, $dt->format('W'));
    } else if ($mode === 'm') {
        return sprintf("%04dm%02d", $y, $dt->format('n'));
    }
    return sprintf("%04dd%03d", $y, $dt->format('z'));
}

$tz_rome = new DateTimeZone('Europe/Rome');

// 1. Interroga il DB per trovare l'ultimo epoch disponibile tra le tabelle configurate
$max_epoch = time(); // fallback all'ora attuale
$conn_max = @mysqli_connect($db_host, $db_user, $db_pass, $db_name);

if ($conn_max && isset($tab) && is_array($tab)) {
    @mysqli_set_charset($conn_max, "utf8mb4");
    $maxParts = [];

    foreach ($tab as $t) {
        $tbl = preg_replace('/[^A-Za-z0-9_]/', '', $t["table"]);
        $where = "1=1";
        if (isset($t["device"])) {
            $dev = mysqli_real_escape_string($conn_max, $t["device"]);
            $where .= " AND device = '$dev'";
        }
        $maxParts[] = "SELECT MAX(epoch) AS max_e FROM `$tbl` WHERE $where";
    }

    if (!empty($maxParts)) {
        $sqlMax = "SELECT MAX(max_e) AS global_max FROM (" . implode(" UNION ", $maxParts) . ") tmp";
        $resMax = @mysqli_query($conn_max, $sqlMax);
        if ($resMax && $rowMax = mysqli_fetch_assoc($resMax)) {
            if (!empty($rowMax['global_max'])) {
                $max_epoch = (int)$rowMax['global_max'];
            }
        }
    }
    @mysqli_close($conn_max);
}

// 2. Definizione del periodo massimo consentito (basato sul DB) e di TODAY (giorno solare attuale)
$dt_max = new DateTime('@' . $max_epoch);
$dt_max->setTimezone($tz_rome);
$dt_max->setTime(0, 0, 0);

$dt_today = new DateTime('now', $tz_rome);
$dt_today->setTime(0, 0, 0);

// Parse del parametro active period o fallback all'ultimo giorno disponibile a DB
$current_mode = 'd';
$dt = clone $dt_max;

if (preg_match('/^(\d{4})([dwm])(\d+)$/', $q_raw, $m)) {
    $year = (int)$m[1];
    $type = $m[2];
    $num  = (int)$m[3];
    $current_mode = $type;

    $dt->setDate($year, 1, 1);
    if ($type === 'd') {
        $dt->modify("+$num days");
    } else if ($type === 'w') {
        $dt->setISODate($year, $num, 1);
    } else if ($type === 'm') {
        $dt->setDate($year, $num, 1);
    }
}

// Calcola il limite di navigazione in base alla modalità (D/W/M)
if ($current_mode === 'w') {
    $limit_dt = clone $dt_max;
    $limit_dt->setISODate((int)$dt_max->format('Y'), (int)$dt_max->format('W'), 1);
} else if ($current_mode === 'm') {
    $limit_dt = clone $dt_max;
    $limit_dt->setDate((int)$dt_max->format('Y'), (int)$dt_max->format('n'), 1);
} else {
    $limit_dt = clone $dt_max;
}

if ($dt > $limit_dt) {
    $dt = clone $limit_dt;
}

// Calcolo periodi Precedente e Successivo
$dt_prev = clone $dt;
$dt_next = clone $dt;

if ($current_mode === 'w') {
    $dt_prev->modify('-1 week');
    $dt_next->modify('+1 week');
} else if ($current_mode === 'm') {
    $dt_prev->modify('-1 month');
    $dt_next->modify('+1 month');
} else {
    $dt_prev->modify('-1 day');
    $dt_next->modify('+1 day');
}

$is_future = ($dt_next > $limit_dt);

$q_prev  = build_q_param($dt_prev, $current_mode);
$q_next  = build_q_param($dt_next, $current_mode);
$q       = build_q_param($dt, $current_mode);
$q_today = build_q_param($dt_today, $current_mode);

$q_mode_d = build_q_param($dt, 'd');
$q_mode_w = build_q_param($dt, 'w');
$q_mode_m = build_q_param($dt, 'm');

$q_reload = "?q=" . $q;

if ($current_mode === 'w') {
    $end_w = clone $dt;
    $end_w->modify('+6 days');
    $date_label = "Week " . $dt->format('W/Y') . " (" . $dt->format('d/m') . " - " . $end_w->format('d/m') . ")";
} else if ($current_mode === 'm') {
    $date_label = "Month " . $dt->format('m/Y');
} else {
    $date_label = $dt->format('d/m/Y');
}

$h_axis_title = "Data / Ora (Ora Italiana)";

// Elaborazione dati con sqlproc.php
$labels = $labels ?? [];
$seriesOpt = $seriesOpt ?? [];
$axisTitleLeft = $axisTitleLeft ?? "";
$axisTitleRight = $axisTitleRight ?? "";
$mytitle = $mytitle ?? "";

$header = array_merge(["x"], $labels);

ob_start();
$local_sqlproc = getcwd() . '/sqlproc.php';
if (file_exists($local_sqlproc)) {
    include $local_sqlproc;
} else {
    header('HTTP/1.1 500 Internal Server Error');
    die("Errore: 'sqlproc.php' non trovato nella directory corrente (" . getcwd() . ")." . PHP_EOL);
}
$dataRows = ob_get_clean();

$dds = $dds ?? "";
$dde = $dde ?? "";
$title = trim("$mytitle $dds $dde");

$rowsText = trim($dataRows);
$rowsText = rtrim($rowsText, ", \r\n\t");

$rows = [];
if ($rowsText !== "") {
    $jsonish = "[" . preg_replace("/'/", "\"", $rowsText) . "]";
    $rows = json_decode($jsonish, true);

    if (!is_array($rows)) {
        $rows = [];
    }
}

$dataRowsJs = [];
foreach ($rows as $r) {
    $out = [];
    $out[] = json_encode($r[0] ?? "", JSON_UNESCAPED_UNICODE);

    for ($j = 1; $j < count($header); $j++) {
        if (!array_key_exists($j, $r) || $r[$j] === null) {
            $out[] = "null";
        } else {
            $out[] = sprintf("%.5f", (float)$r[$j]);
        }
    }

    $dataRowsJs[] = "[" . implode(", ", $out) . "]";
}

$dataRowsFinal = implode(",\n        ", $dataRowsJs);

$axisRange = $axisRange ?? [
    0 => ['min' => 0, 'max' => 1],
    1 => ['min' => 0, 'max' => 1],
];

$vAxes = [];
for ($axis = 0; $axis <= 1; $axis++) {
    $vAxes[$axis] = [
        'title' => ($axis === 0) ? $axisTitleLeft : $axisTitleRight
    ];

    if (
        isset($axisRange[$axis]) &&
        isset($axisRange[$axis]['min']) &&
        isset($axisRange[$axis]['max'])
    ) {
        $min = (float)$axisRange[$axis]['min'];
        $max = (float)$axisRange[$axis]['max'];

        if ($min == $max) {
            $min -= 1;
            $max += 1;
        }

        $vAxes[$axis]['viewWindow'] = [
            'min' => $min,
            'max' => $max
        ];
    }
}

$vAxesJs = json_encode((object)$vAxes, JSON_UNESCAPED_UNICODE);
$seriesOptJs = json_encode((object)$seriesOpt, JSON_UNESCAPED_UNICODE);
?>
<!doctype html>
<html lang="it">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no" />
  <title><?= htmlspecialchars($title, ENT_QUOTES, 'UTF-8') ?></title>
  <style>
    html, body {
      margin: 0;
      padding: 2px 4px;
      font-family: Arial, sans-serif;
      overflow-x: hidden;
    }
    .nav-toolbar {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: 4px;
      background: #f1f3f5;
      padding: 4px 6px;
      border-radius: 4px;
      border: 1px solid #ced4da;
      margin-bottom: 2px;
      font-size: 13px;
    }
    .nav-btn {
      display: inline-block;
      padding: 4px 8px;
      background: #ffffff;
      color: #333333;
      text-decoration: none;
      border: 1px solid #adb5bd;
      border-radius: 3px;
      font-weight: bold;
      line-height: 1.2;
    }
    .nav-btn:hover { background: #e9ecef; }
    .nav-btn.active {
      background: #0d6efd;
      color: #ffffff;
      border-color: #0d6efd;
    }
    .nav-btn.disabled {
      opacity: 0.4;
      cursor: not-allowed;
      pointer-events: none;
      background: #e9ecef;
    }
    .nav-btn-act {
      background: #198754;
      color: #ffffff;
      border-color: #198754;
    }
    .nav-btn-act:hover { background: #157347; }
    .nav-date-label {
      margin-left: 4px;
      font-weight: bold;
      color: #212529;
    }
  </style>
  <script src="https://www.gstatic.com/charts/loader.js"></script>
  <script>
    google.charts.load('current', {packages:['corechart']});
    google.charts.setOnLoadCallback(drawChart);

    window.addEventListener('resize', drawChart);

    function drawChart() {
      const data = new google.visualization.DataTable();

      data.addColumn('string', <?= json_encode($header[0] ?? "x", JSON_UNESCAPED_UNICODE) ?>);

<?php for ($i = 1; $i < count($header); $i++): ?>
      data.addColumn('number', <?= json_encode($header[$i], JSON_UNESCAPED_UNICODE) ?>);
<?php endfor; ?>

      data.addRows([
        <?= $dataRowsFinal !== "" ? "\n        " . $dataRowsFinal . "\n      " : "" ?>
      ]);

      const isMobile = window.innerWidth < 600;

      const options = {
        title: <?= json_encode($title, JSON_UNESCAPED_UNICODE) ?>,
        curveType: 'none',
        legend: {
          position: 'top',
          textStyle: { fontSize: isMobile ? 10 : 12 }
        },
        chartArea: {
          top: isMobile ? 35 : 30,
          left: isMobile ? 45 : '5%',
          right: isMobile ? 15 : '3%',
          bottom: isMobile ? 90 : 110,
          width: isMobile ? '85%' : '92%',
          height: isMobile ? '65%' : '70%'
        },
        hAxis: {
          title: <?= json_encode($h_axis_title, JSON_UNESCAPED_UNICODE) ?>,
          slantedText: true,
          slantedTextAngle: 60,
          textStyle: { fontSize: isMobile ? 9 : 11 }
        },
        vAxes: <?= $vAxesJs ?>,
        series: <?= $seriesOptJs ?>
      };

      new google.visualization.LineChart(document.getElementById('curve_chart'))
        .draw(data, options);
    }
  </script>
</head>
<body>

  <!-- Navigation Toolbar -->
  <div class="nav-toolbar">
    <a href="?q=<?= $q_prev ?>" class="nav-btn" title="Previous">&laquo;</a>
    <?php if ($is_future): ?>
      <span class="nav-btn disabled" title="Limit reached">&raquo;</span>
    <?php else: ?>
      <a href="?q=<?= $q_next ?>" class="nav-btn" title="Next">&raquo;</a>
    <?php endif; ?>
    <a href="?q=<?= $q_today ?>" class="nav-btn nav-btn-act" title="Current Real Date">TODAY</a>
    &nbsp;|
    <a href="?q=<?= $q_mode_d ?>" class="nav-btn <?= $current_mode === 'd' ? 'active' : '' ?>">DAY</a>
    <a href="?q=<?= $q_mode_w ?>" class="nav-btn <?= $current_mode === 'w' ? 'active' : '' ?>">WEEK</a>
    <a href="?q=<?= $q_mode_m ?>" class="nav-btn <?= $current_mode === 'm' ? 'active' : '' ?>">MONTH</a>
    &nbsp;|
    <a href="<?= $q_reload ?>" class="nav-btn" title="Reload view">&#128259;</a>
    <span class="nav-date-label"><?= htmlspecialchars($date_label, ENT_QUOTES, 'UTF-8') ?></span>
  </div>

  <div id="curve_chart" style="width:100%; height:calc(100vh - 48px); min-height:400px;"></div>

</body>
</html>
