# 参数目录构建与发布

唯一维护源是 `catalog/SW110048.catalog.yaml`，维护字段和操作见 [目录说明](../../docs/reference/CATALOG.md)。`Generate-Catalog.ps1` 负责 YAML 检查、C 数据和目录编码、可选 Andromeda 资源，以及签名发布。CSV/tree 是兼容编译产物。正常固件只链接紧凑运行契约和初始化数据，名称、树、枚举、单位及描述放在外部目录。

## 构建和回归

```powershell
cmake -S tools/catalog -B build-tools -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build-tools
ctest --test-dir build-tools --output-on-failure
tools/catalog/Generate-Catalog.ps1 -Mode Check
```

CMake 在构建目录编译 .NET Framework 4.8 工具，使用固定 YamlDotNet 17.0.0 解析源，然后从当前 YAML 生成隔离产物。首次构建需要还原该 NuGet 包；可用 CMake `-DCATALOG_PACK=<已有exe>` 指定已构建的工具。源码编码器仍为既有 C `catalog_gen`。回归覆盖源校验、确定性、树归属、元数据与初值、描述和运行规则分离、失败保护及 Check 的只读行为。

```powershell
tools/catalog/Generate-Catalog.ps1 -Mode Generate -HostRoot ../CPA_Dnstest1
tools/catalog/Generate-Catalog.ps1 -Mode Publish -HostRoot ../CPA_Dnstest1 -KeyName EXISTING_CNG_KEY_NAME
```

发布使用已有 Windows CNG ECDSA P-256 命名钥，校验公钥配对，不创建生产钥或导出私钥。已有内容相同且签名有效的包可以复用，不覆盖不同内容的发布文件。暂存中的编码、语言资源校验或签名失败时，正式产物保留或恢复；生成报告最后提交。

## 编码与测试接缝

`CatalogPack.exe source --input FILE.yaml --output STAGE/catalog` 检查 YAML 并导出编码输入。`catalog_gen` 复用类型和值的编码与目录索引实现，完整输入由主脚本传递。旧 wire 测试显式使用 `--legacy-fixture`，完整描述只进入测试构建目录。

正文 PARAM/TREE/UI 记录均有 u16LE wire 长度前缀，INDEX 的 u32LE 偏移指向前缀后的 wire 字节。`.runtime` 是 `BVPV-RUNTIME-1`（14 字节，不含 NUL）+ 参数数 u16LE + 按地址排序的 8B 记录 `[addr3,type1,elemCount2LE,elemSize2LE]`。ERR 为长度前缀后的 `[code1,textLen1,ASCII]`。

ACAT 512B 头包含目标、版本、运行和正文/格式摘要、数量及五段边界。签名为原始 64B P-256 r/s，摘要为 `SHA256(ASCII "BVPV-CATALOG-V1" + NUL + header[0..447])`。设备和上位机保留各自信任校验。ECDSA 签名字节允许变化；确定性比较未签名正文和语义内容。

```powershell
dotnet build tools/catalog/Andromeda.CatalogPack.csproj -c Release
tools/catalog/bin/Release/net48/Andromeda.CatalogPack.exe --self-test
```

`Build-TestCatalog.ps1` 从当前未签名正文调用既有 `--self-test`，在构建目录生成测试 `.acat` 和 `.public.bin`。测试钥仅驻内存，测试包不作为发布包。`tests/host` 从当前 YAML 构建实际 compact C 和该测试包，再让生产目录校验器验证；`tests/device` 的离线旧 wire fixture 同样随当前源重建。
