/**
 * gateway_wifi_config.cpp
 *
 * Non-blocking WiFi state machine for the Power Telemetry Gateway.
 *
 * State machine:
 *
 *   AP_ACTIVE --(creds exist at boot / POST /api/connect)--> AP_STA_CONNECTING
 *       ^                                                           |
 *       | STA drops / timeout / forget / disconnect                 | WL_CONNECTED
 *       |                                                           v
 *       +---------------------------------------- STA_CONNECTED (AP off)
 *
 * AP is always on except while STA is actively connected.
 * DNS catch-all redirects all queries to 192.168.4.1 while AP is active.
 */

#include "gateway_wifi_config.h"
#include "gateway_web.h"
#include "gateway_state.h"
#include "log_async.h"
#include "tdma_protocol.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include "hal/hal_nvs.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

// -- State -------------------------------------------------------------------------------------
enum WifiState
{
  WIFI_STATE_AP_ACTIVE,         // AP on, no STA
  WIFI_STATE_AP_STA_CONNECTING, // AP on + STA connecting in background
  WIFI_STATE_STA_CONNECTED,     // STA up, AP off
};

static WifiState s_state        = WIFI_STATE_AP_ACTIVE;
static uint32_t  s_connectStart = 0;
static bool      s_apActive     = false;
static bool      s_staConnected = false;
static bool      s_apEventRegistered = false; // guard: register AP_START handler once
static DNSServer s_dns;
static char      s_apSsid[32]   = {};   // "PowerTelemeter_XXXX", built at boot
static String    s_apPassword   = "";   // empty = open network

// -- NTP state ---------------------------------------------------------------------------------
#define NTP_POLL_MS    2000UL       // poll interval while waiting for first sync
#define NTP_RESYNC_MS  3600000UL    // re-anchor millis clock from system time once per hour

static bool     s_ntpSynced    = false;
static uint32_t s_ntpLastCheck = 0;

static int32_t nvsLoadTzOffset()
{
  HalNvs_t h = halNvsOpen("wifi-cfg", true);
  int32_t off = h ? halNvsGetInt(h, "tzoff", NTP_UTC_OFFSET_SEC) : NTP_UTC_OFFSET_SEC;
  if (h) halNvsClose(h);
  return off;
}

static void nvsSaveTzOffset(int32_t offsetSec)
{
  HalNvs_t h = halNvsOpen("wifi-cfg", false);
  if (h) { halNvsPutInt(h, "tzoff", offsetSec); halNvsClose(h); }
}

static void startNtp()
{
  int32_t off = nvsLoadTzOffset();
  configTime(off, 0, NTP_SERVER);
  s_ntpSynced    = false;
  s_ntpLastCheck = millis();
  logAsync("[WIFI] NTP: syncing via %s (UTC%+d)\n",
           NTP_SERVER, (int)(off / 3600));
}

static void stopNtp()
{
  s_ntpSynced = false;
}

static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);
static const uint8_t   DNS_PORT = 53;

// -- NVS -------------------------------------------------------
static void nvsLoad(String &ssid, String &pass)
{
  char s[64] = {}, p[64] = {};
  HalNvs_t h = halNvsOpen("wifi-cfg", true);
  if (h) {
    halNvsGetStr(h, "ssid", s, sizeof(s));
    halNvsGetStr(h, "pass", p, sizeof(p));
    halNvsClose(h);
  }
  ssid = String(s);
  pass = String(p);
}

static void nvsSave(const String &ssid, const String &pass)
{
  HalNvs_t h = halNvsOpen("wifi-cfg", false);
  if (h) {
    halNvsPutStr(h, "ssid", ssid.c_str());
    halNvsPutStr(h, "pass", pass.c_str());
    halNvsClose(h);
  }
}

// Clears only STA credentials; intentionally preserves "appass" and "tzoff".
static void nvsClear()
{
  HalNvs_t h = halNvsOpen("wifi-cfg", false);
  if (h) {
    halNvsRemove(h, "ssid");
    halNvsRemove(h, "pass");
    halNvsRemove(h, "sip");
    halNvsRemove(h, "sgw");
    halNvsRemove(h, "ssn");
    halNvsRemove(h, "sdns");
    halNvsClose(h);
  }
}

