#include "i18n_text.h"
#include <LittleFS.h>

// Cờ in-memory cho macro TR — true=EN, false=VI. Default VI (false).
bool i18n_isEn_ = false;

static const char* LANG_PATH = "/lang.txt";

void i18n_Init() {
  // LittleFS PHẢI đã mount trước (storage_Init lo việc đó). Gọi i18n_Init sau
  // storage_Init trong main.cpp.
  if (!LittleFS.exists(LANG_PATH)) {
    i18n_isEn_ = false;
    Serial.println("[i18n] /lang.txt khong co -> default vi");
    return;
  }
  File f = LittleFS.open(LANG_PATH, "r");
  if (!f) { i18n_isEn_ = false; return; }
  String s = f.readString();
  f.close();
  s.trim();
  i18n_isEn_ = (s == "en");
  Serial.printf("[i18n] load lang='%s'\n", i18n_isEn_ ? "en" : "vi");
}

void i18n_SetLang(const char* lang) {
  if (!lang) return;
  bool wantEn = (strcmp(lang, "en") == 0);
  File f = LittleFS.open(LANG_PATH, "w");
  if (f) {
    f.print(wantEn ? "en" : "vi");
    f.close();
  }
  i18n_isEn_ = wantEn;
  Serial.printf("[i18n] set lang='%s'\n", wantEn ? "en" : "vi");
}

const char* i18n_GetLang() {
  return i18n_isEn_ ? "en" : "vi";
}