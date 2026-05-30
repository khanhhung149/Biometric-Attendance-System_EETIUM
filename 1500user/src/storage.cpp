#include "Arduino.h"
#include "storage.h"

String Sqid, Empid, Empname, EmpEmail, EmpPos, Empfid;
String sts7, sts8, sts9, sts10;
int sqlreturn = 0;
int sqlrows = 0;
sqlite3 *test1_db;
String web_content = "";

static const char* data = "Callback function called";
static const char* data1 = "Callback function called1";
static char *zErrMsg = 0;
static char *zErrMsg1 = 0;

// Forward declarations cho auto-recovery (định nghĩa ở cuối file)
void storage_OnDbError(int rc);
void storage_OnDbSuccess();
bool storage_Init() {
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed");
        return false;
    }

    // Migration: rename file cũ /backup.db → /backup.db nếu thiết bị từng dùng tên cũ.
    // Tránh mất data của user đã có sẵn khi update firmware.
    if (LittleFS.exists("/backup.db") && !LittleFS.exists("/backup.db")) {
        if (LittleFS.rename("/backup.db", "/backup.db")) {
            Serial.println("[DB] migrated /backup.db → /backup.db");
        } else {
            Serial.println("[DB] migration failed, keeping /backup.db");
        }
    }

    sqlite3_initialize();
    if (openDb("/littlefs/backup.db", &test1_db)) {
        return false;
    }

    // Bootstrap schema nếu DB rỗng (sau uploadfs hoặc factory reset).
    // sqlite3_open tự tạo file nhưng KHÔNG có table — phải CREATE TABLE IF NOT EXISTS.
    char *zErrBoot = 0;
    int rcBoot = sqlite3_exec(test1_db,
        "CREATE TABLE IF NOT EXISTS attendance ("
        "id INTEGER PRIMARY KEY, "
        "eid TEXT, "
        "employee_name TEXT, "
        "employee_email TEXT, "
        "position TEXT, "
        "fpid INTEGER)",
        0, 0, &zErrBoot);
    if (rcBoot != SQLITE_OK) {
        Serial.printf("[DB] CREATE TABLE attendance failed: %s\n", zErrBoot ? zErrBoot : "?");
        if (zErrBoot) sqlite3_free(zErrBoot);
    } else {
        Serial.println("[DB] attendance table ready");
    }

    // Cấu hình SQLite tiết kiệm memory cho ESP32
    // - cache_size âm = KB; -50 = giới hạn cache 50KB (default ~2MB, quá lớn cho ESP32!)
    // - temp_store=MEMORY: tạm trong RAM (nhanh nhưng tốn RAM, OK với DB nhỏ)
    // - journal_mode=MEMORY: journal trong RAM (mất ACID khi reset, nhưng giảm flash wear)
    char *zErr = 0;
    // Cache 20KB (giảm từ default ~2MB!) — tiết kiệm DRAM cho TLS handshake
    sqlite3_exec(test1_db, "PRAGMA cache_size = -20;", 0, 0, &zErr);
    if (zErr) sqlite3_free(zErr);
    sqlite3_exec(test1_db, "PRAGMA temp_store = MEMORY;", 0, 0, &zErr);
    if (zErr) sqlite3_free(zErr);

    // Tạo INDEX để SELECT * FROM attendance WHERE fpid=N không full-scan
    // Speed up query + tiết kiệm memory với DB có nhiều rows
    sqlite3_exec(test1_db, "CREATE INDEX IF NOT EXISTS idx_attendance_fpid ON attendance(fpid);",
                 0, 0, &zErr);
    if (zErr) {
      Serial.printf("[DB] CREATE INDEX failed: %s\n", zErr);
      sqlite3_free(zErr);
    } else {
      Serial.println("[DB] INDEX on attendance(fpid) ready");
    }

    return true;
}