static String nvsLoadApPassword()
{
  char buf[64] = {};
  HalNvs_t h = halNvsOpen("wifi-cfg", true);
  if (h) { halNvsGetStr(h, "appass", buf, sizeof(buf)); halNvsClose(h); }
  return String(buf);
}

static void nvsSaveApPassword(const String &pw)
{
  HalNvs_t h = halNvsOpen("wifi-cfg", false);
  if (h) { halNvsPutStr(h, "appass", pw.c_str()); halNvsClose(h); }
}

void wifiClearCredentials()
{
  nvsClear();
}

void wifiFactoryResetNvs()
{
  HalNvs_t h = halNvsOpen("wifi-cfg", false);
  if (h) { halNvsClear(h); halNvsClose(h); }
}

// Static IP NVS -- keys sip/sgw/ssn/sdns; all empty = DHCP
static void nvsLoadStaticIp(String &ip, String &gw, String &sn, String &dns1)
{
  char sip[40]={}, sgw[40]={}, ssn[40]={}, sdns[40]={};
  HalNvs_t h = halNvsOpen("wifi-cfg", true);
  if (h) {
    halNvsGetStr(h, "sip",  sip,  sizeof(sip));
    halNvsGetStr(h, "sgw",  sgw,  sizeof(sgw));
    halNvsGetStr(h, "ssn",  ssn,  sizeof(ssn));
    halNvsGetStr(h, "sdns", sdns, sizeof(sdns));
    halNvsClose(h);
  }
  ip   = String(sip);
  gw   = String(sgw);
  sn   = String(ssn);
  dns1 = String(sdns);
}

static void nvsSaveStaticIp(const String &ip, const String &gw,
                             const String &sn, const String &dns1)
{
  HalNvs_t h = halNvsOpen("wifi-cfg", false);
  if (h) {
    halNvsPutStr(h, "sip",  ip.c_str());
    halNvsPutStr(h, "sgw",  gw.c_str());
    halNvsPutStr(h, "ssn",  sn.c_str());
    halNvsPutStr(h, "sdns", dns1.c_str());
    halNvsClose(h);
  }
}

static void nvsClearStaticIp()
{
  HalNvs_t h = halNvsOpen("wifi-cfg", false);
  if (h) {
    halNvsRemove(h, "sip");
    halNvsRemove(h, "sgw");
    halNvsRemove(h, "ssn");
    halNvsRemove(h, "sdns");
    halNvsClose(h);
  }
}

// -- AP helpers ----------------------------------------------------------------
static void startAP()
{
  if (s_apActive) return;

  // Register mDNS-on-AP-start handler exactly once across all startAP() calls.
  if (!s_apEventRegistered) {
    WiFi.onEvent([](WiFiEvent_t /*event*/, WiFiEventInfo_t /*info*/) {
      MDNS.begin("telemeter");
      logAsync("[WIFI] mDNS: http://telemeter.local (AP mode)\n");
    }, ARDUINO_EVENT_WIFI_AP_START);
    s_apEventRegistered = true;
  }

  WiFi.enableAP(true);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
  const char* pw = s_apPassword.isEmpty() ? nullptr : s_apPassword.c_str();
  bool ok = WiFi.softAP(s_apSsid, pw);
  s_apActive = true;
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(DNS_PORT, "*", AP_IP);
  logAsync("[WIFI] AP %s | SSID: %s | IP: %s\n",
           ok ? "UP" : "FAIL", s_apSsid,
           WiFi.softAPIP().toString().c_str());
  configASSERT(ok);
}

static void stopAP()
{
  if (!s_apActive) return;
  MDNS.end();
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.enableAP(false);
  s_apActive = false;
  logAsync("[WIFI] AP disabled -- STA is up\n");
}

