/**
 * gateway_web.cpp
 *
 * ESPAsyncWebServer + AsyncWebSocket implementation.
 * Serves the dashboard SPA from LittleFS and exposes the full API
 * defined in dashboard-api-reference.md (v7).
 *
 * REST endpoints:
 *   GET  /api/nodes
 *   GET  /api/node/{id}/live
 *   GET  /api/node/{id}/history
 *   POST /api/node/{id}/relay
 *   POST /api/node/{id}/name
 *   POST /api/time
 *   GET  /api/status
 *
 * WebSocket ws://<ip>/ws
 *   Commands: relay_manual, relay_schedule, relay_clear, set_threshold,
 *             nudge, rename, set_time, clear_energy, clear_all_energy, get_nodes
 *   Push:     telemetry, nodes, name_changed, time_set, relay_ack, schedule_ack,
 *             clear_ack, threshold_ack, nudge_ack, energy_cleared, all_energy_cleared
 */

#include "gateway_web.h"
#include "gateway_state.h"
#include "gateway_tdma_task.h"
#include "gateway_wifi_config.h"
#include "fram_store.h"
#include "log_async.h"
#ifdef PKT_ENCRYPTION
  #include "crypto.h"
  #include "rfid_provision.h"
#endif
#ifdef KNN_INFERENCE
  #include "knn_db.h"
  #include "knn_training.h"
#endif
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "hal/hal_sys.h"
#include "hal/hal_rtos.h"
#include "hal/hal_nvs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// -- Server and WebSocket instances -------------------------------------------
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// -- Dashboard auth -----------------------------------------------------------
static String   s_dashPassword   = "";       // empty = no auth required
static char     s_sessionToken[33] = {};     // 32-char hex + null; empty = no session

static uint32_t s_authWsIds[8]   = {};
static uint8_t  s_authWsCount    = 0;

// -- WS broadcast queue (uint8_t slotIdx, depth 16) ---------------------------
static QueueHandle_t s_wsBroadcastQueue = nullptr;

// =================================================
// Auth -- NVS, token, session helpers
// =================================================

static String nvsDashPassLoad() {
  char buf[64] = {};
  HalNvs_t h = halNvsOpen("wifi-cfg", true);
  if (h) {
    halNvsGetStr(h, "dashpass", buf, sizeof(buf));
    halNvsClose(h);
  }
  return String(buf);
}

static void nvsDashPassSave(const String &pw) {
  HalNvs_t h = halNvsOpen("wifi-cfg", false);
  if (h) {
    halNvsPutStr(h, "dashpass", pw.c_str());
    halNvsClose(h);
  }
}

static void generateToken() {
  for (int i = 0; i < 4; i++) {
    uint32_t r = halRandom();
    snprintf(s_sessionToken + i * 8, 9, "%08lx", (unsigned long)r);
  }
}

static bool webCheckAuth(AsyncWebServerRequest *req) {
  if (s_dashPassword.isEmpty()) return true;
  if (s_sessionToken[0] == '\0') return false;
  if (!req->hasHeader("X-Auth-Token")) return false;
  return req->header("X-Auth-Token") == s_sessionToken;
}

static void wsAuthAdd(uint32_t id) {
  for (uint8_t i = 0; i < s_authWsCount; i++)
    if (s_authWsIds[i] == id) return;
  if (s_authWsCount < 8) s_authWsIds[s_authWsCount++] = id;
}

static void wsAuthRemove(uint32_t id) {
  for (uint8_t i = 0; i < s_authWsCount; i++) {
    if (s_authWsIds[i] == id) {
      s_authWsIds[i] = s_authWsIds[--s_authWsCount];
      return;
    }
  }
}

static bool wsIsAuth(uint32_t id) {
  if (s_dashPassword.isEmpty()) return true;
  for (uint8_t i = 0; i < s_authWsCount; i++)
    if (s_authWsIds[i] == id) return true;
  return false;
}

// =================================================
// JSON helpers -- build node objects from NodeState
// =================================================

/** Populate a JsonObject with the "summary" fields (used by /api/nodes and nodes push). */
static void nodeToSummaryJson(JsonObject obj, const NodeState &ns)
{
  char timeBuf[9];
  gwTimeString(timeBuf);

  bool online = ns.hasData && (ns.missedSfs < NODE_TIMEOUT_SFS);

  obj["id"] = ns.slotId;
  obj["label"] = ns.label;
  obj["online"] = online;
  obj["rssi"] = ns.rssi;
  // The x / 10.0f * 10.0f may look useless, but this converts raw modbus register to floating point values
  obj["voltage"] = roundf(ns.latest.voltage / 10.0f * 10.0f) / 10.0f;
  obj["current"] = roundf(ns.latest.current / 1000.0f * 1000.0f) / 1000.0f;
  obj["power"] = roundf(ns.latest.power / 10.0f * 10.0f) / 10.0f;
  obj["energy"] = ns.accumEnergy;
  obj["frequency"] = roundf(ns.latest.frequency / 10.0f * 10.0f) / 10.0f;
  obj["powerFactor"] = roundf(ns.latest.powerFactor / 100.0f * 100.0f) / 100.0f;
  obj["relayState"] = ns.relayState;
  obj["relayMode"] = ns.relayMode;
  obj["schedState"] = ns.schedState;
  obj["alarmState"] = ns.alarmState;
  obj["alarmThreshold"] = ns.latest.alarmThreshold;
  obj["age"] = (uint32_t)((millis() - ns.lastSeen) / 1000UL);
  obj["pending"] = ns.pending;

  bool hasSched = (ns.relayMode == 1 && ns.schedState > 0);
  obj["hasSched"] = hasSched;

#ifdef KNN_INFERENCE
  JsonObject knn_obj = obj["knn"].to<JsonObject>();
  knn_obj["label"]   = knnGetLabel(ns.knnLabelIdx);
  knn_obj["state"]   = knnMachineStateName(ns.knnMachineState);
  knn_obj["distSq"]  = ns.knnDistSq;
#endif

  // Load classifier
  JsonObject clf = obj["classifier"].to<JsonObject>();
  if (ns.classByte == CLASS_BYTE_UNSUPPORTED) {
    clf["supported"] = false;
  } else {
    uint8_t classId, conf;
    bool trans;
    unpackClassByte(ns.classByte, classId, conf, trans);
    bool pending = (trans && conf == 0);
    clf["supported"] = true;
    clf["pending"]   = pending;
    if (!pending) {
      static const char* const CLASS_NAMES[6] = {
        "Resistive", "Capacitive", "Motor", "Fan", "SMPS", "Lighting"
      };
      clf["classId"]    = classId;
      clf["className"]  = (classId < 6) ? CLASS_NAMES[classId] : "Unknown";
      clf["confidence"] = conf;
      clf["transient"]  = trans;
    }
  }
}

