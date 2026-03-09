/*
 * ini.c — Lightweight INI file parser / writer
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ini.h"

#define INI_LINE_MAX 1024

/* ── String helpers ──────────────────────────────────────────────────────── */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

static int icase_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ── ini_read_str ─────────────────────────────────────────────────────────── */

bool ini_read_str(const char *path, const char *section, const char *key,
                  char *out, size_t out_size)
{
    if (!path || !section || !key || !out || out_size == 0) return false;

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[INI_LINE_MAX];
    bool in_section = false;
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        char *p = trim(line);

        /* Skip blank lines and comments */
        if (!*p || *p == ';' || *p == '#') continue;

        /* Section header */
        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close) {
                *close = '\0';
                in_section = icase_eq(p + 1, section);
            }
            continue;
        }

        if (!in_section) continue;

        /* key=value */
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(p);
        char *v = trim(eq + 1);
        /* Strip inline comment */
        char *ic = strchr(v, ';');
        if (ic) { *ic = '\0'; trim(v); }

        if (icase_eq(k, key)) {
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

float ini_read_float(const char *path, const char *section, const char *key, float def)
{
    char buf[64];
    if (!ini_read_str(path, section, key, buf, sizeof(buf))) return def;
    return (float)atof(buf);
}

int ini_read_int(const char *path, const char *section, const char *key, int def)
{
    char buf[32];
    if (!ini_read_str(path, section, key, buf, sizeof(buf))) return def;
    return atoi(buf);
}

bool ini_read_bool(const char *path, const char *section, const char *key, bool def)
{
    char buf[16];
    if (!ini_read_str(path, section, key, buf, sizeof(buf))) return def;
    if (icase_eq(buf, "1") || icase_eq(buf, "true") || icase_eq(buf, "yes")) return true;
    if (icase_eq(buf, "0") || icase_eq(buf, "false") || icase_eq(buf, "no")) return false;
    return def;
}

/* ── ini_write_str ────────────────────────────────────────────────────────
 *
 * Algorithm:
 *  1. Read entire file into a list of lines (or start with empty list).
 *  2. Find the target section.  If absent, append it.
 *  3. Inside the section scan for key=*.  If found, replace that line.
 *     If not found, insert a new line just before the next [Section] or EOF.
 *  4. Write all lines back to the file.
 * ───────────────────────────────────────────────────────────────────────── */

#define MAX_LINES 4096

bool ini_write_str(const char *path, const char *section, const char *key, const char *val)
{
    if (!path || !section || !key || !val) return false;

    /* ── Read existing lines ── */
    char **lines    = calloc(MAX_LINES, sizeof(char *));
    int    nlines   = 0;
    bool   ok       = false;

    if (!lines) return false;

    FILE *f = fopen(path, "r");
    if (f) {
        char buf[INI_LINE_MAX];
        while (nlines < MAX_LINES - 2 && fgets(buf, sizeof(buf), f)) {
            /* Normalize line ending */
            char *nl = strrchr(buf, '\n');
            if (nl) *nl = '\0';
            nl = strrchr(buf, '\r');
            if (nl) *nl = '\0';
            lines[nlines++] = _strdup(buf);
        }
        fclose(f);
    }

    /* ── Locate section and key ── */
    int section_line = -1;  /* index of [section] header */
    int key_line     = -1;  /* index of existing key=value line */
    int insert_at    = -1;  /* where to insert if key not found */
    bool in_sect     = false;

    for (int i = 0; i < nlines; i++) {
        char tmp[INI_LINE_MAX];
        strncpy(tmp, lines[i], INI_LINE_MAX - 1);
        tmp[INI_LINE_MAX - 1] = '\0';
        char *p = trim(tmp);

        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close) {
                *close = '\0';
                if (icase_eq(p + 1, section)) {
                    section_line = i;
                    in_sect = true;
                } else if (in_sect) {
                    /* Hit next section while inside ours — insert before this line */
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
        if (icase_eq(trim(p), key)) {
            key_line = i;
            break;
        }
    }
    if (in_sect && insert_at < 0) insert_at = nlines;  /* end of file */

    /* ── Build the new line ── */
    char new_line[INI_LINE_MAX];
    snprintf(new_line, sizeof(new_line), "%s=%s", key, val);

    if (key_line >= 0) {
        /* Replace in-place */
        free(lines[key_line]);
        lines[key_line] = _strdup(new_line);
    } else if (section_line >= 0) {
        /* Insert before next section / EOF */
        if (insert_at < 0) insert_at = nlines;
        if (nlines < MAX_LINES - 1) {
            /* Shift lines down */
            for (int i = nlines; i > insert_at; i--)
                lines[i] = lines[i - 1];
            lines[insert_at] = _strdup(new_line);
            nlines++;
        }
    } else {
        /* Section not found — append section + key at end */
        char sec_line[256];
        snprintf(sec_line, sizeof(sec_line), "[%s]", section);
        if (nlines < MAX_LINES - 3) {
            /* Blank separator if file is non-empty */
            if (nlines > 0) lines[nlines++] = _strdup("");
            lines[nlines++] = _strdup(sec_line);
            lines[nlines++] = _strdup(new_line);
        }
    }

    /* ── Write back ── */
    f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < nlines; i++)
            fprintf(f, "%s\n", lines[i] ? lines[i] : "");
        fclose(f);
        ok = true;
    }

    for (int i = 0; i < nlines; i++) free(lines[i]);
    free(lines);
    return ok;
}

bool ini_write_float(const char *path, const char *section, const char *key, float val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", (double)val);
    return ini_write_str(path, section, key, buf);
}

bool ini_write_int(const char *path, const char *section, const char *key, int val)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", val);
    return ini_write_str(path, section, key, buf);
}
