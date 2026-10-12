# Fake 设备参数目录

日常只编辑 UTF-8 文件 `SW110048.catalog.yaml`。按实际树层级排列分组和参数；双语标题、运行契约、复位初值、目录描述、主窗口绑定、错误文案及原来源说明均在该文件中。具体字段和维护方式见 [参数树维护说明](../docs/reference/CATALOG.md)。

## 生成与检查

从 Test446（USB-HID-release）工程根目录运行，PowerShell 5.1 或 7 均可：

```powershell
cmake -S tools/catalog -B build-tools -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build-tools
tools/catalog/Generate-Catalog.ps1 -Mode Check
tools/catalog/Generate-Catalog.ps1 -Mode Generate
ctest --test-dir build-tools --output-on-failure
```

`Check` 在暂存目录重建并核对正式产物；`Generate` 检查后更新派生文件，提供 `HostRoot` 时同步上位机资源。`Publish` 要求 `HostRoot` 与已有 `KeyName`，完成所有检查后生成签名 `.acat`。脚本接受 `CatalogGen`、`CatalogPack`、`SourceRoot`、`OutputRoot`，便于隔离回归；不需要 Python。

```powershell
tools/catalog/Generate-Catalog.ps1 -Mode Publish `
  -HostRoot 'C:\Users\17907\Desktop\CPA_Dnstest1' `
  -KeyName 'AURASKY_RELEASE_KEY'
```

修改 `description.default` 不会修改复位初值 `runtime.initial`。运行规则变化需要配套重编译固件；双语标题和绑定变化可能需要重编译上位机，按生成报告执行。设备安装仍使用 Andromeda“工具 → 更新”。所有 Fake 参数写入保存在 RAM，USB 拔插保留，MCU 复位恢复编译初值；RWA 不表示本分支已接入 EEPROM。

## 自动产物

以下文件由 YAML 生成并标记来源，修改内容应回到 YAML：

- `SW110048.catalog.csv`、`SW110048.tree.csv`：二进制编码及旧 wire 回归的兼容输入。
- `groups.v2.json`：USB 分组双语资源的导入清单。
- `ui-bindings.csv`、`errors.csv`、`source-info.json`：绑定、错误文本的编码输入与来源/运行契约摘要。
- `generation.v2.json`：自动规模、来源和派生文件摘要及部署要求。
- `../App/generated/bvpv_catalog_data.c`：按地址排序的紧凑运行契约与 RAM 初值。
- `../App/generated/bvpv_catalog_bindings.h`：设备编译期绑定允许表。
- `../build-catalog/SW110048.catalog.body` 及 `.json`、`.runtime`：未签名静态正文与发布侧信息，构建目录不提交。

节点编号、数量、索引和摘要均自动计算。旧 DAT/UI/preserved 及人工来源清单在迁移逐项验收后删除，历史证据通过 Git 和迁移记录追溯。测试中的真实目录包从当前 YAML 在构建目录生成，使用仅驻内存的临时测试钥。

正式包位于 `../release/<target>-<catalogVersion>.acat`。修改目录内容后，先在 YAML 中提高 `device.catalogVersion`；相同版本的有效相同包会复用，不同内容的已发布文件不会覆盖。
