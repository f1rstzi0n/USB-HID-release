/* catalog_gen：把统一目录 CSV 编译成设备端 C 表（bvpv_catalog_data.c）。
 * 用法：catalog_gen <in.catalog.csv> <out.c> [in.tree.csv]
 * 产出：独立运行契约与初始化数据，或用于测试的完整目录表。
 * 解析按表头列名（与列序无关），schema 见 docs/CATALOG.md。 */
#if defined(__MINGW32__)
#define __USE_MINGW_ANSI_STDIO 1
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bvpv_catalog.h"
#include "../../App/content/bvpv_catalog_format.h"
#include "csv.h"

#define MAX_ROWS BVPV_CATALOG_MAX_PARAMS
#define MAX_ENUM 32u
#define MAX_TREE_RECORDS 65534u

typedef struct {
    uint8_t kind;
    uint16_t node_id;
    uint16_t parent_id;
    char key[256];
    uint32_t addr;
} tree_row_t;

static tree_row_t tree_rows[MAX_TREE_RECORDS];
static size_t tree_count;

typedef struct {
    uint32_t addr;
    char     name[64];
    char     type[24];
    char     access[8];
    char     group[4096];
    char     def[193];
    char     min[64];
    char     max[64];
    char     unit[256];
    char     runtime_access[8];
    char     runtime_min[64];
    char     runtime_max[64];
    uint8_t  initial[3500];
    int      initial_size;
    int      elem_count;
    int      runtime_visible;
    int      visible;
    int      depth;
    /* 枚举 */
    int      enum_count;
    uint32_t enum_val[MAX_ENUM];
    char     enum_text[MAX_ENUM][256];
    /* COMPOSITE 尺寸 */
    int      comp_size;
} row_t;
static unsigned device_type = 2;
static char device_name[64] = "BV500-USB";

/* ---------------- 文件读取 ---------------- */

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long  sz;
    char *buf;
    if (!fp) { fprintf(stderr, "无法打开: %s\n", path); return NULL; }
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return NULL; }
    buf[sz] = '\0';
    fclose(fp);
    return buf;
}

static size_t split_lines(char *buf, char ***lines_out)
{
    size_t cap = 256, n = 0;
    char **lines = (char **)malloc(cap * sizeof(char *));
    char *p = buf, *start = buf;
    while (*p) {
        if (*p == '\n') {
            *p = '\0';
            if (p > start && p[-1] == '\r') { p[-1] = '\0'; }
            if (n >= cap) { cap *= 2; lines = (char **)realloc(lines, cap * sizeof(char *)); }
            lines[n++] = start;
            start = p + 1;
        }
        p++;
    }
    if (p > start) {
        if (n >= cap) { cap++; lines = (char **)realloc(lines, cap * sizeof(char *)); }
        lines[n++] = start;
    }
    *lines_out = lines;
    return n;
}

static int col_index(char **hdr, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(csv_trim(hdr[i]), name) == 0) { return (int)i; }
    }
    return -1;
}

/* ---------------- 类型/值解析 ---------------- */

static const char *type_macro(const char *tok)
{
    if (strcmp(tok, "UINT8") == 0)        return "BVPV_TYPE_UINT8";
    if (strcmp(tok, "UINT16") == 0)       return "BVPV_TYPE_UINT16";
    if (strcmp(tok, "UINT32") == 0)       return "BVPV_TYPE_UINT32";
    if (strcmp(tok, "UINT64") == 0)       return "BVPV_TYPE_UINT64";
    if (strcmp(tok, "INT32") == 0)        return "BVPV_TYPE_INT32";
    if (strcmp(tok, "INT32LE") == 0)      return "BVPV_TYPE_INT32";
    if (strcmp(tok, "FLOAT32") == 0)      return "BVPV_TYPE_FLOAT32";
    if (strcmp(tok, "FIXED16.16") == 0)   return "BVPV_TYPE_FIXED16_16";
    if (strcmp(tok, "FRAC16.16") == 0)    return "BVPV_TYPE_FIXED16_16";
    if (strcmp(tok, "UFIXED16.16") == 0)  return "BVPV_TYPE_UFIXED16_16";
    if (strcmp(tok, "UFRAC16") == 0)      return "BVPV_TYPE_UFRAC16";
    if (strcmp(tok, "TEXT8") == 0)        return "BVPV_TYPE_TEXT8";
    if (strcmp(tok, "TEXT16") == 0)       return "BVPV_TYPE_TEXT16";
    if (strcmp(tok, "TEXT32") == 0)       return "BVPV_TYPE_TEXT32";
    if (strcmp(tok, "COMPOSITE") == 0)    return "BVPV_TYPE_COMPOSITE";
    return "BVPV_TYPE_UNDEFINED";
}

static const char *access_macro(const char *tok)
{
    if (strcmp(tok, "RWA") == 0) return "BVPV_ACC_RWA";
    if (strcmp(tok, "ROO") == 0) return "BVPV_ACC_ROO";
    if (strcmp(tok, "RWO") == 0) return "BVPV_ACC_RWO";
    if (strcmp(tok, "OWO") == 0) return "BVPV_ACC_OWO";
    return "BVPV_ACC_OOO";
}

/* 类型 token → 内部编号（前向声明，定义在 emit_le_bytes 之后） */
static int type_code_of(const char *tok);

static int numeric_base(const char *text)
{
    if (*text == '-' || *text == '+') text++;
    return text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
}

/* 把文本值按类型解析为 LE 字节，返回字节数（=类型尺寸）。空文本 → 0 填充。
 * TEXT 拷贝字符串（NUL 填充到类型尺寸）；COMPOSITE 从 Default 解析十六进制字节串
 * （空 → 全 0；字节数必须等于 comp_size，由调用前的校验段拦截，此处只兜底）。 */
