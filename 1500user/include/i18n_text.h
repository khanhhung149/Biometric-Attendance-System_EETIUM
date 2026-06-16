#ifndef I18N_TEXT_H
#define I18N_TEXT_H

#include "Arduino.h"

// Macro TR — translate inline: TR("vi", "en") trả VI nếu lang=vi, EN nếu lang=en.
// Đặt EN trực tiếp cạnh VI → maintenance dễ, không cần dict trung tâm, không runtime
// lookup. Dùng được cho String concat: TR("Chờ ", "Wait ") + remain + "s".
//
// Web UI lưu language ở localStorage 'eetium_lang' → khi user bấm VI/EN trong
// Settings, JS POST /setlang để ESP32 ghi /lang.txt + update i18n_isEn_ in-memory.
extern bool i18n_isEn_;
static inline bool i18n_IsEn() { return i18n_isEn_; }

#define TR(vi, en) (i18n_IsEn() ? (en) : (vi))

// Load /lang.txt → set i18n_isEn_. Default "vi" nếu file không có/rỗng.
void i18n_Init();

// Set lang code "vi" hoặc "en" → ghi /lang.txt + update i18n_isEn_. Không reboot.
void i18n_SetLang(const char* lang);

// Trả "vi" hoặc "en" (cho endpoint /getlang).
const char* i18n_GetLang();

#endif