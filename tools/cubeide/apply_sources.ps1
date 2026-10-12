# CubeMX 再生成后恢复本工程唯一构建定义；APP/Boot 各自独立。
[CmdletBinding()]
param([string]$Root)
$ErrorActionPreference='Stop'
if(!$Root){$Root=Join-Path $PSScriptRoot '../..'}
$Root=(Resolve-Path -LiteralPath $Root).Path
[xml]$description=[IO.File]::ReadAllText((Join-Path $Root '.project'))
$name=$description.projectDescription.name
$boot=$name.EndsWith('-Boot')
$path=Join-Path $Root '.cproject'
[xml]$doc=[IO.File]::ReadAllText($path)
$include=@('Core/Inc','Drivers/STM32F4xx_HAL_Driver/Inc','Drivers/STM32F4xx_HAL_Driver/Inc/Legacy',
 'Drivers/CMSIS/Device/ST/STM32F4xx/Include','Drivers/CMSIS/Include',
 'App/protocol','App/usb','App/firmware','Drivers/USB','Drivers/Crypto',
 'Drivers/TinyUSB/src','Drivers/MbedTLS/include','Drivers/MbedTLS/library')
$sources=@('Core','App','Drivers','Drivers/MbedTLS/library','Drivers/TinyUSB/src',
 'Drivers/TinyUSB/src/common','Drivers/TinyUSB/src/device','Drivers/TinyUSB/src/class/hid',
 'Drivers/TinyUSB/src/portable/synopsys/dwc2')