/** Populate a JsonObject with full detail fields (used by /api/node/{id}/live and telemetry push). */
static void nodeToDetailJson(JsonObject obj, const NodeState &ns)
{
  nodeToSummaryJson(obj, ns);

  // Schedule fields only when active
  bool hasSched = (ns.relayMode == 1 && ns.schedState > 0);
  if (hasSched)
  {
    char schedStart[6], schedEnd[6];
    snprintf(schedStart, sizeof(schedStart), "%02d:%02d",
             ns.latest.schedSH, ns.latest.schedSM);
    snprintf(schedEnd, sizeof(schedEnd), "%02d:%02d",
             ns.latest.schedEH, ns.latest.schedEM);
    obj["schedStart"] = schedStart;
    obj["schedEnd"] = schedEnd;
  }

  // Gateway clock status
  char timeBuf[9];
  gwTimeString(timeBuf);
  obj["timeSet"] = g_timeSet;
  obj["time"] = timeBuf;
}

/** Extract node ID from path like "/api/node/3/live" -> 3 */
// TODO implement this using regex if possible
static int parseNodeIdFromPath(const String &path)
{
  // path format: /api/node/<id>/<action>
  // "/api/node" is 9 chars (0-8); the '/' before the ID is at index 9.
  int first = path.indexOf('/', 9); // finds the '/' at index 9
  if (first < 0)
    return -1;
  int second = path.indexOf('/', first + 1);
  if (second < 0)
    return -1;
  return path.substring(first + 1, second).toInt();
}

// -----------------------------------------------------------------------------
// WebSocket broadcast helpers
// -----------------------------------------------------------------------------

// Performs the actual JSON serialisation + ws.textAll(). Called from Core 0 only.
static void doBroadcastTelemetry(uint8_t slotIdx)
{
  if (ws.count() == 0)
    return;

  JsonDocument doc;
  doc["type"] = "telemetry";

  JsonObject nodeObj = doc["node"].to<JsonObject>();

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(10)) == pdTRUE)
  {
    if (g_nodes[slotIdx].active)
    {
      nodeToDetailJson(nodeObj, g_nodes[slotIdx]);
    }
    xSemaphoreGive(g_nodesMutex);
  }

  char timeBuf[9];
  gwTimeString(timeBuf);
  doc["timeSet"] = g_timeSet;
  doc["time"] = timeBuf;

  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

