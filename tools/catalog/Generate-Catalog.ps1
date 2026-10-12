[CmdletBinding()]
param(
    [ValidateSet('Check','Generate','Publish')][string]$Mode = 'Check',
    [string]$CatalogGen = '', [string]$CatalogPack = '',
    [string]$SourceRoot = '', [string]$OutputRoot = '',
    [string]$HostRoot = '', [string]$KeyName = '', [string]$PublicKey = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$SourceRoot) { $SourceRoot = Join-Path $repo 'catalog' }
if (!$OutputRoot) { $OutputRoot = $repo }
if (!$PublicKey) { $PublicKey = Join-Path $repo 'release-public/public-key.bin' }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if ($HostRoot) { $HostRoot = [IO.Path]::GetFullPath($HostRoot) }
if ($Mode -eq 'Publish' -and (!$HostRoot -or !$KeyName)) { throw 'Publish 要求 HostRoot 和既有 CNG KeyName。' }
if ($HostRoot -and !(Test-Path -LiteralPath (Join-Path $HostRoot 'localization/modules') -PathType Container)) { throw 'HostRoot 缺少 Andromeda 本地化工程。' }
$utf8 = [Text.UTF8Encoding]::new($false, $true)
function Hash-File([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Run-Tool([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "工具失败：$Program（退出码 $LASTEXITCODE）。" }
}
function Needs-Build([string]$Binary, [string[]]$Sources) {
    if (!(Test-Path -LiteralPath $Binary)) { return $true }
    $stamp = (Get-Item -LiteralPath $Binary).LastWriteTimeUtc
    foreach ($file in $Sources) { if ((Get-Item -LiteralPath $file).LastWriteTimeUtc -gt $stamp) { return $true } }
    return $false
}
$source = Join-Path $SourceRoot 'SW110048.catalog.yaml'
$sourceHash = Hash-File $source
if (!$sourceHash) { throw "维护源不存在：$source" }
$stage = Join-Path ([IO.Path]::GetTempPath()) ('usb-catalog-' + [Guid]::NewGuid().ToString('N'))
$keepStage = $false
$outputs = [Collections.Generic.List[object]]::new()
$nativeFiles = @('catalog/SW110048.catalog.csv','catalog/SW110048.tree.csv','catalog/groups.v2.json',
    'catalog/ui-bindings.csv','catalog/errors.csv','catalog/source-info.json',
    'App/generated/bvpv_catalog_data.c','App/generated/bvpv_catalog_data.h','App/generated/bvpv_catalog_bindings.h',
    'build-catalog/SW110048.catalog.body','build-catalog/SW110048.catalog.body.json','build-catalog/SW110048.catalog.body.runtime')
$hostFiles = @('localization/modules/BvpvUsbTree.json',
    'Other/CPALocalization/CPA4.Localization/Resources/Strings.resx',
    'Other/CPALocalization/CPA4.Localization/Resources/Strings.zh-CN.resx',
    'Other/CPALocalization/CPA4.Localization/Generated/StringKeys.g.cs',
    'localization/generated/catalog-summary.json','localization/generated/generation.json',
    'Other/CPACommunication/CPA4.Communication/Bvpv/Usb/Generated/AndromedaCatalogBindings.g.cs')
function Add-Output([string]$Relative, [bool]$HostFile = $false) {
    $root = if ($HostFile) { $HostRoot } else { $OutputRoot }
    $stagedRoot = if ($HostFile) { Join-Path $stage 'host' } else { $stage }
    $destination = [IO.Path]::GetFullPath((Join-Path $root $Relative))
    if (!$destination.StartsWith($root.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw '产物路径越界。' }
    $outputs.Add([pscustomobject]@{relative=$Relative;host=$HostFile;source=(Join-Path $stagedRoot $Relative);destination=$destination;before=(Hash-File $destination)})
}
foreach ($file in $nativeFiles) { Add-Output $file }
if ($HostRoot) { foreach ($file in $hostFiles) { Add-Output $file $true } }
$reportPath = Join-Path $OutputRoot 'catalog/generation.v2.json'
$reportBefore = Hash-File $reportPath
$oldInfo = $null
$oldInfoPath = Join-Path $OutputRoot 'catalog/source-info.json'
if (Test-Path -LiteralPath $oldInfoPath) {
    try {
        $oldInfo = [IO.File]::ReadAllText($oldInfoPath,$utf8) | ConvertFrom-Json
        foreach ($field in @('runtimeContractSha256','runtimeRulesSha256','initialValueSha256','bindingsSha256','device')) {
            if (!$oldInfo.PSObject.Properties[$field]) { $oldInfo = $null; break }
        }
        if ($oldInfo -and (!$oldInfo.device -or !$oldInfo.device.PSObject.Properties['name'] -or !$oldInfo.device.PSObject.Properties['type'])) { $oldInfo = $null }
    } catch { $oldInfo = $null }
}
try {
    foreach ($directory in @('catalog','App/generated','build-catalog')) { [void][IO.Directory]::CreateDirectory((Join-Path $stage $directory)) }
    if (!$CatalogPack) {
        $CatalogPack = Join-Path $repo 'tools/catalog/bin/Release/net48/Andromeda.CatalogPack.exe'
        $packSources = @('Andromeda.CatalogPack.csproj','CatalogPack.cs','CatalogSource.cs') | ForEach-Object { Join-Path $PSScriptRoot $_ }
        if (Needs-Build $CatalogPack $packSources) {
            $packDir = Join-Path $stage 'pack'
            Run-Tool 'dotnet' @('build',(Join-Path $PSScriptRoot 'Andromeda.CatalogPack.csproj'),'-c','Release','--output',$packDir,('-p:BaseIntermediateOutputPath=' + (Join-Path $stage 'pack-obj/')))
            $CatalogPack = Join-Path $packDir 'Andromeda.CatalogPack.exe'
        }
    }
    if (!$CatalogGen) {
        $CatalogGen = Join-Path $repo 'build-tools/catalog_gen.exe'
        $genSources = @('catalog_gen.c','csv.c','csv.h','CMakeLists.txt') | ForEach-Object { Join-Path $PSScriptRoot $_ }
        $genSources += @('App/catalog/bvpv_catalog.c','App/catalog/bvpv_catalog.h','App/content/bvpv_catalog_format.h','App/service/bvpv_errcode.c','App/service/bvpv_errcode.h') | ForEach-Object { Join-Path $repo $_ }
        if (Needs-Build $CatalogGen $genSources) {
            $toolDir = Join-Path $stage 'tools'
            Run-Tool 'cmake' @('-S',$PSScriptRoot,'-B',$toolDir,'-G','Ninja','-DCMAKE_C_COMPILER=gcc','-DBUILD_TESTING=OFF')
            Run-Tool 'cmake' @('--build',$toolDir,'--target','catalog_gen')
            $CatalogGen = Join-Path $toolDir 'catalog_gen.exe'
        }
    }
    $catalog = Join-Path $stage 'catalog'
    Run-Tool $CatalogPack @('source','--input',$source,'--output',$catalog)
    $info = [IO.File]::ReadAllText((Join-Path $catalog 'source-info.json'),$utf8) | ConvertFrom-Json
    $csv = Join-Path $catalog 'SW110048.catalog.csv'
    $body = Join-Path $stage 'build-catalog/SW110048.catalog.body'
    $firmware = Join-Path $stage 'App/generated/bvpv_catalog_data.c'
    Run-Tool $CatalogGen @($csv,$firmware,(Join-Path $catalog 'SW110048.tree.csv'),$body,(Join-Path $catalog 'ui-bindings.csv'),(Join-Path $catalog 'errors.csv'))
    [IO.File]::Copy((Join-Path $repo 'App/generated/bvpv_catalog_data.h'),(Join-Path $stage 'App/generated/bvpv_catalog_data.h'),$true)
    $bindings = @(Import-Csv -LiteralPath (Join-Path $catalog 'ui-bindings.csv') -Encoding UTF8)
    $header = @('/* 自动生成自 catalog/SW110048.catalog.yaml，请勿手改。 */','#ifndef BVPV_CATALOG_BINDINGS_H','#define BVPV_CATALOG_BINDINGS_H','#include <stdint.h>',
        ('#define BVPV_CATALOG_BINDING_COUNT {0}u' -f $bindings.Count),'static const struct { const char *key; uint32_t addr; } bvpv_catalog_bindings[] = {')
    foreach ($binding in $bindings) { $header += ('    {{ "{0}", {1}u }},' -f $binding.Role,$binding.Address) }
    if (!$bindings.Count) { $header += '    { "", 0u }' }
    $header += '};','#endif'
    [IO.File]::WriteAllText((Join-Path $stage 'App/generated/bvpv_catalog_bindings.h'),($header -join [char]10) + [char]10,$utf8)
    if ($HostRoot) {
        $actualHost = @(& (Join-Path $PSScriptRoot 'Generate-HostResources.ps1') -HostRoot $HostRoot -OutputRoot (Join-Path $stage 'host') -GroupsManifest (Join-Path $catalog 'groups.v2.json') -BindingsPath (Join-Path $catalog 'ui-bindings.csv'))
        if (@(Compare-Object $hostFiles $actualHost).Count) { throw '上位机生成器返回了非约定产物。' }
    }
    if ($Mode -eq 'Publish') {
        $relative = 'release/{0}-{1}.acat' -f $info.device.target,$info.device.catalogVersion
        Add-Output $relative
        [void][IO.Directory]::CreateDirectory((Join-Path $stage 'release'))
        $packageOutput = $outputs[$outputs.Count-1]
        $signArgs = @('--input',$body,'--version',$info.device.catalogVersion,'--target',$info.device.target,'--key',$KeyName,'--public-key',$PublicKey,'--output',$packageOutput.source)
        if ($packageOutput.before) { $signArgs += '--reuse-existing',$packageOutput.destination }
        Run-Tool $CatalogPack $signArgs
    }
    if ((Hash-File $source) -ne $sourceHash) { throw '生成期间 YAML 被修改，请重试。' }
    $hashes = [ordered]@{}
    foreach ($output in $outputs) {
        $hash = Hash-File $output.source
        if (!$hash) { throw "缺少完整产物：$($output.relative)" }
        $hashes[$(if ($output.host) { 'host/' + $output.relative } else { $output.relative })] = $hash
    }
    $firmwareChanged = !$oldInfo -or $oldInfo.runtimeContractSha256 -ne $info.runtimeContractSha256 -or
        $oldInfo.runtimeRulesSha256 -ne $info.runtimeRulesSha256 -or
        $oldInfo.initialValueSha256 -ne $info.initialValueSha256 -or $oldInfo.bindingsSha256 -ne $info.bindingsSha256 -or
        $oldInfo.device.name -ne $info.device.name -or $oldInfo.device.type -ne $info.device.type
    $hostChanged = @($outputs | Where-Object { $_.host -and $_.before -ne (Hash-File $_.source) }).Count -gt 0
    $report = [ordered]@{schemaVersion=3;source='SW110048.catalog.yaml';sourceSha256=$sourceHash;
        device=$info.device;parameterCount=$info.parameterCount;groupCount=$info.groupCount;
        treeRecordCount=$info.treeRecordCount;uiCount=$info.uiCount;errorCount=$info.errorCount;initialByteCount=$info.initialByteCount;
        runtimeContractSha256=$info.runtimeContractSha256;runtimeRulesSha256=$info.runtimeRulesSha256;initialValueSha256=$info.initialValueSha256;bindingsSha256=$info.bindingsSha256;files=$hashes}
    $report['requirements'] = [ordered]@{firmwareRebuild=$firmwareChanged;hostRebuild=$hostChanged;hostGenerated=[bool]$HostRoot;catalogInstallation=$true}
    $stagedReport = Join-Path $catalog 'generation.v2.json'
    [IO.File]::WriteAllText($stagedReport,($report | ConvertTo-Json -Depth 6) + [char]10,$utf8)
    if ($Mode -eq 'Check') {
        $different = @($outputs | Where-Object { $_.before -ne (Hash-File $_.source) } | ForEach-Object { $_.relative })
        if (!$reportBefore) { $different += 'catalog/generation.v2.json' }
        else {
            $storedReport = [IO.File]::ReadAllText($reportPath,$utf8) | ConvertFrom-Json
            if ($storedReport.schemaVersion -ne 3 -or $storedReport.sourceSha256 -ne $sourceHash -or
                $storedReport.runtimeContractSha256 -ne $info.runtimeContractSha256 -or
                $storedReport.runtimeRulesSha256 -ne $info.runtimeRulesSha256 -or
                $storedReport.initialValueSha256 -ne $info.initialValueSha256) { $different += 'catalog/generation.v2.json' }
            foreach ($name in $hashes.Keys) {
                $recorded = $storedReport.files.PSObject.Properties[$name]
                if (!$recorded -or $recorded.Value -ne $hashes[$name]) { $different += 'catalog/generation.v2.json'; break }
            }
        }
        if ($different.Count) { throw ('派生产物过期，请运行 Generate：' + ($different -join ', ')) }
        Write-Host "Check 通过：$($info.parameterCount) 个参数，$($info.groupCount) 个分组；正式文件未改动。"
        return
    }
    $outputs.Add([pscustomobject]@{relative='catalog/generation.v2.json';host=$false;source=$stagedReport;destination=$reportPath;before=$reportBefore})
    foreach ($output in $outputs) { if ((Hash-File $output.destination) -ne $output.before) { throw "生成期间产物被修改，停止发布：$($output.destination)" } }
    $changed = [Collections.Generic.List[object]]::new()
    try {
        foreach ($output in $outputs) {
            if ((Hash-File $output.source) -eq $output.before) { continue }
            $output | Add-Member -NotePropertyName backup -NotePropertyValue (Join-Path $stage ('backup/' + $changed.Count))
            if ($output.before) { [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output.backup)); [IO.File]::Copy($output.destination,$output.backup,$false) }
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output.destination))
            if ((Hash-File $output.destination) -ne $output.before) { throw "发布期间产物被修改：$($output.destination)" }
            $pending = $output.destination + '.catalog-' + [Guid]::NewGuid().ToString('N')
            try {
                [IO.File]::Copy($output.source,$pending,$false)
                if ($output.before) { [IO.File]::Replace($pending,$output.destination,[NullString]::Value) }
                else { [IO.File]::Move($pending,$output.destination) }
                $changed.Add($output)
            } finally { if ([IO.File]::Exists($pending)) { [IO.File]::Delete($pending) } }
        }
    } catch {
        $failure = $_
        foreach ($output in @($changed.ToArray() | Sort-Object { $_.relative -eq 'catalog/generation.v2.json' } -Descending)) {
            try {
                if ((Hash-File $output.destination) -ne (Hash-File $output.source)) { throw '回滚期间文件被外部修改。' }
                if ($output.before) { [IO.File]::Copy($output.backup,$output.destination,$true) }
                elseif ([IO.File]::Exists($output.destination)) { [IO.File]::Delete($output.destination) }
            } catch { $keepStage = $true; Write-Warning "回滚未完成：$($output.destination)；恢复备份保留在 $stage。" }
        }
        throw $failure
    }
    Write-Host "$Mode 完成：$($info.parameterCount) 个参数，$($info.groupCount) 个分组；写入 $($changed.Count) 个产物。"
    if ($firmwareChanged) { Write-Host '配套要求：运行契约、初值或编译期绑定发生变化，需要重新编译固件。' }
    if ($hostChanged) { Write-Host '配套要求：上位机自动资源发生变化，需要重新编译上位机。' }
    if (!$firmwareChanged -and !$hostChanged) { Write-Host '配套要求：目录内容变化仅需安装目录包。' }
    if (!$HostRoot) { Write-Host '未生成上位机资源；完整发布请使用 Publish 和 HostRoot。' }
    if ($Mode -eq 'Publish') { Write-Host "签名包：$($packageOutput.destination)；安装时仍由设备验证运行契约。" }
} finally {
    # 暂存路径为本次创建的随机绝对路径，删除前再次确认位于系统临时目录。
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (!$keepStage -and $stage.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
