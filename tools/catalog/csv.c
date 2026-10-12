#include "csv.h"

const char *csv_skip_bom(const char *s)
{
    /* UTF-8 BOM = EF BB BF */
    if ((unsigned char)s[0] == 0xEFu && (unsigned char)s[1] == 0xBBu &&
        (unsigned char)s[2] == 0xBFu) {
        return s + 3;
    }
    return s;
}

size_t csv_split_line(char *line, char **fields, size_t max_fields)
{
    size_t n = 0;
    char  *p = line;

    for (;;) {
        char *f;
        int   more = 0;

        if (n >= max_fields || *p == '\0') {
            break;
        }

        /* 最后一个允许字段：吸收剩余全部（含逗号），供 DAT 备注列等使用 */
        if (n == max_fields - 1) {
            char *e;
            if (*p == '"') {
                /* 引号字段仍按引号解析以去掉外层引号 */
                char *w;
                p++;
                f = p;
                w = p;
                while (*p != '\0') {
                    if (*p == '"') {
                        if (p[1] == '"') { *w++ = '"'; p += 2; }
                        else { p++; break; }
                    } else {
                        *w++ = *p++;
                    }
                }
                *w = '\0';
            } else {
                /* 去掉行尾 \r \n */
                f = p;
                e = p;
                while (*e != '\0') { e++; }
                while (e > p && (e[-1] == '\r' || e[-1] == '\n')) { *--e = '\0'; }
            }
            fields[n++] = f;
            break;
        }

        if (*p == '"') {
            /* 引号包裹字段：就地折叠 "" → "，读到闭引号 */
            char *w;
            p++;
            f = p;
            w = p;
            while (*p != '\0') {
                if (*p == '"') {
                    if (p[1] == '"') {
                        *w++ = '"';
                        p += 2;
                    } else {
                        p++;
                        break;   /* 闭引号 */
                    }
                } else {
                    *w++ = *p++;
                }
            }
            *w = '\0';
            while (*p != '\0' && *p != ',') {
                p++;             /* 跳过闭引号后到逗号前的杂散字符 */
            }
            if (*p == ',') {
                p++;
                more = 1;
            }
        } else {
            f = p;
            while (*p != '\0' && *p != ',') {
                p++;
            }
            if (*p == ',') {
                *p = '\0';
                p++;
                more = 1;
            } else {
                *p = '\0';
            }
        }
        fields[n++] = f;
        if (!more) {
            break;
        }
        /* 行尾逗号代表最后一个空字段，与带引号的空串等价。 */
        if (*p == '\0') {
            if (n < max_fields) { fields[n++] = p; }
            break;
        }
    }
    return n;
}

char *csv_trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    end = s;
    while (*end != '\0') {
        end++;
    }
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
        *--end = '\0';
    }
    return s;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

int csv_parse_hex_bytes(const char *text, uint8_t *out, int max)
{
    int n = 0, hi = -1;

    for (; *text != '\0'; text++) {
        int d;
        if (*text == ' ' || *text == '\t' || *text == ',' ||
            *text == ':' || *text == '-') {
            continue;
        }
        d = hex_nibble(*text);
        if (d < 0) { return -1; }
        if (hi < 0) { hi = d; continue; }
        if (n >= max) { return -1; }
        out[n++] = (uint8_t)((hi << 4) | d);
        hi = -1;
    }
    return (hi >= 0) ? -1 : n;   /* 奇数个半字节 */
}

int csv_format_hex_bytes(const uint8_t *b, int n, char *out, size_t outsz)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;

    if (outsz < (size_t)n * 2u + 1u) { return -1; }
    for (i = 0; i < n; i++) {
        out[i * 2 + 0] = hx[(b[i] >> 4) & 0x0Fu];
        out[i * 2 + 1] = hx[b[i] & 0x0Fu];
    }
    out[n * 2] = '\0';
    return n * 2;
}
