/*
 * ini.c — intentionally empty.
 *
 * All INI parser/writer implementation lives in include/ini.h as static
 * functions so that every translation unit that includes ini.h gets its
 * own copy without requiring a separate link step.  This avoids LNK2019
 * when test executables compile stereo.c in isolation (without ini.c).
 */
