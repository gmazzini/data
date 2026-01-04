<?php
include "local.php";

$q = $_GET['q'] ?? null;

$header = array_merge(["x"], $labels);

ob_start();
include __DIR__ . "/sqlproc.php";
$dataRows = ob_get_clean();

$dds = $dds ?? "";
$dde = $dde ?? "";
$title = trim("$mytitle $dds $dde");

$yLeftMin0  = !empty($yLeftMin0);
$yRightMin0 = !empty($yRightMin0);

$rowsText = trim($dataRows);
$rowsText = rtrim($rowsText, ", \r\n\t");

$rows = [];
if ($rowsText !== "") {
  $jsonish = "[" . preg_replace("/'/", "\"", $rowsText) . "]";
  $rows = json_decode($jsonish, true);
  if (!is_array($rows)) $rows = [];
}

$minAxis = [0 => null, 1 => null];

foreach ($rows as $r) {
  for ($j = 1; $j < count($r); $j++) {
    $v = $r[$j];
    if ($v === null) continue;

    $axis = 0;
    if (isset($seriesOpt[$j-1]['targetAxisIndex'])) {
      $axis = (int)$seriesOpt[$j-1]['targetAxisIndex'];
    }

    if ($minAxis[$axis] === null || $v < $minAxis[$axis]) {
      $minAxis[$axis] = $v;
    }
  }
}

foreach ($rows as &$r) {
  for ($j = 1; $j < count($r); $j++) {
    if ($r[$j] === null) continue;

    $axis = 0;
    if (isset($seriesOpt[$j-1]['targetAxisIndex'])) {
      $axis = (int)$seriesOpt[$j-1]['targetAxisIndex'];
    }

    if ($axis === 0 && $yLeftMin0 && $minAxis[0] !== null) {
      $r[$j] = $r[$j] - $minAxis[0];
    } elseif ($axis === 1 && $yRightMin0 && $minAxis[1] !== null) {
      $r[$j] = $r[$j] - $minAxis[1];
    }
  }
}
unset($r);

$dataRowsJs = [];
foreach ($rows as $r) {
  $out = [];
  $out[] = json_encode($r[0], JSON_UNESCAPED_UNICODE);
  for ($j = 1; $j < count($r); $j++) {
    $out[] = ($r[$j] === null) ? "null" : sprintf("%.5f", (float)$r[$j]);
  }
  $dataRowsJs[] = "[" . implode(", ", $out) . "]";
}
$dataRowsFinal = implode(",\n        ", $dataRowsJs);

$vAxes = [
  0 => ['title' => $axisTitleLeft],
  1 => ['title' => $axisTitleRight],
];

if ($yLeftMin0 && $minAxis[0] !== null) {
  $vAxes[0]['title'] = $axisTitleLeft . " (offset: -" . $minAxis[0] . ")";
}
if ($yRightMin0 && $minAxis[1] !== null) {
  $vAxes[1]['title'] = $axisTitleRight . " (offset: -" . $minAxis[1] . ")";
}
?>
<!doctype html>
<html lang="it">
<head>
  <meta charset="utf-8" />
  <title><?= htmlspecialchars($title, ENT_QUOTES, 'UTF-8') ?></title>
  <script src="https://www.gstatic.com/charts/loader.js"></script>
  <script>
    google.charts.load('current', {packages:['corechart']});
    google.charts.setOnLoadCallback(drawChart);

    function drawChart() {
      const data = google.visualization.arrayToDataTable([
        <?= json_encode($header, JSON_UNESCAPED_UNICODE) ?>,
        <?= $dataRowsFinal !== "" ? "\n        " . $dataRowsFinal . "\n      " : "" ?>
      ]);

      const options = {
        title: <?= json_encode($title, JSON_UNESCAPED_UNICODE) ?>,
        curveType: 'none',
        legend: { position: 'top' },

        hAxis: {
          title: "Data/Ora",
          slantedText: true,
          slantedTextAngle: 90
        },

        vAxes: <?= json_encode($vAxes, JSON_UNESCAPED_UNICODE) ?>,

        series: <?= json_encode($seriesOpt, JSON_UNESCAPED_UNICODE) ?>,
      };

      new google.visualization.LineChart(document.getElementById('curve_chart'))
        .draw(data, options);
    }
  </script>
</head>
<body>
  <div id="curve_chart" style="width:1700px; height:700px"></div>
</body>
</html>
