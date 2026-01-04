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
        <?= $dataRows ?>
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

        vAxes: {
          0: { title: <?= json_encode($axisTitleLeft,  JSON_UNESCAPED_UNICODE) ?> },
          1: { title: <?= json_encode($axisTitleRight, JSON_UNESCAPED_UNICODE) ?> }
        },

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
