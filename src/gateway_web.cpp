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
 *   GET  /api/node/{id}/rules
 *   POST /api/node/{id}/rules   JSON body: {"rules":[...]}
 *   POST /api/node/{id}/relay
 *   POST /api/node/{id}/name
 *   POST /api/time
 *   GET  /api/status
 *
 * WebSocket ws://<ip>/ws
 *   Commands: relay_manual, auto_enable, auto_disable, set_threshold,
 *             nudge, rename, set_time, clear_energy, clear_all_energy,
 *             get_nodes, set_rules
 *   Push:     telemetry, nodes, name_changed, time_set, relay_ack, auto_ack,
 *             threshold_ack, nudge_ack, energy_cleared,
 *             all_energy_cleared, rules_queued
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
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// -- Server and WebSocket instances -------------------------------------------
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// -- WS broadcast queue (uint8_t slotIdx, depth 16) ---------------------------
static QueueHandle_t s_wsBroadcastQueue = nullptr;

// =================================================
// JSON helpers — build node objects from NodeState
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
  obj["alarmState"] = ns.alarmState;
  obj["alarmThreshold"] = ns.latest.alarmThreshold;
  obj["age"] = (uint32_t)((millis() - ns.lastSeen) / 1000UL);
  obj["pending"] = ns.pending;

  // Rule engine status
  static const char* sourceNames[] = {"manual", "protection", "schedule", "default"};
  JsonObject ruleStatus = obj["ruleStatus"].to<JsonObject>();
  ruleStatus["count"]              = ns.ruleCount;
  ruleStatus["engineActive"]       = (bool)ns.ruleEngineEnabled;
  ruleStatus["hasRules"]           = (bool)ns.hasAutoRule;
  ruleStatus["protectionLatched"]  = (bool)ns.protectionLatched;
  ruleStatus["relaySource"]        = sourceNames[ns.relaySource & 0x03];
  ruleStatus["deliveryActive"]     = ns.ruleDelivery.active;
}

/** Populate a JsonObject with full detail fields (used by /api/node/{id}/live and telemetry push). */
static void nodeToDetailJson(JsonObject obj, const NodeState &ns)
{
  nodeToSummaryJson(obj, ns);

  // Gateway clock status
  char timeBuf[9];
  gwTimeString(timeBuf);
  obj["timeSet"] = g_timeSet;
  obj["time"] = timeBuf;
}

/** Extract node ID from path like "/api/node/3/live" → 3 */
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

// Non-blocking queue-post called from the TDMA task (Core 1).
void webBroadcastTelemetry(uint8_t slotIdx)
{
  if (!s_wsBroadcastQueue) return;
  uint8_t idx = slotIdx;
  xQueueSend(s_wsBroadcastQueue, &idx, 0);
}

static void wsBroadcastTask(void* /*params*/)
{
  uint8_t idx;
  char logLine[LOG_LINE_MAX];
  while (true) {
    if (xQueueReceive(s_wsBroadcastQueue, &idx, pdMS_TO_TICKS(100)) == pdTRUE)
      doBroadcastTelemetry(idx);
    while (logLineDequeue(logLine, sizeof(logLine)))
      doBroadcastLogLine(logLine);
  }
}

void webBroadcastTaskStart()
{
  s_wsBroadcastQueue = xQueueCreate(16, sizeof(uint8_t));
  configASSERT(s_wsBroadcastQueue);
  xTaskCreatePinnedToCore(wsBroadcastTask, "WS_BCAST", 4096, nullptr, 1, nullptr, 0);
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
  doc["count"] = count;
  doc["timeSet"] = g_timeSet;
  doc["time"] = timeBuf;
}

