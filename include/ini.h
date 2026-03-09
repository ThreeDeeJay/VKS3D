#pragma once
/*
 * ini.h — Lightweight INI file parser / writer for VKS3D
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

#include <stdbool.h>
#include <stddef.h>

/* ── Readers ─────────────────────────────────────────────────────────────── */

/* Read a raw string value. Returns true on success, copies into out[out_size]. */
bool ini_read_str  (const char *path, const char *section, const char *key,
                    char *out, size_t out_size);

float ini_read_float(const char *path, const char *section, const char *key, float def);
int   ini_read_int  (const char *path, const char *section, const char *key, int   def);
bool  ini_read_bool (const char *path, const char *section, const char *key, bool  def);

/* ── Writers ─────────────────────────────────────────────────────────────── */

/* Write a key=value.  Creates the file / section if absent.
 * Existing content and comments are preserved. */
bool ini_write_float(const char *path, const char *section, const char *key, float val);
bool ini_write_int  (const char *path, const char *section, const char *key, int   val);
bool ini_write_str  (const char *path, const char *section, const char *key, const char *val);
