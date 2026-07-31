<?php
// sqlproc.php
// Data processing engine (Real-time SQL aggregation).
//
// Delta modes:
//   delta = 0: average value within each aggregation bucket
//   delta = 1: increment occurring within each aggregation bucket
//   delta = 2: cumulative increment from the beginning of the requested period

// Load local.php dynamically from current working directory
$local_config = getcwd() . '/local.php';

if (file_exists($local_config)) {
    require_once $local_config;
} else {
    http_response_code(500);
    die("Error: Unable to find local.php in current working directory (" . getcwd() . ")\n");
}

// Default query period (Year and Day of Year)
if (!isset($q)) {
    $q = sprintf("%04dd%03d", (int)date("Y"), (int)date("z"));
}

// Parse date range according to period format
if (substr($q, 4, 1) == "d") {
    $year      = (int)substr($q, 0, 4);
    $dayOfYear = (int)substr($q, 5); // 0-indexed day of year (0..365)

    // Using '!' resets time and month to Jan 1st 00:00:00 to prevent current-date bleed
    $aux = date_create_from_format("!Y", (string)$year);
    if ($aux) {
        date_add($aux, date_interval_create_from_date_string("$dayOfYear days"));
        $ds = date_format($aux, "Y/m/d 00:00:00");
        $de = date_format($aux, "Y/m/d 23:59:59");
    } else {
        $ds = date("Y/m/d 00:00:00");
        $de = date("Y/m/d 23:59:59");
    }

    $q = sprintf("%04dd%03d", $year, $dayOfYear);

} elseif (substr($q, 4, 1) == "w") {
    $year = (int)substr($q, 0, 4);
    $week = (int)substr($q, 5);

    // Set explicitly to Monday (day 1) of ISO week
    $aux = date_create_from_format("!Y", (string)$year);
    date_isodate_set($aux, $year, $week, 1);

    $ds = date_format($aux, "Y/m/d 00:00:00");

    date_add($aux, date_interval_create_from_date_string("6 days"));
    $de = date_format($aux, "Y/m/d 23:59:59");

    $q = sprintf("%04dw%02d", $year, $week);

} elseif (substr($q, 4, 1) == "m") {
    $year  = (int)substr($q, 0, 4);
    $month = max(1, min(12, (int)substr($q, 5)));

    // Explicitly set day 01 to prevent 29/30/31 overflow
    $ds  = sprintf("%04d/%02d/01 00:00:00", $year, $month);
    $aux = date_create_from_format("Y/m/d H:i:s", $ds);
    $de  = date_format($aux, "Y/m/t 23:59:59"); // 't' gets exact last day of month

    $q = sprintf("%04dm%02d", $year, $month);

} else {
    $now       = date_create();
    $year      = (int)date_format($now, "Y");
    $dayOfYear = (int)date_format($now, "z");

    $ds = date_format($now, "Y/m/d 00:00:00");
    $de = date_format($now, "Y/m/d 23:59:59");

    $q = sprintf("%04dd%03d", $year, $dayOfYear);
}

$sds = strtotime($ds);
$sde = strtotime($de);

$dds = "from day:" . date("z", $sds) . ":" . date("w", $sds) . " week:" . date("W", $sds) . " month:" . date("m", $sds);
$dde = "to day:" . date("z", $sde) . ":" . date("w", $sde) . " week:" . date("W", $sde) . " month:" . date("m", $sde);

// Default axis ranges
$axisRange = [
    0 => ['min' => 0, 'max' => 1],
    1 => ['min' => 0, 'max' => 1],
];

$axisStats = [
    0 => ['min' => null, 'max' => null],
    1 => ['min' => null, 'max' => null],
];

// Connect to database
$conn = mysqli_connect($db_host, $db_user, $db_pass, $db_name);

if (!$conn) {
    echo "[]";
    return;
}

@mysqli_set_charset($conn, "utf8mb4");

// Build UNION query containing every epoch present in any configured table
$unionParts = [];

for ($i = 0; $i < count($tab); $i++) {
    $tbl = preg_replace('/[^A-Za-z0-9_]/', '', $tab[$i]["table"]);
    $where = "epoch BETWEEN $sds AND $sde";

    if (isset($tab[$i]["device"])) {
        $dev = mysqli_real_escape_string($conn, $tab[$i]["device"]);
        $where .= " AND device = '$dev'";
    }

    $unionParts[] = "SELECT epoch FROM `$tbl` WHERE $where";
}

$unionSql = implode("\n  UNION\n  ", $unionParts);

// Build selected columns and joins
$selectCols = ["e.epoch"];
$joins = [];

$seriesKeys = [];
$seriesAxes = [];
$seriesDeltaModes = [];