// WebSocket command handler
// ----------------------------------------------------------------------------
static void handleWsMessage(AsyncWebSocketClient *client, const String &payload)
{
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

  // -- auto_enable -----------------------------------------------------------
  // Activates the rule engine on the node (rules must already be stored).
  // Sends PKT_SET_RULE with ruleIndex=0xFC (enable without clearing rules).
  if (strcmp(cmd, "auto_enable") == 0)
  {
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF)
    {
      uint8_t pkt[9] = {PKT_SET_RULE, nodeId, 0xFC, 0, 0, 0, 0, 0, 0};
      ok = tdmaQueueCommand(idx, pkt, 9);
    }
    JsonDocument ack;
    ack["type"]    = "auto_ack";
    ack["node"]    = nodeId;
    ack["enabled"] = true;
    ack["success"] = ok;
    wsSendToClient(client, ack);
    return;
  }

  // -- auto_disable ----------------------------------------------------------
  // Pauses the rule engine (rules remain stored; relay reverts to manual).
  // Sends PKT_SET_RULE with ruleIndex=0xFD (disable without clearing rules).
  if (strcmp(cmd, "auto_disable") == 0)
  {
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF)
    {
      uint8_t pkt[9] = {PKT_SET_RULE, nodeId, 0xFD, 0, 0, 0, 0, 0, 0};
      ok = tdmaQueueCommand(idx, pkt, 9);
    }
    JsonDocument ack;
    ack["type"]    = "auto_ack";
    ack["node"]    = nodeId;
    ack["enabled"] = false;
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
    uint8_t h = (uint8_t)(doc["hour"] | 0);
    uint8_t m = (uint8_t)(doc["minute"] | 0);
    uint8_t s = (uint8_t)(doc["second"] | 0);
    setGatewayTime(h, m, s);
    char timeBuf[9];
    gwTimeString(timeBuf);
    JsonDocument bcast;
    bcast["type"] = "time_set";
    bcast["timeSet"] = true;
    bcast["time"] = timeBuf;
    String json;
    serializeJson(bcast, json);
    ws.textAll(json);
    logAsync("[WEB] Time set to %s\n", timeBuf);
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

  // -- set_rules ---------------------------------------------------------------
  // Body: { "cmd":"set_rules","node":N,"rules":[{...},...] }
  // Each rule object: { "type":"default|schedule|protection",
  //   "action":"on|off", "enabled":true,
  //   "field":0-5, "op":0-4, "threshold":uint16, "hysteresis":uint16,
  //   "onTime":0-1439, "offTime":0-1439 }
  if (strcmp(cmd, "set_rules") == 0)
  {
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF)
    {
      JsonArray rulesArr = doc["rules"].as<JsonArray>();
      uint8_t count = 0;
      AutoRule rules[AUTORULE_MAX] = {};

      for (JsonObject r : rulesArr)
      {
        if (count >= AUTORULE_MAX) break;
        const char* typeStr   = r["type"]   | "default";
        const char* actionStr = r["action"] | "off";
        bool enabled          = r["enabled"] | true;

        uint8_t rtype  = RULE_TYPE_DEFAULT;
        if (strcmp(typeStr, "schedule")   == 0) rtype = RULE_TYPE_SCHEDULE;
        if (strcmp(typeStr, "protection") == 0) rtype = RULE_TYPE_PROTECTION;

        uint8_t raction = (strcmp(actionStr, "on") == 0) ? RULE_ACTION_ON : RULE_ACTION_OFF;

        rules[count].flags   = RULE_FLAGS(enabled ? 1 : 0, rtype, raction);
        rules[count].field   = (uint8_t)(r["field"] | 0);
        rules[count].op      = (uint8_t)(r["op"]    | 0);
        rules[count].param_a = (uint16_t)(r["threshold"] | r["onTime"]  | 0);
        rules[count].param_b = (uint16_t)(r["hysteresis"] | r["offTime"] | 0);
        rules[count]._pad    = 0;
        count++;
      }

      ok = tdmaQueueRules(idx, rules, count);
      if (ok) framQueueSaveRules(idx);
    }

    JsonDocument ack;
    ack["type"]    = "rules_queued";
    ack["node"]    = nodeId;
    ack["success"] = ok;
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
    // Push full node list immediately on connect
    JsonDocument doc;
    buildNodesDoc(doc);
    wsSendToClient(client, doc);
  }
  else if (type == WS_EVT_DISCONNECT)
  {
    logAsync("[WS] Client #%u disconnected\n", client->id());
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

/** GET /api/node/{id}/rules — returns JSON array of cached AutoRule[] */
static void handleGetNodeRules(AsyncWebServerRequest *req)
{
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
  JsonArray arr = doc.to<JsonArray>();

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    static const char* typeNames[]   = {"protection", "schedule", "default"};
    static const char* actionNames[] = {"off", "on"};
    const NodeState &ns = g_nodes[idx];
    for (uint8_t i = 0; i < ns.rulesStored; i++)
    {
      const AutoRule &r = ns.rules[i];
      JsonObject robj   = arr.add<JsonObject>();
      robj["enabled"]   = (bool)RULE_ENABLED(r);
      robj["type"]      = typeNames[RULE_TYPE(r) & 0x03];
      robj["action"]    = actionNames[RULE_ACTION(r) & 0x01];
      robj["field"]     = r.field;
      robj["op"]        = r.op;
      robj["param_a"]   = r.param_a;
      robj["param_b"]   = r.param_b;
    }
    xSemaphoreGive(g_nodesMutex);
  }

  String json;
  serializeJson(doc, json);
  req->send(200, "application/json", json);
}

