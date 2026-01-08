#!/usr/bin/env php
<?php
include m7350.php;
declare(strict_types=1);

function send_post(string $keyp,string $from, string $content, int $ts): void {
  $url = 'https://data.mazzini.org/mysms.php';

  $ch = curl_init($url);
  curl_setopt_array($ch, [
    CURLOPT_POST => true,
    CURLOPT_POSTFIELDS => http_build_query([
      'keyp' => $keyp,
      'ts'   => $ts,
      'from' => $from,
      'text' => $content,
    ]),
    CURLOPT_RETURNTRANSFER => true, // non stampare nulla
    CURLOPT_CONNECTTIMEOUT => 3,
    CURLOPT_TIMEOUT => 3,
  ]);

  curl_exec($ch);
  curl_close($ch);
}

function http_json(string $url, array $payload): array {
  $ch = curl_init($url);
  $json = json_encode($payload, JSON_UNESCAPED_SLASHES);
  curl_setopt_array($ch, [
    CURLOPT_POST => true,
    CURLOPT_RETURNTRANSFER => true,
    CURLOPT_HTTPHEADER => ['Content-Type: application/json', 'X-Requested-With: XMLHttpRequest'],
    CURLOPT_POSTFIELDS => $json,
  ]);
  $resp = curl_exec($ch);
  return json_decode((string)$resp, true);
}

function login_token(string $host, string $user, string $pass): string {
  $auth = "http://$host/cgi-bin/qcmap_auth";
  $n = http_json($auth, ['module' => 'authenticator', 'action' => 0]);
  $digest = md5("$user:$pass:{$n['nonce']}");
  $r = http_json($auth, ['module' => 'authenticator', 'action' => 1, 'digest' => $digest]);
  return $r['token'];
}

function web_call(string $host, string $token, string $module, int $action, array $extra = []): array {
  return http_json("http://$host/cgi-bin/qcmap_web_cgi", $extra + ['module' => $module, 'action' => $action, 'token' => $token]);
}

const SMS_READ = 2;
const SMS_DELETE = 5;

$token = login_token($HOST, $USER, $PASS);

while (true) {
  $st = web_call($HOST, $token, 'status', 0);
  if (($st['result'] ?? -1) !== 0) $token = login_token($HOST, $USER, $PASS);

  $inbox = web_call($HOST, $token, 'message', SMS_READ, ['pageNumber' => 1, 'amountPerPage' => 8, 'box' => 0]);
  if (($inbox['result'] ?? -1) !== 0) { $token = login_token($HOST, $USER, $PASS); $inbox = web_call($HOST, $token, 'message', SMS_READ, ['pageNumber' => 1, 'amountPerPage' => 8, 'box' => 0]); }

  $list = $inbox['messageList'] ?? [];
  if (is_array($list) && count($list) > 0) {
    $toDelete = [];
    foreach ($list as $m) {
      $idx = (int)$m['index'];
      $from = (string)($m['from'] ?? '');
      $content = (string)($m['content'] ?? '');
      $content = str_replace(["\r\n", "\r", "\n"], ' ', $content);
      send_post($KEYP, $from, $content, time());
      $toDelete[] = $idx;
    }
    $del = web_call($HOST, $token, 'message', SMS_DELETE, ['box' => 0, 'deleteMessages' => $toDelete]);
    if (($del['result'] ?? -1) !== 0) { $token = login_token($HOST, $USER, $PASS); web_call($HOST, $token, 'message', SMS_DELETE, ['box' => 0, 'deleteMessages' => $toDelete]); }
  }

  sleep(1);
}