static int value_bytes(const char *type, const char *text, int comp_size, uint8_t b[64])
{
    int     n = 0;
    int     i;
    memset(b, 0, 64u);

    if (strcmp(type, "UINT8") == 0) {
        b[0] = (uint8_t)strtoul(text, NULL, numeric_base(text)); n = 1;
    } else if (strcmp(type, "UINT16") == 0 || strcmp(type, "UFRAC16") == 0) {
        uint32_t v = strtoul(text, NULL, numeric_base(text));
        b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); n = 2;
    } else if (strcmp(type, "UINT32") == 0 || strcmp(type, "INT32") == 0 ||
               strcmp(type, "INT32LE") == 0) {
        uint32_t v = (uint32_t)strtoul(text, NULL, numeric_base(text));
        b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24); n = 4;
    } else if (strcmp(type, "UINT64") == 0) {
        unsigned long long v = strtoull(text, NULL, numeric_base(text));
        for (i = 0; i < 8; i++) { b[i] = (uint8_t)(v >> (8 * i)); }
        n = 8;
    } else if (strcmp(type, "FLOAT32") == 0) {
        float    f = (float)strtod(text, NULL);
        uint32_t u;
        memcpy(&u, &f, 4);
        b[0]=(uint8_t)u; b[1]=(uint8_t)(u>>8); b[2]=(uint8_t)(u>>16); b[3]=(uint8_t)(u>>24); n = 4;
    } else if (strcmp(type, "FIXED16.16") == 0 || strcmp(type, "FRAC16.16") == 0) {
        double   d = strtod(text, NULL);
        int32_t  v = (int32_t)(d * 65536.0 + (d >= 0 ? 0.5 : -0.5));
        b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24); n = 4;
    } else if (strcmp(type, "UFIXED16.16") == 0) {
        double   d = strtod(text, NULL);
        uint32_t v = (uint32_t)(d * 65536.0 + 0.5);
        b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24); n = 4;
    } else if (strcmp(type, "TEXT8") == 0) {
        n = 8;  if (text[0]) { size_t l = strlen(text); if (l > 7u) l = 7u; memcpy(b, text, l); }
    } else if (strcmp(type, "TEXT16") == 0) {
        n = 16; if (text[0]) { size_t l = strlen(text); if (l > 15u) l = 15u; memcpy(b, text, l); }
    } else if (strcmp(type, "TEXT32") == 0) {
        n = 32; if (text[0]) { size_t l = strlen(text); if (l > 31u) l = 31u; memcpy(b, text, l); }
    } else if (strcmp(type, "COMPOSITE") == 0) {
        n = comp_size;
        if (text[0] != '\0' && csv_parse_hex_bytes(text, b, comp_size) != comp_size) {
            memset(b, 0, 64u);   /* 长度不符已在校验段报错中止，这里只保证不发射垃圾 */
        }
    } else {
        n = 0;
    }

    if (n <= 0) {
        n = (comp_size > 0) ? comp_size : 1;
    }
    return n;
}

static void emit_le_bytes(FILE *out, const char *type, const char *text, int comp_size)
{
    uint8_t b[64];
    int n = value_bytes(type, text, comp_size, b), i;
    fprintf(out, "{ ");
    for (i = 0; i < n; i++) {
        fprintf(out, "0x%02Xu%s", b[i], (i == n - 1) ? "" : ", ");
    }
    fprintf(out, " }");
}