/** Accumulate JSON body for POST /api/node/{id}/rules into req->_tempObject. */
static void handleNodeBody(AsyncWebServerRequest *req, uint8_t *data,
                           size_t len, size_t index, size_t total)
{
  if (!req->url().endsWith("/rules")) return;
  if (total > 1024) return;  // sanity cap — 8 rules fit comfortably in 512 bytes
  if (index == 0)
  {
    req->_tempObject = malloc(total + 1);
    if (!req->_tempObject) return;
  }
  if (req->_tempObject)
  {
    memcpy(reinterpret_cast<uint8_t*>(req->_tempObject) + index, data, len);
    if (index + len >= total)
      reinterpret_cast<uint8_t*>(req->_tempObject)[total] = '\0';
  }
}

/** POST /api/node/{id}/rules — JSON body: {"rules":[...]} same schema as set_rules WS */
static void handlePostNodeRules(AsyncWebServerRequest *req)
{
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

  const char *body = req->_tempObject
                     ? reinterpret_cast<const char*>(req->_tempObject)
                     : "{}";

  JsonDocument bodyDoc;
  if (deserializeJson(bodyDoc, body) != DeserializationError::Ok)
  {
    req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }

  JsonArray rulesArr = bodyDoc["rules"].as<JsonArray>();
  uint8_t count = 0;
  AutoRule rules[AUTORULE_MAX] = {};

  for (JsonObject r : rulesArr)
  {
    if (count >= AUTORULE_MAX) break;
    const char* typeStr   = r["type"]   | "default";
    const char* actionStr = r["action"] | "off";
    bool enabled          = r["enabled"] | true;

    uint8_t rtype = RULE_TYPE_DEFAULT;
    if (strcmp(typeStr, "schedule")   == 0) rtype = RULE_TYPE_SCHEDULE;
    if (strcmp(typeStr, "protection") == 0) rtype = RULE_TYPE_PROTECTION;

    uint8_t raction = (strcmp(actionStr, "on") == 0) ? RULE_ACTION_ON : RULE_ACTION_OFF;

    rules[count].flags   = RULE_FLAGS(enabled ? 1 : 0, rtype, raction);
    rules[count].field   = (uint8_t)(r["field"] | 0);
    rules[count].op      = (uint8_t)(r["op"]    | 0);
    rules[count].param_a = (uint16_t)(r["threshold"] | r["onTime"]  | 0);
    rules[count].param_b = (uint16_t)(r["hysteresis"] | r["offTime"] | 0);
    rules[count]._pad    = 0;
    count++;
  }

  bool ok = tdmaQueueRules(idx, rules, count);
  if (ok) framQueueSaveRules(idx);

  JsonDocument resp;
  resp["success"] = ok;
  resp["node"]    = nodeId;
  resp["count"]   = count;
  String json;
  serializeJson(resp, json);
  req->send(ok ? 200 : 409, "application/json", json);
}

/** POST /api/time  body: hour=H&minute=M&second=S */
static void handlePostTime(AsyncWebServerRequest *req)
{
  uint8_t h = req->hasParam("hour", true) ? (uint8_t)req->getParam("hour", true)->value().toInt() : 0;
  uint8_t m = req->hasParam("minute", true) ? (uint8_t)req->getParam("minute", true)->value().toInt() : 0;
  uint8_t s = req->hasParam("second", true) ? (uint8_t)req->getParam("second", true)->value().toInt() : 0;
  setGatewayTime(h, m, s);

  char timeBuf[9];
  gwTimeString(timeBuf);

  JsonDocument bcast;
  bcast["type"] = "time_set";
  bcast["timeSet"] = true;
  bcast["time"] = timeBuf;
  String json;
  serializeJson(bcast, json);
  ws.textAll(json);

  logAsync("[WEB] POST /api/time -> %s\n", timeBuf);
  req->send(200, "application/json", "{\"ok\":true}");
}