// Broadcast a single log line to all WebSocket clients. Called from Core 0.
static void doBroadcastLogLine(const char *line)
{
  if (ws.count() == 0) return;
  JsonDocument doc;
  doc["type"] = "log";
  doc["line"] = line;
  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

// Push bulk_complete or bulk_failed event to all WS clients. Core 0 only.
static void doBroadcastBulkEvent()
{
  if (ws.count() == 0) return;

  const BulkSession* bs = &g_bulkSession;
  bool ok = (bs->state == BULK_COMPLETE);

  JsonDocument doc;
  doc["type"]      = ok ? "bulk_complete" : "bulk_failed";
  doc["node"]      = bs->slotIdx + 1;
  doc["dir"]       = bs->dir;
  doc["typeTag"]   = bs->typeTag;
  doc["totalLen"]  = bs->totalLen;
  doc["fragsSent"] = bs->fragsSent;
  doc["fragsAcked"]= bs->fragsAcked;
  doc["durationMs"]= (uint32_t)(millis() - bs->testStartMs);

  if (bs->dir == 0) {
    // DL: compare gateway-side CRC of what was sent vs echo received
    doc["crc32ok"] = (bs->crc32Expected != 0 &&
                      bs->crc32Expected == bs->crc32Received);
  } else {
    // UL: report received CRC as hex string
    char crcHex[12];
    snprintf(crcHex, sizeof(crcHex), "0x%08X", (unsigned)bs->crc32Received);
    doc["crc32"] = crcHex;
  }

#ifdef TRANSPORT_TEST
  doc["scenario"] = bs->testScenario;
  doc["pdrTotal"] = bs->pdrTotal;
  doc["pdrAcked"] = bs->pdrAcked;
  if (bs->pdrTotal > 0)
    doc["pdrPct"]  = (float)bs->pdrAcked / bs->pdrTotal * 100.0f;
#endif

  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

// Non-blocking queue-post called from the TDMA task (Core 1).
void webBroadcastTelemetry(uint8_t slotIdx)
{
  if (!s_wsBroadcastQueue) return;
  uint8_t idx = slotIdx;
  xQueueSend(s_wsBroadcastQueue, &idx, 0);
}

// Signal the WS broadcast task to push a bulk_complete/failed event (Core 1 safe).
void webBroadcastBulkEvent()
{
  if (!s_wsBroadcastQueue) return;
  uint8_t sentinel = 0xFF;
  xQueueSend(s_wsBroadcastQueue, &sentinel, 0);
}

static void wsBroadcastTask(void* /*params*/)
{
  uint8_t idx;
  char logLine[LOG_LINE_MAX];
  while (true) {
    if (xQueueReceive(s_wsBroadcastQueue, &idx, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (idx == 0xFF) doBroadcastBulkEvent();
      else             doBroadcastTelemetry(idx);
    }
    while (logLineDequeue(logLine, sizeof(logLine)))
      doBroadcastLogLine(logLine);
  }
}

void webBroadcastTaskStart()
{
  s_wsBroadcastQueue = xQueueCreate(16, sizeof(uint8_t));
  configASSERT(s_wsBroadcastQueue);
  halTaskCreatePinned(wsBroadcastTask, "WS_BCAST", 4096, nullptr, 1, HAL_CORE_0, nullptr);
}

static void wsSendToClient(AsyncWebSocketClient *client, const JsonDocument &doc)
{
  String json;
  serializeJson(doc, json);
  client->text(json);
}

// ---------------------------------------------------------------------------------
// Shared nodes-list document builder
// Populates doc with {type:"nodes", nodes:[...], count, timeSet, time}.
// Used by get_nodes command, WS_EVT_CONNECT, and webBroadcastAllNodes().
static void buildNodesDoc(JsonDocument &doc)
{
  doc["type"] = "nodes";
  JsonArray arr = doc["nodes"].to<JsonArray>();
  int count = 0;
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    for (uint8_t i = 0; i < MAX_NODES; i++)
    {
      if (g_nodes[i].active)
      {
        JsonObject obj = arr.add<JsonObject>();
        nodeToSummaryJson(obj, g_nodes[i]);
        count++;
      }
    }
    xSemaphoreGive(g_nodesMutex);
  }
  char timeBuf[9];
  gwTimeString(timeBuf);
  doc["count"]     = count;
  doc["timeSet"]   = g_timeSet;
  doc["time"]      = timeBuf;
  doc["ntpSynced"] = wifiIsNtpSynced();
}

// WebSocket command handler
// ----------------------------------------------------------------------------
static void handleWsMessage(AsyncWebSocketClient *client, const String &payload)
{
  // Auth gate -- unauthenticated clients may only send cmd:"auth"
  if (!wsIsAuth(client->id())) {
    JsonDocument auth_doc;
    if (deserializeJson(auth_doc, payload) == DeserializationError::Ok) {
      const char *cmd = auth_doc["cmd"] | "";
      if (strcmp(cmd, "auth") == 0) {
        const char *tok = auth_doc["token"] | "";
        bool ok = s_dashPassword.isEmpty()
                  || (s_sessionToken[0] && strcmp(tok, s_sessionToken) == 0);
        if (ok) {
          wsAuthAdd(client->id());
          client->text("{\"type\":\"auth\",\"ok\":true}");
        } else {
          client->text("{\"type\":\"auth\",\"ok\":false}");
          client->close();
        }
        return;
      }
    }
    client->text("{\"type\":\"auth\",\"ok\":false}");
    client->close();
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok)
    return;

  const char *cmd = doc["cmd"] | "";
  uint8_t nodeId = (uint8_t)(doc["node"] | 0);

  // -- relay_manual --------------------------------------------------------
  if (strcmp(cmd, "relay_manual") == 0)
  {
    uint8_t state = (uint8_t)(doc["state"] | 0);
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF)
    {
      uint8_t pkt[3] = {PKT_RELAY_MANUAL, nodeId, state};
      ok = tdmaQueueCommand(idx, pkt, 3);
    }
    JsonDocument ack;
    ack["type"] = "relay_ack";
    ack["node"] = nodeId;
    ack["success"] = ok;
    wsSendToClient(client, ack);
    return;
  }

  // -- relay_schedule -------------------------------------------------------
  if (strcmp(cmd, "relay_schedule") == 0)
  {
    uint8_t startH = (uint8_t)(doc["startH"] | 0);
    uint8_t startM = (uint8_t)(doc["startM"] | 0);
    uint8_t endH = (uint8_t)(doc["endH"] | 0);
    uint8_t endM = (uint8_t)(doc["endM"] | 0);
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF)
    {
      uint8_t pkt[7] = {PKT_RELAY_SCHEDULE, nodeId, 1,
                        startH, startM, endH, endM};
      ok = tdmaQueueCommand(idx, pkt, 7);
    }
    JsonDocument ack;
    ack["type"] = "schedule_ack";
    ack["node"] = nodeId;
    ack["success"] = ok;
    wsSendToClient(client, ack);
    return;
  }

  // -- relay_clear -----------------------------------------------------------
  if (strcmp(cmd, "relay_clear") == 0)
  {
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF)
    {
      uint8_t pkt[2] = {PKT_RELAY_CLEAR, nodeId};
      ok = tdmaQueueCommand(idx, pkt, 2);
    }
    JsonDocument ack;
    ack["type"] = "clear_ack";
    ack["node"] = nodeId;
    ack["success"] = ok;
    wsSendToClient(client, ack);
    return;
  }

  // -- set_threshold ----------------------------------------------------------
  if (strcmp(cmd, "set_threshold") == 0)
  {
    uint16_t watts = (uint16_t)(doc["watts"] | 0);
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF && watts > 0 && watts <= 23000)
    {
      uint8_t pkt[4] = {PKT_THRESHOLD, nodeId,
                        (uint8_t)(watts & 0xFF), (uint8_t)(watts >> 8)};
      ok = tdmaQueueCommand(idx, pkt, 4);
    }
    JsonDocument ack;
    ack["type"] = "threshold_ack";
    ack["node"] = nodeId;
    ack["success"] = ok;
    wsSendToClient(client, ack);
    return;
  }

  // -- nudge ------------------------------------------------------------------
  if (strcmp(cmd, "nudge") == 0)
  {
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF)
    {
      uint8_t pkt[2] = {PKT_NUDGE, nodeId};
      ok = tdmaQueueCommand(idx, pkt, 2);
    }
    JsonDocument ack;
    ack["type"] = "nudge_ack";
    ack["node"] = nodeId;
    ack["success"] = ok;
    wsSendToClient(client, ack);
    return;
  }

  // -- rename -----------------------------------------------------------------
  if (strcmp(cmd, "rename") == 0)
  {
    const char *name = doc["name"] | "";
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    if (idx != 0xFF && strlen(name) >= 1 && strlen(name) <= 29)
    {
      if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
      {
        strlcpy(g_nodes[idx].label, name, sizeof(g_nodes[idx].label));
        xSemaphoreGive(g_nodesMutex);
      }
      framSaveLabel(idx);
      // Broadcast name_changed to ALL clients
      JsonDocument bcast;
      bcast["type"] = "name_changed";
      bcast["node"] = nodeId;
      bcast["name"] = name;
      String json;
      serializeJson(bcast, json);
      ws.textAll(json);
    }
    return;
  }

  // -- set_time -----------------------------------------------------------------
  if (strcmp(cmd, "set_time") == 0)
  {
    // Always consume the timezone offset when provided -- independent of NTP state.
    if (doc["utcOffset"].is<int32_t>())
    {
      int32_t off = (int32_t)doc["utcOffset"];
      if (off >= -43200 * 60 && off <= 50400 * 60) // sanity: UTC-12 .. UTC+14
        wifiSetTzOffset(off);
    }

    char timeBuf[9];
    gwTimeString(timeBuf);
    JsonDocument bcast;
    bcast["type"]    = "time_set";
    bcast["timeSet"] = true;
    if (wifiIsNtpSynced())
    {
      // NTP is authoritative -- echo current NTP time back to the requesting
      // client only; do not override the synced clock with browser time.
      bcast["time"]   = timeBuf;
      bcast["source"] = "ntp";
      wsSendToClient(client, bcast);
      return;
    }
    // NTP not active -- accept client time as clock source (AP mode or NTP failed)
    uint8_t h = (uint8_t)(doc["hour"]   | 0);
    uint8_t m = (uint8_t)(doc["minute"] | 0);
    uint8_t s = (uint8_t)(doc["second"] | 0);
    setGatewayTime(h, m, s);
    gwTimeString(timeBuf);
    bcast["time"]   = timeBuf;
    bcast["source"] = "client";
    String json;
    serializeJson(bcast, json);
    ws.textAll(json);
    logAsync("[WEB] Time set from client: %s\n", timeBuf);
    return;
  }

  // -- clear_energy -------------------------------------------------------------
  if (strcmp(cmd, "clear_energy") == 0)
  {
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    if (idx != 0xFF)
    {
      if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
      {
        g_nodes[idx].accumEnergy = 0;
        xSemaphoreGive(g_nodesMutex);
      }
    }
    JsonDocument ack;
    ack["type"] = "energy_cleared";
    ack["node"] = nodeId;
    ack["success"] = true;
    wsSendToClient(client, ack);
    return;
  }

  // -- clear_all_energy --------------------------------------------------------
  if (strcmp(cmd, "clear_all_energy") == 0)
  {
    if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
      for (uint8_t i = 0; i < MAX_NODES; i++)
      {
        g_nodes[i].accumEnergy = 0;
      }
      xSemaphoreGive(g_nodesMutex);
    }
    JsonDocument ack;
    ack["type"] = "all_energy_cleared";
    ack["node"] = 0;
    ack["success"] = true;
    wsSendToClient(client, ack);
    return;
  }

  // -- get_nodes ---------------------------------------------------------------
  if (strcmp(cmd, "get_nodes") == 0)
  {
    JsonDocument resp;
    buildNodesDoc(resp);
    wsSendToClient(client, resp);
    return;
  }

