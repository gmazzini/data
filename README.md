# data

`data` is a small high-performance web graphing system written in C.

It replaces the previous PHP data-processing and graph-rendering chain with a single CGI executable that reads data from MariaDB/MySQL, aggregates it, and generates the complete HTML page and SVG graph directly.

The current source is `data.c`, version 2.06.

## Architecture

The complete request path is:

```text
Browser
  |
  v
Apache
  |
  v
data.cgi
  |
  +--> data.conf
  |
  +--> MariaDB/MySQL
  |
  v
HTML + SVG
  |
  v
Browser
```

There is no PHP dependency and no JavaScript graphing library. The graph is rendered as SVG generated directly by the C program.

A single compiled `data.cgi` can be shared by multiple sites. Each site keeps its own `data.conf`, so different sites can use different databases, tables, expressions, labels, axes and aggregation modes without recompiling the program.

Example:

```text
/home/tools/mcp/work/data/data.cgi

/home/www/energycc/
  data.cgi -> /home/tools/mcp/work/data/data.cgi
  data.conf

/home/www/othersite/
  data.cgi -> /home/tools/mcp/work/data/data.cgi
  data.conf
```

When executed as a CGI, `data.cgi` locates `data.conf` in the directory of the requested CGI path. This allows the same executable to be used through symbolic links in multiple site directories.

## Features

- written in C using the MariaDB/MySQL client library
- CGI execution through Apache
- direct HTML and SVG generation
- no PHP
- no JavaScript dependency
- no external graphing service
- DAY, WEEK and MONTH navigation
- previous, next and TODAY navigation
- automatic selection of the latest period available in the database
- Europe/Rome timezone handling
- two independent Y axes
- configurable axis minimum at zero
- configurable number of graph points
- multiple tables and multiple data series
- optional device filter for each source
- SQL expressions as data series
- value, delta and cumulative-total modes
- streaming result processing with `mysql_use_result()`
- automatic Y-axis min/max calculation
- 24 X-axis labels on desktop and 12 on screens up to 600 px

## Requirements

The build requires a C compiler and the MariaDB/MySQL client development files.

On the current Debian installation the build uses `mariadb_config` to obtain the correct compiler and linker flags.

Apache must have CGI support enabled. The current system uses `mod_cgi`.

## Build

From the source directory:

```sh
make
```

This produces:

```text
data.cgi
```

The build currently uses:

```text
-O3 -std=gnu89 -Wall -Wextra
```

GNU89 is used because the project follows C89-style coding while intentionally using `//` comments.

To remove generated executables:

```sh
make clean
```

## Site installation

A site needs only two entries in its document root:

```text
data.cgi
data.conf
```

Normally `data.cgi` is a symbolic link to the shared executable. For example:

```sh
ln -s /home/tools/mcp/work/data/data.cgi /home/www/energycc/data.cgi
```

The configuration remains local to the site:

```text
/home/www/energycc/data.conf
```

This allows a single executable to serve many completely different data sources.

## Apache configuration

A minimal HTTPS VirtualHost directory configuration is:

```apache
DocumentRoot /home/www/energycc
DirectoryIndex data.cgi

<Directory /home/www/energycc>
  Options +ExecCGI +FollowSymLinks
  AddHandler cgi-script .cgi
  Require all granted
</Directory>

<Files "data.conf">
  Require all denied
</Files>
```

`DirectoryIndex data.cgi` makes the site root open the graph directly:

```text
https://energycc.example.org/
```

instead of requiring:

```text
https://energycc.example.org/data.cgi
```

`data.conf` contains database credentials and therefore must not be downloadable through Apache. The `<Files "data.conf">` rule returns HTTP 403 for direct web access while still allowing `data.cgi` to read the file locally.

After changing Apache configuration:

```sh
sudo apache2ctl configtest && sudo systemctl reload apache2
```

## Configuration file

Each non-empty configuration line has the form:

```text
key=value
```

Lines beginning with `#` are ignored.

A typical configuration is:

```text
db_host=127.0.0.1
db_user=energy
db_pass=<password>
db_name=energy
points=1000

title=Potenza e Energia CC
axis_left=Potenza (W)
axis_right=Energia (Wh)
axis_left_format=%.0f
axis_right_format=%.1f

y_left_min0=0
y_right_min0=1

series=vi_cc||v1*i1|L1|0|0
series=vi_cc||v2*i2|L2|0|0
series=vi_cc||v3*i3|L3|0|0
series=energy_cc||e1+e2+e3|E|1|2
```

### Database options

#### `db_host`

MariaDB/MySQL server address.

Default:

```text
127.0.0.1
```

#### `db_port`

MariaDB/MySQL TCP port.

Default:

```text
3306
```

#### `db_user`

Database username. This option is required.

#### `db_pass`

Database password.

#### `db_name`

Database name. This option is required.

## Graph options

### `points`

Target maximum number of aggregated graph points.

Default:

```text
1000
```

The aggregation intentionally preserves the behavior of the previous processing system. If the selected period contains `N` raw epochs, the aggregation size is:

```text
ceil(N / points)
```

One graph point is emitted for each aggregation group.

