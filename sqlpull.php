<?php
include "/home/www/sensori/local.php";

if($argc<3)exit(0);

$device_eui=$argv[1];
$seconds=intval($argv[2]);

if($device_eui=="" || $seconds<=0)exit(0);

function s16($v){
  if($v & 0x8000)$v-=0x10000;
  return $v;
}

function lht65_temp_hum($hex,&$t,&$u){
  $hex=preg_replace('/[^0-9a-fA-F]/','',$hex);
  if(strlen($hex)<12)return false;

  $b=array();
  for($i=0;$i<12;$i+=2){
    $b[]=hexdec(substr($hex,$i,2));
  }

  $t=s16(($b[2]<<8) | $b[3]) / 100;
  $u=((($b[4]<<8) | $b[5]) & 0x0fff) / 10;

  return true;
}

$from=gmdate("Y-m-d H:i:s",time()-$seconds);
$to=gmdate("Y-m-d H:i:s");
$url="https://api.retepaiot.it/payloaddata/".rawurlencode($device_eui);

$body=json_encode(array(
  "auth_key"=>$auth_key,
  "from"=>$from,
  "to"=>$to
));

$ch=curl_init($url);
curl_setopt($ch,CURLOPT_CUSTOMREQUEST,"GET");
curl_setopt($ch,CURLOPT_HTTPHEADER,array("Content-Type: application/json"));
curl_setopt($ch,CURLOPT_POSTFIELDS,$body);
curl_setopt($ch,CURLOPT_RETURNTRANSFER,true);
curl_setopt($ch,CURLOPT_TIMEOUT,2);
curl_setopt($ch,CURLOPT_CONNECTTIMEOUT,2);

$res=curl_exec($ch);
curl_close($ch);

if($res===false || $res=="")exit(0);

$j=json_decode($res,true);
if(!is_array($j))exit(0);
if(!isset($j["data"]) || !is_array($j["data"]))exit(0);

$con=mysqli_connect($db_host,$db_user,$db_pass,$db_name);
if(!$con)exit(0);

$device=mysqli_real_escape_string($con,$device_eui);

foreach($j["data"] as $r){
  if(!isset($r["timestamp"],$r["valore"]))continue;
  if(!lht65_temp_hum($r["valore"],$t,$u))continue;

  $epoch=strtotime($r["timestamp"]." UTC");
  if($epoch<=0)continue;

  $sql="insert into ambient (epoch,device,temperature,humidity) values ($epoch,'$device',$t,$u) ".
       "on duplicate key update temperature=values(temperature),humidity=values(humidity)";
  mysqli_query($con,$sql);
}

mysqli_close($con);
?>
