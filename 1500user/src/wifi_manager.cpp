#include "Arduino.h"
#include "wifi_manager.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include "storage.h"
#include "display.h"
#include "config.h"

static DNSServer dnsServer;

String gsid_ = "";
String mdnsdotlocalurl = "";
String ssid_ = "";
String pass_ = "";
String ip_ = "";
String gateway_ = "";
String dhcpcheck = "";
int tzHours_ = 7;  // default UTC+7 (Việt Nam)
bool wifi_apMode = false;
static String apSsid_ = "";
static String apIP_ = "";

bool wifi_IsAPMode() { return wifi_apMode; }
String wifi_GetAPSSID() { return apSsid_; }
String wifi_GetAPIP() { return apIP_; }

static void startApFallback() {
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_AP_STA);  // AP_STA để có thể scan WiFi xung quanh

    uint64_t mac = ESP.getEfuseMac();
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "%04X", (uint16_t)(mac & 0xFFFF));
    apSsid_ = "EETIUM-Biometric Setup ";
    apSsid_ += suffix;

    // WPA2-PSK: AP_PASSWORD chống open WiFi attack. Hacker phải biết password
    // (ghi trên sticker thiết bị) mới connect được AP setup.
    if (WiFi.softAP(apSsid_.c_str(), AP_PASSWORD)) {
        apIP_ = WiFi.softAPIP().toString();
        wifi_apMode = true;
        Serial.print("AP mode: SSID="); Serial.print(apSsid_);
        Serial.print("  IP="); Serial.println(apIP_);
        SECURE_LOG("AP password: %s\n", AP_PASSWORD);    // chỉ dev build mới in

        // DNS hijack: trả mọi domain về IP của ESP để trigger captive portal
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsServer.start(53, "*", WiFi.softAPIP());
        Serial.println("DNS hijack started on port 53");

        // Kick off WiFi scan async để kết quả sẵn khi user mở trang
        WiFi.scanNetworks(true, false);
        Serial.println("WiFi scan started (async)");

        // OLED chỉ hiện SSID + IP, KHÔNG hiện password (chống xem qua vai).
        // Password admin biết qua sticker/manual riêng — không leak qua màn hình.
        display.clearBuffer();
        display.setFont(DISPLAY_VN_FONT);
        oledDisplayCenter("Cấu hình WiFi", 0, 13);
        int sp = apSsid_.indexOf(' ');
        String l1 = (sp > 0) ? apSsid_.substring(0, sp) : apSsid_;
        String l2 = (sp > 0) ? apSsid_.substring(sp + 1) : "";
        display.setFont(u8g2_font_6x10_tr);
        oledDisplayCenter(l1, 0, 28);
        oledDisplayCenter(l2, 0, 40);
        oledDisplayCenter(apIP_, 0, 58);
        display.sendBuffer();
    } else {
        Serial.println("softAP() failed!");
        display_ShowError("Lỗi AP");
    }
}