for ($i = 0; $i < count($tab); $i++) {
    $tbl = preg_replace('/[^A-Za-z0-9_]/', '', $tab[$i]["table"]);
    $als = "t$i";
    $joinCond = "$als.epoch = e.epoch";

    if (isset($tab[$i]["device"])) {
        $dev = mysqli_real_escape_string($conn, $tab[$i]["device"]);
        $joinCond .= " AND $als.device = '$dev'";
    }

    $joins[] = "LEFT JOIN `$tbl` $als ON $joinCond";

    // Read delta as a real mode, not as a boolean
    $deltaMode = isset($tab[$i]["delta"]) ? (int)$tab[$i]["delta"] : 0;
    if ($deltaMode !== 0 && $deltaMode !== 1 && $deltaMode !== 2) {
        $deltaMode = 0;
    }

    foreach ($tab[$i]["cols"] as $colIndex => $col) {
        $safeCol = preg_replace('/[^A-Za-z0-9_]/', '_', $col);
        $key = "{$als}__{$safeCol}_{$colIndex}";
        $seriesIndex = count($seriesKeys);

        $seriesKeys[] = $key;
        $seriesDeltaModes[$key] = $deltaMode;

        $axis = 0;
        if (isset($seriesOpt[$seriesIndex]['targetAxisIndex'])) {
            $axis = (int)$seriesOpt[$seriesIndex]['targetAxisIndex'];
        }

        $seriesAxes[$key] = $axis;

        // Simple column name
        if (preg_match('/^[A-Za-z_][A-Za-z0-9_]*$/', $col)) {
            $selectCols[] = "$als.`$col` AS `$key`";
            continue;
        }

        // SQL expression: qualify identifiers with the table alias
        $expr = preg_replace_callback(
            '/\b([A-Za-z_][A-Za-z0-9_]*)\b/',
            function ($m) use ($als) {
                $word = strtolower($m[1]);
                static $skip = [
                    'null', 'true', 'false', 'abs', 'avg', 'count', 'sum', 'min', 'max',
                    'if', 'ifnull', 'coalesce', 'nullif', 'round', 'floor', 'ceil', 'date',
                    'now', 'unix_timestamp', 'case', 'when', 'then', 'else', 'end', 'over',
                    'partition', 'by', 'rows', 'range', 'groups', 'current_row', 'unbounded',
                    'preceding', 'following'
                ];

                if (in_array($word, $skip, true)) {
                    return $m[1];
                }

                return $als . ".`" . $m[1] . "`";
            },
            $col
        );

        $selectCols[] = "($expr) AS `$key`";
    }
}

// Count total epochs
$sqlCount = "SELECT COUNT(*) AS N FROM ($unionSql) e";

// Retrieve all data
$sqlData = "SELECT " . implode(", ", $selectCols) . " FROM ($unionSql) e " . implode("\n", $joins) . " ORDER BY e.epoch";

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
    echo "[]";
    return;
}

// Number of raw rows aggregated into each output point
$pointsVal = isset($points) ? max(1, (int)$points) : 96;
$agg = max(1, (int)ceil($N / $pointsVal));

// In-memory temporal buffer
$fp = fopen("php://temp", "w+");

if (!$fp) {
    mysqli_close($conn);
    echo "[]";
    return;
}

// Per-series state
$last = [];
$acc = [];
$cnt = [];
$prevRaw = [];
$baseRaw = [];

foreach ($seriesKeys as $key) {
    $last[$key] = null;
    $acc[$key] = 0.0;
    $cnt[$key] = 0;
    $prevRaw[$key] = null;
    $baseRaw[$key] = null;
}

// Pre-fetch baseline/previous value before the requested period
for ($i = 0; $i < count($tab); $i++) {
    $tbl = preg_replace('/[^A-Za-z0-9_]/', '', $tab[$i]["table"]);
    $whereInit = "epoch < $sds";

    if (isset($tab[$i]["device"])) {
        $dev = mysqli_real_escape_string($conn, $tab[$i]["device"]);
        $whereInit .= " AND device = '$dev'";
    }

    $colsToFetch = [];
    foreach ($tab[$i]["cols"] as $col) {
        if (preg_match('/^[A-Za-z_][A-Za-z0-9_]*$/', $col)) {
            $colsToFetch[] = "`$col`";
        } else {
            $colsToFetch[] = "($col)";
        }
    }

    $sqlInit = "SELECT " . implode(", ", $colsToFetch) . " FROM `$tbl` WHERE $whereInit ORDER BY epoch DESC LIMIT 1";
    $resInit = @mysqli_query($conn, $sqlInit);

    if ($resInit && ($rowInit = mysqli_fetch_row($resInit))) {
        foreach ($tab[$i]["cols"] as $colIndex => $col) {
            $safeCol = preg_replace('/[^A-Za-z0-9_]/', '_', $col);
            $key = "t{$i}__{$safeCol}_{$colIndex}";

            if (isset($rowInit[$colIndex]) && $rowInit[$colIndex] !== null) {
                $valInit = (float)$rowInit[$colIndex];
                $deltaMode = $seriesDeltaModes[$key] ?? 0;

                if ($deltaMode === 1) {
                    $prevRaw[$key] = $valInit;
                } elseif ($deltaMode === 2) {
                    $baseRaw[$key] = $valInit;
                    $prevRaw[$key] = $valInit;
                    $last[$key] = 0.0;
                } else {
                    $last[$key] = $valInit;
                }
            }
        }
    }

    if ($resInit) {
        mysqli_free_result($resInit);
    }
}

