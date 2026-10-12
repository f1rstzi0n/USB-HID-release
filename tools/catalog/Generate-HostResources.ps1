[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$HostRoot,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Parameter(Mandatory=$true)][string]$GroupsManifest,
    [Parameter(Mandatory=$true)][string]$BindingsPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$HostRoot = [IO.Path]::GetFullPath($HostRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
if ($OutputRoot.Equals($HostRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $OutputRoot.StartsWith($HostRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    $HostRoot.StartsWith($OutputRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw '上位机生成必须使用原工作区之外的隔离目录。'
}
$utf8 = [Text.UTF8Encoding]::new($false, $true)
function Read-Normalized([string]$Path) { return [IO.File]::ReadAllText($Path, $utf8).Replace("`r`n", "`n") }
function Stage-File([string]$Source, [string]$Relative) {
    $destination = Join-Path $OutputRoot $Relative
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
    [IO.File]::Copy($Source, $destination, $true)
}
function Run-Logged([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "上位机生成工具失败：$Program，退出码 $LASTEXITCODE。" }
}
[void][IO.Directory]::CreateDirectory($OutputRoot)
Copy-Item -LiteralPath (Join-Path $HostRoot 'localization') -Destination $OutputRoot -Recurse -Force
$unrelatedModules = @{}
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $HostRoot 'localization/modules') -Filter '*.json' -File) {
    if ($file.Name -ne 'BvpvUsbTree.json') { $unrelatedModules[$file.Name] = Read-Normalized $file.FullName }
}
# CLI check 检查实际源码引用；只复制扫描器读取的文本，不带 bin/obj。
foreach ($relative in @('CPA4','Other/CPALocalization/CPA4.Localization','Other/Shared',
        'Other/CPAUpdate/CPA4.Update','Other/CPAFirmware/CPA4.Firmware','Tools/CPA4.Localization.Desktop')) {
    $directory = Join-Path $HostRoot $relative
    if (!(Test-Path -LiteralPath $directory -PathType Container)) { continue }
    $files = @(& rg --files --glob '*.cs' --glob '*.xaml' --glob '*.razor' --glob '*.cshtml' --glob '!**/bin/**' --glob '!**/obj/**' $directory)
    if ($LASTEXITCODE -gt 1) { throw "无法枚举上位机源码：$relative" }
    foreach ($file in $files) { Stage-File $file $file.Substring($HostRoot.Length + 1) }
}
$importRelative = 'scripts/Import-UsbTreeLocalization.ps1'
Stage-File (Join-Path $HostRoot $importRelative) $importRelative
& (Join-Path $OutputRoot $importRelative) -Manifest $GroupsManifest | ForEach-Object { Write-Host $_ }

$bindings = @(Import-Csv -LiteralPath $BindingsPath -Encoding UTF8)
$roles = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$lines = [Collections.Generic.List[string]]::new()
foreach ($line in @('// 自动生成：来自 USB-HID 单一参数目录；目录动作仍由内置业务流程执行。',
        'using System;', 'using System.Collections.Generic;', 'using System.Collections.ObjectModel;', '',
        'namespace Vat.ValveControl.Communication.Bvpv.Usb', '{',
        '    internal static class AndromedaCatalogBindings', '    {',
        '        internal static readonly IReadOnlyDictionary<string, uint> RoleAddresses =',
        '            new ReadOnlyDictionary<string, uint>(new Dictionary<string, uint>(StringComparer.Ordinal)', '            {')) {
    $lines.Add($line)
}
foreach ($binding in $bindings) {
    if ($binding.Role -cnotmatch '^[a-z][a-z0-9.-]{0,62}$' -or !$roles.Add($binding.Role) -or
        $binding.Address -cnotmatch '^0x[0-9A-Fa-f]{6}$') { throw '生成的主窗口绑定包含非法或重复角色/地址。' }
    $lines.Add(('                {{"{0}", {1}u}},' -f $binding.Role, $binding.Address.ToUpperInvariant().Replace('0X','0x')))
}
foreach ($line in @('            });', '    }', '}')) { $lines.Add($line) }
$bindingRelative = 'Other/CPACommunication/CPA4.Communication/Bvpv/Usb/Generated/AndromedaCatalogBindings.g.cs'
$bindingOutput = Join-Path $OutputRoot $bindingRelative
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($bindingOutput))
[IO.File]::WriteAllText($bindingOutput, ($lines -join "`n") + "`n", $utf8)

$cliProject = Join-Path $HostRoot 'Tools/CPA4.Localization.Cli/CPA4.Localization.Cli.csproj'
Run-Logged 'dotnet' @('build', $cliProject, '-c', 'Debug', '-f', 'net48', '-p:Platform=x86', '-m:1', '-nodeReuse:false', '--verbosity', 'quiet')
$cli = Join-Path $HostRoot 'Tools/CPA4.Localization.Cli/bin/x86/Debug/net48/win-x86/cpa4-l10n.exe'
foreach ($command in @('validate','generate','check')) { Run-Logged $cli @($command, '--root', $OutputRoot) }
foreach ($name in $unrelatedModules.Keys) {
    if ((Read-Normalized (Join-Path $OutputRoot "localization/modules/$name")) -cne $unrelatedModules[$name]) {
        throw "本地化工具改写了无关模块，未发布：$name"
    }
}
@(
    'localization/modules/BvpvUsbTree.json'
    $bindingRelative
    'Other/CPALocalization/CPA4.Localization/Resources/Strings.resx'
    'Other/CPALocalization/CPA4.Localization/Resources/Strings.zh-CN.resx'
    'Other/CPALocalization/CPA4.Localization/Generated/StringKeys.g.cs'
    'localization/generated/catalog-summary.json'
    'localization/generated/generation.json'
)
