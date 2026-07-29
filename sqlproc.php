<?php
// sqlproc.php
// Data processing and caching engine.
//
// Delta modes:
//   delta = 0: average value within each aggregation bucket
//   delta = 1: increment occurring within each aggregation bucket
//   delta = 2: cumulative increment from the beginning of the requested period
//
// For delta = 2, the baseline is the last known value before the beginning
// of the requested period. If no previous value exists, the first available
// value in the period becomes the baseline.

// Load local.php dynamically from current working directory
$local_config = getcwd() . '/local.php';

if (file_exists($local_config)) {
    require_once $local_config;
} else {
    http_response_code(500);
    die(
        "Error: Unable to find local.php in current working directory (" .
        getcwd() .
        ")\n"
    );
}

// Ensure cache directory exists
if (isset($cache_dir) && !is_dir($cache_dir)) {
    @mkdir($cache_dir, 0775, true);
}

// Default query period
if (!isset($q)) {
    $q = date("Y\\dz");
}

// Parse date range according to period format
if (substr($q, 4, 1) == "d") {
    $aux = date_create_from_format(
        "Yz",
        substr($q, 0, 4) . substr($q, 5)
    );

    $ds = date_format($aux, "Y/m/d 00:00:00");
    $de = date_format($aux, "Y/m/d 23:59:59");

    $q = sprintf(
        "%04dd%03d",
        substr($q, 0, 4),
        substr($q, 5)
    );

} elseif (substr($q, 4, 1) == "w") {
    $aux = date_create();

    date_isodate_set(
        $aux,
        substr($q, 0, 4),
        substr($q, 5)
    );

    $ds = date_format($aux, "Y/m/d 00:00:00");

    date_add(
        $aux,
        date_interval_create_from_date_string("6 days")
    );

    $de = date_format($aux, "Y/m/d 23:59:59");

    $q = sprintf(
        "%04dw%02d",
        substr($q, 0, 4),
        substr($q, 5)
    );

} elseif (substr($q, 4, 1) == "m") {
    $aux = date_create_from_format(
        "Yn",
        substr($q, 0, 4) . substr($q, 5)
    );

    $ds = date_format($aux, "Y/m/01 00:00:00");
    $de = date_format($aux, "Y/m/t 23:59:59");

    $q = sprintf(
        "%04dm%02d",
        substr($q, 0, 4),
        substr($q, 5)
    );

} else {
    $aux = date_create();

    $ds = date_format($aux, "Y/m/d 00:00:00");
    $de = date_format($aux, "Y/m/d 23:59:59");

    $q = date("Y\\dz");

    $q = sprintf(
        "%04dd%03d",
        substr($q, 0, 4),
        substr($q, 5)
    );
}

// Determine active period key to check whether request is live or historical
if (substr($q, 4, 1) == "d") {
    $xq = date("Y\\dz");

    $vq = sprintf(
        "%04dd%03d",
        substr($xq, 0, 4),
        substr($xq, 5)
    );

} elseif (substr($q, 4, 1) == "w") {
    $xq = date("Y\\wW");

    $vq = sprintf(
        "%04dw%02d",
        substr($xq, 0, 4),
        substr($xq, 5)
    );

} elseif (substr($q, 4, 1) == "m") {
    $xq = date("Y\\mn");

    $vq = sprintf(
        "%04dm%02d",
        substr($xq, 0, 4),
        substr($xq, 5)
    );

} else {
    $vq = $q;
}

$is_live = ($q == $vq);

$sds = strtotime($ds);
$sde = strtotime($de);

$dds =
    "from day:" . date("z", $sds) .
    ":" . date("w", $sds) .
    " week:" . date("W", $sds) .
    " month:" . date("m", $sds);

$dde =
    "to day:" . date("z", $sde) .
    ":" . date("w", $sde) .
    " week:" . date("W", $sde) .
    " month:" . date("m", $sde);

// Default axis ranges
$axisRange = [
    0 => [
        'min' => 0,
        'max' => 1
    ],
    1 => [
        'min' => 0,
        'max' => 1
    ],
];

$axisStats = [
    0 => [
        'min' => null,
        'max' => null
    ],
    1 => [
        'min' => null,
        'max' => null
    ],
];

// Unique cache key generation
$cache_key = md5(
    json_encode($tab) .
    "|" .
    $points .
    "|" .
    $q
);

$cache_path = "$cache_dir/$q-$cache_key";
$cache_axis_path = "$cache_path.axis.json";

