#pragma once
/*
 * ini.h — Lightweight INI file parser / writer for VKS3D
 *
 * IMPLEMENTATION IS ENTIRELY IN THIS HEADER (static functions).
 * No separate ini.c or link step required.  Any .c that includes
 * ini.h gets a private copy of each function.  The compiler will
 * dead-strip ones that are never called from that translation unit.
 *
 * Supports:
 *   [Section]
 *   key = value          (whitespace around '=' is trimmed)
 *   ; line comment
 *   # line comment
 *
 * Keys and section names are matched case-insensitively.
 * Writer preserves existing file content and comments; it replaces the
 * value of an existing key in-place and appends new keys to the section.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>

#define INI__LINE_MAX 1024
#define INI__MAX_LINES 4096

/* ── Internal helpers ────────────────────────────────────────────────────── */

static char *ini__trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

static int ini__icase_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ── ini_read_str ─────────────────────────────────────────────────────────── */

static bool ini_read_str(const char *path, const char *section, const char *key,
                         char *out, size_t out_size)
{
    if (!path || !section || !key || !out || out_size == 0) return false;

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[INI__LINE_MAX];
    bool in_section = false;
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        nl = strchr(line, '\r');      if (nl) *nl = '\0';
        char *p = ini__trim(line);

        if (!*p || *p == ';' || *p == '#') continue;

        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close) { *close = '\0'; in_section = ini__icase_eq(p + 1, section); }
            continue;
        }
        if (!in_section) continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = ini__trim(p);
        char *v = ini__trim(eq + 1);
        char *ic = strchr(v, ';'); if (ic) { *ic = '\0'; ini__trim(v); }

        if (ini__icase_eq(k, key)) {
            size_t len = strlen(v);
            if (len >= out_size) len = out_size - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static float ini_read_float(const char *path, const char *section, const char *key, float def)
{
    char buf[64];
    if (!ini_read_str(path, section, key, buf, sizeof(buf))) return def;
    return (float)atof(buf);
}

static int ini_read_int(const char *path, const char *section, const char *key, int def)
{
    char buf[32];
    if (!ini_read_str(path, section, key, buf, sizeof(buf))) return def;
    return atoi(buf);
}

static bool ini_read_bool(const char *path, const char *section, const char *key, bool def)
{
    char buf[16];
    if (!ini_read_str(path, section, key, buf, sizeof(buf))) return def;
    if (ini__icase_eq(buf, "1") || ini__icase_eq(buf, "true") || ini__icase_eq(buf, "yes")) return true;
    if (ini__icase_eq(buf, "0") || ini__icase_eq(buf, "false") || ini__icase_eq(buf, "no")) return false;
    return def;
}

/* ── ini_write_str ────────────────────────────────────────────────────────── */

static bool ini_write_str(const char *path, const char *section, const char *key, const char *val)
{
    if (!path || !section || !key || !val) return false;

    char **lines  = (char **)calloc(INI__MAX_LINES, sizeof(char *));
    int    nlines = 0;
    bool   ok     = false;
    if (!lines) return false;

    FILE *f = fopen(path, "r");
    if (f) {
        char buf[INI__LINE_MAX];
        while (nlines < INI__MAX_LINES - 2 && fgets(buf, sizeof(buf), f)) {
            char *nl = strrchr(buf, '\n'); if (nl) *nl = '\0';
            nl = strrchr(buf, '\r');       if (nl) *nl = '\0';
            lines[nlines++] = _strdup(buf);
        }
        fclose(f);
    }

    int  section_line = -1, key_line = -1, insert_at = -1;
    bool in_sect      = false;

    for (int i = 0; i < nlines; i++) {
        char tmp[INI__LINE_MAX];
        strncpy(tmp, lines[i], INI__LINE_MAX - 1);
        tmp[INI__LINE_MAX - 1] = '\0';
        char *p = ini__trim(tmp);

        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close) {
                *close = '\0';
                if (ini__icase_eq(p + 1, section)) {
                    section_line = i; in_sect = true;
                } else if (in_sect) {
                    if (insert_at < 0) insert_at = i;
                    in_sect = false;
                }
            }
            continue;
        }
        if (!in_sect) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        if (ini__icase_eq(ini__trim(p), key)) { key_line = i; break; }
    }
    if (in_sect && insert_at < 0) insert_at = nlines;

    char new_line[INI__LINE_MAX];
    snprintf(new_line, sizeof(new_line), "%s=%s", key, val);

    if (key_line >= 0) {
        free(lines[key_line]);
        lines[key_line] = _strdup(new_line);
    } else if (section_line >= 0) {
        if (insert_at < 0) insert_at = nlines;
        if (nlines < INI__MAX_LINES - 1) {
            for (int i = nlines; i > insert_at; i--) lines[i] = lines[i - 1];
            lines[insert_at] = _strdup(new_line);
            nlines++;
        }
    } else {
        char sec_line[256];
        snprintf(sec_line, sizeof(sec_line), "[%s]", section);
        if (nlines < INI__MAX_LINES - 3) {
            if (nlines > 0) lines[nlines++] = _strdup("");
            lines[nlines++] = _strdup(sec_line);
            lines[nlines++] = _strdup(new_line);
        }
    }

    f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < nlines; i++) fprintf(f, "%s\n", lines[i] ? lines[i] : "");
        fclose(f);
        ok = true;
    }
    for (int i = 0; i < nlines; i++) free(lines[i]);
    free(lines);
    return ok;
}

static bool ini_write_float(const char *path, const char *section, const char *key, float val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", (double)val);
    return ini_write_str(path, section, key, buf);
}

static bool ini_write_int(const char *path, const char *section, const char *key, int val)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", val);
    return ini_write_str(path, section, key, buf);
}