static void emit_string(FILE *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    fputc('"', out);
    for (; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', out);
        if (*p < 32 || *p == 127) fprintf(out, "\\%03o", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}

/* 值缓冲字节数（=元素尺寸） */
static int elem_size_of(const char *type, int comp_size)
{
    if (strcmp(type, "COMPOSITE") == 0) { return comp_size; }
    return (int)bvpv_type_size((uint8_t)type_code_of(type));
}

/* 类型 token → 内部编号（复用 catalog 的类型尺寸表） */
static int type_code_of(const char *tok)
{
    const char *m = type_macro(tok);
    /* 把 "BVPV_TYPE_UINT8" 映射回编号 */
    if (strcmp(m, "BVPV_TYPE_UINT8") == 0)       return BVPV_TYPE_UINT8;
    if (strcmp(m, "BVPV_TYPE_UINT16") == 0)      return BVPV_TYPE_UINT16;
    if (strcmp(m, "BVPV_TYPE_UINT32") == 0)      return BVPV_TYPE_UINT32;
    if (strcmp(m, "BVPV_TYPE_UINT64") == 0)      return BVPV_TYPE_UINT64;
    if (strcmp(m, "BVPV_TYPE_INT32") == 0)       return BVPV_TYPE_INT32;
    if (strcmp(m, "BVPV_TYPE_FLOAT32") == 0)     return BVPV_TYPE_FLOAT32;
    if (strcmp(m, "BVPV_TYPE_FIXED16_16") == 0)  return BVPV_TYPE_FIXED16_16;
    if (strcmp(m, "BVPV_TYPE_UFIXED16_16") == 0) return BVPV_TYPE_UFIXED16_16;
    if (strcmp(m, "BVPV_TYPE_UFRAC16") == 0)     return BVPV_TYPE_UFRAC16;
    if (strcmp(m, "BVPV_TYPE_TEXT8") == 0)       return BVPV_TYPE_TEXT8;
    if (strcmp(m, "BVPV_TYPE_TEXT16") == 0)      return BVPV_TYPE_TEXT16;
    if (strcmp(m, "BVPV_TYPE_TEXT32") == 0)      return BVPV_TYPE_TEXT32;
    if (strcmp(m, "BVPV_TYPE_COMPOSITE") == 0)   return BVPV_TYPE_COMPOSITE;
    return BVPV_TYPE_UNDEFINED;
}

/* 解析枚举列 "值=文本;值=文本;…"（值可负） */
static int parse_enum(row_t *r, const char *s)
{
    char buf[16384];
    char *save = NULL;
    char *tok;
    if (strlen(s) >= sizeof(buf)) { return 0; }
    snprintf(buf, sizeof(buf), "%s", s);
    r->enum_count = 0;
    for (tok = strtok_r(buf, ";", &save); tok;
         tok = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(tok, '=');
        int   k = r->enum_count;
        if (!eq || k >= (int)MAX_ENUM || strlen(eq + 1) >= sizeof(r->enum_text[k])) { return 0; }
        *eq = '\0';
        r->enum_val[k] = (uint32_t)strtol(tok, NULL, 0);
        snprintf(r->enum_text[k], sizeof(r->enum_text[k]), "%s", eq + 1);
        r->enum_count++;
    }
    return 1;
}

/* 深度 = Group 段数（'-' 分隔），空=0 */
static int depth_of(const char *group)
{
    int d = 0;
    const char *p = group;
    if (!group[0]) { return 0; }
    d = 1;
    while (*p) {
        if (*p == '-') { d++; }
        p++;
    }
    return d;
}

/* 数值必须完整解析，拒绝空值、符号和尾随垃圾。 */
static int parse_unsigned(const char *text, int base, unsigned long *value)
{
    char *end;
    if (!text[0] || text[0] == '+' || text[0] == '-') { return 0; }
    errno = 0;
    *value = strtoul(text, &end, base);
    return !errno && *end == '\0';
}

/* 命名树是独立生成输入；读取时再次校验，不能编译出部分目录。 */
static int read_tree(const char *path, const row_t *rows, size_t count)
{
    char *buf = read_file(path), **lines, *hdr[8];
    size_t line_count, hcount, i, j;
    int c_kind, c_node, c_parent, c_key, c_addr;
    uint8_t nodes[65536] = {0};
    uint8_t seen[MAX_ROWS] = {0};
    if (!buf) { return 0; }
    line_count = split_lines((char *)csv_skip_bom(buf), &lines);
    if (line_count < 2) { fprintf(stderr, "树记录为空\n"); return 0; }
    hcount = csv_split_line(lines[0], hdr, 8);
    c_kind = col_index(hdr, hcount, "Kind");
    c_node = col_index(hdr, hcount, "NodeId");
    c_parent = col_index(hdr, hcount, "ParentId");
    c_key = col_index(hdr, hcount, "Key");
    c_addr = col_index(hdr, hcount, "Address");
    if (c_kind < 0 || c_node < 0 || c_parent < 0 || c_key < 0 || c_addr < 0) {
        fprintf(stderr, "树缺少必填列\n"); return 0;
    }
    nodes[0] = 1;
    for (i = 1; i < line_count; i++) {
        char *f[8]; size_t nf; unsigned long kind, node, parent;
        tree_row_t *r;
        if (csv_trim(lines[i])[0] == '\0') { continue; }
        nf = csv_split_line(lines[i], f, 8);
        if (nf != hcount || tree_count >= MAX_TREE_RECORDS) { goto invalid; }
        if (!parse_unsigned(f[c_kind], 10, &kind) ||
            !parse_unsigned(f[c_node], 10, &node) ||
            !parse_unsigned(f[c_parent], 10, &parent)) { goto invalid; }
        if (node > 65535 || parent > 65535 || !nodes[parent]) { goto invalid; }
        r = &tree_rows[tree_count];
        r->kind = (uint8_t)kind; r->node_id = (uint16_t)node;
        r->parent_id = (uint16_t)parent;
        if (kind == 1) {
            size_t k, len = strlen(f[c_key]);
            if (!node || nodes[node] || !len || len > 255 || f[c_addr][0]) { goto invalid; }
            for (k = 0; k < len; k++) {
                char ch = f[c_key][k];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.')) { goto invalid; }
            }
            for (j = 0; j < tree_count; j++) {
                if (tree_rows[j].kind == 1 && strcmp(tree_rows[j].key, f[c_key]) == 0) { goto invalid; }
            }
            memcpy(r->key, f[c_key], len + 1);
            nodes[node] = 1;
        } else if (kind == 2) {
            unsigned long addr;
            if (!parse_unsigned(f[c_addr], 0, &addr)) { goto invalid; }
            if (node || f[c_key][0] || addr > 0xFFFFFFu) { goto invalid; }
            r->addr = (uint32_t)addr;
            for (j = 0; j < count && rows[j].addr != r->addr; j++) { }
            if (j == count || seen[j]) { goto invalid; }
            seen[j] = 1;
        } else { goto invalid; }
        tree_count++;
    }
    for (j = 0; j < count; j++) { if (!seen[j]) { goto invalid; } }
    free(lines); free(buf);
    return 1;
invalid:
    fprintf(stderr, "树记录非法、父节点未出现、参数重复或遗漏（行 %zu）\n", i + 1);
    free(lines); free(buf);
    return 0;
}
static void address_order(const row_t *rows, size_t n, uint16_t *order)
{
    size_t i;
    for (i = 0; i < n; i++) order[i] = (uint16_t)i;
    for (i = 1; i < n; i++) {
        uint16_t current = order[i];
        size_t j = i;
        while (j > 0u && rows[order[j - 1u]].addr > rows[current].addr) {
            order[j] = order[j - 1u];
            j--;
        }
        order[j] = current;
    }
}

static void emit_runtime(FILE *out, const row_t *rows, size_t n)
{
    uint16_t order[MAX_ROWS];
    size_t i;
    address_order(rows, n, order);
    fprintf(out, "/* 自动生成：仅运行契约与 RAM 初值；静态描述位于外部目录。 */\n"
                 "#include \"bvpv_catalog.h\"\n\n");
    for (i = 0; i < n; i++) {
        int j;
        fprintf(out, "static uint8_t val_%zu[%d] = { ", i, rows[i].initial_size);
        for (j = 0; j < rows[i].initial_size; j++)
            fprintf(out, "0x%02Xu%s", rows[i].initial[j], j + 1 == rows[i].initial_size ? "" : ", ");
        fprintf(out, " }");
        fprintf(out, ";\n");
    }
    fprintf(out, "\nstatic uint8_t g_states[%zu];\n"
                 "static bvpv_change_entry_t g_changes[64];\n"
                 "static const bvpv_runtime_param_t g_runtime[%zu] = {\n", n, n);
    for (i = 0; i < n; i++) {
        const row_t *r = &rows[order[i]];
        fprintf(out, "    { .addr = 0x%06Xu, .type = %s, .access = %s,\n"
                     "      .elem_count = %du, .elem_size = %du, .flags = %s,\n",
                r->addr, type_macro(r->type), access_macro(r->runtime_access), r->elem_count,
                elem_size_of(r->type, r->comp_size),
                r->runtime_visible ? "BVPV_FLAG_DEFAULT_VISIBLE" : "0u");
        fprintf(out, "      .ext_flags = %uu", (r->runtime_min[0] ? BVPV_EXT_HAS_MIN : 0u) |
                                                (r->runtime_max[0] ? BVPV_EXT_HAS_MAX : 0u));
        if (r->runtime_min[0]) { fprintf(out, ", .min = "); emit_le_bytes(out, r->type, r->runtime_min, r->comp_size); }
        if (r->runtime_max[0]) { fprintf(out, ", .max = "); emit_le_bytes(out, r->type, r->runtime_max, r->comp_size); }
        fprintf(out, ", .value = val_%u },\n", order[i]);
    }
    fprintf(out, "};\nstatic bvpv_service_ctx_t g_catalog_ctx = {\n"
                 "    .runtime = g_runtime, .states = g_states, .count = %zuu,\n"
                 "    .device_type = %uu, .device_name = \"%s\",\n"
                 "    .changes = g_changes, .change_capacity = 64u, .tree_count = %zuu\n"
                 "};\nstatic bool g_catalog_initialized;\n\n"
                 "bvpv_service_ctx_t *bvpv_catalog_data_ctx(void)\n{\n"
                 "    if (!g_catalog_initialized) {\n"
                 "        bvpv_catalog_init_states(&g_catalog_ctx);\n"
                 "        g_catalog_initialized = true;\n"
                 "    }\n    return &g_catalog_ctx;\n}\n", n, device_type, device_name, tree_count);
    for (i = 0; i < n; i++) if (!strcmp(rows[i].name, "FirmwareVersion")) break;
    fprintf(out, "\nconst char *bvpv_catalog_data_app_identity(void) { return \"%s\"; }\n",
            i < n ? (const char *)rows[i].initial : "");
}

typedef struct {
    uint8_t bytes[BVPV_CATALOG_MAX_RECORD];
    uint16_t used;
    int failed;
} wire_t;

static void append(wire_t *w, const void *data, size_t bytes)
{
    if (bytes > sizeof(w->bytes) - w->used) { w->failed = 1; return; }
    memcpy(w->bytes + w->used, data, bytes);
    w->used = (uint16_t)(w->used + bytes);
}
static void u8(wire_t *w, uint8_t v) { append(w, &v, 1u); }
static void u16(wire_t *w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    append(w, b, sizeof(b));
}
static void u32(wire_t *w, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    append(w, b, sizeof(b));
}
static void file_u16(FILE *out, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    (void)fwrite(b, 1u, sizeof(b), out);
}
static void file_u32(FILE *out, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    (void)fwrite(b, 1u, sizeof(b), out);
}
static void numeric_tlv(wire_t *w, uint8_t tag, const row_t *r, const char *text)
{
    uint8_t bytes[64];
    int size = value_bytes(r->type, text, r->comp_size, bytes);
    /* Undefined has a one-byte RAM sentinel but no wire element bytes. */
    if (type_code_of(r->type) == BVPV_TYPE_UNDEFINED) size = 0;
    u8(w, tag); u16(w, (uint16_t)size); append(w, bytes, (size_t)size);
}
static void parameter_wire(wire_t *w, const row_t *r)
{
    uint16_t ext_at, ext_start;
    int k;
    memset(w, 0, sizeof(*w));
    u8(w, (uint8_t)(r->addr >> 16)); u8(w, (uint8_t)(r->addr >> 8)); u8(w, (uint8_t)r->addr);
    u8(w, (uint8_t)type_code_of(r->type));
    u8(w, !strcmp(r->access, "RWA") ? BVPV_ACC_RWA : !strcmp(r->access, "ROO") ? BVPV_ACC_ROO :
          !strcmp(r->access, "RWO") ? BVPV_ACC_RWO : !strcmp(r->access, "OWO") ? BVPV_ACC_OWO : BVPV_ACC_OOO);
    u8(w, r->visible ? BVPV_FLAG_DEFAULT_VISIBLE : 0u); u8(w, 0u); u16(w, (uint16_t)r->elem_count);
    u8(w, (uint8_t)strlen(r->name)); append(w, r->name, strlen(r->name));
    ext_at = w->used; u16(w, 0u); ext_start = w->used;
    if (r->min[0]) numeric_tlv(w, 1u, r, r->min);
    if (r->max[0]) numeric_tlv(w, 2u, r, r->max);
    if (r->def[0]) numeric_tlv(w, 3u, r, r->def);
    if (r->enum_count) {
        uint16_t length = 0u;
        for (k = 0; k < r->enum_count; k++) length = (uint16_t)(length + 5u + strlen(r->enum_text[k]));
        u8(w, 4u); u16(w, length);
        for (k = 0; k < r->enum_count; k++) {
            u32(w, r->enum_val[k]); u8(w, (uint8_t)strlen(r->enum_text[k]));
            append(w, r->enum_text[k], strlen(r->enum_text[k]));
        }
    }
    if (r->unit[0]) {
        u8(w, 5u); u16(w, (uint16_t)strlen(r->unit)); append(w, r->unit, strlen(r->unit));
    }
    w->bytes[ext_at] = (uint8_t)(w->used - ext_start);
    w->bytes[ext_at + 1u] = (uint8_t)((w->used - ext_start) >> 8);
}

typedef struct { char role[64]; uint32_t address; uint8_t order; } ui_role_t;
typedef struct { uint8_t code; char text[256]; } error_text_t;
static ui_role_t main_ui[64];
static error_text_t errors[256];
static size_t main_ui_count, error_count;

static int read_description(const char *path, int ui)
{
    char *buf = read_file(path), **lines, *header[4];
    size_t count, fields, i, j;
    if (!buf) return 0;
    count = split_lines((char *)csv_skip_bom(buf), &lines);
    fields = count ? csv_split_line(lines[0], header, 4) : 0;
    if ((ui && (fields != 3 || strcmp(header[0], "Role") || strcmp(header[1], "Address") || strcmp(header[2], "Order"))) ||
        (!ui && (fields != 2 || strcmp(header[0], "Code") || strcmp(header[1], "Text")))) goto bad;
    for (i = 1; i < count; i++) {
        char *f[4]; unsigned long value, order;
        if (!lines[i][0]) continue;
        if (csv_split_line(lines[i], f, 4) != fields) goto bad;
        if (ui) {
            ui_role_t *r;
            size_t length = strlen(f[0]);
            if (main_ui_count == 64 || !length || length > 63 ||
                !parse_unsigned(f[1], 0, &value) || value > 0xffffff ||
                !parse_unsigned(f[2], 10, &order) || order > 255) goto bad;
            for (j = 0; j < main_ui_count; j++) if (!strcmp(main_ui[j].role, f[0])) goto bad;
            r = &main_ui[main_ui_count++];
            memcpy(r->role, f[0], length + 1); r->address = (uint32_t)value; r->order = (uint8_t)order;
        } else {
            error_text_t *r;
            size_t length = strlen(f[1]);
            if (error_count == 256 || !length || length > 255 || !parse_unsigned(f[0], 0, &value) || value > 255) goto bad;
            for (j = 0; j < error_count; j++) if (errors[j].code == value) goto bad;
            for (j = 0; j < length; j++) if ((unsigned char)f[1][j] < 32 || (unsigned char)f[1][j] > 126) goto bad;
            r = &errors[error_count++]; r->code = (uint8_t)value; memcpy(r->text, f[1], length + 1);
        }
    }
    free(lines); free(buf); return ui || error_count > 0;
bad:
    fprintf(stderr, "绑定或错误文本 CSV 非法: %s\n", path);
    free(lines); free(buf); return 0;
}

/* 输出 unsigned body 与仅供主机包装的结构/运行契约侧文件，不产生可刷入包。 */
static int emit_body(const char *path, const row_t *rows, size_t n)
{
    uint32_t offsets[MAX_ROWS + MAX_TREE_RECORDS], tree_at, ui_at, index_at, error_at, body_end;
    uint16_t order[MAX_ROWS];
    char side[1024];
    FILE *out;
    size_t i, ui_count = 0u;
    wire_t wire;
    if (strlen(path) + 16u >= sizeof(side)) return 0;
    out = fopen(path, "wb");
    if (!out) return 0;
    for (i = 0; i < n; i++) {
        parameter_wire(&wire, &rows[i]);
        if (wire.failed) { fclose(out); return 0; }
        offsets[i] = (uint32_t)ftell(out) + 2u;
        file_u16(out, wire.used); (void)fwrite(wire.bytes, 1u, wire.used, out);
    }
    tree_at = (uint32_t)ftell(out);
    for (i = 0; i < tree_count; i++) {
        const tree_row_t *r = &tree_rows[i];
        memset(&wire, 0, sizeof(wire)); u8(&wire, r->kind);
        if (r->kind == BVPV_TREE_GROUP) {
            u16(&wire, r->node_id); u16(&wire, r->parent_id);
            u8(&wire, (uint8_t)strlen(r->key)); append(&wire, r->key, strlen(r->key));
        } else {
            u16(&wire, r->parent_id); u8(&wire, (uint8_t)(r->addr >> 16));
            u8(&wire, (uint8_t)(r->addr >> 8)); u8(&wire, (uint8_t)r->addr);
        }
        offsets[n + i] = (uint32_t)ftell(out) + 2u;
        file_u16(out, wire.used); (void)fwrite(wire.bytes, 1u, wire.used, out);
    }
    ui_at = (uint32_t)ftell(out);
    for (i = 0; i < main_ui_count; i++) {
        const ui_role_t *r = &main_ui[i];
        size_t j;
        for (j = 0; j < n && rows[j].addr != r->address; j++) {}
        if (j == n) { fclose(out); return 0; }
        memset(&wire, 0, sizeof(wire));
        u8(&wire, (uint8_t)strlen(r->role)); append(&wire, r->role, strlen(r->role));
        u8(&wire, (uint8_t)(r->address >> 16)); u8(&wire, (uint8_t)(r->address >> 8));
        u8(&wire, (uint8_t)r->address); u8(&wire, r->order);
        file_u16(out, wire.used); (void)fwrite(wire.bytes, 1u, wire.used, out); ui_count++;
    }
    index_at = (uint32_t)ftell(out);
    for (i = 0; i < n + tree_count; i++) {
        file_u32(out, offsets[i]);
    }
    error_at = (uint32_t)ftell(out);
    for (i = 0; i < error_count; i++) {
        const error_text_t *r = &errors[i];
        size_t length = strlen(r->text);
        if (length == 0u || length > 255u) { fclose(out); return 0; }
        file_u16(out, (uint16_t)(length + 2u));
        (void)fputc(r->code, out); (void)fputc((int)length, out);
        (void)fwrite(r->text, 1u, length, out);
    }
    body_end = (uint32_t)ftell(out);
    if (body_end > 1048576u - BVPV_CATALOG_HEADER_SIZE) { fclose(out); return 0; }
    if (ferror(out)) { fclose(out); return 0; }
    if (fclose(out)) return 0;
    snprintf(side, sizeof(side), "%s.json", path);
    out = fopen(side, "wb");
    if (!out) return 0;
    fprintf(out, "{\"parameterCount\":%zu,\"treeCount\":%zu,\"uiCount\":%zu,\"errorCount\":%zu,"
                 "\"parameterOffset\":0,\"parameterBytes\":%u,"
                 "\"treeOffset\":%u,\"treeBytes\":%u,"
                 "\"uiOffset\":%u,\"uiBytes\":%u,"
                 "\"indexOffset\":%u,\"indexBytes\":%zu,\"errorOffset\":%u,\"errorBytes\":%u}\n",
            n, tree_count, ui_count, error_count, tree_at, tree_at, ui_at - tree_at, ui_at,
            index_at - ui_at, index_at, (n + tree_count) * BVPV_CATALOG_INDEX_SIZE,
            error_at, body_end - error_at);
    if (fclose(out)) return 0;
    snprintf(side, sizeof(side), "%s.runtime", path);
    out = fopen(side, "wb");
    if (!out) return 0;
    (void)fwrite("BVPV-RUNTIME-1", 1u, 14u, out); file_u16(out, (uint16_t)n);
    address_order(rows, n, order);
    for (i = 0; i < n; i++) {
        const row_t *r = &rows[order[i]];
        uint8_t b[4] = {(uint8_t)(r->addr >> 16), (uint8_t)(r->addr >> 8),
                        (uint8_t)r->addr, (uint8_t)type_code_of(r->type)};
        (void)fwrite(b, 1u, sizeof(b), out); file_u16(out, (uint16_t)r->elem_count);
        file_u16(out, (uint16_t)elem_size_of(r->type, r->comp_size));
    }
    if (ferror(out)) { fclose(out); return 0; }
    return fclose(out) == 0;
}

int main(int argc, char **argv)
{
    char   *buf;
    char  **lines;
    size_t  nlines;
    char   *hdr[40];
    size_t  nhdr;
    int     c_group, c_name, c_addr, c_type, c_acc, c_def, c_min, c_max,
            c_unit, c_enum, c_vis, c_initial, c_elem_count, c_elem_size,
            c_runtime_access, c_runtime_min, c_runtime_max, c_runtime_visible,
            c_device_type, c_device_name;
    static row_t rows[MAX_ROWS];
    size_t  n = 0, i;
    size_t  li;
    FILE   *out;
    int legacy = argc > 1 && strcmp(argv[1], "--legacy-fixture") == 0;
    if (legacy) { argv++; argc--; }

    if (argc != 3 && argc != 4 && (legacy || argc != 7)) {
        fprintf(stderr, "用法: catalog_gen [--legacy-fixture] <in.catalog.csv> <out.c> [in.tree.csv] [unsigned.body ui-bindings.csv errors.csv]\n");
        return 1;
    }
    buf = read_file(argv[1]);
    if (!buf) { return 1; }
    nlines = split_lines((char *)csv_skip_bom(buf), &lines);
    if (nlines == 0) { fprintf(stderr, "空文件\n"); return 1; }

    nhdr     = csv_split_line(lines[0], hdr, 40);
    c_group  = col_index(hdr, nhdr, "Group");
    c_name   = col_index(hdr, nhdr, "Name");
    c_addr   = col_index(hdr, nhdr, "Address");
    c_type   = col_index(hdr, nhdr, "Type");
    c_acc    = col_index(hdr, nhdr, "Access");
    c_def    = col_index(hdr, nhdr, "Default");
    c_min    = col_index(hdr, nhdr, "Min");
    c_max    = col_index(hdr, nhdr, "Max");
    c_unit   = col_index(hdr, nhdr, "Unit");
    c_enum   = col_index(hdr, nhdr, "Enum");
    c_vis    = col_index(hdr, nhdr, "Visible");

    c_initial = col_index(hdr, nhdr, "InitialBytes");
    c_elem_count = col_index(hdr, nhdr, "ElementCount");
    c_elem_size = col_index(hdr, nhdr, "ElementSize");
    c_runtime_access = col_index(hdr, nhdr, "RuntimeAccess");
    c_runtime_min = col_index(hdr, nhdr, "RuntimeMin");
    c_runtime_max = col_index(hdr, nhdr, "RuntimeMax");
    c_runtime_visible = col_index(hdr, nhdr, "RuntimeVisible");
    c_device_type = col_index(hdr, nhdr, "DeviceType");
    c_device_name = col_index(hdr, nhdr, "DeviceName");

    if (c_name < 0 || c_addr < 0 || c_type < 0 || c_acc < 0) {
        fprintf(stderr, "缺必填列（Name/Address/Type/Access）\n");
        return 1;
    }

    for (li = 1; li < nlines; li++) {
        char  *f[40];
        size_t nf;
        row_t *r;
        if (csv_trim(lines[li])[0] == '\0') { continue; }
        if (n >= MAX_ROWS) { fprintf(stderr, "参数超过生成器上限\n"); return 1; }
        nf = csv_split_line(lines[li], f, 40);
        if (nf != nhdr) { fprintf(stderr, "参数 CSV 字段数量错误\n"); return 1; }
        r = &rows[n];
        memset(r, 0, sizeof(*r));
        /* 先检查文本容器长度，再复制，禁止 snprintf 静默丢失来源字节。 */
        if ((c_name >= 0 && (size_t)c_name < nf && strlen(f[c_name]) >= sizeof(r->name)) ||
            (c_unit >= 0 && (size_t)c_unit < nf && strlen(f[c_unit]) >= sizeof(r->unit)) ||
            (c_group >= 0 && (size_t)c_group < nf && strlen(f[c_group]) >= sizeof(r->group)) ||
            (c_def >= 0 && (size_t)c_def < nf && strlen(f[c_def]) >= sizeof(r->def))) {
            fprintf(stderr, "参数名称、单位、分组或默认值超出生成器字段长度\n"); return 1;
        }

        if (c_name >= 0 && (size_t)c_name < nf) {
            snprintf(r->name, sizeof(r->name), "%s", f[c_name]);
        }
        if (r->name[0] == '\0') { continue; }
        if (c_addr >= 0 && (size_t)c_addr < nf) { r->addr = (uint32_t)strtoul(csv_trim(f[c_addr]), NULL, 0); }
        if (c_type >= 0 && (size_t)c_type < nf) { snprintf(r->type, sizeof(r->type), "%s", csv_trim(f[c_type])); }
        if (c_acc >= 0 && (size_t)c_acc < nf)   { snprintf(r->access, sizeof(r->access), "%s", csv_trim(f[c_acc])); }
        if (c_group >= 0 && (size_t)c_group < nf) { snprintf(r->group, sizeof(r->group), "%s", f[c_group]); }
        if (c_def >= 0 && (size_t)c_def < nf)   { snprintf(r->def, sizeof(r->def), "%s", f[c_def]); }
        if (c_min >= 0 && (size_t)c_min < nf)   { snprintf(r->min, sizeof(r->min), "%s", csv_trim(f[c_min])); }
        if (c_max >= 0 && (size_t)c_max < nf)   { snprintf(r->max, sizeof(r->max), "%s", csv_trim(f[c_max])); }
        if (c_unit >= 0 && (size_t)c_unit < nf) { snprintf(r->unit, sizeof(r->unit), "%s", f[c_unit]); }
        r->visible = (c_vis >= 0 && (size_t)c_vis < nf) ? (atoi(f[c_vis]) != 0) : 1;
        if (c_enum >= 0 && (size_t)c_enum < nf && f[c_enum][0] != '\0') {
            if (!parse_enum(r, f[c_enum])) { fprintf(stderr, "枚举超出数量/UTF-8 字节长度边界或格式非法\n"); return 1; }
        }
        r->depth = depth_of(r->group);
        r->elem_count = c_elem_count >= 0 ? atoi(f[c_elem_count]) : 1;
        r->comp_size = 0;
        if (strcmp(r->type, "COMPOSITE") == 0) {
            r->comp_size = c_elem_size >= 0 ? atoi(f[c_elem_size]) : 0;
            if (r->comp_size < 1 || r->comp_size > 64) { fprintf(stderr, "COMPOSITE 必须明确声明 1..64 字节尺寸\n"); return 1; }
        }
        snprintf(r->runtime_access, sizeof(r->runtime_access), "%s", c_runtime_access >= 0 ? f[c_runtime_access] : r->access);
        snprintf(r->runtime_min, sizeof(r->runtime_min), "%s", c_runtime_min >= 0 ? f[c_runtime_min] : r->min);
        snprintf(r->runtime_max, sizeof(r->runtime_max), "%s", c_runtime_max >= 0 ? f[c_runtime_max] : r->max);
        r->runtime_visible = c_runtime_visible >= 0 ? atoi(f[c_runtime_visible]) : r->visible;
        {
            int esz = elem_size_of(r->type, r->comp_size);
            int expected = (esz > 0 ? esz : 1) * r->elem_count;
            if (r->elem_count < 1 || r->elem_count > 256 || expected > (int)sizeof(r->initial)) { fprintf(stderr, "元素数量或初始化数据超限\n"); return 1; }
            if (c_initial >= 0) {
                r->initial_size = csv_parse_hex_bytes(f[c_initial], r->initial, (int)sizeof(r->initial));
                if (r->initial_size != expected) { fprintf(stderr, "初始化字节数与运行契约不符\n"); return 1; }
            } else {
                if (r->elem_count != 1) return 1;
                r->initial_size = value_bytes(r->type, r->def, r->comp_size, r->initial);
            }
        }
        if (c_device_type >= 0) device_type = (unsigned)atoi(f[c_device_type]);
        if (c_device_name >= 0) {
            if (strlen(f[c_device_name]) >= sizeof(device_name) || strpbrk(f[c_device_name], "\"\\\n\r")) { fprintf(stderr, "设备名称无法表示\n"); return 1; }
            snprintf(device_name, sizeof(device_name), "%s", f[c_device_name]);
        }
        n++;
    }

    /* ---------------- 校验：待发射的值必须放得进目标字段 ----------------
     * 宁可构建期报错，也不静默截断：被截断的 Min/Max 仍会被服务层按 elem_size 读出，
     * 等于越界读相邻结构体字段。默认值无此限制（>8B 走 def_ext 指向的独立数组）。 */
    {
        int bad = 0;
        for (i = 0; i < n; i++) {
            int esz = elem_size_of(rows[i].type, rows[i].comp_size);
            size_t j, ext_size = 0;
            if (rows[i].min[0]) { ext_size += 3 + (size_t)esz; }
            if (rows[i].max[0]) { ext_size += 3 + (size_t)esz; }
            if (rows[i].def[0]) { ext_size += 3 + (size_t)esz; }
            if (rows[i].unit[0]) { ext_size += 3 + strlen(rows[i].unit); }
            if (rows[i].enum_count) {
                ext_size += 3;
                for (j = 0; j < (size_t)rows[i].enum_count; j++) { ext_size += 5 + strlen(rows[i].enum_text[j]); }
            }
            if (ext_size + 12u + strlen(rows[i].name) > 3500u) { fprintf(stderr, "错误：带名称的属性快照记录超出 3500 字节分页容量\n"); bad++; }
            if (strncmp(rows[i].type, "TEXT", 4) == 0 && strlen(rows[i].def) >= (size_t)esz) {
                fprintf(stderr, "错误：文本默认值无法完整放入类型容量\n"); bad++;
            }
            if (rows[i].addr > 0xFFFFFFu) { fprintf(stderr, "错误：参数地址不是 24 位\n"); bad++; }
            for (j = 0; rows[i].name[j]; j++) {
                unsigned char ch = (unsigned char)rows[i].name[j];
                if (ch < 0x20 || ch > 0x7E) { fprintf(stderr, "错误：参数名称不是 ASCII\n"); bad++; break; }
            }
            for (j = 0; j < i; j++) {
                if (rows[j].addr == rows[i].addr || strcmp(rows[j].name, rows[i].name) == 0) {
                    fprintf(stderr, "错误：参数名称或地址重复\n"); bad++; break;
                }
            }
            if (esz <= 0) { esz = 1; }
            if ((esz > 8) && ((rows[i].min[0] != '\0') || (rows[i].max[0] != '\0'))) {
                fprintf(stderr,
                        "错误：%s(0x%06X) 元素 %d 字节 >8，Min/Max 放不进内联 8 字节数组"
                        "（Min/Max 仅数值型有意义）\n",
                        rows[i].name, rows[i].addr, esz);
                bad++;
            }
            if (rows[i].min[0] || rows[i].max[0]) {
                uint8_t lower[64], upper[64];
                uint8_t type = (uint8_t)type_code_of(rows[i].type);
                if (type == BVPV_TYPE_UNDEFINED || type > BVPV_TYPE_UFRAC16 ||
                    (rows[i].min[0] && (value_bytes(rows[i].type, rows[i].min, rows[i].comp_size, lower),
                                       !bvpv_value_is_finite(type, lower))) ||
                    (rows[i].max[0] && (value_bytes(rows[i].type, rows[i].max, rows[i].comp_size, upper),
                                       !bvpv_value_is_finite(type, upper))) ||
                    (rows[i].min[0] && rows[i].max[0] && bvpv_value_compare(type, lower, upper) > 0)) {
                    fprintf(stderr, "错误：范围必须为有限数值且 Min<=Max\n"); bad++;
                }
            }
            /* COMPOSITE 的 Default 是十六进制字节串，长度必须与 comp_size 严格相等：
             * 多出来的字节会被静默丢弃、少掉的会补零，两种都是"值悄悄变了"，宁可构建期报错。 */
            if (strcmp(rows[i].type, "COMPOSITE") == 0 && rows[i].def[0] != '\0') {
                uint8_t tmp[64];
                int     got = csv_parse_hex_bytes(rows[i].def, tmp, (int)sizeof(tmp));
                if (got < 0) {
                    fprintf(stderr, "错误：COMPOSITE %s(0x%06X) 的 Default 不是合法十六进制字节串：\"%s\"\n",
                            rows[i].name, rows[i].addr, rows[i].def);
                    bad++;
                } else if (got != rows[i].comp_size) {
                    fprintf(stderr, "错误：COMPOSITE %s(0x%06X) 的 Default 有 %d 字节，"
                                    "但类型声明 comp_size=%d 字节\n",
                            rows[i].name, rows[i].addr, got, rows[i].comp_size);
                    bad++;
                }
            }
        }
        if (bad != 0) {
            fprintf(stderr, "共 %d 处无法表示，已中止（未写输出文件）\n", bad);
            return 1;
        }
    }

    if (argc >= 4 && !read_tree(argv[3], rows, n)) { return 1; }
    if (argc == 7 && (!read_description(argv[5], 1) || !read_description(argv[6], 0))) return 1;

    if (!legacy) {
        if (argc == 7 && !emit_body(argv[4], rows, n)) {
            fprintf(stderr, "无法完整写外置目录正文\n"); return 1;
        }
        out = fopen(argv[2], "wb");
        if (!out) return 1;
        emit_runtime(out, rows, n);
        if (ferror(out)) { fclose(out); return 1; }
        return fclose(out) == 0 ? 0 : 1;
    }

    /* ---------------- 生成 C ---------------- */
    out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "无法写: %s\n", argv[2]); return 1; }

    {
        const char *source_name = strrchr(argv[1], '/');
        const char *backslash = strrchr(argv[1], '\\');
        if (!source_name || (backslash && backslash > source_name)) { source_name = backslash; }
        source_name = source_name ? source_name + 1 : argv[1];
        fprintf(out, "/* 自动生成，请勿手改。由 tools/catalog/catalog_gen 从 %s 生成。\n", source_name);
    }
    fprintf(out, " * 参数数：%zu。重新生成：见 docs/CATALOG.md。 */\n", n);
    fprintf(out, "#include \"bvpv_catalog.h\"\n\n");

    /* 枚举表（仅为含枚举的参数生成） */
    for (i = 0; i < n; i++) {
        if (rows[i].enum_count > 0) {
            int k;
            fprintf(out, "static const bvpv_enum_item_t enum_%zu[] = {\n", i);
            for (k = 0; k < rows[i].enum_count; k++) {
                fprintf(out, "    { %uu, ", rows[i].enum_val[k]);
                emit_string(out, rows[i].enum_text[k]); fprintf(out, " },\n");
            }
            fprintf(out, "};\n");
        }
    }
    fprintf(out, "\n");

    /* 值缓冲（RAM，初值=Default） */
    for (i = 0; i < n; i++) {
        int j;
        fprintf(out, "static uint8_t val_%zu[%d] = { ", i, rows[i].initial_size);
        for (j = 0; j < rows[i].initial_size; j++) fprintf(out, "0x%02Xu%s", rows[i].initial[j], j + 1 == rows[i].initial_size ? "" : ", ");
        fprintf(out, " }");
        fprintf(out, ";\n");
    }
    fprintf(out, "\n");

    /* 运行时状态（RAM） */
    fprintf(out, "static uint8_t g_states[%zu];\n", n);
    fprintf(out, "static bvpv_change_entry_t g_changes[64];\n\n");

    /* 元素 >8B 的默认值：内联 def[8] 装不下，另起 const 数组供 def_ext 指向。
     * 必须写满 elem_size 字节——服务层按 elem_size 发送 Default TLV，
     * 少写一字节就是越界读（历史缺陷：曾从 def[8] 越界发出 16/32 字节）。 */
    for (i = 0; i < n; i++) {
        if ((elem_size_of(rows[i].type, rows[i].comp_size) > 8) && (rows[i].def[0] != '\0')) {
            fprintf(out, "static const uint8_t defext_%zu[%d] = ", i,
                    elem_size_of(rows[i].type, rows[i].comp_size));
            emit_le_bytes(out, rows[i].type, rows[i].def, rows[i].comp_size);
            fprintf(out, ";\n");
        }
    }
    fprintf(out, "\n");

    /* 元数据表（const，Flash） */
    fprintf(out, "static const bvpv_param_t g_params[%zu] = {\n", n);
    for (i = 0; i < n; i++) {
        row_t *r = &rows[i];
        int   esz = elem_size_of(r->type, r->comp_size);
        fprintf(out, "    { .addr = 0x%06Xu, .type = %s, .access = %s,\n",
                r->addr, type_macro(r->type), access_macro(r->access));
        fprintf(out, "      .flags = %s, .depth = %du, .elem_count = %du,\n",
                r->visible ? "BVPV_FLAG_DEFAULT_VISIBLE" : "0u", r->depth, r->elem_count);
        fprintf(out, "      .name = "); emit_string(out, r->name);
        /* ext_flags + min/max/def */
        {
            int has_min = (r->min[0] != '\0');
            int has_max = (r->max[0] != '\0');
            int has_def = (r->def[0] != '\0');
            int any_ext = has_min || has_max || has_def;
            if (any_ext) {
                fprintf(out, ",\n      .ext_flags = ");
                {
                    int first = 1;
                    if (has_min) { fprintf(out, "BVPV_EXT_HAS_MIN"); first = 0; }
                    if (has_max) { fprintf(out, "%sBVPV_EXT_HAS_MAX", first ? "" : " | "); first = 0; }
                    if (has_def) { fprintf(out, "%sBVPV_EXT_HAS_DEFAULT", first ? "" : " | "); first = 0; }
                }
                if (has_min) { fprintf(out, ", .min = "); emit_le_bytes(out, r->type, r->min, r->comp_size); }
                if (has_max) { fprintf(out, ", .max = "); emit_le_bytes(out, r->type, r->max, r->comp_size); }
                /* 默认值：元素 ≤8B 内联进 def[]，>8B（TEXT16/32、COMPOSITE）指向 defext_ 数组 */
                if (has_def) {
                    if (esz <= 8) {
                        fprintf(out, ", .def = ");
                        emit_le_bytes(out, r->type, r->def, r->comp_size);
                    } else {
                        fprintf(out, ", .def_ext = defext_%zu", i);
                    }
                }
            }
        }
        if (r->unit[0] != '\0') {
            fprintf(out, ", .unit = "); emit_string(out, r->unit);
        }
        if (r->enum_count > 0) {
            fprintf(out, ", .enums = enum_%zu, .enum_count = %du", i, r->enum_count);
        }
        if (strcmp(r->type, "COMPOSITE") == 0) {
            fprintf(out, ", .elem_size = %du", esz);
        }
        fprintf(out, ",\n      .value = val_%zu },\n", i);
    }
    fprintf(out, "};\n\n");

    /* 查找索引只存原表下标，保留参数与树的既有顺序。 */
    {
        static uint16_t order[MAX_ROWS];
        for (i = 0; i < n; i++) order[i] = (uint16_t)i;
        for (i = 1; i < n; i++) {
            uint16_t current = order[i];
            size_t j = i;
            while (j > 0u && rows[order[j - 1u]].addr > rows[current].addr) {
                order[j] = order[j - 1u];
                j--;
            }
            order[j] = current;
        }
        fprintf(out, "static const uint16_t g_addr_order[%zu] = {\n", n);
        for (i = 0; i < n; i++) {
            if (i % 16u == 0u) fprintf(out, "    ");
            fprintf(out, "%uu%s", order[i], i + 1u == n ? "" : ", ");
            if (i % 16u == 15u || i + 1u == n) fprintf(out, "\n");
        }
        fprintf(out, "};\n\n");
    }

    if (tree_count) {
        fprintf(out, "/* v2 命名树：记录前序，根节点 0 隐式存在。 */\n");
        fprintf(out, "static const bvpv_tree_record_t g_tree[%zu] = {\n", tree_count);
        for (i = 0; i < tree_count; i++) {
            const tree_row_t *r = &tree_rows[i];
            if (r->kind == 1) {
                fprintf(out, "    { .kind = 1u, .node_id = %uu, .parent_id = %uu, .key = \"%s\" },\n", r->node_id, r->parent_id, r->key);
            } else {
                fprintf(out, "    { .kind = 2u, .parent_id = %uu, .addr = 0x%06Xu },\n", r->parent_id, r->addr);
            }
        }
        fprintf(out, "};\n\n");
    }

    /* 上下文 + 初始化 */
    fprintf(out,
        "/* 目录服务上下文（静态，覆盖整个运行期） */\n"
        "static bvpv_service_ctx_t g_catalog_ctx = {\n"
        "    .params = g_params, .addr_order = g_addr_order, .states = g_states, .count = %zuu,\n"
        "    .device_type = 2u, .device_name = \"BV500-USB\",\n"
        "    .changes = g_changes, .change_capacity = 64u,\n",
        n);
    if (tree_count) { fprintf(out, "    .tree = g_tree, .tree_count = %zuu,\n", tree_count); }
    fprintf(out,
        "};\n"
        "static bool g_catalog_initialized;\n\n"
        "/* 首次调用初始化运行时状态；USB 重连不清空 RAM 值及 pending。 */\n"
        "bvpv_service_ctx_t *bvpv_catalog_data_ctx(void)\n"
        "{\n"
        "    if (!g_catalog_initialized) {\n"
        "        bvpv_catalog_init_states(&g_catalog_ctx);\n"
        "        g_catalog_initialized = true;\n"
        "    }\n"
        "    return &g_catalog_ctx;\n"
        "}\n");

    fclose(out);
    fprintf(stderr, "生成 %s：%zu 个参数\n", argv[2], n);
    return 0;
}
