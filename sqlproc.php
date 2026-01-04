<?php
include "local.php";

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

$cache_path = "$cache_dir/$q";
if (file_exists($cache_path)) {
  echo file_get_contents($cache_path);
  return;
}

$unionParts = [];
for ($i = 0; $i < count($tab); $i++) {
  $tbl = $tab[$i]["table"];
  $unionParts[] = "SELECT epoch FROM $tbl WHERE epoch BETWEEN $sds AND $sde";
}
$unionSql = implode("\n  UNION\n  ", $unionParts);

$selectCols = ["e.epoch"];
$joins      = [];
$seriesKeys = [];

for ($i = 0; $i < count($tab); $i++) {
  $tbl = $tab[$i]["table"];
  $als = "t$i";
  $joins[] = "LEFT JOIN $tbl $als ON $als.epoch = e.epoch";

  foreach ($tab[$i]["cols"] as $colIndex => $col) {
    $safeCol = preg_replace('/[^A-Za-z0-9_]/', '_', $col);
    $key = "{$als}__{$safeCol}_{$colIndex}";
    $seriesKeys[] = $key;
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
          'if','ifnull','coalesce',
          'round','floor','ceil',
          'date','now','unix_timestamp'
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

$conn = mysqli_connect($db_host, $db_user, $db_pass, $db_name);
if (!$conn) {
  echo "[]";
  return;
}

@mysqli_set_charset($conn, "utf8mb4");

$resN = mysqli_query($conn, $sqlCount);
if (!$resN) {
  mysqli_close($conn);
  echo "[]";
  return;
}
$rowN = mysqli_fetch_assoc($resN);
mysqli_free_result($resN);

$N = (int)$rowN["N"];
if ($N <= 0) {
  mysqli_close($conn);
  // scrivo cache vuota
  file_put_contents($cache_path, "[]");
  echo "[]";
  return;
}

$agg = max(1, (int)ceil($N / $points));

$fp = fopen($cache_path, "w");
if (!$fp) {
  mysqli_close($conn);
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
  mysqli_close($conn);
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
      $m = $cnt[$k] ? ($acc[$k] / $cnt[$k]) : ($seen[$k] ? null : 0.0);
      $out[] = ($m === null) ? "null" : sprintf("%9.5f", $m);
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
    $m = $cnt[$k] ? ($acc[$k] / $cnt[$k]) : ($seen[$k] ? null : 0.0);
    $out[] = ($m === null) ? "null" : sprintf("%9.5f", $m);
  }

  fprintf($fp, "[%s],\n", implode(", ", $out));
}

mysqli_free_result($res);
mysqli_close($conn);
fclose($fp);

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
}

?>