// -- STA helper -----------------------------------------------------
static void beginSTA(const String &ssid, const String &pass)
{
  WiFi.enableSTA(true);
  WiFi.setHostname("telemeter");
  WiFi.setAutoReconnect(true);

  // Apply static IP if configured; fall through to DHCP otherwise.
  String sip, sgw, ssn, sdns;
  nvsLoadStaticIp(sip, sgw, ssn, sdns);
  if (!sip.isEmpty()) {
    IPAddress ip, gw, sn, dns1;
    if (ip.fromString(sip) && gw.fromString(sgw) && sn.fromString(ssn)) {
      if (sdns.isEmpty() || !dns1.fromString(sdns)) dns1 = gw; // fallback: gateway as DNS
      WiFi.config(ip, gw, sn, dns1);
      logAsync("[WIFI] Static IP: %s / %s via %s\n",
               sip.c_str(), ssn.c_str(), sgw.c_str());
    }
  }

  if (pass.isEmpty())
    WiFi.begin(ssid.c_str());
  else
    WiFi.begin(ssid.c_str(), pass.c_str());

  s_connectStart = millis();
  s_staConnected = false;
  logAsync("[WIFI] STA connecting to \"%s\" ...\n", ssid.c_str());
}

// -- HTTP REST handlers ----------------------------------
static void handleInfo(AsyncWebServerRequest *req)
{
  JsonDocument doc;
  doc["version"]      = FW_VERSION_STR;
  doc["apSsid"]       = s_apSsid;
  doc["apIp"]         = AP_IP.toString();
  doc["apActive"]     = s_apActive;
  doc["staConnected"] = s_staConnected;
  if (s_staConnected)
  {
    doc["staSSID"] = WiFi.SSID();
    doc["staIP"]   = WiFi.localIP().toString();
  }
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

// Async scan: first call triggers the scan and returns {scanning:true}.
// Client retries until scanning:false with the full network list.
// 300 ms dwell per channel (vs default 120 ms) improves reliability in AP+STA
// mode where the radio shares time between serving AP clients and scanning.
// In AP_ACTIVE the SDK background reconnect is paused before scanning so the
// radio is free; resumeReconnect() restores the STA attempt on completion.
// Auto-retries up to 2 times on 0-result scans; caps WIFI_SCAN_FAILED at 3
// retries to prevent an indefinite scanning state.
static uint8_t s_scanRetries = 0;
static uint8_t s_scanFails   = 0;

static void resumeReconnect()
{
  if (s_state != WIFI_STATE_AP_ACTIVE) return;
  String ssid, pass;
  nvsLoad(ssid, pass);
  if (!ssid.isEmpty()) {
    beginSTA(ssid, pass);
    s_state = WIFI_STATE_AP_STA_CONNECTING;
  } else {
    WiFi.setAutoReconnect(true);
  }
}

static void startScan()
{
  if (s_state == WIFI_STATE_AP_ACTIVE) {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false);
  }
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false,
                    /*passive=*/false, /*ms_per_chan=*/300);
}

