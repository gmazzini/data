<?php
include "local.php";

$q = $_GET['q'] ?? null;

$labels = $labels ?? [];
$seriesOpt = $seriesOpt ?? [];
$axisTitleLeft = $axisTitleLeft ?? "";
$axisTitleRight = $axisTitleRight ?? "";
$mytitle = $mytitle ?? "";

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

  if (!is_array($rows)) {
    $rows = [];
  }
}

$minAxis = [
  0 => null,
  1 => null
];

foreach ($rows as $r) {
  for ($j = 1; $j < count($header); $j++) {
    if (!array_key_exists($j, $r)) {
      continue;
    }

    $v = $r[$j];

    if ($v === null) {
      continue;
    }

    $axis = 0;

    if (isset($seriesOpt[$j - 1]['targetAxisIndex'])) {
      $axis = (int)$seriesOpt[$j - 1]['targetAxisIndex'];
    }

    if (!isset($minAxis[$axis])) {
      $minAxis[$axis] = null;
    }

    if ($minAxis[$axis] === null || $v < $minAxis[$axis]) {
      $minAxis[$axis] = $v;
    }
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

$vAxes = [
  0 => ['title' => $axisTitleLeft],
  1 => ['title' => $axisTitleRight],
];

if ($yLeftMin0 && isset($minAxis[0]) && $minAxis[0] !== null) {
  $vAxes[0]['viewWindow'] = [
    'min' => (float)$minAxis[0]
  ];
  $vAxes[0]['title'] = $axisTitleLeft . " (min: " . $minAxis[0] . ")";
}

if ($yRightMin0 && isset($minAxis[1]) && $minAxis[1] !== null) {
  $vAxes[1]['viewWindow'] = [
    'min' => (float)$minAxis[1]
  ];
  $vAxes[1]['title'] = $axisTitleRight . " (min: " . $minAxis[1] . ")";
}

$vAxesJs = json_encode((object)$vAxes, JSON_UNESCAPED_UNICODE);
$seriesOptJs = json_encode((object)$seriesOpt, JSON_UNESCAPED_UNICODE);
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
      const data = new google.visualization.DataTable();

      data.addColumn('string', <?= json_encode($header[0] ?? "x", JSON_UNESCAPED_UNICODE) ?>);

<?php for ($i = 1; $i < count($header); $i++): ?>
      data.addColumn('number', <?= json_encode($header[$i], JSON_UNESCAPED_UNICODE) ?>);
<?php endfor; ?>

      data.addRows([
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

        vAxes: <?= $vAxesJs ?>,

        series: <?= $seriesOptJs ?>

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
