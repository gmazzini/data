<?php
include "local.php";
$headers=getallheaders();
$data=json_decode(file_get_contents('php://input'),true);
$device=$headers["device"];
$auth=$headers["auth"];
if($auth!=$myauth)exit(0);
file_put_contents("/home/www/sensori/mem/".time().".".$device,print_r($headers,true).print_r($data,true).PHP_EOL);
$con=mysqli_connect($db_host, $db_user, $db_pass, $db_name);
$q=base64_decode($data["data"]);
if(ord($q[2])>0x80)$t=((ord($q[2])<<8 | ord($q[3]))-0xFFFF)/100;
else $t=(ord($q[2])<<8 | ord($q[3]))/100;
$u=(ord($q[4])<<8 | ord($q[5]))/10;
$m=time();
mysqli_query($con,"insert into $device (epoch,temperature,humidity) values ($m,$t,$u)");
mysqli_close($con);
?>