// Serve historical requests directly from disk cache if present
if (
    !$is_live &&
    file_exists($cache_path) &&
    file_exists($cache_axis_path)
) {
    $axisJson = file_get_contents($cache_axis_path);
    $axisTmp = json_decode($axisJson, true);

    if (is_array($axisTmp)) {
        $axisRange = $axisTmp;
    }

    echo file_get_contents($cache_path);
    return;
}

// Connect to database
$conn = mysqli_connect(
    $db_host,
    $db_user,
    $db_pass,
    $db_name
);

if (!$conn) {
    echo "[]";
    return;
}

@mysqli_set_charset($conn, "utf8mb4");

// Build UNION query containing every epoch present in any configured table
$unionParts = [];

for ($i = 0; $i < count($tab); $i++) {
    $tbl = preg_replace(
        '/[^A-Za-z0-9_]/',
        '',
        $tab[$i]["table"]
    );

    $where = "epoch BETWEEN $sds AND $sde";

    if (isset($tab[$i]["device"])) {
        $dev = mysqli_real_escape_string(
            $conn,
            $tab[$i]["device"]
        );

        $where .= " AND device = '$dev'";
    }

    $unionParts[] =
        "SELECT epoch " .
        "FROM `$tbl` " .
        "WHERE $where";
}

$unionSql = implode(
    "\n  UNION\n  ",
    $unionParts
);

// Build selected columns and joins
$selectCols = ["e.epoch"];
$joins = [];

$seriesKeys = [];
$seriesAxes = [];
$seriesDeltaModes = [];

for ($i = 0; $i < count($tab); $i++) {
    $tbl = preg_replace(
        '/[^A-Za-z0-9_]/',
        '',
        $tab[$i]["table"]
    );

    $als = "t$i";
    $joinCond = "$als.epoch = e.epoch";

    if (isset($tab[$i]["device"])) {
        $dev = mysqli_real_escape_string(
            $conn,
            $tab[$i]["device"]
        );

        $joinCond .= " AND $als.device = '$dev'";
    }

    $joins[] =
        "LEFT JOIN `$tbl` $als " .
        "ON $joinCond";

    // Read delta as a real mode, not as a boolean
    $deltaMode = isset($tab[$i]["delta"])
        ? (int)$tab[$i]["delta"]
        : 0;

    // Accept only supported modes
    if (
        $deltaMode !== 0 &&
        $deltaMode !== 1 &&
        $deltaMode !== 2
    ) {
        $deltaMode = 0;
    }

    foreach ($tab[$i]["cols"] as $colIndex => $col) {
        $safeCol = preg_replace(
            '/[^A-Za-z0-9_]/',
            '_',
            $col
        );

        $key = "{$als}__{$safeCol}_{$colIndex}";
        $seriesIndex = count($seriesKeys);

        $seriesKeys[] = $key;
        $seriesDeltaModes[$key] = $deltaMode;

        $axis = 0;

        if (
            isset(
                $seriesOpt[$seriesIndex]['targetAxisIndex']
            )
        ) {
            $axis = (int)$seriesOpt[
                $seriesIndex
            ]['targetAxisIndex'];
        }

        $seriesAxes[$key] = $axis;

        // Simple column name
        if (
            preg_match(
                '/^[A-Za-z_][A-Za-z0-9_]*$/',
                $col
            )
        ) {
            $selectCols[] =
                "$als.`$col` AS `$key`";

            continue;
        }

        // SQL expression: qualify identifiers with the table alias
        $expr = preg_replace_callback(
            '/\b([A-Za-z_][A-Za-z0-9_]*)\b/',
            function ($m) use ($als) {
                $word = strtolower($m[1]);

                static $skip = [
                    'null',
                    'true',
                    'false',
                    'abs',
                    'avg',
                    'count',
                    'sum',
                    'min',
                    'max',
                    'if',
                    'ifnull',
                    'coalesce',
                    'nullif',
                    'round',
                    'floor',
                    'ceil',
                    'date',
                    'now',
                    'unix_timestamp',
                    'case',
                    'when',
                    'then',
                    'else',
                    'end',
                    'over',
                    'partition',
                    'by',
                    'rows',
                    'range',
                    'groups',
                    'current_row',
                    'unbounded',
                    'preceding',
                    'following'
                ];

                if (in_array($word, $skip, true)) {
                    return $m[1];
                }

                return
                    $als .
                    ".`" .
                    $m[1] .
                    "`";
            },
            $col
        );

        $selectCols[] =
            "($expr) AS `$key`";
    }
}

// Count epochs
$sqlCount = "
SELECT COUNT(*) AS N
FROM (
  $unionSql
) e
";

