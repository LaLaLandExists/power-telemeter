/**
 * gateway_web.h
 * ESPAsyncWebServer + WebSocket interface for the power telemetry gateway.
 * All REST endpoints and WS push events are handled here.
 */
#pragma once
#include <Arduino.h>

/**
 * Initialize web server and register all REST + WebSocket handlers.
 * Node labels are loaded from FRAM via framLoadAll() in main.cpp before
 * this is called -- no file I/O happens here.
 */
void webServerSetup();

/**
 * Start the WebSocket broadcast drain task (Core 0, priority 1).
 * Must be called once from setup() after webServerSetup().
 */
void webBroadcastTaskStart();

/**
 * Enqueue a telemetry broadcast for slotIdx.
 * Non-blocking -- safe to call from the TDMA task on Core 1.
 * The actual JSON serialisation and ws.textAll() happen on Core 0.
 *
 * @param slotIdx  0-based slot index into g_nodes[]
 */
void webBroadcastTelemetry(uint8_t slotIdx);

/**
 * Push a "nodes" event to all WS clients (full node list).
 * Called every 5 s from the gateway loop (Core 0) and on WS connect.
 */
void webBroadcastAllNodes();