This means that `points=1000` is a target maximum, not a request for exactly 1000 points. For example, 1030 raw epochs produce an aggregation size of 2 and therefore approximately 515 plotted points.

If the number of raw epochs is smaller than `points`, no aggregation by count is required and the available epochs are used directly.

### `title`

Main graph title.

Default:

```text
Data
```

### `axis_left`

Title of Y axis 0, displayed on the left.

### `axis_right`

Title of Y axis 1, displayed on the right.

### `axis_left_format`

Required when at least one series uses axis 0. It is the C floating-point format used to display numeric labels on the left Y axis.

Examples:

```text
axis_left_format=%.1f
axis_left_format=%3.1f
axis_left_format=%.2g
```

### `axis_right_format`

Required when at least one series uses axis 1. It is the C floating-point format used to display numeric labels on the right Y axis.

Examples:

```text
axis_right_format=%.0f
axis_right_format=%6.2f
axis_right_format=%.3g
```

There is deliberately no default axis format. A format must be explicitly configured for every axis that is actually used.

For safety, the parser accepts only one floating-point conversion with optional numeric width and precision. Accepted conversion letters are:

```text
f F e E g G
```

Examples of accepted formats include `%f`, `%.1f`, `%3.1f`, `%6.2e` and `%.3g`. Width is limited to 20 and precision to 10.

Formats such as `%s`, `%n`, `%lf`, `*`, flags, text prefixes/suffixes or multiple conversions are rejected.

`%f` is the correct form for values passed to `printf()` as `double`; `%lf` is not used here.

### `y_left_min0`

If non-zero, the minimum of the left Y axis is forced to zero when all values are positive.

Example:

```text
y_left_min0=1
```

### `y_right_min0`

Equivalent option for the right Y axis.

Example:

```text
y_right_min0=1
```

## Series configuration

Each graph series is configured with one `series=` line.

The format is:

```text
series=table|device|expression|label|axis|delta
```

There are exactly six fields separated by `|`.

### Field 1: `table`

MariaDB/MySQL table containing the source data.

The table name may contain only letters, digits and underscore.

The table must contain an `epoch` column because all sources are aligned by epoch.

Example:

```text
vi_cc
```

### Field 2: `device`

Optional device filter.

If empty, all rows from the table are eligible:

```text
series=vi_cc||v1*i1|L1|0|0
```

If specified, the query adds:

```sql
device='<value>'
```

Example:

```text
series=sensors|boiler|temperature|Boiler|0|0
```

### Field 3: `expression`

SQL expression evaluated for the series.

It may be a single column:

```text
power
```

or an expression:

```text
v1*i1
```

```text
e1+e2+e3
```

The expression is evaluated by MariaDB/MySQL. Identifiers are automatically qualified with the internal table alias when the main query is built.

### Field 4: `label`

Text displayed in the graph legend.

Examples:

```text
L1
L2
L3
E
```

### Field 5: `axis`

Selects the Y axis:

```text
0 = left axis
1 = right axis
```

Example:

```text
series=vi_cc||v1*i1|L1|0|0
```

places `L1` on the left axis.

### Field 6: `delta`

Selects how values are processed.

#### `delta=0` - normal value

Values inside each aggregation group are averaged.

This is appropriate for measurements such as power, temperature, voltage or current.

Example:

```text
series=vi_cc||v1*i1|L1|0|0
```

#### `delta=1` - incremental delta

The program calculates the difference between each value and the preceding raw value and sums those differences inside the aggregation group.

A value immediately before the selected period is read when available, so the first delta of the period can be calculated correctly.

If a counter decreases, it is treated as a reset and the current value is used as the new increment.

This mode is suitable for counters where each graph bucket should show the increment occurring inside that bucket.

#### `delta=2` - cumulative total from period start

The program reads the latest value immediately before the selected period and uses it as the baseline. Each plotted value represents:

```text
current value - period baseline
```

The result therefore grows cumulatively through the selected day, week or month.

Example:

```text
series=energy_cc||e1+e2+e3|E|1|2
```

This is suitable for cumulative energy counters when the graph should show energy accumulated since the beginning of the selected period.

## Multiple sources

Different series can come from different tables and can also use different device filters.

For the requested time range, the program builds a common ordered epoch set from all configured sources and joins each source to that timeline.

This allows series sampled at different epochs to appear together on the same graph.

For periods with missing values, the processing logic preserves the last usable aggregated value where appropriate, matching the behavior of the original data-processing implementation.

## Time periods and URL parameter

The CGI uses the `q` query parameter to select a period.

### Day

Format:

```text
YYYYdDDD
```

`DDD` is the zero-based day number within the year.

Example:

```text
?q=2026d228
```

### ISO week

Format:

```text
YYYYwWW
```

Example:

```text
?q=2026w34
```

### Month

Format:

```text
YYYYmMM
```

Example:

```text
?q=2026m08
```

If `q` is omitted, the program starts from the latest available database epoch and selects its day.

Navigation is limited so that NEXT cannot move past the latest period represented in the configured database sources.

The TODAY button uses the current real date rather than the latest database date.

All period calculations use:

