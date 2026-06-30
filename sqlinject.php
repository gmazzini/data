<?php
include "local.php";

$headers=getallheaders();
$raw=file_get_contents('php://input');
$m=time();

$hdevice=isset($headers["device"])?$headers["device"]:"unknown";
$hdevice=preg_replace('/[^A-Za-z0-9_.:-]/','_',$hdevice);

if(!is_dir("/home/www/sensori/mem/raw")){
  @mkdir("/home/www/sensori/mem/raw",0775,true);
}

file_put_contents(
  "/home/www/sensori/mem/raw/".$m.".".$hdevice,
  print_r($headers,true).
  "\n---- raw body ----\n".
  $raw.
  PHP_EOL
);

$data=json_decode($raw,true);

$auth=isset($headers["auth"])?$headers["auth"]:"";
if($auth!=$myauth)exit(0);

if(!is_array($data))exit(0);
if(!isset($data["data"]))exit(0);

$q=base64_decode($data["data"],true);
if($q===false)exit(0);
if(strlen($q)<6)exit(0);

$v=(ord($q[2])<<8) | ord($q[3]);
if($v & 0x8000)$v-=0x10000;
$t=$v/100;

$u=((ord($q[4])<<8) | ord($q[5]))/10;

$dev_eui_hex="";
if(isset($data["devEUI"])){
  $devraw=base64_decode($data["devEUI"],true);
  if($devraw!==false && strlen($devraw)>0)$dev_eui_hex=bin2hex($devraw);
}

$device=$hdevice;
if($dev_eui_hex!="")$device=$dev_eui_hex;
if($device=="unknown" && isset($data["deviceName"]))$device=$data["deviceName"];

file_put_contents(
  "/home/www/sensori/mem/".$m.".".$hdevice,
  print_r($headers,true).
  print_r($data,true).
  "\n---- decoded helper ----\n".
  "device_header=".$hdevice."\n".
  "device_name=".(isset($data["deviceName"])?$data["deviceName"]:"")."\n".
  "devEUI_hex=".$dev_eui_hex."\n".
  "device_used=".$device."\n".
  "payload_hex=".bin2hex($q)."\n".
  "temperature=".$t."\n".
  "humidity=".$u."\n".
  PHP_EOL
);

$con=mysqli_connect($db_host,$db_user,$db_pass,$db_name);
if(!$con)exit(0);

$device=mysqli_real_escape_string($con,$device);

$sql="insert into ambient (epoch,device,temperature,humidity) values ($m,'$device',$t,$u)";
if(!mysqli_query($con,$sql)){
  file_put_contents(
    "/home/www/sensori/mem/mysql_error.log",
    date("Y-m-d H:i:s")." ".mysqli_error($con)." ".$sql."\n",
    FILE_APPEND
  );
}

mysqli_close($con);
?>