/** GET /api/status */
static void handleGetStatus(AsyncWebServerRequest *req)
{
  JsonDocument doc;
  char timeBuf[9];
  gwTimeString(timeBuf);

  int nodeCount = 0;
  for (uint8_t i = 0; i < MAX_NODES; i++)
    if (g_nodes[i].active)
      nodeCount++;

  doc["uptime"] = millis() / 1000UL;
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["wifiRSSI"] = wifiIsStaConnected() ? WiFi.RSSI() : 0;
  doc["ip"] = wifiIsStaConnected() ? WiFi.localIP().toString() : "192.168.4.1";
  doc["loraFreq"] = LORA_CHANNELS[0];
  doc["nodeCount"] = nodeCount;
  doc["maxNodes"] = MAX_NODES;
  doc["wsClients"] = ws.count();
  doc["timeSet"] = g_timeSet;
  doc["time"] = timeBuf;
  doc["apActive"] = wifiIsApActive();
  doc["version"]  = FW_VERSION_STR;
  doc["wifiMode"] = wifiIsApActive() ? "ap" : "sta";
  doc["ssid"]     = wifiIsApActive() ? CONFIG_AP_SSID : WiFi.SSID();

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
// POST /api/provision       — kick off card write; returns 202 immediately
// GET  /api/provision/status — poll for result: idle | pending | ok | fail
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
  if (s_provState == RFID_PENDING) {
    req->send(409, "application/json", "{\"error\":\"already in progress\"}");
    return;
  }
  s_provState = RFID_PENDING;
  xTaskCreate(provisionWriteTask, "RFID_PROV", 4096, nullptr, 1, nullptr);
  req->send(202, "application/json", "{\"status\":\"pending\",\"message\":\"tap MIFARE card to PN532 within 5 seconds\"}");
}

static void handleGetProvisionStatus(AsyncWebServerRequest* req) {
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

#endif // PKT_ENCRYPTION

// -----------------------------------------------------------------------------
// Gateway reboot
// POST /api/reboot — responds immediately then restarts after 300 ms
// -----------------------------------------------------------------------------
static void rebootTask(void* /*params*/) {
  vTaskDelay(pdMS_TO_TICKS(300));
  ESP.restart();
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
void webServerSetup()
{
  // WebSocket handler
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // REST routes
  server.on("/api/nodes", HTTP_GET, handleGetNodes);
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/time", HTTP_POST, handlePostTime);

  // Per-node routes — match on prefix, dispatch on action suffix
  server.on("/api/node", HTTP_ANY,
    [](AsyncWebServerRequest *req) {
      String path  = req->url();
      bool isPost  = (req->method() == HTTP_POST);

      if      (path.endsWith("/live"))                      handleGetNodeLive(req);
      else if (path.endsWith("/history"))                   handleGetNodeHistory(req);
      else if (path.endsWith("/rules") && !isPost)          handleGetNodeRules(req);
      else if (path.endsWith("/rules") &&  isPost)          handlePostNodeRules(req);
      else if (path.endsWith("/relay") && isPost)           handlePostNodeRelay(req);
      else if (path.endsWith("/name")  && isPost)           handlePostNodeName(req);
      else req->send(404, "application/json", "{\"error\":\"not found\"}");
    },
    nullptr,          // no file upload
    handleNodeBody    // JSON body accumulator for POST /rules
  );

  // -- Feature flags — lets the dashboard adapt without separate builds -------
  server.on("/api/features", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json",
      "{\"encryption\":"
#ifdef PKT_ENCRYPTION
      "true"
#else
      "false"
#endif
      "}");
  });

  // -- Reboot ----------------------------------------------------------------
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"status\":\"rebooting\"}");
    xTaskCreate(rebootTask, "reboot", 1024, nullptr, 1, nullptr);
  });

  // -- RFID provisioning (encrypted builds only) ----------------------------
#ifdef PKT_ENCRYPTION
  server.on("/api/provision",        HTTP_POST, handlePostProvision);
  server.on("/api/provision/status", HTTP_GET,  handleGetProvisionStatus);
#endif

  // -- WiFi config routes (scan, connect, status, etc.) ---------------------
  // Registers /api/info, /api/scan, /api/connect, /api/wifistatus, /api/disconnect, /api/forget.
  wifiRegisterRoutes(server);

  // Static files from LittleFS (dashboard SPA + wifi_config.html)
  // Serve static files from LittleFS.
  // ESPAsyncWebServer probes for filename.gz before filename on every
  // request. The "file not found" log lines for .gz are benign — the
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
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  // Catch-all: OPTIONS pre-flight + captive-portal redirect when AP is active
  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->method() == HTTP_OPTIONS) {
      AsyncWebServerResponse *resp = req->beginResponse(204);
      resp->addHeader("Access-Control-Allow-Origin",  "*");
      resp->addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
      resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
      req->send(resp);
    } else {
      // Redirect OS captive-portal probes to dashboard when AP on
      wifiHandleCatchAll(req);
    }
  });

  server.begin();
  logAsync("[WEB] Server started on port 80\n");
}