```text
Europe/Rome
```

including daylight-saving-time transitions.

## Data processing

For each request the program performs these main operations:

1. reads `data.conf`;
2. connects once to MariaDB/MySQL;
3. finds the latest epoch available across configured sources;
4. parses or determines the selected day, week or month;
5. counts the common raw epochs for the selected period;
6. reads the value immediately before the period for series that require previous-state information;
7. executes the main ordered data query;
8. consumes the result as a stream using `mysql_use_result()`;
9. aggregates values in memory;
10. calculates independent Y-axis ranges;
11. generates the complete HTML and SVG response.

The result set is processed sequentially and is not copied into a large PHP-style object structure.

## SVG rendering

The graph is generated directly by `data.cgi` as SVG.

The current renderer uses:

- two Y axes;
- automatic Y ranges;
- horizontal grid lines;
- series legend;
- responsive SVG scaling;
- 24 X-axis time labels on desktop;
- 12 X-axis time labels on displays up to 600 px wide.

No browser-side graph calculation is required.

## Recommended database indexes

For good performance, tables should normally have an index on `epoch`.

For sources using a `device` filter, an index covering both device and epoch is recommended, typically:

```sql
INDEX(device, epoch)
```

The exact index design should still be checked against the real table structure and query plan.

## Limits

The current compiled limits are:

```text
maximum series: 64
maximum sources: 64
```

A source is identified by the combination of table, device and delta mode.


## Google Drive kWh importer

`kwh_proc` imports quarter-hour kWh files from a Google Drive folder into a selected MySQL table and deletes each Drive file only after its database import succeeds.

Current source version:

```text
kwh_proc.c  1.10
```

Usage:

```sh
./kwh_proc GOOGLE_FOLDER_ID MYSQL_TABLE_NAME
```

Example:

```sh
./kwh_proc 1AbCdEfGhIjKlMnOpQrStUvWxYz energy_kwh
```

The destination table must contain at least:

```sql
epoch
kwh
```

and `epoch` should normally be unique or a primary key because imports use `ON DUPLICATE KEY UPDATE`.

The program reads its Google OAuth access token from:

```text
/home/www/data/google_access_token
```

and its MySQL credentials from:

```text
/home/tools/setup_energy.c
```

### Processing flow

For each execution `kwh_proc`:

1. validates the Google Drive folder ID and MySQL table name;
2. sets the timezone to `Europe/Rome`;
3. opens one MySQL connection and one reusable CURL handle;
4. lists all files in the requested Drive folder, following `nextPageToken` pagination up to 500 files;
5. exports Google Sheets as CSV or downloads ordinary files directly;
6. parses quarter-hour values from the CSV;
7. converts each interval to a local epoch, preserving the existing October daylight-saving transition handling used by the original importer;
8. writes the values to MySQL inside a transaction using multi-row INSERT batches;
9. commits the transaction;
10. deletes the Drive file only after a successful database import.

If download, parsing, database insertion, commit or Drive deletion fails for any file, that failure is reported and the program exits with a non-zero status after processing the remaining files.

### CSV handling

The importer accepts both semicolon and comma separated input and handles quoted fields. Numeric values are cleaned before conversion and decimal commas are converted to decimal points.

The first CSV column is expected to contain dates in:

```text
DD/MM/YYYY
```

The remaining columns represent 15-minute values, up to 96 intervals per CSV row. The existing two-line October DST representation is preserved: the second line for a repeated date is interpreted as standard-time data for the repeated hour.

### Improvements in version 1.10

Compared with version 1.9, version 1.10 adds:

- Drive pagination instead of silently stopping at the first 100 files;
- geometric HTTP response-buffer growth instead of reallocating for every received chunk;
- one reusable CURL handle and response buffer;
- strict validation of the Drive folder ID and SQL table name;
- checked numeric conversion with `strtod()` instead of `atof()`;
- batched multi-row MySQL INSERT statements instead of one SQL query per quarter hour;
- explicit transaction, commit and rollback error handling;
- deletion from Drive only after a committed import;
- non-zero process exit status when one or more files fail;
- bounded JSON and URL handling.


## Annual kWh and PUN analysis

`kwh_analisys` analyzes one calendar year of fiscal kWh measurements, cumulative energy meters and 15-minute PUN prices, then writes the annual results to Google Sheets.

Current source version:

```text
kwh_analisys.c  1.26
```

Usage:

```sh
./kwh_analisys <kwh_so|kwh_cc> <year>
```

Examples:

```sh
./kwh_analisys kwh_so 2026
./kwh_analisys kwh_cc 2026
```

The selected measurement type determines the cumulative meter table and the target Google Sheet tabs:

```text
kwh_so -> energy_so, h_so, m_so, d_so
kwh_cc -> energy_cc, h_cc, m_cc, d_cc
```

The program calculates:

- hourly annual kWh totals;
- monthly F1/F2/F3 and total kWh;
- monthly and annual cumulative-meter energy differences;
- monthly average PUN by ARERA band;
- cost using the real 15-minute PUN (`cp`);
- cost using the corresponding monthly band-average PUN (`cF`);
- number of days where `cp > cF`.

