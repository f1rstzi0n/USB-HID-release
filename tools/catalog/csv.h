/* 极简 CSV 解析（UTF-8，一条记录一行；支持双引号包裹字段、"" 转义引号、BOM 跳过）。
 * 供目录编码器和镜像工具共用。 */
#ifndef CSV_H
#define CSV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 跳过 UTF-8 BOM（若存在），返回跳过后的指针。 */
const char *csv_skip_bom(const char *s);

/* 把一行（不含换行）就地切分成字段：写入 NUL 终止符，fields[] 收各字段指针。
 * 支持 "..." 包裹字段（内嵌逗号，""→"）。返回字段数（≤max_fields）。 */
size_t csv_split_line(char *line, char **fields, size_t max_fields);

/* 去除字段首尾空白（原地）。返回新起点。 */
char *csv_trim(char *s);

/* ---- 十六进制字节串字段（COMPOSITE 参数的 Default 列用）----
 * 约定：**磁盘上写紧凑大写形式**（如 "0000000050F2C3B6"），一个字节两个字符、无分隔符，
 * 因为 catalog_gen 的 def[64] 装不下 24 字节的分隔写法（"00 00 …" 要 71 字符）。
 * 解析端宽松：空格/制表/逗号/冒号/横线都当分隔符跳过，小写也接受，便于人工编辑。 */

/* 解析十六进制字节串到 out（最多 max 字节）。返回字节数；
 * 含非十六进制字符、半字节落单、或超出 max 时返回 -1。空串返回 0。 */
int csv_parse_hex_bytes(const char *text, uint8_t *out, int max);

/* 格式化为紧凑大写十六进制，写入 out（含 NUL）。返回字符数；放不下返回 -1。 */
int csv_format_hex_bytes(const uint8_t *b, int n, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* CSV_H */
