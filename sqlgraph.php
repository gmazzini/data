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

$vAxes = [
  0 => ['title' => $axisTitleLeft],
  1 => ['title' => $axisTitleRight],
];

for ($axis = 0; $axis <= 1; $axis++) {
  if (
    isset($axisRange[$axis]) &&
    isset($axisRange[$axis]['min']) &&
    isset($axisRange[$axis]['max'])
  ) {
    $min = (float)$axisRange[$axis]['min'];
    $max = (float)$axisRange[$axis]['max'];

    if ($min == $max) {
      $min = $min - 1;
      $max = $max + 1;
    }

    $vAxes[$axis]['viewWindow'] = [
      'min' => $min,
      'max' => $max
    ];

    if ($axis === 0) {
      $vAxes[$axis]['title'] = $axisTitleLeft . " (" . $min . " - " . $max . ")";
    } else {
      $vAxes[$axis]['title'] = $axisTitleRight . " (" . $min . " - " . $max . ")";
    }
  }
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
