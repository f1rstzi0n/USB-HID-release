# 检查迁移生产 C：块首定义变量和循环变量；第三方头作为 system include。
[CmdletBinding()]
param([string]$Compiler=$env:ARM_GCC)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if(!$Compiler){
 $plugins='D:\Software\STMicroelectronics\STM32CubeIDE\STM32CubeIDE\plugins'
 $tool=Get-ChildItem -LiteralPath $plugins -Directory -Filter 'com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*.win32_*'|Select-Object -First 1
 if(!$tool){throw '请设置 ARM_GCC 或传入 -Compiler。'}
 $Compiler=Join-Path $tool.FullName 'tools/bin/arm-none-eabi-gcc.exe'
}
[xml]$project=[IO.File]::ReadAllText((Join-Path $root '.cproject'))
$includes=@($project.SelectNodes('//option[@valueType="includePath"]/listOptionValue')|ForEach-Object{$_.value}|Select-Object -Unique)
$args=@('-mcpu=cortex-m4','-mthumb','-std=gnu11','-DUSE_HAL_DRIVER','-DSTM32F446xx','-DHSE_VALUE=12000000','-DMBEDTLS_CONFIG_FILE="fw_mbedtls_config.h"',
 '-fsyntax-only','-Wdeclaration-after-statement','-Werror=declaration-after-statement')
foreach($include in $includes){
 $path=[IO.Path]::GetFullPath((Join-Path (Join-Path $root 'Debug') $include))
 $kind=if($path -match 'STM32F4xx_HAL|CMSIS|TinyUSB|MbedTLS|Middlewares'){'-isystem'}else{'-I'}
 $args+=@($kind,$path)
}
$folders=@('App','Drivers/USB','Drivers/Crypto')
if(Test-Path -LiteralPath (Join-Path $root 'Drivers/W25Q64')){$folders+='Drivers/W25Q64'}
$files=@(Get-Item -LiteralPath (Join-Path $root 'Core/Src/main.c'))
foreach($folder in $folders){$files+=Get-ChildItem -LiteralPath (Join-Path $root $folder) -Recurse -File -Filter '*.c'}
foreach($file in $files){
 $text=[IO.File]::ReadAllText($file.FullName)
 $code=[regex]::Replace($text,'/\*[\s\S]*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*''',' ')
 if($code -match 'for\s*\(\s*(?:unsigned|signed|int|char|size_t|u?int(?:8|16|32|64)_t)\s'){throw "循环变量未在块首定义：$($file.FullName)"}
 & $Compiler @args $file.FullName
 if($LASTEXITCODE -ne 0){throw "块首变量/语法检查失败：$($file.FullName)"}
}
Write-Host "PASS $($files.Count) 个迁移生产 C 文件：块首定义与循环变量"
