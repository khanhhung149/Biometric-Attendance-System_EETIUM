#ifndef STORAGE_H
#define STORAGE_H

#include "Arduino.h"
#include <FS.h>
#include <LittleFS.h>
#include <sqlite3.h>

bool storage_Init();
void storage_LogUsage();
String storage_UsageJson();  // JSON dung lượng FS + backlog cho endpoint /storage
// Kiểm tra định kỳ low watermark — gọi từ main loop mỗi 5 phút.
// Trả: 0 = OK, 1 = warning (>80%), 2 = critical (>95%).
int storage_CheckLowWatermark();
String readFile(fs::FS &fs, const char * path);
void writeFile(fs::FS &fs, const char * path, const char * message);
int db_exec1(sqlite3 *db, const char *sql);
int db_exec(sqlite3 *db, const char *sql);
int openDb(const char *filename, sqlite3 **db);

extern sqlite3 *test1_db;
extern String Sqid, Empid, Empname, EmpEmail, EmpPos, Empfid;
extern String sts7, sts8, sts9, sts10;
extern int sqlreturn;
extern int sqlrows;
extern String web_content;
#endif