#ifdef TRANSPORT_TEST
  // -- transport_test ---------------------------------------------------------
  // Enqueues a GFSK bulk transfer for Phase 4/5 transport validation.
  // Payload scenarios: echo, boundary, pattern, pdr_measure, broadcast_500.
  if (strcmp(cmd, "transport_test") == 0)
  {
    uint8_t     idx      = tdmaFindSlotByNodeId(nodeId);
    const char* scenario = doc["scenario"] | "echo";
    uint16_t    size     = (uint16_t)(doc["size"] | 0);

    JsonDocument ack;
    ack["type"] = "transport_test_ack";
    ack["node"] = nodeId;
    ack["scenario"] = scenario;

    if (idx == 0xFF) {
      ack["success"] = false;
      ack["reason"]  = "node_not_found";
      wsSendToClient(client, ack);
      return;
    }

    // Build test payload based on scenario
    static uint8_t s_testPayload[BULK_MAX_PAYLOAD];
    uint16_t payloadLen = 0;
    bool validScenario  = true;

    if (strcmp(scenario, "echo") == 0) {
      payloadLen = 16;
      memset(s_testPayload, 0xAB, payloadLen);
    } else if (strcmp(scenario, "boundary") == 0) {
      if (size == 0 || size > BULK_MAX_PAYLOAD) {
        ack["success"] = false;
        ack["reason"]  = "overflow_rejected";
        wsSendToClient(client, ack);
        return;
      }
      payloadLen = size;
      for (uint16_t i = 0; i < payloadLen; i++) s_testPayload[i] = (uint8_t)(i & 0xFF);
    } else if (strcmp(scenario, "pattern") == 0) {
      payloadLen = BULK_MAX_PAYLOAD;
      for (uint16_t i = 0; i < payloadLen; i++) s_testPayload[i] = (uint8_t)(i & 0xFF);
    } else if (strcmp(scenario, "pdr_measure") == 0) {
      payloadLen = 2000;
      memset(s_testPayload, 0xAA, payloadLen);
    } else if (strcmp(scenario, "broadcast_500") == 0) {
      payloadLen = 500;
      for (uint16_t i = 0; i < payloadLen; i++) s_testPayload[i] = (uint8_t)(i & 0xFF);
    } else {
      validScenario = false;
    }

    if (!validScenario) {
      ack["success"] = false;
      ack["reason"]  = "unknown_scenario";
      wsSendToClient(client, ack);
      return;
    }

    bool ok = bulkEnqueue(idx, 0, 0xF0, s_testPayload, payloadLen);
    if (ok) {
      g_bulkSession.testScenario = (strcmp(scenario, "echo")         == 0) ? BULK_TEST_ECHO
                                 : (strcmp(scenario, "boundary")      == 0) ? BULK_TEST_BOUNDARY
                                 : (strcmp(scenario, "pattern")       == 0) ? BULK_TEST_PATTERN
                                 : (strcmp(scenario, "pdr_measure")   == 0) ? BULK_TEST_PDR
                                 : BULK_TEST_BROADCAST500;
    }
    ack["success"] = ok;
    ack["size"]    = payloadLen;
    if (!ok) ack["reason"] = "session_busy_or_inactive";
    wsSendToClient(client, ack);
    return;
  }