static void handleScan(AsyncWebServerRequest *req)
{
  int n = WiFi.scanComplete();

  if (n == WIFI_SCAN_FAILED)
  {
    s_scanRetries = 0;
    if (s_scanFails++ < 3) {
      startScan();
      req->send(202, "application/json", "{\"scanning\":true,\"networks\":[]}");
    } else {
      s_scanFails = 0;
      resumeReconnect();
      req->send(200, "application/json", "{\"scanning\":false,\"networks\":[]}");
    }
    return;
  }

  if (n == WIFI_SCAN_RUNNING)
  {
    req->send(202, "application/json", "{\"scanning\":true,\"networks\":[]}");
    return;
  }

  // Retry up to 2 times when scan returns 0 networks -- common in AP+STA mode
  // on the first attempt while the radio is busy serving AP clients.
  if (n == 0 && s_scanRetries < 2)
  {
    s_scanRetries++;
    WiFi.scanDelete();
    startScan();
    req->send(202, "application/json", "{\"scanning\":true,\"networks\":[]}");
    return;
  }

  s_scanRetries = 0;
  s_scanFails   = 0;
  resumeReconnect();
  JsonDocument doc;
  doc["scanning"] = false;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++)
  {
    JsonObject net = arr.add<JsonObject>();
    net["ssid"]   = WiFi.SSID(i);
    net["rssi"]   = WiFi.RSSI(i);
    net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

static void handleConnect(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  String ssid = "", pass = "";
  if (req->hasParam("ssid", true))
    ssid = req->getParam("ssid", true)->value();
  if (req->hasParam("password", true))
    pass = req->getParam("password", true)->value();
  ssid.trim();

  if (ssid.isEmpty())
  {
    req->send(400, "application/json",
              "{\"status\":\"error\",\"message\":\"SSID cannot be empty\"}");
    return;
  }

  nvsSave(ssid, pass);

  // Restore AP before dropping STA so the device stays reachable throughout
  // the transition -- even if the new credentials fail.
  if (s_staConnected)
  {
    MDNS.end();
    s_staConnected = false;
  }
  startAP(); // no-op if AP is already active

  WiFi.setAutoReconnect(false); // prevent SDK from reconnecting to old SSID before beginSTA
  WiFi.disconnect(false);
  beginSTA(ssid, pass);
  s_state = WIFI_STATE_AP_STA_CONNECTING;
  req->send(200, "application/json", "{\"status\":\"connecting\"}");
}

/** GET /api/wifistatus -- polled by dashboard after POST /api/connect.
 *  Distinct from GET /api/status (dashboard gateway-info endpoint). */
static void handleWifiStatus(AsyncWebServerRequest *req)
{
  JsonDocument doc;
  doc["apActive"]   = s_apActive;
  doc["connecting"] = (s_state == WIFI_STATE_AP_STA_CONNECTING);
  doc["connected"]  = s_staConnected;
  if (s_staConnected)
  {
    doc["ip"]   = WiFi.localIP().toString();
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
  }
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

static void handleDisconnect(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  WiFi.setAutoReconnect(false); // make disconnect sticky
  WiFi.disconnect(false);
  s_staConnected = false;

  if (s_state == WIFI_STATE_STA_CONNECTED)
  {
    // AP was off -- restore it so the client isn't left stranded.
    MDNS.end();
    startAP();
    logAsync("[WIFI] STA disconnected by user -- AP restored\n");
  }
  else
  {
    logAsync("[WIFI] STA connect attempt cancelled by user\n");
  }

  s_state = WIFI_STATE_AP_ACTIVE;
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleForget(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  nvsClear();
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  s_staConnected = false;
  MDNS.end();
  startAP(); // no-op if AP is already active
  s_state = WIFI_STATE_AP_ACTIVE;
  req->send(200, "application/json", "{\"ok\":true}");
  logAsync("[WIFI] Credentials cleared -- AP restored\n");
}

// GET /api/staticip -- returns current static IP config
static void handleGetStaticIp(AsyncWebServerRequest *req)
{
  String ip, gw, sn, dns1;
  nvsLoadStaticIp(ip, gw, sn, dns1);
  JsonDocument doc;
  doc["enabled"] = !ip.isEmpty();
  doc["ip"]      = ip;
  doc["gateway"] = gw;
  doc["subnet"]  = sn;
  doc["dns"]     = dns1;
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

// POST /api/staticip -- body: ip=&gateway=&subnet=&dns=
static void handleSetStaticIp(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  String ip = "", gw = "", sn = "255.255.255.0", dns1 = "";
  if (req->hasParam("ip",      true)) ip   = req->getParam("ip",      true)->value();
  if (req->hasParam("gateway", true)) gw   = req->getParam("gateway", true)->value();
  if (req->hasParam("subnet",  true)) sn   = req->getParam("subnet",  true)->value();
  if (req->hasParam("dns",     true)) dns1 = req->getParam("dns",     true)->value();
  ip.trim(); gw.trim(); sn.trim(); dns1.trim();

  IPAddress vip, vgw, vsn;
  if (ip.isEmpty() || !vip.fromString(ip) || !vgw.fromString(gw) || !vsn.fromString(sn))
  {
    req->send(400, "application/json",
              "{\"ok\":false,\"message\":\"Invalid IP, gateway, or subnet\"}");
    return;
  }

  nvsSaveStaticIp(ip, gw, sn, dns1);
  logAsync("[WIFI] Static IP saved: %s / %s via %s\n",
           ip.c_str(), sn.c_str(), gw.c_str());
  req->send(200, "application/json", "{\"ok\":true}");
}

// GET /api/staticip/clear -- reset to DHCP
static void handleClearStaticIp(AsyncWebServerRequest *req)
{
  nvsClearStaticIp();
  logAsync("[WIFI] Static IP cleared -- will use DHCP\n");
  req->send(200, "application/json", "{\"ok\":true}");
}

// GET /api/ap -- returns SSID and whether a password is currently set
static void handleGetAp(AsyncWebServerRequest *req)
{
  JsonDocument doc;
  doc["ssid"]        = s_apSsid;
  doc["hasPassword"] = !s_apPassword.isEmpty();
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

// POST /api/ap -- body: password=<string> (blank = open network, min 8 chars if non-empty)
static void handleSetAp(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  String pass = "";
  if (req->hasParam("password", true))
    pass = req->getParam("password", true)->value();
  pass.trim();

  if (!pass.isEmpty() && pass.length() < 8) {
    req->send(400, "application/json",
              "{\"ok\":false,\"message\":\"Minimum 8 characters\"}");
    return;
  }

  nvsSaveApPassword(pass);
  s_apPassword = pass;

  // Restart AP immediately so the new password takes effect now.
  if (s_apActive) {
    stopAP();
    startAP();
  }

  logAsync("[WIFI] AP password %s\n", pass.isEmpty() ? "cleared (open)" : "updated");
  req->send(200, "application/json", "{\"ok\":true}");
}

/** Catch-all -- redirects OS captive-portal probes to the config page.
 *  Only redirects when AP is active; falls through to 404 otherwise. */
static void handleCatchAll(AsyncWebServerRequest *req)
{
  if (s_apActive)
    req->redirect("http://telemeter.local");
  else
    req->send(404, "text/plain", "Not found");
}

// -- Public API -----------------------------------------------------------------------

void wifiConfigBegin()
{
  // Build device-unique SSID from CRC-16/CCITT of eFuse MAC.
  uint16_t uid = computeDeviceUID();
  snprintf(s_apSsid, sizeof(s_apSsid), "PowerTelemeter_%04X", uid);
  s_apPassword = nvsLoadApPassword();

  // Check force-AP pin (hold GPIO0 / BOOT button at power-on to skip saved creds)
  pinMode(CONFIG_PIN, INPUT);
  delay(50);
  bool forceAP = (digitalRead(CONFIG_PIN) == LOW);
  if (forceAP)
    logAsync("[WIFI] GPIO%d LOW -- skipping saved credentials\n", CONFIG_PIN);

  WiFi.mode(WIFI_AP_STA);
  startAP();
  s_state = WIFI_STATE_AP_ACTIVE;

  if (!forceAP)
  {
    String ssid, pass;
    nvsLoad(ssid, pass);
    if (!ssid.isEmpty())
    {
      beginSTA(ssid, pass);
      s_state = WIFI_STATE_AP_STA_CONNECTING;
    }
    else
    {
      logAsync("[WIFI] No saved credentials -- staying in AP mode\n");
    }
  }
}

void wifiRegisterRoutes(AsyncWebServer &server)
{
  server.on("/api/info",            HTTP_GET,  handleInfo);
  server.on("/api/scan",            HTTP_GET,  handleScan);
  server.on("/api/connect",         HTTP_POST, handleConnect);
  server.on("/api/wifistatus",      HTTP_GET,  handleWifiStatus);
  server.on("/api/disconnect",      HTTP_GET,  handleDisconnect);
  server.on("/api/forget",          HTTP_GET,  handleForget);
  server.on("/api/staticip",        HTTP_GET,  handleGetStaticIp);
  server.on("/api/staticip",        HTTP_POST, handleSetStaticIp);
  server.on("/api/staticip/clear",  HTTP_GET,  handleClearStaticIp);
  server.on("/api/ap",              HTTP_GET,  handleGetAp);
  server.on("/api/ap",              HTTP_POST, handleSetAp);
  logAsync("[WIFI] Config routes registered\n");
}

void wifiConfigLoop()
{
  if (s_apActive) s_dns.processNextRequest();

  switch (s_state)
  {
  case WIFI_STATE_AP_STA_CONNECTING:
    if (WiFi.status() == WL_CONNECTED)
    {
      s_staConnected = true;
      stopAP(); // stopAP() calls MDNS.end()
      s_state = WIFI_STATE_STA_CONNECTED;
      MDNS.begin("telemeter");
      startNtp();
      logAsync("[WIFI] STA connected | IP: %s | RSSI: %d dBm\n",
               WiFi.localIP().toString().c_str(), WiFi.RSSI());
      logAsync("[WIFI] mDNS: http://telemeter.local\n");
    }
    else if (millis() - s_connectStart > WIFI_CONNECT_TIMEOUT_MS)
    {
      logAsync("[WIFI] STA timed out - staying in AP mode\n");
      WiFi.disconnect(false);
      s_state = WIFI_STATE_AP_ACTIVE;
    }
    break;

  case WIFI_STATE_STA_CONNECTED:
    if (WiFi.status() != WL_CONNECTED)
    {
      s_staConnected = false;
      stopNtp();
      MDNS.end();
      startAP();
      s_state = WIFI_STATE_AP_ACTIVE;
      logAsync("[WIFI] STA dropped - AP restored as fallback\n");
      // setAutoReconnect(true) is still set; SDK will reconnect in background
      // and AP_ACTIVE case below will promote back to STA_CONNECTED.
      break;
    }
    // NTP sync polling -- check every 2s until first sync, then re-anchor hourly
    {
      uint32_t now_ms   = millis();
      uint32_t poll_iv  = s_ntpSynced ? NTP_RESYNC_MS : NTP_POLL_MS;
      if (now_ms - s_ntpLastCheck >= poll_iv)
      {
        s_ntpLastCheck = now_ms;
        struct tm ti;
        if (getLocalTime(&ti, 0))
        {
          bool first = !s_ntpSynced;
          setGatewayTime((uint8_t)ti.tm_hour, (uint8_t)ti.tm_min, (uint8_t)ti.tm_sec);
          s_ntpSynced = true;
          if (first)
            logAsync("[WIFI] NTP synced: %02d:%02d:%02d (UTC%+d)\n",
                     ti.tm_hour, ti.tm_min, ti.tm_sec,
                     (int)(NTP_UTC_OFFSET_SEC / 3600));
        }
      }
    }
    break;

  case WIFI_STATE_AP_ACTIVE:
    // SDK auto-reconnect may silently re-establish STA (e.g. after router reboot).
    if (WiFi.status() == WL_CONNECTED)
    {
      s_staConnected = true;
      stopAP(); // stopAP() calls MDNS.end()
      s_state = WIFI_STATE_STA_CONNECTED;
      MDNS.begin("telemeter");
      startNtp();
      logAsync("[WIFI] Auto-reconnected | IP: %s\n",
               WiFi.localIP().toString().c_str());
      logAsync("[WIFI] mDNS: http://telemeter.local\n");
    }
    break;
  }
}

bool wifiIsApActive()     { return s_apActive; }
bool wifiIsStaConnected() { return s_staConnected; }
bool wifiIsNtpSynced()    { return s_ntpSynced; }
const char* wifiGetApSsid() { return s_apSsid; }

void wifiSetTzOffset(int32_t offsetSec)
{
  nvsSaveTzOffset(offsetSec);
  if (s_staConnected)
  {
    // Reconfigure NTP with new timezone immediately; next 2s poll will re-anchor
    configTime(offsetSec, 0, NTP_SERVER);
    s_ntpSynced    = false;
    s_ntpLastCheck = millis();
    logAsync("[WIFI] Timezone updated: UTC%+d -- resyncing NTP\n",
             (int)(offsetSec / 3600));
  }
  else
  {
    logAsync("[WIFI] Timezone saved: UTC%+d (applied on next STA connect)\n",
             (int)(offsetSec / 3600));
  }
}

void wifiHandleCatchAll(AsyncWebServerRequest *req) { handleCatchAll(req); }
