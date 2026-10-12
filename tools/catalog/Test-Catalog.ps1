param(
    [Parameter(Mandatory=$true)][string]$CatalogGen,
    [Parameter(Mandatory=$true)][string]$CatalogPack,
    [ValidateSet('source_integrity','source_boundaries','deterministic','group_mapping','metadata_preservation','translation_guards','tree_guards','description_runtime','failure_protection','check_readonly','maintenance')][string]$Case,
    [string]$WorkRoot = ''
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if(!$WorkRoot){$WorkRoot=Join-Path $repo 'build-tools/catalog-validation'}
$work=Join-Path ([IO.Path]::GetFullPath($WorkRoot)) ($Case+'-'+[Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Force -Path $work)
$utf8=New-Object Text.UTF8Encoding($false)
$generator=Join-Path $PSScriptRoot 'Generate-Catalog.ps1'
$sourceFile='SW110048.catalog.yaml'
function Assert($Condition,[string]$Message){if(!$Condition){throw $Message}}
function Write-Text([string]$Path,[string]$Text){[IO.File]::WriteAllText($Path,$Text,$utf8)}
function Read-Text([string]$Path){return [IO.File]::ReadAllText($Path,$utf8).Replace("`r`n","`n")}
function Write-Rows([string]$Path,$Rows){Write-Text $Path ((($Rows|ConvertTo-Csv -NoTypeInformation)-join "`n")+"`n")}
function Generate([string]$Out,[string]$Source='', [string]$Mode='Generate', [string]$Encoder=''){
    $arguments=@{Mode=$Mode;CatalogGen=$CatalogGen;CatalogPack=$CatalogPack;OutputRoot=$Out}
    if($Source){$arguments.SourceRoot=$Source}
    if($Encoder){$arguments.CatalogGen=$Encoder}
    & $generator @arguments | Out-Null
}
function Copy-Source([string]$Name){
    $dest=Join-Path $work $Name
    [void](New-Item -ItemType Directory -Path $dest)
    Copy-Item -LiteralPath (Join-Path $repo ('catalog/'+$sourceFile)) -Destination (Join-Path $dest $sourceFile)
    return $dest
}
function Patch-Field([string]$Source,[string]$Field,[int]$Index,[string]$Value){
    $path=Join-Path $Source $sourceFile;$text=Read-Text $path
    $matches=[regex]::Matches($text,('(?m)^([ \t]*(?:-[ \t]+)?'+[regex]::Escape($Field)+':[ \t]*)([^\n]*)$'))
    Assert ($matches.Count -gt $Index) "测试字段不存在：$Field/$Index"
    $m=$matches[$Index]
    Write-Text $path ($text.Substring(0,$m.Index)+$m.Groups[1].Value+$Value+$text.Substring($m.Index+$m.Length))
}
function Expect-SourceFailure([string]$Source,[string]$Name){
    $out=Join-Path $work $Name;$failed=$false
    try{Generate $out $Source}catch{$failed=$true}
    Assert $failed "错误来源未拒绝：$Name"
    Assert (!(Test-Path -LiteralPath (Join-Path $out 'App/generated/bvpv_catalog_data.c'))) "错误来源发布了部分目录：$Name"
}
function Snapshot([string]$Root){
    $result=@{}
    if(Test-Path -LiteralPath $Root){foreach($file in Get-ChildItem -LiteralPath $Root -File -Recurse){$relative=$file.FullName.Substring($Root.Length);$result[$relative]=(Get-FileHash -LiteralPath $file.FullName).Hash}}
    return $result
}
function Assert-Snapshot($Before,[string]$Root){
    $after=Snapshot $Root
    Assert ($Before.Count -eq $after.Count) '正式产物集合被改变。'
    foreach($key in $Before.Keys){Assert ($after.ContainsKey($key) -and $Before[$key] -eq $after[$key]) "正式产物被改变：$key"}
}
function Value-Buffers([string]$Path){
    $text=Read-Text $Path
    return @([regex]::Matches($text,'(?m)^static uint8_t val_\d+\[\d+\] = .*;')|ForEach-Object{$_.Value})
}
function Patch-NumericParameter([string]$Source,[string]$Section,[string]$Field,[string]$Value){
    $path=Join-Path $Source $sourceFile;$text=Read-Text $path
    $types=[regex]::Matches($text,'(?m)^([ \t]*)type:[ \t]*["'']?(UINT8|UINT16|UINT32|FLOAT32)["'']?[ \t]*$')
    Assert ($types.Count -gt 0) '没有数值参数可用于测试。'
    $type=$types[0];$indent=$type.Groups[1].Value.Length
    $end=[regex]::Match($text.Substring($type.Index+$type.Length),'(?m)^[ \t]{0,'+[string]$indent+'}(?:-[ \t]+)?(?:name|key):')
    $length=if($end.Success){$type.Length+$end.Index}else{$text.Length-$type.Index}
    $block=$text.Substring($type.Index,$length)
    $pattern='(?ms)(^[ \t]*'+[regex]::Escape($Section)+':[ \t]*\n(?:(?!^[ \t]*[A-Za-z]+:[ \t]*\n).)*?^[ \t]*'+[regex]::Escape($Field)+':[ \t]*)[^\n]*'
    $match=[regex]::Match($block,$pattern)
    if($match.Success){$changed=$block.Substring(0,$match.Index)+$match.Groups[1].Value+$Value+$block.Substring($match.Index+$match.Length)}
    else {
        $sectionMatch=[regex]::Match($block,'(?m)^([ \t]*)'+[regex]::Escape($Section)+':[ \t]*\n')
        Assert $sectionMatch.Success "数值参数缺少$Section"
        $at=$sectionMatch.Index+$sectionMatch.Length
        $changed=$block.Substring(0,$at)+$sectionMatch.Groups[1].Value+'  '+$Field+': '+$Value+"`n"+$block.Substring($at)
    }
    Write-Text $path ($text.Substring(0,$type.Index)+$changed+$text.Substring($type.Index+$length))
}
if($Case -eq 'source_integrity'){
    $source=Copy-Source 'unknown';$path=Join-Path $source $sourceFile;Write-Text $path ("unknownField: true`n"+(Read-Text $path));Expect-SourceFailure $source 'unknown_field'
    $source=Copy-Source 'duplicate_yaml';$path=Join-Path $source $sourceFile;Write-Text $path ((Read-Text $path)+"`nschemaVersion: 1`n");Expect-SourceFailure $source 'duplicate_yaml_key'
    $source=Copy-Source 'duplicate_address';$text=Read-Text (Join-Path $source $sourceFile);$address=[regex]::Match($text,'(?m)^[ \t]*address:[ \t]*([^\n]+)$').Groups[1].Value;Patch-Field $source 'address' 1 $address;Expect-SourceFailure $source 'duplicate_address'
    $source=Copy-Source 'bad_type';Patch-Field $source 'type' 1 'NO_SUCH_TYPE';Expect-SourceFailure $source 'invalid_type'
    $source=Copy-Source 'bad_address';Patch-Field $source 'address' 0 '"0x1000000"';Expect-SourceFailure $source 'invalid_address'
    $source=Copy-Source 'bad_initial';Patch-NumericParameter $source 'runtime' 'initial' '"999999999999999999999999999999"';Expect-SourceFailure $source 'unrepresentable_initial'
}
elseif($Case -eq 'source_boundaries'){
    $head=@'
schemaVersion: 1
device:
  catalogId: SW110048
  sourceVersion: TEST
  firmwareVersion: TEST.1
  catalogVersion: 1.0.0
  target: BV500-F446
  type: 2
  name: BV500-USB
tree:
  - key: system
    title:
      zh-CN: 系统
      en-US: System
    children:
'@
    $tail="uiBindings: []`nerrors:`n  - code: `"0x00`"`n    text: OK`n"
    $identity=@'
      - name: FirmwareVersion
        address: "0x640105"
        type: TEXT16
        elementCount: 1
        elementSize: 16
        runtime:
          access: ROO
          initial: TEST.1
          visible: true
        description:
          default: TEST.1
'@
    function Tiny-Parameter([int]$Index=1){
        $address='0x'+$Index.ToString('X6')
        return @"
      - name: One$Index
        address: "$address"
        type: UINT16
        elementCount: 1
        elementSize: 2
        runtime:
          access: RWA
          initial: "0"
          min: "0"
          max: "100"
          visible: true
        description:
          default: "0"
"@+"`n"
    }
    function Expect-Tiny([string]$Name,[string]$Parameter,[bool]$Valid=$false,[string]$TailText=$tail){
        $path=Join-Path $work ($Name+'.yaml');Write-Text $path ($head+"`n"+$Parameter+$identity+"`n"+$TailText)
        $ErrorActionPreference='Continue';$messages=@(& $CatalogPack source --input $path 2>&1);$code=$LASTEXITCODE;$ErrorActionPreference='Stop'
        Assert (($code -eq 0) -eq $Valid) "YAML边界校验错误：$Name $($messages -join ' ')"
        if(!$Valid){Assert (($messages -join "`n") -match '\.yaml:\d+:\d+:') "YAML错误未包含源位置：$Name"}
    }
    function Encode-Tiny([string]$Name,[bool]$LeadingZero=$false){
        $out=Join-Path $work ($Name+'_encoder');[void](New-Item -ItemType Directory -Path $out)
        & $CatalogPack source --input (Join-Path $work ($Name+'.yaml')) --output $out | Out-Null
        Assert ($LASTEXITCODE -eq 0) "$Name 源导出失败。"
        $csv=Join-Path $out 'SW110048.catalog.csv'
        if($LeadingZero){
            $rows=@(Import-Csv -LiteralPath $csv -Encoding UTF8)
            $rows[0].Default='010';$rows[0].Min='08';$rows[0].Max='010';$rows[0].RuntimeMin='08';$rows[0].RuntimeMax='010'
            Write-Rows $csv $rows
        }
        $c=Join-Path $out ($Name+'.c');$body=Join-Path $out ($Name+'.body')
        & $CatalogGen $csv $c (Join-Path $out 'SW110048.tree.csv') $body (Join-Path $out 'ui-bindings.csv') (Join-Path $out 'errors.csv') 2>&1 | Out-Null
        Assert ($LASTEXITCODE -eq 0) "$Name 二进制编码失败。"
        return [pscustomobject]@{Text=(Read-Text $c);Bytes=[IO.File]::ReadAllBytes($body);Body=$body;Rows=@(Import-Csv -LiteralPath $csv -Encoding UTF8)}
    }
    $param=Tiny-Parameter
    Expect-Tiny 'valid' $param $true
    $decimal=$param.Replace('initial: "0"','initial: "08"').Replace('min: "0"','min: "08"').Replace('max: "100"','max: "010"').Replace('default: "0"','default: "010"')
    Expect-Tiny 'leading_zero' $decimal $true
    $decimalEncoded=Encode-Tiny 'leading_zero' $true
    Assert ($decimalEncoded.Text.Contains('val_0[2] = { 0x08u, 0x00u }') -and $decimalEncoded.Text.Contains('.min = { 0x08u, 0x00u }, .max = { 0x0Au, 0x00u }')) '带前导零的十进制初值或硬范围被当作八进制。'
    $extendedAt=14+[int]$decimalEncoded.Bytes[11]
    Assert ([BitConverter]::ToString($decimalEncoded.Bytes,$extendedAt,15) -eq '01-02-00-08-00-02-02-00-0A-00-03-02-00-0A-00') '带前导零的目录min/max/default没有按十进制编码。'
    & $CatalogPack --self-test --input $decimalEncoded.Body | Out-Null
    Assert ($LASTEXITCODE -eq 0) '前导零目录签名自检失败。'
    $spaces=$param.Replace('type: UINT16','type: TEXT8').Replace('elementSize: 2','elementSize: 8').Replace('initial: "0"','initial: " hi "').Replace('default: "0"','default: " hi "').Replace('          min: "0"'+"`n",'').Replace('          max: "100"'+"`n",'')+'          unit: " x "'+"`n"
    Expect-Tiny 'text_spaces' $spaces $true
    $spacesEncoded=Encode-Tiny 'text_spaces';$extendedAt=14+[int]$spacesEncoded.Bytes[11]
    Assert ($spacesEncoded.Rows[0].InitialBytes -eq '2068692000000000') '文本初值两侧空格在源导出时丢失。'
    Assert ([BitConverter]::ToString($spacesEncoded.Bytes,$extendedAt,17) -eq '03-08-00-20-68-69-20-00-00-00-00-05-03-00-20-78-20') '目录文本默认描述或单位两侧空格被C编码器裁剪。'
    $array=$param.Replace('elementCount: 1','elementCount: 2').Replace('initial: "0"','initial: ["010", "11"]').Replace('default: "0"','default: "010"')
    Expect-Tiny 'array' $array $true
    $arrayEncoded=Encode-Tiny 'array'
    Assert ($arrayEncoded.Rows[0].InitialBytes -eq '0A000B00' -and $arrayEncoded.Text.Contains('val_0[4] = { 0x0Au, 0x00u, 0x0Bu, 0x00u }')) '多元素初值未保持完整字节。'
    $fixed=$param.Replace('type: UINT16','type: FIXED16.16').Replace('elementSize: 2','elementSize: 4').Replace('initial: "0"','initial: "-0.25"').Replace('min: "0"','min: "-1"')
    Expect-Tiny 'fixed_negative' $fixed $true
    $fixedEncoded=Encode-Tiny 'fixed_negative'
    Assert ($fixedEncoded.Rows[0].InitialBytes -eq '00C0FFFF') '负定点初始化字节错误。'
    $floating=$param.Replace('type: UINT16','type: FLOAT32').Replace('elementSize: 2','elementSize: 4').Replace('min: "0"','min: "-3e38"').Replace('max: "100"','max: "3e38"')
    Expect-Tiny 'float_large_range' $floating $true
    Expect-Tiny 'overflow' ($param.Replace('initial: "0"','initial: "65536"'))
    Expect-Tiny 'missing_initial' ($param.Replace('          initial: "0"'+"`n",''))
    Expect-Tiny 'bad_type' ($param.Replace('type: UINT16','type: UINT17'))
    Expect-Tiny 'size_mismatch' ($param.Replace('elementSize: 2','elementSize: 4'))
    Expect-Tiny 'zero_count' ($param.Replace('elementCount: 1','elementCount: 0'))
    Expect-Tiny 'widen_range' ($param.Replace('default: "0"','default: "0"'+"`n"+'          min: "-1"'))
    Expect-Tiny 'widen_access' ($param.Replace('access: RWA','access: ROO').Replace('default: "0"','default: "0"'+"`n"+'          access: RWA'))
    Expect-Tiny 'widen_visibility' ($param.Replace('visible: true','visible: false').Replace('default: "0"','default: "0"'+"`n"+'          visible: true'))
    Expect-Tiny 'unknown_role' $param $false "uiBindings:`n  - role: button.execute-script`n    parameter: One1`n    order: 1`nerrors: []`n"
    Expect-Tiny 'unknown_reference' $param $false "uiBindings:`n  - role: status.product`n    parameter: Missing`n    order: 1`nerrors: []`n"
    $enums="          enums:`n"+((0..32|ForEach-Object{'            - value: "'+$_+'"'+"`n"+'              text: "item"'})-join "`n")+"`n"
    Expect-Tiny 'enum_count' ($param+$enums)
    Expect-Tiny 'unit_255' ($param+'          unit: "'+('界'*85)+'"'+"`n") $true
    Expect-Tiny 'unit_258' ($param+'          unit: "'+('界'*86)+'"'+"`n")
    Expect-Tiny 'enum_utf8_258' ($param+"          enums:`n            - value: `"0`"`n              text: `""+('界'*86)+"`"`n")
    $many=((1..2301|ForEach-Object{Tiny-Parameter $_})-join '')
    Expect-Tiny 'parameter_count' $many
    $restricted=((1..65|ForEach-Object{(Tiny-Parameter $_).Replace('default: "0"','default: "0"'+"`n"+'          min: "1"')})-join '')
    Expect-Tiny 'range_overrides' $restricted
    $enums="          enums:`n"+((0..27|ForEach-Object{'            - value: "'+$_+'"'+"`n"+'              text: "'+('x'*90)+'"'})-join "`n")+"`n"
    $large=((1..450|ForEach-Object{(Tiny-Parameter $_)+$enums})-join '')
    Expect-Tiny 'catalog_slot' $large
}
elseif($Case -eq 'deterministic'){
    $first=Join-Path $work 'first';$second=Join-Path $work 'second';Generate $first;Generate $second
    foreach($relative in @('catalog/SW110048.catalog.csv','catalog/SW110048.tree.csv','catalog/groups.v2.json','App/generated/bvpv_catalog_data.c','App/generated/bvpv_catalog_bindings.h','build-catalog/SW110048.catalog.body','build-catalog/SW110048.catalog.body.json','build-catalog/SW110048.catalog.body.runtime')){
        Assert ((Get-FileHash -LiteralPath (Join-Path $first $relative)).Hash -eq (Get-FileHash -LiteralPath (Join-Path $second $relative)).Hash) "重复生成不一致：$relative"
        if($relative -notlike 'build-catalog/*'){Assert ((Read-Text (Join-Path $first $relative)) -ceq (Read-Text (Join-Path $repo $relative))) "仓库生成文件过期：$relative"}
    }
}
elseif($Case -eq 'group_mapping'){
    Generate $work
    $tree=@(Import-Csv -LiteralPath (Join-Path $work 'catalog/SW110048.tree.csv') -Encoding UTF8)
    $rows=@(Import-Csv -LiteralPath (Join-Path $work 'catalog/SW110048.catalog.csv') -Encoding UTF8)
    $groups=[IO.File]::ReadAllText((Join-Path $work 'catalog/groups.v2.json'))|ConvertFrom-Json
    $seen=@{'0'=$true};$addresses=@{};$keys=@{}
    foreach($record in $tree){
        Assert ($seen.ContainsKey($record.ParentId)) '父节点必须先于子节点。'
        if($record.Kind -eq '1'){
            Assert (!$seen.ContainsKey($record.NodeId) -and !$keys.ContainsKey($record.Key)) '分组重复。'
            $seen[$record.NodeId]=$true;$keys[$record.Key]=$true
        }else{Assert ($record.Kind -eq '2' -and !$addresses.ContainsKey($record.Address)) '参数非法或重复归属。';$addresses[$record.Address]=$true}
    }
    foreach($row in $rows){Assert ($addresses.ContainsKey($row.Address)) "参数未归属树：$($row.Name)"}
    foreach($group in $groups.groups){Assert ($keys.ContainsKey($group.key) -and $group.'zh-CN' -and $group.'en-US') '分组双语资源不完整。'}
    Assert ($addresses.Count -eq $rows.Count -and $keys.Count -eq @($groups.groups).Count -and $tree.Count -eq $rows.Count+$keys.Count) '树和参数集合不一致。'
}
elseif($Case -eq 'maintenance'){
    $baseline=Join-Path $work 'baseline';Generate $baseline
    $source=Copy-Source 'maintenance_source';$path=Join-Path $source $sourceFile
    Patch-Field $source 'key' 0 '"maintenance"';Patch-Field $source 'zh-CN' 0 '"维护分组"';Patch-Field $source 'en-US' 0 '"Maintenance group"'
    $text=Read-Text $path
    $nodes=@()
    foreach($m in [regex]::Matches($text,'(?m)^([ \t]*)name:[ \t]*[^\n]*$')){
        if($m.Groups[1].Value.Length -le 4){continue}
        $bullet=[regex]::Match($text.Substring(0,$m.Index),'(?m)^([ \t]*)-[ \t]*\n\z')
        if($bullet.Success -and $bullet.Groups[1].Value.Length+2 -eq $m.Groups[1].Value.Length){$nodes+=@([pscustomobject]@{Start=$bullet.Index;Name=$m;Indent=$bullet.Groups[1].Value.Length})}
        if($nodes.Count -eq 2){break}
    }
    Assert ($nodes.Count -eq 2 -and $nodes[0].Indent -eq $nodes[1].Indent) '迁移源前两个参数不是同组。'
    $scanAt=$nodes[1].Name.Index+$nodes[1].Name.Length+1
    $next=[regex]::Match($text.Substring($scanAt),'(?m)^[ \t]{0,'+$nodes[1].Indent+'}-[ \t]*\n')
    Assert $next.Success '没有可识别的参数结尾。'
    $end=$scanAt+$next.Index
    $first=$text.Substring($nodes[0].Start,$nodes[1].Start-$nodes[0].Start)
    $second=$text.Substring($nodes[1].Start,$end-$nodes[1].Start)
    $swapped=$text.Substring(0,$nodes[0].Start)+$second+$first+$text.Substring($end)
    $indent=' '*$nodes[0].Indent;$mapIndent=$indent+'  '
    $addedGroup=$indent+"-`n"+$mapIndent+'key: "maintenance.temporary"'+"`n"+$mapIndent+"title:`n"+$mapIndent+'  zh-CN: "临时组"'+"`n"+$mapIndent+'  en-US: "Temporary group"'+"`n"+$mapIndent+"children:`n"
    $moved=[regex]::Replace($second+$first,'(?m)^(?=.)','    ')
    Write-Text $path ($text.Substring(0,$nodes[0].Start)+$addedGroup+$moved+$text.Substring($end))
    $changed=Join-Path $work 'changed';Generate $changed $source
    $tree=@(Import-Csv -LiteralPath (Join-Path $changed 'catalog/SW110048.tree.csv') -Encoding UTF8)
    $oldTree=@(Import-Csv -LiteralPath (Join-Path $baseline 'catalog/SW110048.tree.csv') -Encoding UTF8)
    $before=@($oldTree|Where-Object Kind -eq '2');$after=@($tree|Where-Object Kind -eq '2')
    Assert ($tree[0].Key -eq 'maintenance' -and $after[0].Address -eq $before[1].Address -and $after[1].Address -eq $before[0].Address) '仅编辑YAML没有更新树key或顺序。'
    $groups=[IO.File]::ReadAllText((Join-Path $changed 'catalog/groups.v2.json'))|ConvertFrom-Json
    Assert ($groups.groups[0].'zh-CN' -eq '维护分组' -and $groups.groups[0].'en-US' -eq 'Maintenance group') '双语标题未派生。'
    Assert (@($groups.groups|Where-Object key -eq 'maintenance.temporary').Count -eq 1 -and $tree.Count -eq $oldTree.Count+1) '新增分组没有自动计算编号与数量。'
    $initialBefore=@(Import-Csv -LiteralPath (Join-Path $baseline 'catalog/SW110048.catalog.csv') -Encoding UTF8|Sort-Object Address|ForEach-Object{$_.Address+':'+$_.InitialBytes})
    $initialAfter=@(Import-Csv -LiteralPath (Join-Path $changed 'catalog/SW110048.catalog.csv') -Encoding UTF8|Sort-Object Address|ForEach-Object{$_.Address+':'+$_.InitialBytes})
    Assert (($initialBefore -join "`n") -ceq ($initialAfter -join "`n")) '树维护改变了复位初始化字节。'
    Write-Text $path $swapped
    Generate $changed $source
    $removedTree=@(Import-Csv -LiteralPath (Join-Path $changed 'catalog/SW110048.tree.csv') -Encoding UTF8)
    Assert ($removedTree.Count -eq $oldTree.Count -and @($removedTree|Where-Object Key -eq 'maintenance.temporary').Count -eq 0) '删除分组需要人工修改编号或数量。'
}
elseif($Case -eq 'metadata_preservation'){
    Generate $work
    $new=@(Import-Csv -LiteralPath (Join-Path $work 'catalog/SW110048.catalog.csv') -Encoding UTF8)
    $old=@(Import-Csv -LiteralPath (Join-Path $repo 'catalog/SW110048.catalog.csv') -Encoding UTF8)
    Assert ($new.Count -eq $old.Count) '已提交目录过期。'
    $byAddress=@{};foreach($row in $new){$byAddress[$row.Address]=$row}
    foreach($row in $old){foreach($field in @('Name','Type','Access','Default','Min','Max','Unit','Enum','Visible','Widget','Remark','RuntimeAccess','RuntimeMin','RuntimeMax','RuntimeVisible','InitialBytes','ElementCount','ElementSize')){Assert ($byAddress[$row.Address].$field -ceq $row.$field) "有效数据变化：$($row.Name).$field"}}
    $actual=Value-Buffers (Join-Path $work 'App/generated/bvpv_catalog_data.c');$stored=Value-Buffers (Join-Path $repo 'App/generated/bvpv_catalog_data.c')
    Assert ($actual.Count -eq $new.Count -and ($actual -join "`n") -ceq ($stored -join "`n")) '全部复位初始化缓冲应与当前已提交源一致。'
    $c=Read-Text (Join-Path $work 'App/generated/bvpv_catalog_data.c')
    Assert ($c.Contains('bvpv_runtime_param_t g_runtime') -and !$c.Contains('bvpv_param_t g_params') -and !$c.Contains('g_tree[') -and !$c.Contains('.name =')) '固件重新链接了完整描述表。'
}
elseif($Case -eq 'translation_guards'){
    $source=Copy-Source 'missing_translation';Patch-Field $source 'en-US' 0 '""';Expect-SourceFailure $source 'missing_translation'
    $source=Copy-Source 'non_ascii';Patch-Field $source 'key' 0 '"系统"';Expect-SourceFailure $source 'non_ascii_key'
    $source=Copy-Source 'duplicate_group';$text=Read-Text (Join-Path $source $sourceFile);$key=[regex]::Match($text,'(?m)^[ \t]*(?:-[ \t]+)?key:[ \t]*([^\n]+)$').Groups[1].Value;Patch-Field $source 'key' 1 $key;Expect-SourceFailure $source 'duplicate_group'
}
elseif($Case -eq 'description_runtime'){
    $baseline=Join-Path $work 'baseline';Generate $baseline
    $source=Copy-Source 'description';Patch-NumericParameter $source 'description' 'default' '"1"';$changed=Join-Path $work 'description_out';Generate $changed $source
    Assert (((Value-Buffers (Join-Path $baseline 'App/generated/bvpv_catalog_data.c')) -join "`n") -ceq ((Value-Buffers (Join-Path $changed 'App/generated/bvpv_catalog_data.c')) -join "`n")) '修改默认描述改变了初始化值。'
    Assert ((Get-FileHash -LiteralPath (Join-Path $baseline 'build-catalog/SW110048.catalog.body')).Hash -ne (Get-FileHash -LiteralPath (Join-Path $changed 'build-catalog/SW110048.catalog.body')).Hash) '修改默认描述未改变目录。'
    $source=Copy-Source 'runtime';Patch-NumericParameter $source 'runtime' 'initial' '"1"';$changed=Join-Path $work 'runtime_out';Generate $changed $source
    Assert (((Value-Buffers (Join-Path $baseline 'App/generated/bvpv_catalog_data.c')) -join "`n") -cne ((Value-Buffers (Join-Path $changed 'App/generated/bvpv_catalog_data.c')) -join "`n")) '修改初值未改变编译初始化值。'
    $source=Copy-Source 'widen';Patch-NumericParameter $source 'description' 'min' '"-999999"';Expect-SourceFailure $source 'widen_range'
    $source=Copy-Source 'widen_access';Patch-NumericParameter $source 'description' 'access' 'RWA';Expect-SourceFailure $source 'widen_access'
}
elseif($Case -eq 'failure_protection'){
    $out=Join-Path $work 'published';Generate $out;$before=Snapshot $out
    $failed=$false;try{Generate $out '' 'Generate' (Join-Path $work 'missing_encoder.exe')}catch{$failed=$true}
    Assert $failed '缺失编码器未失败。';Assert-Snapshot $before $out
    $failed=$false;try{Generate $out '' 'Generate' $CatalogPack}catch{$failed=$true}
    Assert $failed '编码工具返回非零未失败。';Assert-Snapshot $before $out
    $source=Copy-Source 'bad_source';Patch-Field $source 'address' 0 '"bad"';$failed=$false;try{Generate $out $source}catch{$failed=$true};Assert $failed '错误输入未失败。';Assert-Snapshot $before $out
    $failed=$false;try{& $generator -Mode Publish -CatalogGen $CatalogGen -CatalogPack $CatalogPack -OutputRoot $out -KeyName 'NO_SUCH_KEY' | Out-Null}catch{$failed=$true};Assert $failed '缺少HostRoot的发布未失败。';Assert-Snapshot $before $out
    $commitOut=Join-Path $work 'commit_failure';[void](New-Item -ItemType Directory -Path $commitOut)
    Write-Text (Join-Path $commitOut 'firmware') '原文件必须保留'
    $commitBefore=Snapshot $commitOut
    $failed=$false;try{Generate $commitOut}catch{$failed=$true}
    Assert $failed '提交期间目录路径被文件占用时未失败。'
    Assert-Snapshot $commitBefore $commitOut
    Assert (!(Test-Path -LiteralPath (Join-Path $commitOut 'catalog/generation.v2.json'))) '部分提交失败仍发布了报告。'
}
elseif($Case -eq 'check_readonly'){
    $out=Join-Path $work 'published';Generate $out;$before=Snapshot $out
    $sourceBefore=(Get-FileHash -LiteralPath (Join-Path $repo ('catalog/'+$sourceFile))).Hash
    Generate $out '' 'Check';Assert-Snapshot $before $out
    Assert ($sourceBefore -eq (Get-FileHash -LiteralPath (Join-Path $repo ('catalog/'+$sourceFile))).Hash) 'Check改动了维护源。'
    $path=Join-Path $out 'catalog/SW110048.tree.csv';[IO.File]::AppendAllText($path,"`n",$utf8);$before=Snapshot $out
    $failed=$false;try{Generate $out '' 'Check'}catch{$failed=$true};Assert $failed 'Check未报告过期产物。';Assert-Snapshot $before $out
}
elseif($Case -eq 'tree_guards'){
    $catalog=Join-Path $work 'tiny.catalog.csv';Write-Text $catalog "Name,Address,Type,Access`nOne,0x010101,UINT8,RWA`nTwo,0x010102,UINT8,ROO`n"
    $header='Kind,NodeId,ParentId,Key,Address';$good=@($header,'1,1,0,system,','1,2,1,system.identity,','2,0,2,,0x010101','2,0,1,,0x010102')
    $variants=[ordered]@{
        valid=$good
        invalid_kind=@($header,'7,1,0,system,','2,0,0,,0x010101','2,0,0,,0x010102')
        duplicate_node=@($good[0],$good[1],'1,1,0,other,',$good[3],$good[4])
        unknown_parent=@($good[0],'1,1,9,system,',$good[2],$good[3],$good[4])
        cycle=@($good[0],'1,1,2,system,',$good[2],$good[3],$good[4])
        duplicate_parameter=@($good[0],$good[1],$good[2],$good[3],$good[3])
        missing_parameter=@($good[0],$good[1],$good[2],$good[3])
        unknown_parameter=@($good[0],$good[1],$good[2],$good[3],'2,0,1,,0x010103')
        truncated_record=@($good[0],$good[1],$good[2],$good[3],'2,0,1,')
        invalid_number=@($good[0],'1,1,bad,system,',$good[2],$good[3],$good[4])
        negative_number=@($good[0],'1,1,-0,system,',$good[2],$good[3],$good[4])
        duplicate_key=@($good[0],$good[1],'1,2,1,system,',$good[3],$good[4])
    }
    foreach($entry in $variants.GetEnumerator()){
        $tree=Join-Path $work ($entry.Key+'.csv');$output=Join-Path $work ($entry.Key+'.c');Write-Text $tree (($entry.Value -join "`n")+"`n")
        $ErrorActionPreference='Continue'; & $CatalogGen $catalog $output $tree 2>&1 | Out-Null;$code=$LASTEXITCODE;$ErrorActionPreference='Stop'
        Assert (($code -eq 0) -eq ($entry.Key -eq 'valid')) "树校验错误：$($entry.Key)"
        Assert ((Test-Path -LiteralPath $output) -eq ($entry.Key -eq 'valid')) "非法树发布部分文件：$($entry.Key)"
    }
}
Write-Output "PASS catalog_$Case"