#endif // TRANSPORT_TEST
}

// -----------------------------------------------------------------------------
// WebSocket event handler
// -----------------------------------------------------------------------------
static void onWsEvent(AsyncWebSocket * /*server*/, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT)
  {
    logAsync("[WS] Client #%u connected from %s\n",
             client->id(), client->remoteIP().toString().c_str());
    // If no password, auto-authenticate and push node list immediately
    if (s_dashPassword.isEmpty()) {
      wsAuthAdd(client->id());
      client->text("{\"type\":\"auth\",\"ok\":true}");
    }
    // Else: client must send {cmd:"auth",token:"..."} first
  }
  else if (type == WS_EVT_DISCONNECT)
  {
    logAsync("[WS] Client #%u disconnected\n", client->id());
    wsAuthRemove(client->id());
  }
  else if (type == WS_EVT_DATA)
  {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
    {
      String msg((char *)data, len);
      handleWsMessage(client, msg);
    }
  }
}

// -----------------------------------------------------------------------------
// REST route handlers
// -----------------------------------------------------------------------------

/** GET /api/nodes */
static void handleGetNodes(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  JsonDocument doc;
  JsonArray arr = doc["nodes"].to<JsonArray>();
  int count = 0;

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    for (uint8_t i = 0; i < MAX_NODES; i++)
    {
      if (g_nodes[i].active)
      {
        JsonObject obj = arr.add<JsonObject>();
        nodeToSummaryJson(obj, g_nodes[i]);
        count++;
      }
    }
    xSemaphoreGive(g_nodesMutex);
  }

  char timeBuf[9];
  gwTimeString(timeBuf);
  doc["count"] = count;
  doc["timeSet"] = g_timeSet;
  doc["time"] = timeBuf;

  String json;
  serializeJson(doc, json);
  req->send(200, "application/json", json);
}

/** GET /api/node/{id}/live */
static void handleGetNodeLive(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  int nodeId = parseNodeIdFromPath(req->url());
  if (nodeId < 1 || nodeId > MAX_NODES)
  {
    req->send(400, "application/json", "{\"error\":\"invalid id\"}");
    return;
  }
  uint8_t idx = tdmaFindSlotByNodeId((uint8_t)nodeId);
  if (idx == 0xFF)
  {
    req->send(404, "application/json", "{\"error\":\"not found\"}");
    return;
  }

  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    nodeToDetailJson(obj, g_nodes[idx]);
    xSemaphoreGive(g_nodesMutex);
  }

  String json;
  serializeJson(doc, json);
  req->send(200, "application/json", json);
}

/** GET /api/node/{id}/history */
static void handleGetNodeHistory(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  int nodeId = parseNodeIdFromPath(req->url());
  if (nodeId < 1 || nodeId > MAX_NODES)
  {
    req->send(400, "application/json", "{\"error\":\"invalid id\"}");
    return;
  }
  uint8_t idx = tdmaFindSlotByNodeId((uint8_t)nodeId);
  if (idx == 0xFF)
  {
    req->send(404, "application/json", "[]");
    return;
  }

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    NodeState *ns = &g_nodes[idx];
    // Walk the circular buffer in chronological order
    int start = (ns->histCount < HISTORY_MAX_POINTS)
                    ? 0
                    : ns->histHead;
    for (int i = 0; i < ns->histCount; i++)
    {
      int cidx = (start + i) % HISTORY_MAX_POINTS;
      JsonObject pt = arr.add<JsonObject>();
      pt["t"] = ns->history[cidx].t;
      pt["v"] = ns->history[cidx].v;
      pt["i"] = ns->history[cidx].i;
      pt["p"] = ns->history[cidx].p;
    }
    xSemaphoreGive(g_nodesMutex);
  }

  String json;
  serializeJson(doc, json);
  req->send(200, "application/json", json);
}

/** POST /api/node/{id}/relay  body: state=0|1 */
static void handlePostNodeRelay(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  int nodeId = parseNodeIdFromPath(req->url());
  if (!req->hasParam("state", true))
  {
    req->send(400, "application/json", "{\"error\":\"missing state\"}");
    return;
  }
  uint8_t state = (uint8_t)req->getParam("state", true)->value().toInt();
  uint8_t idx = tdmaFindSlotByNodeId((uint8_t)nodeId);
  bool ok = false;
  if (idx != 0xFF)
  {
    uint8_t pkt[3] = {PKT_RELAY_MANUAL, (uint8_t)nodeId, state};
    ok = tdmaQueueCommand(idx, pkt, 3);
  }
  req->send(200, "application/json", ok ? "{\"success\":true}" : "{\"success\":false}");
}

/** POST /api/node/{id}/name  body: name=<string> */
static void handlePostNodeName(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  int nodeId = parseNodeIdFromPath(req->url());
  if (!req->hasParam("name", true))
  {
    req->send(400, "application/json", "{\"error\":\"missing name\"}");
    return;
  }
  String name = req->getParam("name", true)->value();
  if (name.length() < 1 || name.length() > 29)
  {
    req->send(400, "application/json", "{\"error\":\"name length 1-29\"}");
    return;
  }
  uint8_t idx = tdmaFindSlotByNodeId((uint8_t)nodeId);
  if (idx == 0xFF)
  {
    req->send(404, "application/json", "{\"error\":\"not found\"}");
    return;
  }
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    strlcpy(g_nodes[idx].label, name.c_str(), sizeof(g_nodes[idx].label));
    xSemaphoreGive(g_nodesMutex);
  }
  framSaveLabel(idx);

  JsonDocument bcast;
  bcast["type"] = "name_changed";
  bcast["node"] = nodeId;
  bcast["name"] = name;
  String json;
  serializeJson(bcast, json);
  ws.textAll(json);

  req->send(200, "application/json", "{\"ok\":true}");
}

