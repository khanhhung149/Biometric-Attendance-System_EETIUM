/*
 * i18n.js — Vietnamese/English language toggle for EETIUM Biometric web UI
 *
 * Cách dùng: chỉ cần thêm <script src="/i18n.js"></script> vào <head> của HTML.
 * Script tự dịch text matching trong dictionary T, lưu lựa chọn vào localStorage,
 * và observe DOM cho AJAX content (db.html SPA load Settings/Enroll/Account vào #table).
 *
 * Cách dùng đặc biệt:
 *   data-i18n="Key"        → force dịch element này, key = text tiếng Anh gốc
 *   data-i18n-html="Key"   → giống trên nhưng dùng innerHTML (cho chuỗi có <code>, <b>...)
 *   data-i18n-placeholder="Key" → dịch placeholder của input/textarea
 *
 * Single source of truth — thêm key mới CHỈ sửa file này.
 */
(function () {
  const T = {
    // ===== Navigation / Header =====
    "Database": { vi: "Dữ liệu", en: "Database" },
    "Enroll": { vi: "Đăng ký mới", en: "Enroll" },
    "Settings": { vi: "Cài đặt", en: "Settings" },
    "Account": { vi: "Tài khoản", en: "Account" },
    "Biometric Database Server": { vi: "Máy chủ Vân tay EETIUM", en: "Biometric Database Server" },
    "Logout": { vi: "Đăng xuất", en: "Logout" },
    "Sign Out": { vi: "Đăng xuất", en: "Sign Out" },
    "Sign out": { vi: "Đăng xuất", en: "Sign out" },

    // ===== Login page =====
    "Username": { vi: "Tên đăng nhập", en: "Username" },
    "Password": { vi: "Mật khẩu", en: "Password" },
    "Login": { vi: "Đăng nhập", en: "Login" },
    "Sign In": { vi: "Đăng nhập", en: "Sign In" },
    "Remember me": { vi: "Ghi nhớ", en: "Remember me" },
    "Login Name": { vi: "Tên đăng nhập", en: "Login Name" },
    "Login Password": { vi: "Mật khẩu", en: "Login Password" },

    // ===== Settings page =====
    "Google Script URL": { vi: "URL Google Script", en: "Google Script URL" },
    "IP Mode": { vi: "Chế độ IP", en: "IP Mode" },
    "Auto IP (DHCP)": { vi: "Tự động (DHCP)", en: "Auto IP (DHCP)" },
    "Manual IP": { vi: "IP thủ công", en: "Manual IP" },
    "Gateway": { vi: "Gateway", en: "Gateway" },
    "Timezone (offset UTC)": { vi: "Múi giờ (offset UTC)", en: "Timezone (offset UTC)" },
    "Cancel": { vi: "Hủy", en: "Cancel" },
    "Save": { vi: "Lưu", en: "Save" },
    "Submit": { vi: "Gửi", en: "Submit" },
    "Date": { vi: "Ngày", en: "Date" },
    "Time": { vi: "Giờ", en: "Time" },
    "mDNS Hostname": { vi: "Tên mDNS", en: "mDNS Hostname" },
    "Device Name": { vi: "Tên thiết bị", en: "Device Name" },

    // ===== Account page =====
    "Old Password": { vi: "Mật khẩu cũ", en: "Old Password" },
    "Current Password": { vi: "Mật khẩu hiện tại", en: "Current Password" },
    "Current login password": { vi: "Mật khẩu hiện tại", en: "Current login password" },
    "New Password": { vi: "Mật khẩu mới", en: "New Password" },
    "Confirm Password": { vi: "Xác nhận mật khẩu", en: "Confirm Password" },
    "Change Password": { vi: "Đổi mật khẩu", en: "Change Password" },
    "Update Account": { vi: "Cập nhật tài khoản", en: "Update Account" },
    "Change": { vi: "Đổi", en: "Change" },
    "Change login name": { vi: "Đổi tên đăng nhập", en: "Change login name" },
    "Change the login password": { vi: "Đổi mật khẩu đăng nhập", en: "Change the login password" },
    "New login name": { vi: "Tên đăng nhập mới", en: "New login name" },
    "New login password": { vi: "Mật khẩu mới", en: "New login password" },
    "Retype new login password": { vi: "Nhập lại mật khẩu mới", en: "Retype new login password" },
    "End current session": { vi: "Kết thúc phiên hiện tại", en: "End current session" },

    // ===== Database table / Enroll / EditUser =====
    "Employee ID": { vi: "Mã NV", en: "Employee ID" },
    "Employee ID (auto)": { vi: "Mã NV (tự động)", en: "Employee ID (auto)" },
    "Auto": { vi: "Tự động", en: "Auto" },
    "(Auto)": { vi: "(Tự động)", en: "(Auto)" },
    "Name": { vi: "Họ tên", en: "Name" },
    "Full Name": { vi: "Họ tên", en: "Full Name" },
    "Email": { vi: "Email", en: "Email" },
    "Email ID": { vi: "Email", en: "Email ID" },
    "Position": { vi: "Chức vụ", en: "Position" },
    "Designations": { vi: "Chức vụ", en: "Designations" },
    "Department": { vi: "Phòng ban", en: "Department" },
    "Staff": { vi: "Nhân viên", en: "Staff" },
    "Fingerprint ID": { vi: "Mã vân tay", en: "Fingerprint ID" },
    "Finger Print ID": { vi: "Mã vân tay", en: "Finger Print ID" },
    "Action": { vi: "Thao tác", en: "Action" },
    "Actions": { vi: "Thao tác", en: "Actions" },
    "Delete": { vi: "Xóa", en: "Delete" },
    "Edit": { vi: "Sửa", en: "Edit" },
    "Edit user info": { vi: "Sửa thông tin user", en: "Edit user info" },
    "Re-enroll": { vi: "Đăng ký lại", en: "Re-enroll" },
    "Retake sample": { vi: "Lấy lại mẫu", en: "Retake sample" },
    "Fingerprints": { vi: "Vân tay đã đăng ký", en: "Fingerprints" },
    "Add fingerprint": { vi: "Thêm vân tay", en: "Add fingerprint" },
    "Add User": { vi: "Thêm user", en: "Add User" },
    "Scan": { vi: "Quét vân tay", en: "Scan" },
    "Place Finger": { vi: "Đặt ngón tay", en: "Place Finger" },
    "Total users": { vi: "Tổng số user", en: "Total users" },
    "Page": { vi: "Trang", en: "Page" },
    "Prev": { vi: "Trước", en: "Prev" },
    "Next": { vi: "Sau", en: "Next" },
    "Search": { vi: "Tìm kiếm", en: "Search" },

    // ===== Table headers (UPPER) — dùng làm key cụ thể, tránh đụng "No" common =====
    "NO": { vi: "STT", en: "NO" },
    "FULL NAME": { vi: "HỌ TÊN", en: "FULL NAME" },
    "EMAIL": { vi: "EMAIL", en: "EMAIL" },
    "DESIGNATIONS": { vi: "CHỨC VỤ", en: "DESIGNATIONS" },
    "FINGER PRINT ID": { vi: "MÃ VÂN TAY", en: "FINGER PRINT ID" },
    "MÃ NV": { vi: "MÃ NV", en: "EMPLOYEE ID" },
    "THAO TÁC": { vi: "THAO TÁC", en: "ACTION" },

    // ===== Account: Backup & Restore section =====
    "Database Backup & Restore": { vi: "Sao lưu & Phục hồi DB", en: "Database Backup & Restore" },
    "Backup": { vi: "Sao lưu", en: "Backup" },
    "Restore": { vi: "Phục hồi", en: "Restore" },
    "Backup database": { vi: "Sao lưu database", en: "Backup database" },
    "Restore database": { vi: "Phục hồi database", en: "Restore database" },
    "Sync check": { vi: "Kiểm tra đồng bộ", en: "Sync check" },
    "Compare database records vs sensor templates": { vi: "So sánh DB records với sensor templates", en: "Compare database records vs sensor templates" },
    "Download": { vi: "Tải xuống", en: "Download" },
    "Upload": { vi: "Tải lên", en: "Upload" },
    "Check": { vi: "Kiểm tra", en: "Check" },
    "BackupDesc": {
      vi: "Tải <code>backup.db</code> về trước khi chạy <code>uploadfs</code>",
      en: "Download <code>backup.db</code> before running <code>uploadfs</code>"
    },
    "RestoreDesc": {
      vi: "Tải lên file <code>backup.db</code> đã sao lưu (thiết bị sẽ khởi động lại)",
      en: "Upload a previously backed-up <code>backup.db</code> (device will restart)"
    },

    // ===== AP Setup page =====
    "WiFi Setup": { vi: "Cấu hình WiFi", en: "WiFi Setup" },
    "Network Name": { vi: "Tên mạng", en: "Network Name" },
    "SSID": { vi: "SSID", en: "SSID" },
    "WiFi Password": { vi: "Mật khẩu WiFi", en: "WiFi Password" },
    "Available Networks": { vi: "Mạng khả dụng", en: "Available Networks" },
    "Refresh": { vi: "Làm mới", en: "Refresh" },
    "Connect": { vi: "Kết nối", en: "Connect" },
    "Scanning...": { vi: "Đang quét...", en: "Scanning..." },
    "Connected": { vi: "Đã kết nối", en: "Connected" },
    "Connecting...": { vi: "Đang kết nối...", en: "Connecting..." },
    "Finish": { vi: "Hoàn tất", en: "Finish" },

    // ===== Common =====
    "Yes": { vi: "Có", en: "Yes" },
    "OK": { vi: "OK", en: "OK" },
    "Confirm": { vi: "Xác nhận", en: "Confirm" },
    "Loading...": { vi: "Đang tải...", en: "Loading..." },
    "Error": { vi: "Lỗi", en: "Error" },
    "Success": { vi: "Thành công", en: "Success" },
    "Restart": { vi: "Khởi động lại", en: "Restart" }
    // Lưu ý: KHÔNG đặt "No": "Không" ở đây vì xung đột với "No" header table (= STT).
    // Modal yes/no dùng "Hủy"/"Cancel" thay vì "No".
  };

  const LANG_KEY = 'eetium_lang';
  function getLang() {
    return localStorage.getItem(LANG_KEY) || 'vi';
  }
  function setLang(l) {
    localStorage.setItem(LANG_KEY, l);
    applyAll();
    updateToggle();
  }

  // Selector rộng — bao tất cả element thường chứa text cần dịch.
  // Gộp từ inline cũ của Settings/db (có thêm span, .account-title, .field, .change,
  // .signout-btn, .action-btn) để giữ nguyên hành vi.
  const SCAN_SELECTOR =
    'button, label, h1, h2, h3, h4, h5, th, option, a, span, ' +
    '.brand-title, .form-label, .nav-link, .btn, .card-title, ' +
    '.account-title, .field, .change, .signout-btn, .action-btn';

  function translateElement(el) {
    if (el.children.length > 0) return;             // có child → skip
    if (el.hasAttribute('data-i18n')) return;       // explicit attr đã handle
    let key = el.getAttribute('data-i18n-key');
    if (!key) {
      const text = (el.textContent || '').trim();
      if (T[text]) { key = text; el.setAttribute('data-i18n-key', key); }
    }
    if (key && T[key]) {
      const lang = getLang();
      const tr = T[key][lang];
      if (tr !== undefined) el.textContent = tr;
    }
  }

  function applyAll() {
    const lang = getLang();
    // 1. data-i18n attribute (text-only, ưu tiên cao)
    document.querySelectorAll('[data-i18n]').forEach(function (el) {
      const key = el.getAttribute('data-i18n');
      if (T[key] && T[key][lang] !== undefined) el.textContent = T[key][lang];
    });
    // 2. data-i18n-html attribute (cho chuỗi có <code>, <b>, v.v.)
    document.querySelectorAll('[data-i18n-html]').forEach(function (el) {
      const key = el.getAttribute('data-i18n-html');
      if (T[key] && T[key][lang] !== undefined) el.innerHTML = T[key][lang];
    });
    // 3. Placeholder
    document.querySelectorAll('input[placeholder], textarea[placeholder]').forEach(function (el) {
      let key = el.getAttribute('data-i18n-placeholder') || el.getAttribute('data-i18n-ph-key');
      if (!key) {
        const t = (el.getAttribute('placeholder') || '').trim();
        if (T[t]) { key = t; el.setAttribute('data-i18n-ph-key', key); }
      }
      if (key && T[key] && T[key][lang] !== undefined) el.setAttribute('placeholder', T[key][lang]);
    });
    // 4. Auto-detect text content cho selector rộng
    document.querySelectorAll(SCAN_SELECTOR).forEach(translateElement);
  }

  function updateToggle() {
    const btn = document.getElementById('langToggle');
    if (btn) btn.textContent = getLang() === 'vi' ? 'EN' : 'VI';
  }

  function init() {
    applyAll();
    updateToggle();
    // Re-translate khi DOM thay đổi (AJAX content load vào #table)
    if (window.MutationObserver) {
      let pending = false;
      new MutationObserver(function () {
        if (pending) return;
        pending = true;
        requestAnimationFrame(function () { applyAll(); pending = false; });
      }).observe(document.body, { childList: true, subtree: true });
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

  // Expose API cho debug + nút EN/VI có sẵn trong Settings page
  window.i18n = {
    apply: applyAll,
    setLang: setLang,
    getLang: getLang
  };
})();
