param(
    [Parameter(Mandatory=$true)][string]$CatalogPack,
    [Parameter(Mandatory=$true)][string]$InputBody,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$Output=[IO.Path]::GetFullPath($Output)
$parent=Split-Path -Parent $Output
$stage=Join-Path $parent ('test-sign-'+[Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $stage)
try {
    $package=Join-Path $stage 'generated.acat'
    & $CatalogPack --self-test --input $InputBody --output $package
    if($LASTEXITCODE -ne 0){throw '当前源目录测试包签名或验证失败。'}
    Move-Item -LiteralPath $package -Destination $Output -Force
    Move-Item -LiteralPath ($package+'.public.bin') -Destination ($Output+'.public.bin') -Force
} finally {
    # stage 是本次调用在输出目录内创建的唯一子目录。
    $stageFull=[IO.Path]::GetFullPath($stage)
    $parentFull=[IO.Path]::GetFullPath($parent).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    if(!$stageFull.StartsWith($parentFull,[StringComparison]::OrdinalIgnoreCase)){throw '测试签名暂存路径超出输出目录。'}
    if(Test-Path -LiteralPath $stageFull){Remove-Item -LiteralPath $stageFull -Recurse -Force}
}