ARERA band classification uses Europe/Rome local time, Sundays and Italian national holidays including Easter Monday.

### PUN lookup and missing prices

The PUN lookup is allocated dynamically from the exact length of the requested calendar year. A separate validity bitmap distinguishes a real price of `0.0` from a missing PUN record.

An energy interval without a corresponding PUN price is skipped from the cost comparison and counted explicitly. The final diagnostic line reports:

```text
PUN: <loaded> slots loaded, <used> cost intervals used, <missing> intervals skipped for missing PUN
```

This avoids silently treating missing market data as zero-cost energy.

### Database processing

The annual fiscal table, `pun_15m` and cumulative energy table are each streamed with `mysql_use_result()`. Monthly cumulative-meter boundary values are collected during the single annual energy-table pass, replacing the previous thirteen independent boundary queries.

Energy deltas are accepted only when consecutive meter samples are between 60 and 1200 seconds apart and the positive delta is below 100 kWh, preserving the previous calculation rules.

### Google Sheets update

The hourly, monthly and comparison ranges are now written with one Google Sheets `values:batchUpdate` request instead of three independent PUT requests. The comparison tab is written only for years from 2025 onward, matching the previous layout.

`kwh_analisys` requires the MariaDB/MySQL client library, libcurl and libm.

## Project files

The source directory contains only:

```text
data.c      C source
data.cgi    compiled CGI executable
Makefile    build rules
README.md   this documentation
```

Site-specific `data.conf` files are deliberately stored in the corresponding site directories and are not kept in the source directory.

## GME price importer

The project also contains `gme_mysql.c`, a command-line importer for Italian electricity market prices published by GME (Gestore dei Mercati Energetici).

Current version:

```text
// Gianluca Mazzini @2026- Version 1.13
```

The program authenticates against the GME API, requests MGP zonal prices at 15-minute granularity for one calendar day, extracts the `PUN` zone values from the returned ZIP archive and stores them in MariaDB/MySQL table `pun_15m`.

The processing path is:

```text
GME Auth API
   |
   v
JWT token
   |
   v
GME RequestData API
   |
   v
Base64 response
   |
   v
ZIP archive in memory
   |
   v
JSON data
   |
   v
PUN prices
   |
   v
MariaDB/MySQL pun_15m
```

### Build requirements

In addition to the MariaDB/MySQL client development files already required by `data.cgi`, `gme_mysql` requires:

```text
libcurl
libzip
pkg-config
```

The Makefile obtains the required compiler and linker flags automatically with:

```text
mariadb_config
pkg-config --cflags --libs libcurl libzip
```

Running:

```sh
make
```

builds both:

```text
data.cgi
gme_mysql
```

### Usage

```sh
./gme_mysql LOGIN PASSWORD YYYYMMDD
```

Example:

```sh
./gme_mysql mylogin mypassword 20260817
```

The date must be a valid calendar date in `YYYYMMDD` format.

The program uses timezone:

```text
Europe/Rome
```

and stores one price every 900 seconds starting from local midnight of the requested date.

### Expected daily record counts

The GME request uses 15-minute granularity (`PT15`). Depending on daylight-saving-time transitions, a valid day contains:

```text
92 records   23-hour day
96 records   normal 24-hour day
100 records  25-hour day
```

Any other number of PUN records is treated as an error and no data is written.

### Database destination

Database credentials are currently taken from:

```text
/home/tools/setup_energy.c
```

using the existing macros:

```text
USER
PASSWORD
DB
```

The destination table is:

```text
pun_15m
```

and must provide at least:

```text
epoch
c
```

The importer executes an upsert, so an existing row with the same key is updated instead of duplicated.

Conceptually:

```sql
INSERT INTO pun_15m (epoch,c)
VALUES (...), (...), ...
ON DUPLICATE KEY UPDATE c=VALUES(c)
```

For best behavior, `epoch` should therefore be a primary key or unique key.

### Runtime behavior

A successful execution prints only:

```text
COUNT=N
```

where `N` is normally 92, 96 or 100.

Errors are written to standard error and the process exits with a non-zero status.

### Implementation notes

Version 1.13 improves the previous 1.12 implementation in several areas:

- reuses one CURL easy handle for authentication and data retrieval;
- reuses a dynamically growing HTTP response buffer instead of reallocating it for every received chunk;
- checks HTTP response status codes in addition to CURL transport errors;
- validates the requested calendar date instead of accepting normalized invalid dates;
- validates token and `contentResponse` extraction before dereferencing JSON positions;
- decodes Base64 with bounded unsigned arithmetic;
- verifies ZIP entry size and complete ZIP reads;
- selects the first regular ZIP entry rather than blindly assuming entry zero is the data file;
- validates numeric price conversion with `strtod()`;
- performs one multi-row MySQL upsert instead of approximately 92/96/100 separate SQL queries;
- uses a single cleanup path so every allocated resource is released consistently;
- avoids keeping the large HTTP response buffer after the ZIP payload has been decoded.

The JSON parsing remains intentionally lightweight because the program needs only three tightly controlled fields from the GME responses: `token`, `contentResponse`, and the PUN `Price` values.