// Retrieve all data
$sqlData = "
SELECT " . implode(", ", $selectCols) . "
FROM (
  $unionSql
) e
" . implode("\n", $joins) . "
ORDER BY e.epoch
";

// Count number of input rows
$resN = mysqli_query($conn, $sqlCount);

if (!$resN) {
    mysqli_close($conn);

    if (!$is_live) {
        file_put_contents(
            $cache_axis_path,
            json_encode($axisRange)
        );
    }

    echo "[]";
    return;
}

$rowN = mysqli_fetch_assoc($resN);
mysqli_free_result($resN);

$N = (int)$rowN["N"];

if ($N <= 0) {
    mysqli_close($conn);

    if (!$is_live) {
        file_put_contents(
            $cache_path,
            "[]"
        );

        file_put_contents(
            $cache_axis_path,
            json_encode($axisRange)
        );
    }

    echo "[]";
    return;
}

// Number of raw rows aggregated into each output point
$agg = max(
    1,
    (int)ceil($N / $points)
);

// Live queries stay in RAM.
// Historical queries are written directly to disk cache.
$fp = $is_live
    ? fopen("php://temp", "w+")
    : fopen($cache_path, "w");

if (!$fp) {
    mysqli_close($conn);

    if (!$is_live) {
        file_put_contents(
            $cache_axis_path,
            json_encode($axisRange)
        );
    }

    echo "[]";
    return;
}

// Per-series state
$last = [];
$acc = [];
$cnt = [];

// Previous raw value, used by delta = 1
$prevRaw = [];

// Baseline value at the beginning of the requested period,
// used by delta = 2
$baseRaw = [];

foreach ($seriesKeys as $key) {
    $last[$key] = null;
    $acc[$key] = 0.0;
    $cnt[$key] = 0;
    $prevRaw[$key] = null;
    $baseRaw[$key] = null;
}

