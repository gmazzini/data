<?php
  $var = print_r($_POST, true);
  file_put_contents("/home/www/sensori/error",$var);
?>
