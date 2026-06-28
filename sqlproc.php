<?php
include_once "local.php";

if (!is_dir($cache_dir)) {
  @mkdir($cache_dir, 0775, true);
}

if (!isset($q)) {
  $q = date("Y\\dz");
}

if (substr($q, 4, 1) == "d") {
  $aux = date_create_from_format("Yz", substr($q, 0, 4) . substr($q, 5));
  $ds  = date_format($aux, "Y/m/d 00:00:00");
  $de  = date_format($aux, "Y/m/d 23:59:59");
  $q   = sprintf("%04dd%03d", substr($q, 0, 4), substr($q, 5));
} else if (substr($q, 4, 1) == "w") {
  $aux = date_create();
  date_isodate_set($aux, substr($q, 0, 4), substr($q, 5));
  $ds  = date_format($aux, "Y/m/d 00:00:00");
  date_add($aux, date_interval_create_from_date_string("6 days"));
  $de  = date_format($aux, "Y/m/d 23:59:59");
  $q   = sprintf("%04dw%02d", substr($q, 0, 4), substr($q, 5));
} else if (substr($q, 4, 1) == "m") {
  $aux = date_create_from_format("Yn", substr($q, 0, 4) . substr($q, 5));
  $ds  = date_format($aux, "Y/m/01 00:00:00");
  $de  = date_format($aux, "Y/m/t 23:59:59");
  $q   = sprintf("%04dm%02d", substr($q, 0, 4), substr($q, 5));
} else {
  $aux = date_create();
  $ds  = date_format($aux, "Y/m/d 00:00:00");
  $de  = date_format($aux, "Y/m/d 23:59:59");
  $q   = date("Y\\dz");
  $q   = sprintf("%04dd%03d", substr($q, 0, 4), substr($q, 5));
}

$sds = strtotime($ds);
$sde = strtotime($de);
$dds = "from day:".date("z",$sds).":".date("w",$sds)." week:".date("W",$sds)." month:".date("m",$sds);
$dde = "to day:".date("z",$sde).":".date("w",$sde)." week:".date("W",$sde)." month:".date("m",$sde);

$axisRange = [
  0 => ['min' => 0, 'max' => 1],
  1 => ['min' => 0, 'max' => 1],
];

$axisStats = [
  0 => ['min' => null, 'max' => null],
  1 => ['min' => null, 'max' => null],
];

// Cache retrocompatibile ma distinta per configurazione.
// Serve perché lo stesso q può rappresentare grafici diversi:
// tabelle diverse, device diversi, colonne diverse, points diverso.
$cache_key = md5(json_encode($tab) . "|" . $points . "|" . $q);
$cache_path = "$cache_dir/$q-$cache_key";
$cache_axis_path = "$cache_path.axis.json";

if (file_exists($cache_path) && file_exists($cache_axis_path)) {
  $axisJson = file_get_contents($cache_axis_path);
  $axisTmp = json_decode($axisJson, true);

  if (is_array($axisTmp)) {
    $axisRange = $axisTmp;
  }

  echo file_get_contents($cache_path);
  return;
}

$conn = mysqli_connect($db_host, $db_user, $db_pass, $db_name);
if (!$conn) {
  echo "[]";
  return;
}

@mysqli_set_charset($conn, "utf8mb4");

$unionParts = [];
for ($i = 0; $i < count($tab); $i++) {
  $tbl = $tab[$i]["table"];

  $where = "epoch BETWEEN $sds AND $sde";

  if (isset($tab[$i]["device"])) {
    $dev = mysqli_real_escape_string($conn, $tab[$i]["device"]);
    $where .= " AND device = '$dev'";
  }

  $unionParts[] = "SELECT epoch FROM $tbl WHERE $where";
}
$unionSql = implode("\n  UNION\n  ", $unionParts);

$selectCols = ["e.epoch"];
$joins      = [];
$seriesKeys = [];
$seriesAxes = [];