## Project files

The source directory currently contains:

```text
data.c       web graph CGI source
data.cgi     compiled web graph CGI
gme_mysql.c  GME PUN 15-minute price importer source
gme_mysql    compiled GME importer
Makefile     build rules
README.md    project documentation
```


## PUN processor

`pun.c` is the post-processing program for the 15-minute PUN values stored by `gme_mysql` in the `pun_15m` table.

Current version:

```text
1.21
```

It accepts one date:

```sh
./pun YYYYMMDD
```

Example:

```sh
./pun 20260810
```

The requested date determines both the month to recalculate and the day for which the cheapest continuous three-hour window is calculated.

### Monthly calculations

The program reads the complete requested month from `pun_15m` and calculates:

- `F0`: average price over all intervals;
- `F1`: average price in ARERA F1 intervals;
- `F2`: average price in ARERA F2 intervals;
- `F3`: average price in ARERA F3 intervals;
- monthly minimum price;
- monthly maximum price;
- `BESTDAY`: day with the lowest daily average;
- `WORSTDAY`: day with the highest daily average;
- `MINDAY`: day containing the longest consecutive run at the monthly minimum price.

Italian national holidays and Easter Monday are included when assigning F1/F2/F3 bands.

All time calculations use:

```text
Europe/Rome
```

The timezone is configured before any `mktime()` or local-time conversion.

### Three-hour minimum window

For the requested day the program finds the cheapest continuous three-hour interval.

A three-hour interval contains:

```text
12 x 15-minute records
```

Candidate records must also be contiguous in epoch time, exactly 900 seconds apart. Missing database records therefore cannot accidentally create a false three-hour window.

The daily buffers support up to 100 quarter-hour records. This is important on the autumn daylight-saving transition day, which contains 25 hours and therefore 100 15-minute records. Spring transition days contain 92 records and normal days contain 96.

If the requested day does not contain a complete continuous three-hour window, the program stops with an error instead of writing a zero result.

### Google Sheets output

The program updates the spreadsheet configured by `SPREADSHEET_ID` and uses these tabs:

```text
pun
h
```

The OAuth2 access token is read from:

```text
/home/www/data/google_access_token
```

The `pun` tab receives the monthly summary. The `h` tab receives the requested date, start time of the cheapest three-hour window and its average price.

Sheet numeric IDs are resolved dynamically from their tab names.

Google operations are grouped to minimize HTTP calls:

1. one metadata request resolves both sheet IDs;
2. one `batchUpdate` sorts both tabs ascending;
3. one values `batchUpdate` writes both target rows;
4. one `batchUpdate` sorts both tabs descending again.

The same CURL handle and HTTP response buffer are reused for all requests.

If an error occurs after the tabs have been sorted ascending, the cleanup path attempts to restore descending order before terminating.

### Database access

The program reads:

```sql
SELECT epoch,c
FROM pun_15m
WHERE epoch BETWEEN <month-start> AND <month-end>
ORDER BY epoch
```

The MySQL result is streamed with `mysql_use_result()` rather than being copied into a complete result set in memory.

Daily and monthly aggregates are calculated during that single pass through the database data.

### Build

`pun` is built together with the other project programs:

```sh
make
```

The complete build now produces:

```text
data.cgi
gme_mysql
pun
```

`pun` requires the MariaDB/MySQL client library and libcurl.

The project also contains:

```text
kwh_analisys.c  annual kWh/PUN analysis source
kwh_analisys    compiled annual analysis program
```

## RetePAIoT sensor puller

`sensor_pull` replaces the previous PHP cron importer `/home/tools/data/sqlpull.php`. It is a normal command-line program, not a CGI.

It reads database credentials and the RetePAIoT `auth_key` from the existing sensor graph configuration:

```text
/home/www/sensori/data.conf
```

Required configuration keys are:

```text
db_host
db_user
db_pass
db_name
optional db_port
auth_key
```

The same `data.conf` remains usable by `data.cgi`; version 2.07 of `data.c` accepts `auth_key` as an optional configuration key and otherwise ignores it.

Usage:

```sh
./sensor_pull DEVICE_EUI SECONDS
```

Example equivalent to the previous cron job:

```sh
./sensor_pull a8404138b85ba1be 600
```

For each execution the program:

1. calculates the UTC lookback interval;
2. sends the authenticated GET request with JSON body to the RetePAIoT `payloaddata` API;
3. decodes Dragino LHT65 temperature and humidity from the first six payload bytes, preserving the previous PHP semantics;
4. parses the API timestamps as UTC;
5. inserts all valid values into `ambient(epoch,device,temperature,humidity)` using batched multi-row upserts inside a transaction.

On success it prints:

```text
INSERTED=N
```

If RetePAIoT reports `Nessun payload disponibile` for the requested lookback window, this is treated as a normal successful run and the program prints `INSERTED=0`, matching the non-error behavior of the previous PHP cron importer.

The intended cron replacement is:

```cron
*/10 * * * * /home/tools/mcp/work/data/sensor_pull a8404138b85ba1be 600
```

## Shared energy runtime configuration