if(!$boot){
 $include+=@('App/catalog','App/content','App/service','App/generated','Drivers/W25Q64',
 'Middlewares/Third_Party/FreeRTOS/Source/include','Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2',
 'Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F')
 $sources+=@('Middlewares','Drivers/TinyUSB/src/class/msc')
}
$excluding=@{
 'Drivers'='TinyUSB|MbedTLS'
 'Drivers/TinyUSB/src'='class|common|device|host|osal|portable|typec'
 'Drivers/TinyUSB/src/class/hid'='hid_host.c'
 'Drivers/TinyUSB/src/class/msc'='msc_host.c'
 'Drivers/TinyUSB/src/portable/synopsys/dwc2'='hcd_dwc2.c'
}
foreach($cfg in $doc.SelectNodes('//configuration[@artifactExtension="elf"]')){
 $config=$cfg.GetAttribute('name')
 $compiler=$cfg.SelectSingleNode('.//tool[contains(@superClass,"tool.c.compiler")]')
 $inc=$compiler.SelectSingleNode('option[@valueType="includePath"]')
 foreach($n in @($inc.ChildNodes)){[void]$inc.RemoveChild($n)}
 foreach($item in $include){
  $n=$doc.CreateElement('listOptionValue');$n.SetAttribute('builtIn','false');$n.SetAttribute('value',"../$item");[void]$inc.AppendChild($n)
 }
 $symbols=$compiler.SelectSingleNode('option[@valueType="definedSymbols"]')
 foreach($n in @($symbols.ChildNodes)){
  if($n.value -match '^(HSE_VALUE|USER_VECT_TAB_ADDRESS|VECT_TAB_OFFSET|MBEDTLS_CONFIG_FILE|FW_BOOTLOADER)(=|$)'){[void]$symbols.RemoveChild($n)}
 }
 $offset=if($boot){'0'}else{'0x10200'}
 foreach($value in @('HSE_VALUE=12000000','USER_VECT_TAB_ADDRESS',"VECT_TAB_OFFSET=$offset",'MBEDTLS_CONFIG_FILE="fw_mbedtls_config.h"')){
  $n=$doc.CreateElement('listOptionValue');$n.SetAttribute('builtIn','false');$n.SetAttribute('value',$value);[void]$symbols.AppendChild($n)
 }
 if($boot){
  $opt=$compiler.SelectSingleNode('option[contains(@superClass,"optimization.level")]')
  $opt.SetAttribute('value','com.st.stm32cube.ide.mcu.gnu.managedbuild.tool.c.compiler.option.optimization.level.value.os');$opt.SetAttribute('valueType','enumerated')
 }
 $entries=$cfg.SelectSingleNode('sourceEntries')
 foreach($n in @($entries.ChildNodes)){[void]$entries.RemoveChild($n)}
 foreach($item in $sources){
  $n=$doc.CreateElement('entry');$n.SetAttribute('flags','VALUE_WORKSPACE_PATH|RESOLVED');$n.SetAttribute('kind','sourcePath');$n.SetAttribute('name',$item)
  if($excluding.ContainsKey($item)){$n.SetAttribute('excluding',$excluding[$item])}
  [void]$entries.AppendChild($n)
 }
 $cfg.SelectSingleNode('.//builder').SetAttribute('buildPath',('$'+'{workspace_loc:/'+$name+'}/'+$config))
 $cfg.SelectSingleNode('.//option[contains(@superClass,"c.linker.option.script")]').SetAttribute('value',('$'+'{workspace_loc:/' + $name + '/STM32F446VETX_FLASH.ld}'))
 $chain=$cfg.SelectSingleNode('.//toolChain')
 foreach($kind in @('converthex','convertbinary')){
  $option=$chain.SelectSingleNode("option[contains(@superClass,'option.$kind')]")
  if(!$option){
   $option=$doc.CreateElement('option');$option.SetAttribute('id',"com.st.stm32cube.ide.mcu.gnu.managedbuild.option.$kind.$config")
   $option.SetAttribute('superClass',"com.st.stm32cube.ide.mcu.gnu.managedbuild.option.$kind");$option.SetAttribute('valueType','boolean');[void]$chain.AppendChild($option)
  }
  $option.SetAttribute('value','true')
 }
 if($boot){
  $float=$chain.SelectSingleNode('option[contains(@superClass,"nanoprintffloat")]');if($float){$float.SetAttribute('value','false')}
 }
}
$settings=[Xml.XmlWriterSettings]::new();$settings.Encoding=[Text.UTF8Encoding]::new($false);$settings.Indent=$true;$settings.IndentChars=[char]9
$writer=[Xml.XmlWriter]::Create($path,$settings);$doc.Save($writer);$writer.Close()
$systemPath=Join-Path $Root 'Core/Src/system_stm32f4xx.c'
$system=[IO.File]::ReadAllText($systemPath)
if(!$system.Contains('#ifndef VECT_TAB_OFFSET')){
 $system=[regex]::Replace($system,'(?m)^[ \t]*#define VECT_TAB_OFFSET[^\r\n]*',('#ifndef VECT_TAB_OFFSET'+[char]10+'#define VECT_TAB_OFFSET '+$offset+[char]10+'#endif'))
 [IO.File]::WriteAllText($systemPath,$system,[Text.UTF8Encoding]::new($false))
}
$linkPath=Join-Path $Root 'STM32F446VETX_FLASH.ld'
$link=[IO.File]::ReadAllText($linkPath)
$link=[regex]::Replace($link,'RAM\s*\(xrw\)\s*:\s*ORIGIN\s*=\s*0x20000000,\s*LENGTH\s*=[^\r\n]+','RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K - 8')
$flash=if($boot){'FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 64K'}else{'FLASH (rx) : ORIGIN = 0x08010200, LENGTH = 0x6FE00'}
$link=[regex]::Replace($link,'FLASH\s*\(rx\)\s*:\s*ORIGIN\s*=\s*0x[0-9A-Fa-f]+,\s*LENGTH\s*=[^\r\n]+',$flash)
if(!$link.Contains('BOOTREQ (')){
 $link=$link.Replace('MEMORY'+[char]10+'{','MEMORY'+[char]10+'{'+[char]10+'  BOOTREQ (rw) : ORIGIN = 0x2001FFF8, LENGTH = 8')
 if(!$link.Contains('BOOTREQ (')){$link=[regex]::Replace($link,'MEMORY\s*\{','MEMORY {'+[char]10+'  BOOTREQ (rw) : ORIGIN = 0x2001FFF8, LENGTH = 8')}
}
if(!$link.Contains('.boot_request')){
 $link=[regex]::Replace($link,'SECTIONS\s*\{','SECTIONS {'+[char]10+'  .boot_request (NOLOAD) : { KEEP(*(.boot_request)) } >BOOTREQ'+[char]10+'  ASSERT(SIZEOF(.boot_request) == 8, "boot mailbox must be 8 bytes")')
}
[IO.File]::WriteAllText($linkPath,$link,[Text.UTF8Encoding]::new($false))
if(!$boot){
 $rtos=Join-Path $Root 'Core/Inc/FreeRTOSConfig.h'
 $text=[IO.File]::ReadAllText($rtos)
 $text=[regex]::Replace($text,'(?m)^#define configTOTAL_HEAP_SIZE[^\r\n]*','#define configTOTAL_HEAP_SIZE ((size_t)20480)')
 [IO.File]::WriteAllText($rtos,$text,[Text.UTF8Encoding]::new($false))
}
Write-Host "PASS $name：源码清单/include/VTOR/mailbox/优化配置已恢复"