for ($i = 0; $i < count($tab); $i++) {
  $tbl = $tab[$i]["table"];
  $als = "t$i";

  $joinCond = "$als.epoch = e.epoch";

  if (isset($tab[$i]["device"])) {
    $dev = mysqli_real_escape_string($conn, $tab[$i]["device"]);
    $joinCond .= " AND $als.device = '$dev'";
  }

  $joins[] = "LEFT JOIN $tbl $als ON $joinCond";

  foreach ($tab[$i]["cols"] as $colIndex => $col) {
    $safeCol = preg_replace('/[^A-Za-z0-9_]/', '_', $col);
    $key = "{$als}__{$safeCol}_{$colIndex}";
    $seriesIndex = count($seriesKeys);

    $seriesKeys[] = $key;

    $axis = 0;
    if (isset($seriesOpt[$seriesIndex]['targetAxisIndex'])) {
      $axis = (int)$seriesOpt[$seriesIndex]['targetAxisIndex'];
    }
    $seriesAxes[$key] = $axis;

    if (preg_match('/^[A-Za-z_][A-Za-z0-9_]*$/', $col)) {
      $selectCols[] = "$als.`$col` AS `$key`";
      continue;
    }

    $expr = preg_replace_callback(
      '/\b([A-Za-z_][A-Za-z0-9_]*)\b/',
      function($m) use ($als) {
        $w = strtolower($m[1]);
        static $skip = [
          'null','true','false',
          'abs','avg','count','sum','min','max',
          'if','ifnull','coalesce','nullif',
          'round','floor','ceil',
          'date','now','unix_timestamp',
          'case','when','then','else','end',
          'over','partition','by',
          'rows','range','groups',
          'current_row','unbounded',
          'preceding','following'
        ];
        if (in_array($w, $skip, true)) return $m[1];
        return $als . ".`" . $m[1] . "`";
      },
      $col
    );

    $selectCols[] = "($expr) AS `$key`";
  }
}

$sqlCount = "
SELECT COUNT(*) AS N
FROM (
  $unionSql
) e
";

$sqlData = "
SELECT " . implode(", ", $selectCols) . "
FROM (
  $unionSql
) e
" . implode("\n", $joins) . "
ORDER BY e.epoch
";

$resN = mysqli_query($conn, $sqlCount);
if (!$resN) {
  mysqli_close($conn);

  file_put_contents($cache_axis_path, json_encode($axisRange));
  echo "[]";
  return;
}

$rowN = mysqli_fetch_assoc($resN);
mysqli_free_result($resN);

$N = (int)$rowN["N"];
if ($N <= 0) {
  mysqli_close($conn);

  file_put_contents($cache_path, "[]");
  file_put_contents($cache_axis_path, json_encode($axisRange));

  echo "[]";
  return;
}

$agg = max(1, (int)ceil($N / $points));

$fp = fopen($cache_path, "w");
if (!$fp) {
  mysqli_close($conn);

  file_put_contents($cache_axis_path, json_encode($axisRange));
  echo "[]";
  return;
}

$last = [];
$seen = [];
$acc  = [];
$cnt  = [];

foreach ($seriesKeys as $k) {
  $last[$k] = null;
  $seen[$k] = false;
  $acc[$k]  = 0.0;
  $cnt[$k]  = 0;
}

$nagg = 0;
$lastEpochInBucket = null;

$res = mysqli_query($conn, $sqlData);
if (!$res) {
  fclose($fp);
  @unlink($cache_path);
  @unlink($cache_axis_path);
  mysqli_close($conn);

  file_put_contents($cache_axis_path, json_encode($axisRange));
  echo "[]";
  return;
}