void wifi_InitAndConnect(){
    ssid_ = readFile(LittleFS, "/ssid.txt"); ssid_.trim();
    pass_ = readFile(LittleFS, "/pass.txt"); pass_.trim();
    ip_ = readFile(LittleFS, "/ip.txt"); ip_.trim();
    gateway_ = readFile(LittleFS, "/gateway.txt"); gateway_.trim();
    dhcpcheck = readFile(LittleFS, "/dhcpcheck.txt"); dhcpcheck.trim();
    gsid_ = readFile(LittleFS, "/gsid.txt"); gsid_.trim();
    mdnsdotlocalurl = readFile(LittleFS, "/mdns.txt"); mdnsdotlocalurl.trim();
    // Đọc timezone từ file (UTC offset hours, vd: 7 cho VN, -5 cho EST)
    String tzStr = readFile(LittleFS, "/tz.txt"); tzStr.trim();
    if (tzStr != "") tzHours_ = tzStr.toInt();

    if (mdnsdotlocalurl == "") {
        mdnsdotlocalurl = DEFAULT_mdns;
    }

#ifdef DEBUG_FORCE_WIFI
    // DEBUG: override credentials từ config.h, bỏ qua LittleFS — test nhanh.
    // Bỏ define DEBUG_FORCE_WIFI trong config.h để revert.
    Serial.println("[DEBUG] DEBUG_FORCE_WIFI active — using DEFAULT_WIFI_SSID/PASS");
    ssid_ = DEFAULT_WIFI_SSID;
    pass_ = DEFAULT_WIFI_PASS;
#endif

    WiFi.mode(WIFI_STA);
    // Bật auto-reconnect: WiFi stack ESP32 tự động kết nối lại NGAY khi mất sóng,
    // chạy ngầm không cần loop polling. persistent(true) lưu credentials để reconnect nhanh.
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

    // dhcpcheck: "2" = Manual IP từ Settings.html (aip dropdown value), "off" = legacy
    if ((dhcpcheck == "2" || dhcpcheck == "off") && ip_ != "" && gateway_ != "") {
        IPAddress local_IP;
        IPAddress gateway;
        IPAddress subnet(255, 255, 255, 0); // Mặc định Subnet Mask cho mạng cục bộ
        
        // Parse chuỗi String thành kiểu IPAddress
        if (local_IP.fromString(ip_) && gateway.fromString(gateway_)) {
            if (!WiFi.config(local_IP, gateway, subnet)) {
                Serial.println("STA Failed to configure Static IP");
            } else {
                Serial.println("Static IP configured.");
            }
        }
    }

    if (ssid_ == "") {
#ifdef DEBUG_FORCE_WIFI
        // Không thể xảy ra vì DEBUG đã set ssid_ ở trên — safeguard
        Serial.println("[DEBUG] no SSID — aborting WiFi init");
        display_ShowError("Lỗi cấu hình WiFi");
        return;
#else
        Serial.println("No WiFi config found. Starting AP setup mode...");
        startApFallback();
        if (!MDNS.begin(mdnsdotlocalurl.c_str())) {
            Serial.println("Error setting up MDNS!");
        } else {
            MDNS.addService("http", "tcp", 80);
            Serial.println("mDNS started: http://" + mdnsdotlocalurl + ".local");
        }
        return;  // không tiếp tục logic STA bên dưới
#endif
    }

    Serial.println("Connecting to configured WiFi: " + ssid_);
    WiFi.begin(ssid_.c_str(), pass_.c_str());
    display_ShowMessage("Đang kết nối...");

    int connectcount = 0;
    while (WiFi.status() != WL_CONNECTED && connectcount < 50) {
        delay(250);
        Serial.print(".");
        connectcount++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("\nConnected. IP address: ");
        Serial.println(WiFi.localIP());
        Serial.print("RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
        // Override DNS = Google + Cloudflare (giữ DHCP IP + gateway)
        WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                    IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
        display.clearBuffer();
        display.setFont(DISPLAY_VN_FONT);
        oledDisplayCenter("Đã kết nối WiFi", 0, 22);
        display.setFont(u8g2_font_6x12_tr);               // IP (ASCII) — font nhỏ
        oledDisplayCenter(WiFi.localIP().toString(), 0, 42);
        display.sendBuffer();
    } else {
#ifdef DEBUG_FORCE_WIFI
        Serial.println("\n[DEBUG] WiFi connect FAILED — KHÔNG fallback AP (debug mode)");
        Serial.print("  SSID="); Serial.println(ssid_);
        display_ShowError("WiFi thất bại");
#else
        Serial.println("\nWiFi Connect Failed! Falling back to AP setup mode.");
        startApFallback();
#endif
    }

    // Cấu hình mDNS
    if (!MDNS.begin(mdnsdotlocalurl.c_str())) {
        Serial.println("Error setting up MDNS!");
    } else {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS started: http://" + mdnsdotlocalurl + ".local");
    }
}

bool wifi_IsConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void wifi_Reconnect() {
    if (wifi_apMode) return;  // đang ở chế độ setup AP, bỏ qua
    Serial.println("WiFi connection lost. Reconnecting...");
    WiFi.disconnect();
    WiFi.reconnect();
}

void wifi_HandleDNS() {
    if (wifi_apMode) dnsServer.processNextRequest();
}