#include "config.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features.h"
#include "util.h"

Config g_config;

static int GetIniSection(const char *s) {
  if (StringEqualsNoCase(s, "[KeyMap]")) return 0;
  if (StringEqualsNoCase(s, "[Graphics]")) return 1;
  if (StringEqualsNoCase(s, "[Sound]")) return 2;
  if (StringEqualsNoCase(s, "[General]")) return 3;
  if (StringEqualsNoCase(s, "[Features]")) return 4;
  if (StringEqualsNoCase(s, "[GamepadMap]")) return 5;
  return -1;
}

bool ParseBool(const char *value, bool *result) {
  bool rv = false;
  switch (*value++ | 32) {
  case '0': if (*value == 0) break; return false;
  case 'f': if (StringEqualsNoCase(value, "alse")) break; return false;
  case 'n': if (StringEqualsNoCase(value, "o")) break; return false;
  case 'o':
    rv = (*value | 32) == 'n';
    if (StringEqualsNoCase(value, rv ? "n" : "ff")) break;
    return false;
  case '1': rv = true; if (*value == 0) break; return false;
  case 'y': rv = true; if (StringEqualsNoCase(value, "es")) break; return false;
  case 't': rv = true; if (StringEqualsNoCase(value, "rue")) break; return false;
  default: return false;
  }
  if (result) {
    *result = rv;
    return true;
  }
  return rv;
}

static bool ParseBoolBit(const char *value, uint32 *data, uint32 mask) {
  bool tmp;
  if (!ParseBool(value, &tmp))
    return false;
  *data = *data & ~mask | (tmp ? mask : 0);
  return true;
}

static bool HandleIniConfig(int section, const char *key, char *value) {
  /* Keyboard and GamepadMap sections are intentionally ignored on PSP.
   * Physical PSP controls are handled by platform/psp/psp_input.c. */
  if (section == 0 || section == 5)
    return true;

  if (section == 1) {
    if (StringEqualsNoCase(key, "EnhancedMode7")) return ParseBool(value, &g_config.enhanced_mode7);
    if (StringEqualsNoCase(key, "NewRenderer")) return ParseBool(value, &g_config.new_renderer);
    if (StringEqualsNoCase(key, "IgnoreAspectRatio")) return ParseBool(value, &g_config.ignore_aspect_ratio);
    if (StringEqualsNoCase(key, "Fullscreen")) { g_config.fullscreen = (uint8)strtol(value, NULL, 10); return true; }
    if (StringEqualsNoCase(key, "WindowScale")) { g_config.window_scale = (uint8)strtol(value, NULL, 10); return true; }
    if (StringEqualsNoCase(key, "OutputMethod") || StringEqualsNoCase(key, "LinearFiltering") ||
        StringEqualsNoCase(key, "WindowSize") || StringEqualsNoCase(key, "LinkGraphics") ||
        StringEqualsNoCase(key, "Shader")) return true;
    if (StringEqualsNoCase(key, "NoSpriteLimits")) return ParseBool(value, &g_config.no_sprite_limits);
  } else if (section == 2) {
    if (StringEqualsNoCase(key, "EnableAudio")) return ParseBool(value, &g_config.enable_audio);
    if (StringEqualsNoCase(key, "AudioFreq")) { g_config.audio_freq = (uint16)strtol(value, NULL, 10); return true; }
    if (StringEqualsNoCase(key, "AudioChannels")) { g_config.audio_channels = (uint8)strtol(value, NULL, 10); return true; }
    if (StringEqualsNoCase(key, "AudioSamples")) { g_config.audio_samples = (uint16)strtol(value, NULL, 10); return true; }
    if (StringEqualsNoCase(key, "EnableMSU")) { g_config.enable_msu = 0; return true; }
    if (StringEqualsNoCase(key, "MSUPath")) { g_config.msu_path = value; return true; }
    if (StringEqualsNoCase(key, "MSUVolume")) { g_config.msuvolume = atoi(value); return true; }
    if (StringEqualsNoCase(key, "ResumeMSU")) return ParseBool(value, &g_config.resume_msu);
  } else if (section == 3) {
    if (StringEqualsNoCase(key, "Autosave")) { g_config.autosave = (bool)strtol(value, NULL, 10); return true; }
    if (StringEqualsNoCase(key, "DisplayPerfInTitle") || StringEqualsNoCase(key, "DisableFrameDelay")) return true;
  }
  return false;
}

static bool ParseOneConfigFile(const char *filename, int depth) {
  char *filedata = (char*)ReadWholeFile(filename, NULL), *p;
  if (!filedata)
    return false;
  
  int section = -2;
  g_config.memory_buffer = filedata;

  for (int lineno = 1; (p = NextLineStripComments(&filedata)) != NULL; lineno++) {
    if (*p == 0)
      continue; // empty line
    if (*p == '[') {
      section = GetIniSection(p);
      if (section < 0)
        fprintf(stderr, "%s:%d: Invalid .ini section %s\n", filename, lineno, p);
    } else if (*p == '!' && SkipPrefix(p + 1, "include ")) {
      char *tt = p + 8;
      char *new_filename = ReplaceFilenameWithNewPath(filename, NextPossiblyQuotedString(&tt));
      if (depth > 10 || !ParseOneConfigFile(new_filename, depth + 1))
        fprintf(stderr, "Warning: Unable to read %s\n", new_filename);
      free(new_filename);
    } else if (section == -2) {
      fprintf(stderr, "%s:%d: Expecting [section]\n", filename, lineno);
    } else {
      char *v = SplitKeyValue(p);
      if (v == NULL) {
        fprintf(stderr, "%s:%d: Expecting 'key=value'\n", filename, lineno);
        continue;
      }
      if (section >= 0 && !HandleIniConfig(section, p, v))
        fprintf(stderr, "%s:%d: Can't parse '%s'\n", filename, lineno, p);
    }
  }
  return true;
}

void ParseConfigFile(const char *filename) {
  g_config.msuvolume = 100;  // default msu volume, 100%

  if (filename != NULL || !ParseOneConfigFile("sm.user.ini", 0)) {
    if (filename == NULL)
      filename = "sm.ini";
    if (!ParseOneConfigFile(filename, 0))
      fprintf(stderr, "Warning: Unable to read config file %s\n", filename);
  }
}
