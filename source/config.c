/* config.c -- simple configuration parser
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <switch.h>

#include "config.h"

#define CONFIG_VARS \
  CONFIG_VAR_INT(screen_width); \
  CONFIG_VAR_INT(screen_height); \
  CONFIG_VAR_INT(language);

Config config;

// actual screen size that is in use right now
int screen_width = 1280;
int screen_height = 720;

static inline void parse_var(const char *name, const char *value) {
  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
}

int read_config(const char *file) {
  char line[1024] = { 0 };

  memset(&config, 0, sizeof(Config));
  config.screen_width = -1;  // auto (picks 720p handheld / 1080p docked)
  config.screen_height = -1;
  config.language = -1;      // follow the console language

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#') continue; // skip comments
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      if (*tmp != 0) {
        *tmp = 0;
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; isspace((int)*tmp); --tmp) *tmp = 0;
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);

  return 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  #define CONFIG_VAR_INT(var) fprintf(f, "%s %d\n", #var, config.var)
  CONFIG_VARS
  #undef CONFIG_VAR_INT

  fclose(f);

  return 0;
}

// ISO-639 3-letter (639-2/B) + 2-letter region, indexed by config.language.
// Shadow of Ronin ships EFIGS + Dutch/Danish/Brazilian/Russian/Turkish plus the
// CJK languages; "eng"/"US" is the safe default. These feed the JNI locale
// callbacks (getCountryCode / GetCurrentLanguageCountryCode); the in-game text
// language itself is chosen by the engine's own fnaDevice_GetSystemLanguage.
static const struct { const char *iso3, *region; } lang_tab[] = {
  { "eng", "US" }, //  0 English
  { "fre", "FR" }, //  1 French
  { "ger", "DE" }, //  2 German
  { "ita", "IT" }, //  3 Italian
  { "spa", "ES" }, //  4 Spanish
  { "dut", "NL" }, //  5 Dutch
  { "dan", "DK" }, //  6 Danish
  { "por", "BR" }, //  7 Portuguese (Brazil)
  { "rus", "RU" }, //  8 Russian
  { "tur", "TR" }, //  9 Turkish
  { "jpn", "JP" }, // 10 Japanese
  { "chi", "CN" }, // 11 Chinese
  { "kor", "KR" }, // 12 Korean
};

#define LANG_TAB_N ((int)(sizeof(lang_tab) / sizeof(*lang_tab)))

// resolves the active language index (config.language, or the console language
// when -1) to an index into lang_tab.
static int resolve_lang_index(void) {
  if (config.language >= 0 && config.language < LANG_TAB_N)
    return config.language;

  // follow the console language
  u64 lcode = 0;
  SetLanguage sl = SetLanguage_ENUS;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&lcode)))
      setMakeLanguage(lcode, &sl);
    setExit();
  }
  switch (sl) {
    case SetLanguage_FR:
    case SetLanguage_FRCA:  return 1;
    case SetLanguage_DE:    return 2;
    case SetLanguage_IT:    return 3;
    case SetLanguage_ES:
    case SetLanguage_ES419: return 4;
    case SetLanguage_NL:    return 5;
    case SetLanguage_PT:
    case SetLanguage_PTBR:  return 7;
    case SetLanguage_RU:    return 8;
    case SetLanguage_JA:    return 10;
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS:
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANT: return 11;
    case SetLanguage_KO:    return 12;
    default:                return 0; // English
  }
}

const char *config_lang_iso3(void) {
  return lang_tab[resolve_lang_index()].iso3;
}

const char *config_region(void) {
  return lang_tab[resolve_lang_index()].region;
}
