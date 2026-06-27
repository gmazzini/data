<?php
include "local.php";

$headers=getallheaders();
$data=json_decode(file_get_contents('php://input'),true);

$auth=isset($headers["auth"])?$headers["auth"]:"";
if($auth!=$myauth)exit(0);

$hdevice=isset($headers["device"])?$headers["device"]:"unknown";

$q=base64_decode($data["data"]);

if(ord($q[2])>0x80)$t=((ord($q[2])<<8 | ord($q[3]))-0xFFFF)/100;
else $t=(ord($q[2])<<8 | ord($q[3]))/100;

$u=(ord($q[4])<<8 | ord($q[5]))/10;
$m=time();

$dev_eui_hex="";
if(isset($data["devEUI"])){
  $devraw=base64_decode($data["devEUI"]);
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
  "temperature=".$t."\n".
  "humidity=".$u."\n".
  PHP_EOL
);

$con=mysqli_connect($db_host,$db_user,$db_pass,$db_name);
if(!$con)exit(0);

$device=mysqli_real_escape_string($con,$device);

mysqli_query($con,"insert into ambient (epoch,device,temperature,humidity) values ($m,'$device',$t,$u)");

mysqli_close($con);
?>