The data project uses `/home/tools/mcp/work/data/setup_energy.conf` for shared MariaDB and remote meter connection settings. Credentials, IP addresses and TCP ports are runtime configuration and are no longer compiled from `/home/tools/setup_energy.c`.

The file uses simple `key=value` syntax and is loaded by `energy_config.c` / `energy_config.h`.

Required keys:

```text
db_host
db_user
db_pass
db_name
db_port
so_host
so_port
cc1_host
cc1_port
cc2_host
cc2_port
cc3_host
cc3_port
```

The configuration is currently used by:

```text
gme_mysql
pun
kwh_proc
kwh_analisys
meter_pull
```

`sensor_pull` remains separate because it reads `/home/www/sensori/data.conf`, which also contains the RetePAIoT `auth_key`.

Current versions after the runtime-configuration migration:

```text
gme_mysql       1.14
pun             1.22
kwh_proc        1.11
kwh_analisys    1.27
meter_pull      1.00
```

## meter_pull

`meter_pull` replaces the historical `G1` collector while preserving the same six numeric modes, register addresses, protocols, tables and scaling.

```text
meter_pull 1    SO voltage/current -> vi_so
meter_pull 2    SO energy          -> energy_so
meter_pull 3    CC voltage/current -> vi_cc
meter_pull 4    CC energy          -> energy_cc
meter_pull 5    LE1 energy         -> energy_le1
meter_pull 6    WJ150 water/traffic -> water_cc + traffic_cc
```

Unlike the historical collector, connection, protocol and MySQL failures return a non-zero exit status, which makes cron failures observable.

Equivalent cron schedule:

```cron
* * * * * sleep 1; /home/tools/mcp/work/data/meter_pull 1; sleep 10; /home/tools/mcp/work/data/meter_pull 1; sleep 10; /home/tools/mcp/work/data/meter_pull 1; sleep 10; /home/tools/mcp/work/data/meter_pull 1; sleep 10; /home/tools/mcp/work/data/meter_pull 1; sleep 10; /home/tools/mcp/work/data/meter_pull 1; sleep 1; /home/tools/mcp/work/data/meter_pull 2
* * * * * sleep 2; /home/tools/mcp/work/data/meter_pull 3; sleep 10; /home/tools/mcp/work/data/meter_pull 3; sleep 10; /home/tools/mcp/work/data/meter_pull 3; sleep 10; /home/tools/mcp/work/data/meter_pull 3; sleep 10; /home/tools/mcp/work/data/meter_pull 3; sleep 10; /home/tools/mcp/work/data/meter_pull 3; sleep 1; /home/tools/mcp/work/data/meter_pull 4
* * * * * sleep 5; /home/tools/mcp/work/data/meter_pull 5; /home/tools/mcp/work/data/meter_pull 6
```
## ambient_udp

`ambient_udp` is the persistent IPv6 UDP receiver for local ambient sensors. It listens on fixed UDP port `54321`, accepts the existing `zigbee_sensor` and `switchbot_sensor` packet formats, and stores samples in `ambient(epoch,device,temperature,humidity)`.

The UDP port is intentionally fixed in the program and is not part of `setup_energy.conf`. MariaDB connection settings are loaded through the shared `energy_config` runtime configuration. The daemon escapes the received device identifier before SQL use, checks/reconnects the database connection before inserts, and handles SIGTERM/SIGINT cleanly.

Configuration/database validation without binding the UDP port:

```sh
./ambient_udp --check
```

Recommended systemd executable:

```text
/home/tools/mcp/work/data/ambient_udp
```



## Radio data services

The data project also owns two long-running radio data services. Both start from the historical 2022 code base and use the 3.00 release line.

### Shared radio configuration

Runtime settings are stored in:

```text
/home/tools/mcp/work/data/radio.conf
```

The file is mode `600` and contains the MariaDB connection used by `adif_rx`, the ADIF authentication secret, and the DX Cluster endpoints used by `dxcluster`. It is separate from `setup_energy.conf`: energy and radio collectors do not share configuration namespaces.

The parser is implemented by `radio_config.c` / `radio_config.h`. Each daemon validates only the settings it actually needs, so `dxcluster` has no logical dependency on MariaDB configuration.

### radio_data

`radio_data.c` / `radio_data.h` contains reusable radio data processing primitives extracted from the historical `log/pfunc.c` dependency:

- state-free ADIF field parsing with an explicit cursor, including multiple records in one ADIF buffer;
- callsign normalization and CTY/DXCC lookup through the `cty` table;
- ADIF UTC date/time conversion to Unix epoch.

The purpose of this module is to make data-format processing independent from the web logger. The `log` application can progressively link these routines instead of owning them inside `pfunc.c`.

### adif_rx

`adif_rx` replaces the historical resident `/home/www/log/prx.cgi`. It is a daemon, not a CGI.

It listens on the configured UDP port (currently `2333`), receives ADIF records, validates the configured secret carried in the ADIF `comment` field, resolves the callsign through the CTY table and inserts the QSO into the `log` table using the existing schema.

Compared with the historical receiver it:

