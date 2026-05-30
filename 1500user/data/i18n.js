/*
 * i18n.js — Vietnamese/English language toggle for EETIUM Biometric web UI
 *
 * Cách dùng: chỉ cần thêm <script src="/i18n.js"></script> vào <head> của HTML.
 * Script tự inject nút EN/VI ở góc phải, lưu lựa chọn vào localStorage,
 * tự dịch text matching trong dictionary T, và observe DOM cho AJAX content.
 *
 * Để force dịch element bất kỳ: thêm attribute data-i18n="KeyExactlyAsEnglish".
 * Để dịch placeholder: data-i18n-placeholder="key".
 */
(function () {
  // Dictionary — thêm key mới ở đây nếu cần. Key = text tiếng Anh gốc.
  const T = {
    // ===== Navigation / Header =====
    "Database": { vi: "Dữ liệu", en: "Database" },
    "Enroll": { vi: "Đăng ký", en: "Enroll" },
    "Settings": { vi: "Cài đặt", en: "Settings" },
    "Account": { vi: "Tài khoản", en: "Account" },
    "Biometric Database Server": { vi: "Máy chủ Vân tay EETIUM", en: "Biometric Database Server" },
    "Logout": { vi: "Đăng xuất", en: "Logout" },
    "Sign Out": { vi: "Đăng xuất", en: "Sign Out" },

    // ===== Login page =====
    "Username": { vi: "Tên đăng nhập", en: "Username" },
    "Password": { vi: "Mật khẩu", en: "Password" },
    "Login": { vi: "Đăng nhập", en: "Login" },
    "Sign In": { vi: "Đăng nhập", en: "Sign In" },
    "Remember me": { vi: "Ghi nhớ", en: "Remember me" },

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
    "New Password": { vi: "Mật khẩu mới", en: "New Password" },
    "Confirm Password": { vi: "Xác nhận mật khẩu", en: "Confirm Password" },
    "Change Password": { vi: "Đổi mật khẩu", en: "Change Password" },
    "Update Account": { vi: "Cập nhật tài khoản", en: "Update Account" },

    // ===== Enroll / Database table =====
    "Employee ID": { vi: "Mã NV", en: "Employee ID" },
    "Name": { vi: "Họ tên", en: "Name" },
    "Full Name": { vi: "Họ tên", en: "Full Name" },
    "Email": { vi: "Email", en: "Email" },
    "Position": { vi: "Chức vụ", en: "Position" },
    "Department": { vi: "Phòng ban", en: "Department" },
    "Action": { vi: "Thao tác", en: "Action" },
    "Actions": { vi: "Thao tác", en: "Actions" },
    "Delete": { vi: "Xóa", en: "Delete" },
    "Edit": { vi: "Sửa", en: "Edit" },
    "Add User": { vi: "Thêm user", en: "Add User" },
    "Scan": { vi: "Quét vân tay", en: "Scan" },
    "Place Finger": { vi: "Đặt ngón tay", en: "Place Finger" },
    "Fingerprint ID": { vi: "ID vân tay", en: "Fingerprint ID" },
    "Total users": { vi: "Tổng số user", en: "Total users" },
    "Page": { vi: "Trang", en: "Page" },
    "Prev": { vi: "Trước", en: "Prev" },
    "Next": { vi: "Sau", en: "Next" },
    "Search": { vi: "Tìm kiếm", en: "Search" },

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
    "No": { vi: "Không", en: "No" },
    "OK": { vi: "OK", en: "OK" },
    "Confirm": { vi: "Xác nhận", en: "Confirm" },
    "Loading...": { vi: "Đang tải...", en: "Loading..." },
    "Error": { vi: "Lỗi", en: "Error" },
    "Success": { vi: "Thành công", en: "Success" },
    "Restart": { vi: "Khởi động lại", en: "Restart" },
    "Backup": { vi: "Sao lưu", en: "Backup" },
    "Restore": { vi: "Phục hồi", en: "Restore" },
    "Download": { vi: "Tải xuống", en: "Download" }
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

  // Selectors có khả năng chứa text cần dịch (tránh dịch tất cả gây slow)
  const SCAN_SELECTOR = 'button, label, h1, h2, h3, h4, h5, th, .brand-title, .form-label, option, a, .nav-link, .btn';

  function translateElement(el) {
    if (el.children.length > 0) return;  // có child element thì skip
    let key = el.getAttribute('data-i18n-key');
    if (!key) {
      // Lần đầu — text gốc có thể là English
      const text = (el.textContent || '').trim();
      if (T[text]) {
        key = text;
        el.setAttribute('data-i18n-key', key);
      }
    }
    if (key && T[key]) {
      const lang = getLang();
      const tr = T[key][lang];
      if (tr !== undefined) el.textContent = tr;
    }
  }

  function applyAll() {
    // Explicit data-i18n attributes (ưu tiên cao)
    document.querySelectorAll('[data-i18n]').forEach(el => {
      const key = el.getAttribute('data-i18n');
      if (T[key] && T[key][getLang()] !== undefined) {
        el.textContent = T[key][getLang()];
      }
    });
    document.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
      const key = el.getAttribute('data-i18n-placeholder');
      if (T[key] && T[key][getLang()] !== undefined) {
        el.setAttribute('placeholder', T[key][getLang()]);
      }
    });
    // Auto-detect các element thông thường
    document.querySelectorAll(SCAN_SELECTOR).forEach(translateElement);
  }

  function updateToggle() {
    const btn = document.getElementById('langToggle');
    if (btn) btn.textContent = getLang() === 'vi' ? 'EN' : 'VI';
  }

  // KHÔNG auto-inject floating button nữa — user dùng nút sẵn có trong Settings.html.
  // Lang change vẫn áp dụng toàn bộ vì localStorage shared cross-page.
  function injectToggle() { /* no-op */ }

  function init() {
    injectToggle();
    applyAll();
    // Re-translate khi DOM thay đổi (AJAX content như Settings/Enroll/Account load vào #table)
    if (window.MutationObserver) {
      let pending = false;
      const obs = new MutationObserver(function () {
        if (pending) return;
        pending = true;
        requestAnimationFrame(function () {
          applyAll();
          pending = false;
        });
      });
      obs.observe(document.body, { childList: true, subtree: true });
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

  // Expose API cho debug / manual call
  window.i18n = { apply: applyAll, setLang: setLang, getLang: getLang };
})();