// Pre-fetch the last known value before the requested period.
//
// For delta = 1:
//   it permits the first increment in the period to be calculated.
//
// For delta = 2:
//   it becomes the baseline representing the beginning of the period.
//
// For normal metrics:
//   it provides sample-and-hold continuity.
for ($i = 0; $i < count($tab); $i++) {
    $tbl = preg_replace(
        '/[^A-Za-z0-9_]/',
        '',
        $tab[$i]["table"]
    );

    $whereInit = "epoch < $sds";

    if (isset($tab[$i]["device"])) {
        $dev = mysqli_real_escape_string(
            $conn,
            $tab[$i]["device"]
        );

        $whereInit .= " AND device = '$dev'";
    }

    $colsToFetch = [];

    foreach ($tab[$i]["cols"] as $col) {
        if (
            preg_match(
                '/^[A-Za-z_][A-Za-z0-9_]*$/',
                $col
            )
        ) {
            $colsToFetch[] = "`$col`";
        } else {
            $colsToFetch[] = "($col)";
        }
    }

    $sqlInit =
        "SELECT " .
        implode(", ", $colsToFetch) .
        " FROM `$tbl`" .
        " WHERE $whereInit" .
        " ORDER BY epoch DESC" .
        " LIMIT 1";

    $resInit = @mysqli_query(
        $conn,
        $sqlInit
    );

    if (
        $resInit &&
        ($rowInit = mysqli_fetch_row($resInit))
    ) {
        foreach (
            $tab[$i]["cols"] as $colIndex => $col
        ) {
            $safeCol = preg_replace(
                '/[^A-Za-z0-9_]/',
                '_',
                $col
            );

            $key =
                "t{$i}__{$safeCol}_{$colIndex}";

            if (
                isset($rowInit[$colIndex]) &&
                $rowInit[$colIndex] !== null
            ) {
                $valInit =
                    (float)$rowInit[$colIndex];

                $deltaMode =
                    $seriesDeltaModes[$key] ?? 0;

                if ($deltaMode === 1) {
                    // Previous counter value for first delta
                    $prevRaw[$key] = $valInit;

                } elseif ($deltaMode === 2) {
                    // Baseline at beginning of requested period
                    $baseRaw[$key] = $valInit;
                    $prevRaw[$key] = $valInit;

                    // Cumulative difference starts at zero
                    $last[$key] = 0.0;

                } else {
                    // Forward-fill baseline for normal metrics
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
$res = mysqli_query(
    $conn,
    $sqlData
);

if (!$res) {
    fclose($fp);

    if (!$is_live) {
        @unlink($cache_path);
        @unlink($cache_axis_path);

        file_put_contents(
            $cache_axis_path,
            json_encode($axisRange)
        );
    }

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

        $deltaMode =
            $seriesDeltaModes[$key] ?? 0;

        if ($deltaMode === 1) {
            // Difference from previous raw value.
            // The differences are summed inside the current bucket.
            if ($prevRaw[$key] !== null) {
                $delta =
                    $currVal -
                    $prevRaw[$key];

                // Preserve previous reset handling
                if ($delta < 0) {
                    $delta = $currVal;
                }

                $acc[$key] += $delta;
                $cnt[$key]++;
            }

            $prevRaw[$key] = $currVal;

        } elseif ($deltaMode === 2) {
            // If no value existed before the requested period,
            // use the first available value as baseline.
            if ($baseRaw[$key] === null) {
                $baseRaw[$key] = $currVal;
            }

            // Keep the cumulative difference corresponding to
            // the most recent value in the current bucket.
            $acc[$key] =
                $currVal -
                $baseRaw[$key];

            $cnt[$key] = 1;
            $prevRaw[$key] = $currVal;

        } else {
            // Standard metric: arithmetic mean within the bucket
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
            $deltaMode =
                $seriesDeltaModes[$key] ?? 0;

            if ($cnt[$key] > 0) {
                if (
                    $deltaMode === 1 ||
                    $deltaMode === 2
                ) {
                    $m = $acc[$key];
                } else {
                    $m =
                        $acc[$key] /
                        $cnt[$key];
                }

                $last[$key] = $m;

            } else {
                if ($deltaMode === 1) {
                    // No increment in this bucket
                    $m = 0.0;

                } elseif ($deltaMode === 2) {
                    // Keep cumulative value reached so far
                    $m = $last[$key];

                } else {
                    // Sample-and-hold forward fill
                    $m = $last[$key];
                }
            }

            if ($m === null) {
                $out[] = "null";

            } else {
                $axis =
                    $seriesAxes[$key] ?? 0;

                if (!isset($axisStats[$axis])) {
                    $axisStats[$axis] = [
                        'min' => null,
                        'max' => null
                    ];
                }

                if (
                    $axisStats[$axis]['min'] === null ||
                    $m < $axisStats[$axis]['min']
                ) {
                    $axisStats[$axis]['min'] = $m;
                }

                if (
                    $axisStats[$axis]['max'] === null ||
                    $m > $axisStats[$axis]['max']
                ) {
                    $axisStats[$axis]['max'] = $m;
                }

                $out[] = sprintf(
                    "%9.5f",
                    $m
                );
            }

            // Reset bucket state
            $acc[$key] = 0.0;
            $cnt[$key] = 0;
        }

        fprintf(
            $fp,
            "[%s],\n",
            implode(", ", $out)
        );

        $nagg = 0;
    }
}

// Emit final incomplete bucket
if (
    $nagg > 0 &&
    $lastEpochInBucket !== null
) {
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
        $deltaMode =
            $seriesDeltaModes[$key] ?? 0;

        if ($cnt[$key] > 0) {
            if (
                $deltaMode === 1 ||
                $deltaMode === 2
            ) {
                $m = $acc[$key];
            } else {
                $m =
                    $acc[$key] /
                    $cnt[$key];
            }

            $last[$key] = $m;

        } else {
            if ($deltaMode === 1) {
                $m = 0.0;

            } elseif ($deltaMode === 2) {
                $m = $last[$key];

            } else {
                $m = $last[$key];
            }
        }

        if ($m === null) {
            $out[] = "null";

        } else {
            $axis =
                $seriesAxes[$key] ?? 0;

            if (!isset($axisStats[$axis])) {
                $axisStats[$axis] = [
                    'min' => null,
                    'max' => null
                ];
            }

            if (
                $axisStats[$axis]['min'] === null ||
                $m < $axisStats[$axis]['min']
            ) {
                $axisStats[$axis]['min'] = $m;
            }

            if (
                $axisStats[$axis]['max'] === null ||
                $m > $axisStats[$axis]['max']
            ) {
                $axisStats[$axis]['max'] = $m;
            }

            $out[] = sprintf(
                "%9.5f",
                $m
            );
        }
    }

    fprintf(
        $fp,
        "[%s],\n",
        implode(", ", $out)
    );
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
        $min =
            (float)$axisStats[$axis]['min'];

        $max =
            (float)$axisStats[$axis]['max'];

        if ($min == $max) {
            $min--;
            $max++;
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

// Deliver response
if ($is_live) {
    rewind($fp);

    echo stream_get_contents($fp);

    fclose($fp);

} else {
    fclose($fp);

    file_put_contents(
        $cache_axis_path,
        json_encode($axisRange)
    );

    echo file_get_contents($cache_path);
}
