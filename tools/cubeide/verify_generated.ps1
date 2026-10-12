# 仅检查生成配置；不烧录、不改变硬件。
[CmdletBinding()]
param([string]$Root)
$ErrorActionPreference='Stop'
if(!$Root){$Root=Join-Path $PSScriptRoot '../..'}
$Root=(Resolve-Path -LiteralPath $Root).Path
function Read([string]$file){[IO.File]::ReadAllText((Join-Path $Root $file))}
function Require([bool]$ok,[string]$why){if(!$ok){throw $why};Write-Host "PASS $why"}
[xml]$project=Read '.project';$name=$project.projectDescription.name;$boot=$name.EndsWith('-Boot')
$ioc=Read ($name+'.ioc');$main=Read 'Core/Src/main.c';$board=Read 'Drivers/USB/board.c'
$it=Read 'Core/Src/stm32f4xx_it.c';$msp=Read 'Core/Src/stm32f4xx_hal_msp.c';$hal=Read 'Core/Inc/stm32f4xx_hal_conf.h'
[xml]$cfg=Read '.cproject'
Require ($ioc -match '(?m)^RCC.HSE_VALUE=12000000\r?$' -and $ioc -match 'RCC.PLLSourceVirtual=RCC_PLLSOURCE_HSE') 'IOC 12MHz HSE PLL 源'
Require ($main -match 'HSEState\s*=\s*RCC_HSE_BYPASS' -and $main -match 'PLLSource\s*=\s*RCC_PLLSOURCE_HSE' -and $main -match 'PLLN\s*=\s*180;') 'HSE bypass / CPU 180MHz'
Require ($board -match 'PLLSAIM = 6' -and $board -match 'PLLSAIN = 96' -and $board -match 'RCC_PLLSAIP_DIV4') '板级 PLLSAIP USB 48MHz'
Require ($ioc -match 'PE4.Signal=GPIO_Input' -and $ioc -match 'PE4.GPIO_PuPd=GPIO_NOPULL') 'PE4 无上下拉高阻'
Require ($ioc -match 'PE8.PinState=GPIO_PIN_SET' -and $ioc -match 'PE11.PinState=GPIO_PIN_SET') 'Flash/Motor CS 初始取消选中'
Require (($main+$msp+$it) -notmatch 'HAL_PCD_Init|HAL_PCD_IRQHandler|MX_USB_OTG_FS_PCD_Init') '无 PCD 接管'
Require ($hal -notmatch '(?m)^#define HAL_PCD_MODULE_ENABLED') '禁用 HAL PCD'
Require ([regex]::Matches($board+$it,'void\s+OTG_FS_IRQHandler\s*\(void\)\s*\{').Count -eq 1) '仅一个 USB ISR'
Require ($main -match 'timer->Instance == TIM7|htim->Instance == TIM7') 'HAL TIM7 毫秒时基'
Require ($cfg.SelectNodes('//configuration[@artifactExtension="elf"]').Count -eq 2) '独立 Debug/Release'
foreach($config in $cfg.SelectNodes('//configuration[@artifactExtension="elf"]')){
 $defines=@($config.SelectNodes('.//tool[contains(@superClass,"tool.c.compiler")]/option[@valueType="definedSymbols"]/listOptionValue')|ForEach-Object{$_.value})
 $offset=if($boot){'VECT_TAB_OFFSET=0'}else{'VECT_TAB_OFFSET=0x10200'}
 Require ($defines -contains 'HSE_VALUE=12000000' -and $defines -contains 'USER_VECT_TAB_ADDRESS' -and $defines -contains $offset) ($config.name+'：时钟和 VTOR')
 $entries=@($config.sourceEntries.entry|ForEach-Object{$_.name})
 Require ($entries -contains 'App' -and $entries -contains 'Drivers/USB' -or ($entries -contains 'Drivers' -and $entries -contains 'App')) ($config.name+'：应用和驱动清单')
 if($boot){
  Require ($entries -notcontains 'Middlewares' -and $entries -notcontains 'Drivers/TinyUSB/src/class/msc') ($config.name+'：Boot 无 RTOS/MSC')
  Require ($config.SelectSingleNode('.//tool[contains(@superClass,"tool.c.compiler")]/option[contains(@superClass,"optimization.level")]').value.EndsWith('.os')) ($config.name+'：Boot -Os')
 }
}
Require ((Read '.cproject') -notmatch 'USB-HID|usb-hid|Desktop[/\\](Test446[/\\]|Test446-Boot[/\\])') '无原工程或对方源码路径'
$link=Read 'STM32F446VETX_FLASH.ld'
Require ($link -match '0x2001FFF8' -and $link -match '\.boot_request\s*\(NOLOAD\)' -and $link -match '128K - 8') '8B SRAM 顶部 NOLOAD mailbox'
Require ($link -match $(if($boot){'ORIGIN = 0x08000000,\s+LENGTH = 64K'}else{'ORIGIN = 0x08010200,\s+LENGTH = 0x6FE00'})) '固定 Flash 地址/容量'
if($boot){
 Require (!(Test-Path -LiteralPath (Join-Path $Root 'Middlewares')) -and !(Test-Path -LiteralPath (Join-Path $Root 'Drivers/W25Q64'))) 'Boot 不携带 RTOS/W25 驱动'
}else{
 Require ((Read 'Core/Inc/FreeRTOSConfig.h') -match 'configTOTAL_HEAP_SIZE.*20480') 'RTOS heap 20KiB'
 Require ($main -match 'stack_size = 3072' -and $main -match 'stack_size = 2048' -and $main -match 'TASK_USB_IO') 'USB worker 3072B / IO 2048B'
 Require (!(Test-Path -LiteralPath (Join-Path $Root 'App/firmware/fw_update.c'))) 'APP 不携带 Boot 擦写状态机'
}