// Đọc kích thước + đếm số dòng (records) của /backlog.txt.
// Đọc theo chunk 512B để nhanh; file thường nhỏ, lớn nhất ~1MB vẫn <100ms.
static void backlogStats(size_t* outBytes, int* outLines) {
    *outBytes = 0;
    *outLines = 0;
    File f = LittleFS.open("/backlog.txt", "r");
    if (!f) return;
    *outBytes = f.size();
    uint8_t buf[512];
    int lines = 0;
    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        for (int i = 0; i < n; i++) if (buf[i] == '\n') lines++;
    }
    f.close();
    *outLines = lines;
}

// Ước lượng số lượt quét offline còn ghi được: mỗi lượt tốn ~DB row + backlog
// line ≈ 230 byte. Dùng để cảnh báo sắp đầy buffer offline.
#define OFFLINE_BYTES_PER_PUNCH 230

void storage_LogUsage() {
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    size_t freeB = (total > used) ? (total - used) : 0;
    float pct    = total > 0 ? (used * 100.0f / total) : 0.0f;

    Serial.print("[FS] used=");
    Serial.print(used);
    Serial.print("B / total=");
    Serial.print(total);
    Serial.print("B (free=");
    Serial.print(freeB);
    Serial.print("B, ");
    Serial.print(pct, 1);
    Serial.println("%)");

    File db = LittleFS.open("/backup.db", "r");
    if (db) {
        Serial.print("[FS] /backup.db = ");
        Serial.print(db.size());
        Serial.println("B");
        db.close();
    }

    size_t blBytes; int blLines;
    backlogStats(&blBytes, &blLines);
    Serial.print("[FS] backlog offline = ");
    Serial.print(blLines);
    Serial.print(" records (");
    Serial.print(blBytes);
    Serial.print("B), con ghi them ~");
    Serial.print(freeB / OFFLINE_BYTES_PER_PUNCH);
    Serial.println(" luot quet offline");

    if (pct > 80.0f) {
        Serial.println("[FS] WARNING: usage > 80%");
    }
}

// JSON dung lượng cho endpoint /storage — xem nhanh khi đang chạy.
String storage_UsageJson() {
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    size_t freeB = (total > used) ? (total - used) : 0;
    float pct    = total > 0 ? (used * 100.0f / total) : 0.0f;

    size_t dbBytes = 0;
    File db = LittleFS.open("/backup.db", "r");
    if (db) { dbBytes = db.size(); db.close(); }

    size_t blBytes; int blLines;
    backlogStats(&blBytes, &blLines);

    char json[256];
    snprintf(json, sizeof(json),
        "{\"total\":%u,\"used\":%u,\"free\":%u,\"pct\":%.1f,"
        "\"db\":%u,\"backlogBytes\":%u,\"backlogLines\":%d,\"estOfflinePunchesLeft\":%u}",
        (unsigned)total, (unsigned)used, (unsigned)freeB, pct,
        (unsigned)dbBytes, (unsigned)blBytes, blLines,
        (unsigned)(freeB / OFFLINE_BYTES_PER_PUNCH));
    return String(json);
}

String readFile(fs::FS &fs, const char * path) {
    // Nếu file chưa tồn tại → tạo file rỗng để lần sau không bị log error.
    // Trả về String() (rỗng) → caller dùng giá trị mặc định.
    if (!fs.exists(path)) {
        File create = fs.open(path, FILE_WRITE);
        if (create) create.close();
        return String();
    }
    File file = fs.open(path);
    if (!file || file.isDirectory()) return String();
    String fileContent = file.readStringUntil('\n');
    file.close();
    return fileContent;
}
void writeFile(fs::FS &fs, const char * path, const char * message) {
  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("[FS] failed to write %s\n", path);
    return;
  }
  if (!file.print(message)) {
    Serial.printf("[FS] write failed: %s\n", path);
  }
  file.close();
}
static int callback1(void *data1, int argc, char **argv, char **azColName) {
  int i;
  String m = "";
  sqlrows++;
  for (i = 0; i < argc; i++) {
    if (i == 0) Sqid = argv[i] ? argv[i] : "NULL";
    else if (i == 1) Empid = argv[i] ? argv[i] : "NULL";
    else if (i == 2) Empname = argv[i] ? argv[i] : "NULL";
    else if (i == 3) EmpEmail = argv[i] ? argv[i] : "NULL";
    else if (i == 4) EmpPos = argv[i] ? argv[i] : "NULL";
    else if (i == 5) Empfid = argv[i] ? argv[i] : "NULL";
    else if (i == 6) sts7 = argv[i] ? argv[i] : "NULL";
    else if (i == 7) sts8 = argv[i] ? argv[i] : "NULL";
    else if (i == 8) sts9 = argv[i] ? argv[i] : "NULL";
    else if (i == 9) sts10 = argv[i] ? argv[i] : "NULL";
    m = argv[i] ? argv[i] : "NULL";
  }
  sqlreturn = m.toInt();
  return 0;
}