- does not include the whole `pfunc.c`;
- does not link unused CURL/QRZ/math/network helpers;
- keeps a MariaDB connection and reconnects when needed;
- escapes SQL string fields;
- validates ADIF dates and frequencies;
- handles SIGTERM/SIGINT cleanly;
- supports `adif_rx --check` for configuration and database validation without binding the UDP port.

The supplied systemd unit is `adif-rx.service`.

### dxcluster

Current release: **3.01**. The local TCP API and spot/filter behavior are unchanged. Version 3.01 fixes service shutdown/restart: the local listener and remote cluster socket are polled with short timeouts, while the signal handler only sets the stop flag. This avoids the historical case where the server thread remained blocked in `accept()` and `systemctl restart dxcluster.service` waited for systemd's stop timeout before forcing termination.


`dxcluster` replaces the historical resident `/home/www/log/pcluster.cgi`. It is also a daemon, not a CGI.

It connects to the configured DX Cluster server (currently `ham.homelinux.org:8000`), logs in with the configured callsign, keeps the last 100000 valid spots in a RAM ring buffer, applies the existing band/mode plan and serves filtered spot lists over the compatible local TCP API (currently `127.0.0.1:22222`). The current `log/pproc.c` can therefore use the new service without a protocol change.

The implementation preserves the existing local response format:

```text
epoch,spotter,frequency_hz,dx
```

It improves the historical implementation by parsing the remote TCP stream as lines rather than assuming one `recv()` equals one cluster message, bounding all spot fields, validating local requests, avoiding long mutex holds while sending client output, handling partial `send()` calls, reconnecting to the remote cluster and shutting down cleanly on signals.

`dxcluster --check` validates the configuration and remote hostname without opening the local service port. The supplied systemd unit is `dxcluster.service`.

### Radio service files

```text
adif_rx.c
adif_rx
adif-rx.service
dxcluster.c
dxcluster
dxcluster.service
radio_config.c
radio_config.h
radio_config.o
radio_data.c
radio_data.h
radio_data.o
radio.conf
```

During migration the historical `prx.cgi` and `pcluster.cgi` may remain running until the new systemd services are explicitly switched over. They must not run concurrently on UDP `2333` / TCP `22222` during the final production cutover.

## Canonical shared radio layer

The canonical radio data implementation for this server lives in `/home/tools/mcp/work/data` and starts from the 2022 code line with release 3.x. New radio applications, including `qsoz`, should reuse this layer instead of copying functions from the historical `log/pfunc.c`.

### `radio_data` 3.01

`radio_data.c` / `radio_data.h` are the canonical implementations of:

```c
int radio_adif_extract(const char **cursor,const char *const *names,int count,RadioAdif *out);
int radio_cty_lookup(MYSQL *con,const char *callsign,RadioCty *out);
time_t radio_adif_time(const char *date,const char *clock);
int radio_locator_to_latlon(const char *locator,double *lat,double *lon);
double radio_distance_km(double lat1,double lon1,double lat2,double lon2);
double radio_bearing_deg(double lat1,double lon1,double lat2,double lon2);
int radio_locator_distance_bearing(const char *locator1,const char *locator2,double *distance_km,double *bearing_deg);
```

Maidenhead locators are accepted at 2, 4 or 6 characters, case-insensitively where appropriate, and are strictly validated. Coordinates are returned at the center of the locator cell with the conventional sign convention: north latitude positive and east longitude positive. Distances are great-circle kilometers using an Earth radius of 6371 km; bearings are initial bearings normalized to `[0,360)` degrees.

The reusable static library is:

```text
/home/tools/mcp/work/data/libradio_data.a
```

Because `radio_cty_lookup()` is in the same canonical object, consumers of the static library should currently link MariaDB and math as well, for example:

```sh
cc ... /home/tools/mcp/work/data/libradio_data.a $(mariadb_config --libs) -lm
```

Do not create private copies of Maidenhead, distance, bearing, ADIF or CTY logic in `qsoz`; include `/home/tools/mcp/work/data/radio_data.h` and link the canonical library.

### `radio_client` 3.00

`radio_client.c` / `radio_client.h` provide the small C client for the local callbook service. The static library is:

```text
/home/tools/mcp/work/data/libradio_client.a
```

API:

```c
int radio_callbook_lookup(const char *host,unsigned int port,int source,
                          const char *callsign,unsigned int timeout,
                          char *response,unsigned long response_cap);
```

Sources and return codes:

```text
RADIO_CALLBOOK_QRZCOM
RADIO_CALLBOOK_QRZRU

RADIO_CALLBOOK_OK        1
RADIO_CALLBOOK_NOTFOUND  0
RADIO_CALLBOOK_ERROR    -1
```

For the current deployment clients should use `127.0.0.1:22223`. Callbook credentials must never be copied into a client such as `qsoz`.

## `callbookd` 3.00

`callbookd` is a persistent localhost callbook acquisition service. It replaces the architectural need for QRZ.com/QRZ.ru HTTP/XML login and lookup routines inside CGI programs. It is deliberately separate from `adif_rx` and `dxcluster` and does not change their protocols or behavior.

Files:

```text
callbookd.c
callbookd
callbookd.service
radio_client.c
radio_client.h
libradio_client.a
```

