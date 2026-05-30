/**
 * EETIUM Biometric Attendance — Google Apps Script backend
 *
 * Cả 2 firmware ESP32 (50user/1500user) gửi batch attendance qua HTTPS POST
 * tới script này. Script ghi vào Google Sheet, dedup theo (date + empid),
 * gộp nhiều lượt trong cùng ngày vào 1 row (Time 1..N).
 *
 * ===== TRIỂN KHAI — KHÔNG CẦN SỬA CODE =====
 * 1. Mở Google Sheet đích (tạo mới hoặc dùng sẵn có).
 * 2. Trong Sheet: Extensions → Apps Script (tạo project mới gắn vào Sheet).
 * 3. Dán TOÀN BỘ code này vào → Save (Ctrl+S).
 * 4. Deploy → New deployment → type "Web app":
 *      - Execute as:    Me
 *      - Who has access: Anyone
 *    → Deploy → Authorize → copy URL dạng
 *      https://script.google.com/macros/s/<GSID>/exec
 * 5. ESP32 → /settings → dán URL vào ô "Google Script ID" → Save. Xong.
 *
 * Script tự dùng Sheet nó được gắn vào (bound script). Tự tạo tab tên
 * "attendance" nếu chưa có. KHÔNG cần biết Sheet ID hay sửa biến nào.
 *
 * ===== STANDALONE MODE (tùy chọn) =====
 * Nếu tạo script tại script.google.com (không gắn vào Sheet) — ví dụ 1
 * script chung cho nhiều Sheet: điền SHEET_ID bên dưới (lấy từ URL
 * Sheet dạng .../d/<SHEET_ID>/edit). Để TRỐNG = dùng bound mode.
 *
 * ===== RE-DEPLOY KHI SỬA CODE =====
 * Save → Deploy → Manage deployments → biểu tượng bút chì → Version
 * "New version" → Deploy. URL KHÔNG đổi → ESP32 không cần config lại.
 *
 * ===== KIỂM TRA TRÙNG LẶP =====
 * Editor → chọn hàm checkDuplicates → Run → View → Logs.
 *
 * ===== HEADER SHEET (tự bootstrap) =====
 * Lần đầu chạy, script tự thêm 9 cột:
 *   Date | Employee ID | Name | Email | Position | Time 1..Time 4
 * Mỗi ngày 1 row/empid; các lượt quét trong ngày fill Time 1, Time 2, ...
 * (mở rộng tự động khi >4 lượt).
 *
 * ===== ĐỊNH DẠNG NGÀY =====
 * Firmware go-live gửi date dạng "29-05-2026" (DD-MM-YYYY). Sheet đang
 * chạy với firmware cũ có thể có "Day_29May2026". KHÔNG trộn 2 format
 * trong cùng Sheet → dedup hỏng. Dùng Sheet TRỐNG mới khi đổi firmware.
 */

// ===== TÙY CHỌN — mặc định để TRỐNG (dùng Sheet đã bind) =====
var SHEET_ID   = "";              // Chỉ điền khi standalone mode (xem header)
var SHEET_NAME = "attendance";    // Tên tab; tự tạo nếu chưa có
// ============================================================

// Lấy sheet đích — bound mode (mặc định) hoặc standalone qua SHEET_ID.
// Tự tạo tab nếu chưa có nên Sheet trống cũng chạy được ngay.
function getTargetSheet_() {
  var ss = SHEET_ID
    ? SpreadsheetApp.openById(SHEET_ID)
    : SpreadsheetApp.getActiveSpreadsheet();
  if (!ss) {
    throw new Error(
      "Không tìm thấy Sheet. Cách 1: mở Sheet → Extensions → Apps Script " +
      "(bound mode). Cách 2: điền SHEET_ID ở đầu file (standalone mode)."
    );
  }
  var sheet = ss.getSheetByName(SHEET_NAME);
  if (!sheet) sheet = ss.insertSheet(SHEET_NAME);
  return sheet;
}

function doGet(e) {
  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
  } catch (err) {
    return ContentService.createTextOutput("ERROR: lock timeout");
  }

  try {
    var sheet = getTargetSheet_();

    // Bootstrap header — chỉ 1 lần khi sheet còn trống
    if (sheet.getRange("A1").isBlank()) {
      sheet.appendRow(["Date", "Employee ID", "Name", "Email", "Position",
                       "Time 1", "Time 2", "Time 3", "Time 4"]);
      sheet.getRange("A1:I1").setFontWeight("bold").setBackground("#d9ead3");
    }

    // ===== BATCH PATH (ESP gửi 1 lúc nhiều records qua e.parameter.batch) =====
    if (e.parameter.batch) {
      var records;
      try {
        records = JSON.parse(e.parameter.batch);
      } catch (err) {
        return ContentService.createTextOutput("ERROR: invalid JSON");
      }
      return processBatch(sheet, records);
    }

    // ===== LEGACY single-record fallback (giữ tương thích query string) =====
    if (e.parameter.date) {
      return processBatch(sheet, [{
        date: e.parameter.date,
        time: e.parameter.time,
        empid: e.parameter.empid,
        empname: e.parameter.empname,
        empemail: e.parameter.empemail,
        emppos: e.parameter.emppos
      }]);
    }

    return ContentService.createTextOutput("No data");
  } finally {
    lock.releaseLock();
  }
}

