# Mbed TLS 3.6.6（裁剪源码）

来源：https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.6
下载：https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/refs/tags/mbedtls-3.6.6
源码包 SHA-256：`de654c64688d0f6c71caf7336010e1fb54070e6e3d1df3999ee432ab59d0dbde`
许可：Apache-2.0 OR GPL-2.0-or-later，见 LICENSE；本项目按 Apache-2.0 使用。

上游文件未经修改。只保留 SHA-256/P-256 验签、必要大整数/ASN.1/平台/固定缓冲分配器的 C 实现；公共/内部头仅保留此配置实际包含的依赖，文件内容保持上游原样。配置为 src/boot/fw_mbedtls_config.h。

编译定义：`MBEDTLS_CONFIG_FILE="fw_mbedtls_config.h"`；include 为 `include`、`library` 和 `src/boot`。链接时须开启函数/数据 section 与 gc-sections，以删除未调用的签名与 ASN.1 输出实现。