The daemon:

- binds only to the configured local interface, currently `127.0.0.1:22223`;
- uses HTTPS through libcurl with certificate and hostname verification;
- parses XML with libxml2 rather than substring/tag-order assumptions;
- ignores unknown XML nodes and does not rely on XML element ordering;
- keeps the QRZ.com session key in memory and reuses it until the server indicates that the session is invalid;
- retries a QRZ.com lookup once after obtaining a new session when the existing session is invalid;
- keeps the QRZ.ru session id in memory with a configured 3600 second lifetime;
- serializes QRZ.ru remote requests and enforces the configured minimum interval, currently 3 seconds;
- recognizes QRZ.ru XML errors even when returned with HTTP 4xx status;
- performs QRZ.ru login with HTTPS POST and callsign lookup with HTTPS GET;
- reconnects to MariaDB when necessary;
- updates the existing `who` table with a prepared statement;
- writes `src='qrz.com'` for QRZ.com data and `src='qrz.ru'` for QRZ.ru data;
- preserves the historical QRZ.ru Cyrillic-to-Latin transliteration behavior;
- handles SIGTERM/SIGINT cleanly and ignores SIGPIPE.

The existing `who` schema is retained; no database migration is required.

### Local callbook protocol

One request per TCP connection:

```text
PING
STATUS
QRZCOM IK4LZH
QRZRU IK4LZH
```

Typical responses:

```text
OK PONG
OK qrz=session qrzru=session
OK IK4LZH qrz.com
OK IK4LZH qrz.ru
NOTFOUND XX0XXX qrz.com
ERROR <diagnostic>
```

An `OK CALL source` response means the corresponding `who` row has already been updated before the response is sent. Consumers normally request the lookup and then read the `who` table using their existing database connection.

### Callbook configuration

Callbook credentials and service settings are centralized exclusively in:

```text
/home/tools/mcp/work/data/radio.conf
```

Relevant keys are:

```ini
callbook_local_host=127.0.0.1
callbook_local_port=22223
callbook_connect_timeout=5
callbook_http_timeout=15
qrz_user=...
qrz_pass=...
qrz_agent=GM-radio-3.00
qrz_ru_user=...
qrz_ru_pass=...
qrz_ru_agent=GM-radio-3.00
qrz_ru_session_ttl=3600
qrz_ru_min_interval=3
```

`radio.conf` remains mode `600`. The QRZ credentials that were initially present in `qsoz/qsoz.conf` have been moved here and removed from the qsoz configuration. `qsoz` must use the local callbook service instead of directly authenticating to either remote provider.

The `radio_config` parser is version 3.01 and understands the new callbook keys. The new keys are not required by `adif_rx` or `dxcluster`; each existing daemon still validates only the configuration it uses.

### Build and validation

The `data` Makefile now builds:

```text
adif_rx
dxcluster
callbookd
libradio_data.a
libradio_client.a
```

A complete validation is:

```sh
cd /home/tools/mcp/work/data
make clean
make
./adif_rx --check
./dxcluster --check
./callbookd --check
```

Current validation completed successfully with no build warnings from the new radio code. Geographic tests validated `JN54ql` at approximately `44.479167 N, 11.375000 E` and `JN54ql -> JO01aa` at approximately `1114.5 km / 314.7 degrees`.

Live API validation of `callbookd` was also completed before installation: QRZ.com login/lookup succeeded, a second lookup reused the in-memory QRZ.com session, QRZ.ru login/lookup succeeded over HTTPS, a second QRZ.ru lookup reused its session and respected the three-second request interval, and both providers successfully updated `who`.

### systemd installation for callbookd

The unit file is prepared in the project but is not automatically copied into `/etc/systemd/system` by the Makefile:

```sh
sudo cp /home/tools/mcp/work/data/callbookd.service /etc/systemd/system/callbookd.service
sudo systemctl daemon-reload
sudo systemctl enable --now callbookd.service
sudo systemctl status callbookd.service --no-pager
```

The service log is configured as:

```text
/home/tools/misc/callbookd.log
```

### qsoz integration contract

The qsoz project should treat `data` as the source of truth for radio primitives and callbook acquisition:

1. include `/home/tools/mcp/work/data/radio_data.h` for ADIF/CTY/Maidenhead/distance/bearing;
2. link `/home/tools/mcp/work/data/libradio_data.a` plus MariaDB and `-lm`;
3. include `/home/tools/mcp/work/data/radio_client.h` for callbook requests;
4. link `/home/tools/mcp/work/data/libradio_client.a`;
5. request QRZ.com or QRZ.ru through `radio_callbook_lookup()` at `127.0.0.1:22223`;
6. read the resulting canonical row from MariaDB table `who`;
7. do not store QRZ credentials, session ids, HTTP code or XML parsing logic in qsoz.

Future migration of the historical `log` project should follow the same pattern: first replace duplicated geographic/ADIF/CTY helpers with `radio_data`, then replace direct `qrzcom()` / `qrzru()` calls with the local callbook service. The old `pfunc.c` functions should only be removed after all consumers have migrated and been tested.