/** POST /api/time  body: hour=H&minute=M&second=S */
static void handlePostTime(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  char timeBuf[9];
  if (wifiIsNtpSynced())
  {
    gwTimeString(timeBuf);
    JsonDocument doc;
    doc["ok"]     = true;
    doc["source"] = "ntp";
    doc["time"]   = timeBuf;
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
    return;
  }

  uint8_t h = req->hasParam("hour", true) ? (uint8_t)req->getParam("hour", true)->value().toInt() : 0;
  uint8_t m = req->hasParam("minute", true) ? (uint8_t)req->getParam("minute", true)->value().toInt() : 0;
  uint8_t s = req->hasParam("second", true) ? (uint8_t)req->getParam("second", true)->value().toInt() : 0;
  setGatewayTime(h, m, s);

  gwTimeString(timeBuf);

  JsonDocument bcast;
  bcast["type"]    = "time_set";
  bcast["timeSet"] = true;
  bcast["time"]    = timeBuf;
  bcast["source"]  = "client";
  String json;
  serializeJson(bcast, json);
  ws.textAll(json);

  logAsync("[WEB] POST /api/time (client) -> %s\n", timeBuf);
  req->send(200, "application/json", "{\"ok\":true}");
}

/** GET /api/status */
static void handleGetStatus(AsyncWebServerRequest *req)
{
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  JsonDocument doc;
  char timeBuf[9];
  gwTimeString(timeBuf);

  int nodeCount = 0;
  for (uint8_t i = 0; i < MAX_NODES; i++)
    if (g_nodes[i].active)
      nodeCount++;

  doc["uptime"] = millis() / 1000UL;
  doc["freeHeap"] = halFreeHeap();
  doc["wifiRSSI"] = wifiIsStaConnected() ? WiFi.RSSI() : 0;
  doc["ip"] = wifiIsStaConnected() ? WiFi.localIP().toString() : "192.168.4.1";
  doc["loraFreq"] = LORA_CHANNELS[0];
  doc["nodeCount"] = nodeCount;
  doc["maxNodes"] = MAX_NODES;
  doc["wsClients"] = ws.count();
  doc["timeSet"]   = g_timeSet;
  doc["time"]      = timeBuf;
  doc["ntpSynced"] = wifiIsNtpSynced();
  doc["apActive"]  = wifiIsApActive();
  doc["version"]   = FW_VERSION_STR;
  doc["wifiMode"]  = wifiIsApActive() ? "ap" : "sta";
  doc["ssid"]      = wifiIsApActive() ? wifiGetApSsid() : WiFi.SSID();

  String json;
  serializeJson(doc, json);
  req->send(200, "application/json", json);
}

// -----------------------------------------------------------------------------
// 5-second periodic nodes broadcast (called from loop())
// -----------------------------------------------------------------------------
static uint32_t lastBcastMs = 0;

// Called from main.cpp loop() on Core 0
void webBroadcastAllNodes()
{
  uint32_t now = millis();
  if (now - lastBcastMs >= 5000)
  {
    lastBcastMs = now;
    if (ws.count() > 0)
    {
      JsonDocument doc;
      buildNodesDoc(doc);
      String json;
      serializeJson(doc, json);
      ws.textAll(json);
    }
  }
  ws.cleanupClients();
}

// -----------------------------------------------------------------------------
// RFID key provisioning (encrypted builds only)
// POST /api/provision       -- kick off card write; returns 202 immediately
// GET  /api/provision/status -- poll for result: idle | pending | ok | fail
// The write task runs in the background so the async web handler is not blocked.
// -----------------------------------------------------------------------------
#ifdef PKT_ENCRYPTION

typedef enum { RFID_IDLE, RFID_PENDING, RFID_OK, RFID_FAIL } RfidProvState_t;
static volatile RfidProvState_t s_provState = RFID_IDLE;

static void provisionWriteTask(void* /*params*/) {
  bool ok = rfidProvisionWrite(cryptoGetKey());
  s_provState = ok ? RFID_OK : RFID_FAIL;
  logAsync("[WEB] Provision card write: %s\n", ok ? "OK" : "FAIL");
  vTaskDelete(nullptr);
}

static void handlePostProvision(AsyncWebServerRequest* req) {
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  if (s_provState == RFID_PENDING) {
    req->send(409, "application/json", "{\"error\":\"already in progress\"}");
    return;
  }
  s_provState = RFID_PENDING;
  xTaskCreate(provisionWriteTask, "RFID_PROV", 4096, nullptr, 1, nullptr);
  req->send(202, "application/json", "{\"status\":\"pending\",\"message\":\"tap MIFARE card to PN532 within 5 seconds\"}");
}

static void handleGetProvisionStatus(AsyncWebServerRequest* req) {
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  JsonDocument doc;
  switch (s_provState) {
    case RFID_IDLE:    doc["status"] = "idle";    break;
    case RFID_PENDING: doc["status"] = "pending"; break;
    case RFID_OK:      doc["status"] = "ok";      break;
    case RFID_FAIL:    doc["status"] = "fail";    break;
  }
  String json;
  serializeJson(doc, json);
  req->send(200, "application/json", json);
}

static volatile bool s_keyRegenPending = false;

static void keyRegenTask(void* /*params*/) {
  g_networkEpoch++;
  logAsync("[WEB] Epoch bumped to %d -- regenerating key after invalidating beacon\n", g_networkEpoch);
  vTaskDelay(pdMS_TO_TICKS(SUPERFRAME_MS + 500));  // guarantee >=1 beacon with new epoch
  cryptoGenerateKey();
  s_keyRegenPending = false;
  vTaskDelete(nullptr);
}