// Execute main query
$res = mysqli_query($conn, $sqlData);

if (!$res) {
    fclose($fp);
    mysqli_close($conn);
    echo "[]";
    return;
}

$nagg = 0;
$lastEpochInBucket = null;

// Process rows
while ($row = mysqli_fetch_assoc($res)) {
    $epoch = (int)$row["epoch"];
    $lastEpochInBucket = $epoch;

    foreach ($seriesKeys as $key) {
        if ($row[$key] === null) {
            continue;
        }

        $currVal = (float)$row[$key];
        $deltaMode = $seriesDeltaModes[$key] ?? 0;

        if ($deltaMode === 1) {
            if ($prevRaw[$key] !== null) {
                $delta = $currVal - $prevRaw[$key];
                if ($delta < 0) {
                    $delta = $currVal;
                }
                $acc[$key] += $delta;
                $cnt[$key]++;
            }
            $prevRaw[$key] = $currVal;

        } elseif ($deltaMode === 2) {
            if ($baseRaw[$key] === null) {
                $baseRaw[$key] = $currVal;
            }
            $acc[$key] = $currVal - $baseRaw[$key];
            $cnt[$key] = 1;
            $prevRaw[$key] = $currVal;

        } else {
            $acc[$key] += $currVal;
            $cnt[$key]++;
        }
    }

    $nagg++;

    // Emit completed bucket
    if ($nagg >= $agg) {
        $dd = $lastEpochInBucket;
        $out = [];

        $out[] = sprintf(
            "'%s%s%s %s%s'",
            date("y", $dd),
            date("m", $dd),
            date("d", $dd),
            date("H", $dd),
            date("i", $dd)
        );

        foreach ($seriesKeys as $key) {
            $deltaMode = $seriesDeltaModes[$key] ?? 0;

            if ($cnt[$key] > 0) {
                if ($deltaMode === 1 || $deltaMode === 2) {
                    $m = $acc[$key];
                } else {
                    $m = $acc[$key] / $cnt[$key];
                }
                $last[$key] = $m;
            } else {
                $m = $last[$key];
                if ($deltaMode === 1 && $m === null) {
                    $m = 0.0;
                }
            }

            if ($m === null) {
                $out[] = "null";
            } else {
                $axis = $seriesAxes[$key] ?? 0;

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

            // Reset bucket state
            $acc[$key] = 0.0;
            $cnt[$key] = 0;
        }

        fprintf($fp, "[%s],\n", implode(", ", $out));
        $nagg = 0;
    }
}

// Emit final incomplete bucket
if ($nagg > 0 && $lastEpochInBucket !== null) {
    $dd = $lastEpochInBucket;
    $out = [];

    $out[] = sprintf(
        "'%s%s%s %s%s'",
        date("y", $dd),
        date("m", $dd),
        date("d", $dd),
        date("H", $dd),
        date("i", $dd)
    );

    foreach ($seriesKeys as $key) {
        $deltaMode = $seriesDeltaModes[$key] ?? 0;

        if ($cnt[$key] > 0) {
            if ($deltaMode === 1 || $deltaMode === 2) {
                $m = $acc[$key];
            } else {
                $m = $acc[$key] / $cnt[$key];
            }
            $last[$key] = $m;
        } else {
            $m = $last[$key];
            if ($deltaMode === 1 && $m === null) {
                $m = 0.0;
            }
        }

        if ($m === null) {
            $out[] = "null";
        } else {
            $axis = $seriesAxes[$key] ?? 0;

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

// Compute axis bounds
for ($axis = 0; $axis <= 1; $axis++) {
    if (
        isset($axisStats[$axis]) &&
        $axisStats[$axis]['min'] !== null &&
        $axisStats[$axis]['max'] !== null
    ) {
        $min = (float)$axisStats[$axis]['min'];
        $max = (float)$axisStats[$axis]['max'];

        if ($min == $max) {
            $min--;
            $max++;
        }

        $axisRange[$axis] = ['min' => $min, 'max' => $max];
    } else {
        $axisRange[$axis] = ['min' => 0, 'max' => 1];
    }
}

// Deliver response directly from memory
rewind($fp);
echo stream_get_contents($fp);
fclose($fp);

mysqli_free_result($res);
mysqli_close($conn);

// Compute axis bounds
for ($axis = 0; $axis <= 1; $axis++) {
    if (
        isset($axisStats[$axis]) &&
        $axisStats[$axis]['min'] !== null &&
        $axisStats[$axis]['max'] !== null
    ) {
        $min = (float)$axisStats[$axis]['min'];
        $max = (float)$axisStats[$axis]['max'];

        if ($min == $max) {
            $min--;
            $max++;
        }

        $axisRange[$axis] = ['min' => $min, 'max' => $max];
    } else {
        $axisRange[$axis] = ['min' => 0, 'max' => 1];
    }
}

// Deliver response directly from memory
rewind($fp);
echo stream_get_contents($fp);
fclose($fp);