while ($row = mysqli_fetch_assoc($res)) {
  $epoch = (int)$row["epoch"];
  $lastEpochInBucket = $epoch;

  foreach ($seriesKeys as $k) {
    if ($row[$k] !== null) {
      $last[$k] = (float)$row[$k];
      $seen[$k] = true;
    }

    $v = ($last[$k] !== null) ? $last[$k] : null;

    if ($v !== null) {
      $acc[$k] += $v;
      $cnt[$k] += 1;
    }
  }

  $nagg++;

  if ($nagg >= $agg) {
    $dd = $lastEpochInBucket;
    $out = [];

    $out[] = sprintf("'%s%s%s %s%s'",
      date("y", $dd), date("m", $dd), date("d", $dd), date("H", $dd), date("i", $dd)
    );

    foreach ($seriesKeys as $k) {
      $m = $cnt[$k] ? ($acc[$k] / $cnt[$k]) : null;

      if ($m === null) {
        $out[] = "null";
      } else {
        $axis = $seriesAxes[$k] ?? 0;

        if (!isset($axisStats[$axis])) {
          $axisStats[$axis] = ['min' => null, 'max' => null];
        }

        if ($axisStats[$axis]['min'] === null || $m < $axisStats[$axis]['min']) {
          $axisStats[$axis]['min'] = $m;
        }

        if ($axisStats[$axis]['max'] === null || $m > $axisStats[$axis]['max']) {
          $axisStats[$axis]['max'] = $m;
        }

        $out[] = sprintf("%9.5f", $m);
      }

      $acc[$k] = 0.0;
      $cnt[$k] = 0;
    }

    fprintf($fp, "[%s],\n", implode(", ", $out));
    $nagg = 0;
  }
}

if ($nagg > 0 && $lastEpochInBucket !== null) {
  $dd = $lastEpochInBucket;
  $out = [];

  $out[] = sprintf("'%s%s%s %s%s'",
    date("y", $dd), date("m", $dd), date("d", $dd), date("H", $dd), date("i", $dd)
  );

  foreach ($seriesKeys as $k) {
    $m = $cnt[$k] ? ($acc[$k] / $cnt[$k]) : null;

    if ($m === null) {
      $out[] = "null";
    } else {
      $axis = $seriesAxes[$k] ?? 0;

      if (!isset($axisStats[$axis])) {
        $axisStats[$axis] = ['min' => null, 'max' => null];
      }

      if ($axisStats[$axis]['min'] === null || $m < $axisStats[$axis]['min']) {
        $axisStats[$axis]['min'] = $m;
      }

      if ($axisStats[$axis]['max'] === null || $m > $axisStats[$axis]['max']) {
        $axisStats[$axis]['max'] = $m;
      }

      $out[] = sprintf("%9.5f", $m);
    }
  }

  fprintf($fp, "[%s],\n", implode(", ", $out));
}

mysqli_free_result($res);
mysqli_close($conn);
fclose($fp);

for ($axis = 0; $axis <= 1; $axis++) {
  if (
    isset($axisStats[$axis]) &&
    $axisStats[$axis]['min'] !== null &&
    $axisStats[$axis]['max'] !== null
  ) {
    $min = (float)$axisStats[$axis]['min'];
    $max = (float)$axisStats[$axis]['max'];

    if ($min == $max) {
      $min = $min - 1;
      $max = $max + 1;
    }

    $axisRange[$axis] = [
      'min' => $min,
      'max' => $max
    ];
  } else {
    $axisRange[$axis] = [
      'min' => 0,
      'max' => 1
    ];
  }
}

file_put_contents($cache_axis_path, json_encode($axisRange));

echo file_get_contents($cache_path);

if (substr($q, 4, 1) == "d") {
  $xq = date("Y\\dz");
  $vq = sprintf("%04dd%03d", substr($xq, 0, 4), substr($xq, 5));
} else if (substr($q, 4, 1) == "w") {
  $xq = date("Y\\wW");
  $vq = sprintf("%04dw%02d", substr($xq, 0, 4), substr($xq, 5));
} else if (substr($q, 4, 1) == "m") {
  $xq = date("Y\\mn");
  $vq = sprintf("%04dm%02d", substr($xq, 0, 4), substr($xq, 5));
} else {
  $vq = $q;
}

if ($q == $vq) {
  @unlink($cache_path);
  @unlink($cache_axis_path);
}

?>