static void handlePostKeyRegen(AsyncWebServerRequest* req) {
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  if (s_keyRegenPending) {
    req->send(409, "application/json", "{\"error\":\"already in progress\"}");
    return;
  }
  s_keyRegenPending = true;
  xTaskCreate(keyRegenTask, "KEYREGEN", 3072, nullptr, 1, nullptr);
  req->send(202, "application/json", "{\"ok\":true}");
}

#endif // PKT_ENCRYPTION

// -----------------------------------------------------------------------------
// Auth endpoints
// -----------------------------------------------------------------------------

/** POST /api/login  (public) */
static void handlePostLogin(AsyncWebServerRequest *req) {
  String pass = req->hasParam("password", true)
                ? req->getParam("password", true)->value() : "";
  if (s_dashPassword.isEmpty() || pass == s_dashPassword) {
    generateToken();
    String body = "{\"ok\":true,\"token\":\"";
    body += s_sessionToken;
    body += "\"}";
    req->send(200, "application/json", body);
  } else {
    memset(s_sessionToken, 0, sizeof(s_sessionToken));
    req->send(401, "application/json", "{\"ok\":false}");
  }
}

/** GET /api/logout  (public) */
static void handleGetLogout(AsyncWebServerRequest *req) {
  memset(s_sessionToken, 0, sizeof(s_sessionToken));
  s_authWsCount = 0;
  req->send(200, "application/json", "{\"ok\":true}");
}

/** GET /api/authstatus  (public -- tells JS whether login is required) */
static void handleGetAuthStatus(AsyncWebServerRequest *req) {
  String body = "{\"hasPassword\":";
  body += s_dashPassword.isEmpty() ? "false}" : "true}";
  req->send(200, "application/json", body);
}

/** GET /api/dashsecure  (protected) */
static void handleGetDashSecure(AsyncWebServerRequest *req) {
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  String body = "{\"hasPassword\":";
  body += s_dashPassword.isEmpty() ? "false}" : "true}";
  req->send(200, "application/json", body);
}

/** POST /api/dashsecure  (protected) */
static void handlePostDashSecure(AsyncWebServerRequest *req) {
  if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  String pass = req->hasParam("password", true)
                ? req->getParam("password", true)->value() : "";
  pass.trim();
  if (!pass.isEmpty() && pass.length() < 8) {
    req->send(400, "application/json", "{\"ok\":false,\"message\":\"Minimum 8 characters\"}");
    return;
  }
  nvsDashPassSave(pass);
  s_dashPassword = pass;
  memset(s_sessionToken, 0, sizeof(s_sessionToken));
  s_authWsCount = 0;
  logAsync("[WEB] Dashboard password %s\n", pass.isEmpty() ? "cleared" : "updated");
  req->send(200, "application/json", "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// Gateway reboot
// POST /api/reboot -- responds immediately then restarts after 300 ms
// -----------------------------------------------------------------------------
static void rebootTask(void* /*params*/) {
  vTaskDelay(pdMS_TO_TICKS(300));  // let HTTP response flush
  halReboot();
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
void webServerSetup()
{
  // Load dashboard password from NVS
  s_dashPassword = nvsDashPassLoad();

  // WebSocket handler
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // Auth routes (public -- no webCheckAuth guard)
  server.on("/api/login",      HTTP_POST, handlePostLogin);
  server.on("/api/logout",     HTTP_GET,  handleGetLogout);
  server.on("/api/authstatus", HTTP_GET,  handleGetAuthStatus);
  server.on("/api/dashsecure", HTTP_GET,  handleGetDashSecure);
  server.on("/api/dashsecure", HTTP_POST, handlePostDashSecure);

  // REST routes
  server.on("/api/nodes", HTTP_GET, handleGetNodes);
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/time", HTTP_POST, handlePostTime);

  // Per-node routes -- match on prefix, dispatch on action suffix
  server.on("/api/node", HTTP_ANY, [](AsyncWebServerRequest *req) {
    // Auth checked inside each individual handler
    String path = req->url();
    bool isPost = (req->method() == HTTP_POST);

    if      (path.endsWith("/live"))    handleGetNodeLive(req);
    else if (path.endsWith("/history")) handleGetNodeHistory(req);
    else if (path.endsWith("/relay") && isPost)  handlePostNodeRelay(req);
    else if (path.endsWith("/name")  && isPost)  handlePostNodeName(req);
    else req->send(404, "application/json", "{\"error\":\"not found\"}");
  });

  // -- Feature flags -- lets the dashboard adapt without separate builds -------
  server.on("/api/features", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    req->send(200, "application/json",
      "{\"encryption\":"
#ifdef PKT_ENCRYPTION
      "true"
#else
      "false"
#endif
      ",\"transport-test\":"
#ifdef TRANSPORT_TEST
      "true"
#else
      "false"
#endif
      ",\"knn\":"
#ifdef KNN_INFERENCE
      "true"
#else
      "false"
#endif
      ",\"knn_embed\":"
#ifdef KNN_MODEL_EMBED
      "true"
#else
      "false"
#endif
      "}");
  });

  // -- K-NN appliance inference (KNN_INFERENCE builds only) ------------------
#ifdef KNN_INFERENCE

  server.on("/api/knn/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    JsonDocument doc;
    doc["loaded"]       = g_knnDb.loaded;
    doc["profileCount"] = g_knnDb.profileCount;
    doc["labelCount"]   = g_knnDb.labelCount;
    doc["fromEmbed"]    = g_knnDb.fromEmbed;
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });

  server.on("/api/knn/export", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    knnSerialize(*resp);
    req->send(resp);
  });

  // Static import buffer -- 8 KB handles models up to ~50 profiles
  static uint8_t s_knnImportBuf[8192];
  static size_t  s_knnImportLen  = 0;
  static bool    s_knnImportOk   = false;

  server.on("/api/knn/import", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
      if (!s_knnImportOk) {
        req->send(400, "application/json", "{\"error\":\"parse failed\"}");
      } else {
        JsonDocument ack;
        ack["ok"]           = true;
        ack["profileCount"] = g_knnDb.profileCount;
        ack["labelCount"]   = g_knnDb.labelCount;
        String json;
        serializeJson(ack, json);
        req->send(200, "application/json", json);
      }
    },
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
       size_t index, size_t total) {
      if (index == 0) {
        s_knnImportLen = 0;
        s_knnImportOk  = (total <= sizeof(s_knnImportBuf));
      }
      if (s_knnImportOk && s_knnImportLen + len <= sizeof(s_knnImportBuf)) {
        memcpy(s_knnImportBuf + s_knnImportLen, data, len);
        s_knnImportLen += len;
      } else {
        s_knnImportOk = false;
      }
      if (index + len == total && s_knnImportOk) {
        s_knnImportOk = knnDeserialize(s_knnImportBuf, s_knnImportLen, false);
        if (s_knnImportOk) framQueueSaveKnn();
      }
    }
  );

  server.on("/api/knn/profiles", HTTP_DELETE, [](AsyncWebServerRequest* req) {
    if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    knnDbClear();
    framQueueSaveKnn();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/knn/train/start", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    if (!req->hasParam("nodeId", true) || !req->hasParam("label", true)) {
      req->send(400, "application/json", "{\"error\":\"missing params\"}");
      return;
    }
    uint8_t nodeId = (uint8_t)req->getParam("nodeId", true)->value().toInt();
    const char* label = req->getParam("label", true)->value().c_str();
    uint32_t durMin = req->hasParam("durationMin", true)
                      ? (uint32_t)req->getParam("durationMin", true)->value().toInt()
                      : 30u;
    if (nodeId < 1 || nodeId > MAX_NODES || label[0] == '\0') {
      req->send(400, "application/json", "{\"error\":\"invalid params\"}");
      return;
    }
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    if (idx == 0xFF) {
      req->send(404, "application/json", "{\"error\":\"node not found\"}");
      return;
    }
    knnTrainStart(idx, label, durMin * 60000UL);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/knn/train/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    if (!req->hasParam("nodeId", true)) {
      req->send(400, "application/json", "{\"error\":\"missing nodeId\"}");
      return;
    }
    uint8_t nodeId = (uint8_t)req->getParam("nodeId", true)->value().toInt();
    uint8_t idx    = tdmaFindSlotByNodeId(nodeId);
    if (idx == 0xFF) {
      req->send(404, "application/json", "{\"error\":\"node not found\"}");
      return;
    }
    knnTrainStop(idx);
    JsonDocument ack;
    ack["ok"]           = true;
    ack["profileCount"] = g_knnDb.profileCount;
    String json;
    serializeJson(ack, json);
    req->send(200, "application/json", json);
  });

  server.on("/api/knn/train/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    JsonDocument doc;
    JsonArray    arr = doc["sessions"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_NODES; i++) {
      const KnnTrainSession& s = g_knnTrain[i];
      if (!s.active && !s.finalized) continue;
      JsonObject obj   = arr.add<JsonObject>();
      obj["slotIdx"]   = i;
      obj["label"]     = s.label;
      obj["active"]    = s.active;
      obj["finalized"] = s.finalized;
      uint32_t now     = millis();
      obj["remainSec"] = s.active && (s.deadlineMs > now)
                         ? (s.deadlineMs - now) / 1000UL : 0u;
      uint32_t total   = 0;
      for (uint8_t st = 0; st < s.nStates; st++) total += s.states[st].cnt;
      obj["samples"] = total;
      obj["nStates"] = s.nStates;
    }
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });

#endif // KNN_INFERENCE

  // -- Reboot ----------------------------------------------------------------
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!webCheckAuth(req)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    req->send(200, "application/json", "{\"status\":\"rebooting\"}");
    xTaskCreate(rebootTask, "reboot", 1024, nullptr, 1, nullptr);
  });

  // -- RFID provisioning + key management (encrypted builds only) -----------
#ifdef PKT_ENCRYPTION
  server.on("/api/provision",        HTTP_POST, handlePostProvision);
  server.on("/api/provision/status", HTTP_GET,  handleGetProvisionStatus);
  server.on("/api/keygen",           HTTP_POST, handlePostKeyRegen);
#endif

  // -- WiFi config routes (scan, connect, status, etc.) ---------------------
  // Registers /api/info, /api/scan, /api/connect, /api/wifistatus, /api/disconnect, /api/forget.
  wifiRegisterRoutes(server);

  // Static files from LittleFS (dashboard SPA + wifi_config.html)
  // Serve static files from LittleFS.
  // ESPAsyncWebServer probes for filename.gz before filename on every
  // request. The "file not found" log lines for .gz are benign -- the
  // library falls back to the uncompressed file automatically.
  // To silence the log: add -D ASYNCWEBSERVER_REGEX=0 and
  // -D CONFIG_ASYNC_TCP_RUNNING_CORE=0 to platformio.ini build_flags,
  // or gzip the HTML files before uploading (recommended for index.html).
  // To gzip: gzip -k index.html && gzip -k wifi_config.html
  // Place the .gz files alongside the originals in the data/ folder.
  server.serveStatic("/", LittleFS, "/")
        .setDefaultFile("/index.html")
        .setCacheControl("no-cache");

  // CORS headers on all responses
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, X-Auth-Token");

  // Catch-all: OPTIONS pre-flight + captive-portal redirect when AP is active
  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->method() == HTTP_OPTIONS) {
      AsyncWebServerResponse *resp = req->beginResponse(204);
      resp->addHeader("Access-Control-Allow-Origin",  "*");
      resp->addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
      resp->addHeader("Access-Control-Allow-Headers", "Content-Type, X-Auth-Token");
      req->send(resp);
    } else {
      // Redirect OS captive-portal probes to dashboard when AP on
      wifiHandleCatchAll(req);
    }
  });

  server.begin();
  logAsync("[WEB] Server started on port 80\n");
}