function doPost(e) {
  // ESP gửi POST Content-Type: application/x-www-form-urlencoded, body
  // "batch=<urlencoded-json>". GAS auto-parse vào e.parameter.batch.
  return doGet(e);
}

// FAST PATH: fetch toàn sheet 1 lần, dùng Map dedup O(1), batch write cuối.
// Giảm 32 records × N rows = O(N²) → 1 lần fetch + N lookups = O(N).
// Cho batch 32 records với sheet 500 rows: ~3-5s → ~600ms.
function processBatch(sheet, records) {
  var lastRow = sheet.getLastRow();
  var lastCol = sheet.getLastColumn();
  if (lastCol < 6) lastCol = 6;  // tối thiểu Date..Time1

  // Pre-fetch toàn bộ data hiện có
  var allData = [];
  var rowMap = {};  // "date|empid" → index trong allData (0-based)
  if (lastRow >= 2) {
    allData = sheet.getRange(2, 1, lastRow - 1, lastCol).getDisplayValues();
    for (var i = 0; i < allData.length; i++) {
      var k = allData[i][0] + "|" + allData[i][1];
      rowMap[k] = i;
    }
  }

  var newRows = [];           // appendRow ở cuối
  var timeUpdates = [];       // {row, col, val} cho row đã tồn tại
  var added = 0, skipped = 0;

  for (var ri = 0; ri < records.length; ri++) {
    var r = records[ri];
    if (!r || !r.date || !r.empid) { skipped++; continue; }

    var key = String(r.date) + "|" + String(r.empid);

    if (rowMap[key] !== undefined) {
      // Row đã tồn tại → tìm slot time trống hoặc skip duplicate time
      var idx = rowMap[key];
      var row = allData[idx];
      var dupe = false;
      var insertCol = -1;
      for (var c = 5; c < row.length; c++) {
        if (row[c] === String(r.time)) { dupe = true; break; }
        if (insertCol === -1 && (row[c] === "" || row[c] === null)) insertCol = c;
      }
      if (dupe) { skipped++; continue; }
      if (insertCol === -1) insertCol = row.length;  // mở rộng cột mới
      timeUpdates.push({ row: idx + 2, col: insertCol + 1, val: r.time });
      // Update local copy để lần dedup trong cùng batch thấy được
      while (row.length <= insertCol) row.push("");
      row[insertCol] = r.time;
      added++;
    } else {
      // Row mới
      var newRow = [r.date, r.empid, r.empname || "", r.empemail || "",
                    r.emppos || "", r.time || ""];
      newRows.push(newRow);
      rowMap[key] = allData.length;
      allData.push(newRow);
      added++;
    }
  }

  // Batch write — setValues nhanh hơn loop appendRow
  if (newRows.length > 0) {
    var startRow = sheet.getLastRow() + 1;
    sheet.getRange(startRow, 1, newRows.length, 6).setValues(newRows);
  }
  for (var i = 0; i < timeUpdates.length; i++) {
    sheet.getRange(timeUpdates[i].row, timeUpdates[i].col).setValue(timeUpdates[i].val);
  }

  return ContentService.createTextOutput("OK added=" + added + " skipped=" + skipped);
}

// Utility: scan sheet tìm duplicate row + duplicate time. Chạy từ editor để verify.
function checkDuplicates() {
  var sheet = getTargetSheet_();
  var lastRow = sheet.getLastRow();
  var lastCol = sheet.getLastColumn();
  if (lastRow < 2) {
    Logger.log("Sheet rỗng");
    return;
  }
  var data = sheet.getRange(2, 1, lastRow - 1, lastCol).getDisplayValues();

  // 1) Duplicate row: cùng (date, empid)
  var rowKey = {};
  var dupRows = [];
  for (var i = 0; i < data.length; i++) {
    var k = data[i][0] + "|" + data[i][1];
    if (rowKey[k]) dupRows.push("Row " + (i + 2) + " trùng row " + rowKey[k] + " key=" + k);
    else rowKey[k] = i + 2;
  }

  // 2) Duplicate time trong cùng row
  var dupTimes = [];
  for (var i = 0; i < data.length; i++) {
    var seen = {};
    for (var c = 5; c < data[i].length; c++) {
      var t = data[i][c];
      if (t === "" || t === null) continue;
      if (seen[t]) {
        dupTimes.push("Row " + (i + 2) + " empid=" + data[i][1] +
                      " time " + t + " ở col " + seen[t] + " và " + (c + 1));
      } else {
        seen[t] = c + 1;
      }
    }
  }

  Logger.log("=== DUPLICATE ROWS (same date+empid): " + dupRows.length + " ===");
  dupRows.slice(0, 50).forEach(function(s) { Logger.log(s); });

  Logger.log("=== DUPLICATE TIMES within same row: " + dupTimes.length + " ===");
  dupTimes.slice(0, 50).forEach(function(s) { Logger.log(s); });

  Logger.log("Total rows: " + (lastRow - 1) + " | Total cols: " + lastCol);
}