int db_exec1(sqlite3 *db, const char *sql) {
  sqlrows = 0;
  int rc = sqlite3_exec(db, sql, callback1, (void*)data1, &zErrMsg1);
  if (rc != SQLITE_OK) {
    Serial.printf("SQL error: %s | %s\n", sql, zErrMsg1);
    sqlite3_free(zErrMsg1);
    storage_OnDbError(rc);
  } else {
    storage_OnDbSuccess();
  }
  return rc;
}

int openDb(const char *filename, sqlite3 **db) {
  int rc = sqlite3_open(filename, db);
  if (rc) {
    Serial.printf("Can't open database: %s\n", sqlite3_errmsg(*db));
    return rc;
  } else {
    Serial.printf("Opened database successfully\n");
  }
  return rc;
}

static int callback(void *data, int argc, char **argv, char **azColName) {
  web_content += "<tr>";
  for (int i = 0; i < argc; i++) {
    web_content += "<td>";
    web_content += argv[i];
    web_content += "</td>";
  }
  // Cột Action: Edit (mở modal sửa thông tin, có nút "Lấy lại mẫu") + Delete
  web_content += "<td>";
  web_content += "<button class='edit-btn' onClick=\"edit('";
  web_content += argv[0];  // id record để fetch + update
  web_content += "')\" data-i18n=\"Edit\">Edit</button> ";
  web_content += "<button class='del-btn' onClick=\"del('";
  web_content += argv[0];
  web_content += "')\" data-i18n=\"Delete\">Delete</button>";
  web_content += "</td></tr>";
  return 0;
}

int db_exec(sqlite3 *db, const char *sql) {
  int rc = sqlite3_exec(db, sql, callback, (void*)data, &zErrMsg);
  if (rc != SQLITE_OK) {
    Serial.printf("SQL error: %s | %s\n", sql, zErrMsg);
    sqlite3_free(zErrMsg);
    storage_OnDbError(rc);
  } else {
    storage_OnDbSuccess();
  }
  return rc;
}

// ===== DB error tracking + auto-recovery =====
// SQLite + SPIFFS thỉnh thoảng vào trạng thái "disk I/O error" liên tục —
// chỉ cách thoát là remount filesystem. Đếm consecutive errors,
// reach threshold → restart ESP để mount lại sạch.
static int consecutiveDbErrors = 0;
#define DB_ERROR_THRESHOLD 3

void storage_OnDbError(int rc) {
  consecutiveDbErrors++;
  Serial.print("[DB] consecutive errors: ");
  Serial.print(consecutiveDbErrors);
  Serial.print("/");
  Serial.println(DB_ERROR_THRESHOLD);
  if (consecutiveDbErrors >= DB_ERROR_THRESHOLD) {
    Serial.println("[DB] Too many I/O errors — restarting ESP to recover...");
    delay(500);
    ESP.restart();
  }
}

void storage_OnDbSuccess() {
  if (consecutiveDbErrors > 0) {
    Serial.print("[DB] recovered after ");
    Serial.print(consecutiveDbErrors);
    Serial.println(" errors");
  }
  consecutiveDbErrors = 0;
}


