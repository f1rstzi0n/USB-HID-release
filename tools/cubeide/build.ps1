# 使用本工程 .cproject 构建；不维护第二份编译清单。
[CmdletBinding()]
param([ValidateSet('Debug','Release','All')][string]$Config='All',[string]$CubeIDE=$env:STM32CUBEIDE_EXE)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
[xml]$project=[IO.File]::ReadAllText((Join-Path $root '.project'));$name=$project.projectDescription.name
if(!$CubeIDE){$CubeIDE='D:\Software\STMicroelectronics\STM32CubeIDE\STM32CubeIDE\stm32cubeidec.exe'}
if(!(Test-Path -LiteralPath $CubeIDE)){throw '请设置 STM32CUBEIDE_EXE 或传入 -CubeIDE。'}
& (Join-Path $PSScriptRoot 'verify_generated.ps1') -Root $root
$logDir=Join-Path $root 'docs/validation';[void][IO.Directory]::CreateDirectory($logDir)
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$log=Join-Path $logDir ("build-$Config-$stamp.log")
$err=Join-Path $logDir ("build-$Config-$stamp.stderr.log")
$workspace=Join-Path ([IO.Path]::GetTempPath()) ('usb-port-'+$name+'-'+[Guid]::NewGuid().ToString('N'))
$configs=if($Config -eq 'All'){@('Debug','Release')}else{@($Config)}
$args=@('-nosplash','-application','org.eclipse.cdt.managedbuilder.core.headlessbuild','-no-indexer','-data',('"'+$workspace+'"'),'-import',('"'+$root+'"'))
foreach($item in $configs){$args+=@('-cleanBuild',"$name/$item")}
$process=Start-Process -FilePath $CubeIDE -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
$deadline=(Get-Date).AddMinutes(5)
$complete=$false
try{
 while((Get-Date) -lt $deadline){
  Start-Sleep -Milliseconds 500
  [string]$text=if(Test-Path -LiteralPath $log){(Get-Content -LiteralPath $log -Raw)}else{''}
  if($text.Contains('Saving workspace.')){$complete=$true;break}
  if($process.HasExited){break}
 }
}finally{
 if(!$process.HasExited){Stop-Process -Id $process.Id -Force}
}
$text=(Get-Content -LiteralPath $log -Raw)
if(!$complete -or $text -match 'Build Failed|valid Toolchain settings|error:'){throw "构建失败：$log；$err"}
foreach($item in $configs){
 $elf=Join-Path $root "$item/$name.elf"
 if(!(Test-Path -LiteralPath $elf)){throw "未生成 $elf"}
 if($name.EndsWith('-Boot') -and (Get-Item (Join-Path $root "$item/$name.bin")).Length -gt 65536){throw 'Boot 超出 64KiB'}
}
Write-Host "PASS $name $Config：$log"
