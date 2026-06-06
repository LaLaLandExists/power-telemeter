# Power Telemetry Gateway — Web API Reference

**Server:** `http://telemeter.local` (mDNS, STA mode) or `http://192.168.4.1` (AP fallback)  
**Port:** 80  
**Content-Type:** all responses are `application/json`  
**CORS:** `Access-Control-Allow-Origin: *` on all responses; pre-flight `OPTIONS` handled automatically.

---

## Table of Contents

1. [Authentication](#1-authentication)
2. [REST — Dashboard](#2-rest--dashboard)
3. [REST — WiFi Configuration](#3-rest--wifi-configuration)
4. [REST — Encrypted Builds Only](#4-rest--encrypted-builds-only-pkt_encryption)
5. [WebSocket `/ws`](#5-websocket-ws)
   - [Connection and auth handshake](#51-connection-and-auth-handshake)
   - [Client commands](#52-client--server-commands)
   - [Server push events](#53-server--client-events)
6. [Data Models](#6-data-models)
7. [Error Responses](#7-error-responses)

---

## 1. Authentication

Authentication is optional. It activates only when a dashboard password has been set via `POST /api/dashsecure`.

### Auth-free mode (default)

When no password is set, every REST endpoint and every WebSocket connection is open. The WebSocket auto-authenticates on connect and immediately sends `{"type":"auth","ok":true}`.

### Password-protected mode

1. Client calls `POST /api/login` with the password to receive a 32-hex-character session token.
2. Every subsequent **REST** request must include the header:
   ```
   X-Auth-Token: <token>
   ```
3. Every subsequent **WebSocket** session must send the `auth` command as the first message:
   ```json
   { "cmd": "auth", "token": "<token>" }
   ```
   The server replies `{"type":"auth","ok":true}` and allows further commands, or closes the connection on failure.
4. A single token is shared across all REST and WS sessions. Calling `POST /api/login` again invalidates and regenerates the token.
5. `GET /api/logout` invalidates the token and de-authenticates all active WS clients.

---

## 2. REST — Dashboard

### `GET /api/nodes`

Returns summary state for all currently registered nodes.

**Auth required:** yes (when password is set)

**Response 200:**
```json
{
  "nodes": [ <NodeSummary>, ... ],
  "count": 2,
  "timeSet": true,
  "time": "14:30:00"
}
```

---

### `GET /api/node/{id}/live`

Returns full detail state for a single node.

**Path param:** `id` — node slot ID, 1–8.

**Auth required:** yes

**Response 200:** `<NodeDetail>` object (see [Data Models](#6-data-models)).

**Response 400:** `{"error":"invalid id"}`  
**Response 404:** `{"error":"not found"}`

---

### `GET /api/node/{id}/history`

Returns the circular history buffer for a node (up to 120 points, one per superframe ~3 s apart).

**Auth required:** yes

**Response 200:**
```json
[
  { "t": 123456, "v": 220.4, "i": 2.345, "p": 516.3 },
  ...
]
```

| Field | Type | Description |
|-------|------|-------------|
| `t` | uint32 | `millis()` timestamp at gateway when packet was received |
| `v` | float | Voltage (V) |
| `i` | float | Current (A) |
| `p` | float | Power (W) |

Points are returned in chronological order. An empty array `[]` is returned when the node has no history.

**Response 404:** `[]`

---

### `POST /api/node/{id}/relay`

Immediately set a node's relay state. Prefer the WebSocket `relay_manual` command; this HTTP endpoint is provided for curl/debug use.

**Auth required:** yes

**Body** (form-encoded):

| Field | Values | Description |
|-------|--------|-------------|
| `state` | `0` or `1` | Desired relay state |

**Response 200:**
```json
{ "success": true }
```
`success` is `false` if the node is not registered or the command queue is full.

---

### `POST /api/node/{id}/name`

Rename a node. Also queues a FRAM write and broadcasts a `name_changed` event to all WebSocket clients.

**Auth required:** yes

**Body** (form-encoded):

| Field | Constraints | Description |
|-------|-------------|-------------|
| `name` | 1–29 characters | New label |

**Response 200:** `{"ok":true}`  
**Response 400:** `{"error":"missing name"}` or `{"error":"name length 1-29"}`  
**Response 404:** `{"error":"not found"}`

---

### `POST /api/time`

Set the gateway clock. When NTP is active (STA mode, synced), the NTP time is authoritative and this endpoint returns the current NTP time without modifying it.

**Auth required:** yes

**Body** (form-encoded):

| Field | Type | Description |
|-------|------|-------------|
| `hour` | 0–23 | |
| `minute` | 0–59 | |
| `second` | 0–59 | |

**Response 200 (NTP active):**
```json
{ "ok": true, "source": "ntp", "time": "14:30:00" }
```

**Response 200 (AP / NTP not synced):**
```json
{ "ok": true }
```
Also broadcasts `{"type":"time_set","timeSet":true,"time":"HH:MM:SS","source":"client"}` to all WS clients.

---

### `GET /api/status`

Returns gateway health and configuration summary.

**Auth required:** yes

**Response 200:**
```json
{
  "uptime": 3600,
  "freeHeap": 182432,
  "wifiRSSI": -62,
  "ip": "192.168.1.42",
  "loraFreq": 433.05,
  "nodeCount": 3,
  "maxNodes": 8,
  "wsClients": 2,
  "timeSet": true,
  "time": "14:30:00",
  "ntpSynced": true,
  "apActive": false,
  "version": "2",
  "wifiMode": "sta",
  "ssid": "HomeRouter"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `uptime` | uint32 | Seconds since boot |
| `freeHeap` | uint32 | Free heap bytes |
| `wifiRSSI` | int | STA RSSI dBm; 0 when disconnected |
| `ip` | string | STA IP or `"192.168.4.1"` in AP mode |
| `loraFreq` | float | LoRa Ch 0 frequency (MHz) |
| `nodeCount` | int | Currently registered nodes |
| `maxNodes` | int | Maximum supported nodes (always 8) |
| `wsClients` | int | Active WebSocket connections |
| `timeSet` | bool | Whether the gateway clock has been set |
| `time` | string | Current gateway time `"HH:MM:SS"` |
| `ntpSynced` | bool | Whether time source is NTP |
| `apActive` | bool | Whether the soft-AP is currently running |
| `version` | string | Firmware version string |
| `wifiMode` | string | `"ap"` or `"sta"` |
| `ssid` | string | AP SSID (AP mode) or connected network (STA mode) |

---

### `GET /api/features`

Returns which optional build-time features are compiled in. The dashboard uses this to conditionally show UI elements.

**Auth required:** yes

**Response 200:**
```json
{ "encryption": false, "transport-test": false }
```

| Field | Description |
|-------|-------------|
| `encryption` | `true` in `*_enc` environments; enables RFID provisioning UI |
| `transport-test` | `true` in `TRANSPORT_TEST` builds; enables bulk transfer test UI |

---

### `POST /api/reboot`

Triggers a gateway reboot after a 300 ms flush delay so the HTTP response can be sent.

**Auth required:** yes

**Response 200:**
```json
{ "status": "rebooting" }
```

---

### `POST /api/login`

Validate a password and obtain a session token. Always public (no auth required).

**Body** (form-encoded):

| Field | Description |
|-------|-------------|
| `password` | Dashboard password |

**Response 200 (success):**
```json
{ "ok": true, "token": "a1b2c3d4e5f6..." }
```

**Response 401 (wrong password):**
```json
{ "ok": false }
```

When no password is set, any value (including empty) is accepted and a token is issued.

---

### `GET /api/logout`

Invalidates the current session token and de-authenticates all active WebSocket clients. Always public.

**Response 200:**
```json
{ "ok": true }
```

---

### `GET /api/authstatus`

Indicates whether the dashboard requires a password. Used by the SPA on load to decide whether to show the login screen. Always public.

**Response 200:**
```json
{ "hasPassword": false }
```

---

### `GET /api/dashsecure`

Returns current password state.

**Auth required:** yes

**Response 200:**
```json
{ "hasPassword": true }
```

---

### `POST /api/dashsecure`

Set or clear the dashboard password. Invalidates any existing session token on success.

**Auth required:** yes

**Body** (form-encoded):

| Field | Constraints | Description |
|-------|-------------|-------------|
| `password` | Empty to clear; min 8 chars if non-empty | New password |

**Response 200:** `{"ok":true}`  
**Response 400:** `{"ok":false,"message":"Minimum 8 characters"}`

---

## 3. REST — WiFi Configuration

### `GET /api/info`

Returns AP and STA connection state.

**Response 200:**
```json
{
  "version": "2",
  "apSsid": "PowerTelemeter_1A2B",
  "apIp": "192.168.4.1",
  "apActive": true,
  "staConnected": false
}
```

When `staConnected` is `true`, additional fields `staSSID` and `staIP` are present.

---

### `GET /api/scan`

Triggers an async WiFi scan on first call; subsequent calls poll for results. The 300 ms/channel dwell improves reliability in AP+STA shared-radio mode. Retries up to 2 times on empty results; gives up after 3 consecutive scan failures.

**While scanning — Response 202:**
```json
{ "scanning": true, "networks": [] }
```

**When complete — Response 200:**
```json
{
  "scanning": false,
  "networks": [
    { "ssid": "HomeRouter", "rssi": -62, "secure": true },
    ...
  ]
}
```

---

### `POST /api/connect`

Save credentials and initiate a STA connection. The AP is kept active during the connection attempt so the device remains reachable if it fails. Poll `GET /api/wifistatus` to track progress.

**Body** (form-encoded):

| Field | Description |
|-------|-------------|
| `ssid` | Target network SSID (required, non-empty) |
| `password` | WPA2 passphrase (optional, empty for open networks) |

**Response 200:**
```json
{ "status": "connecting" }
```

**Response 400:**
```json
{ "status": "error", "message": "SSID cannot be empty" }
```

---

### `GET /api/wifistatus`

Returns real-time STA connection state. Poll this after `POST /api/connect`.

**Response 200:**
```json
{
  "apActive": true,
  "connecting": true,
  "connected": false
}
```

When `connected` is `true`, additional fields `ip`, `ssid`, and `rssi` are present:
```json
{
  "apActive": false,
  "connecting": false,
  "connected": true,
  "ip": "192.168.1.42",
  "ssid": "HomeRouter",
  "rssi": -58
}
```

---

### `GET /api/disconnect`

Drop the STA connection without clearing credentials. Restores the AP.

**Response 200:** `{"ok":true}`

---

### `GET /api/forget`

Clear saved STA credentials and drop the current connection. Restores the AP.

**Response 200:** `{"ok":true}`

---

### `GET /api/staticip`

Returns the current static IP configuration.

**Response 200 (DHCP):**
```json
{ "enabled": false, "ip": "", "gateway": "", "subnet": "", "dns": "" }
```

**Response 200 (static IP configured):**
```json
{
  "enabled": true,
  "ip": "192.168.1.50",
  "gateway": "192.168.1.1",
  "subnet": "255.255.255.0",
  "dns": "8.8.8.8"
}
```

---

### `POST /api/staticip`

Set a static IP. Takes effect on the next `POST /api/connect`. DNS defaults to the gateway address if omitted.

**Body** (form-encoded):

| Field | Required | Description |
|-------|----------|-------------|
| `ip` | yes | Static IP address |
| `gateway` | yes | Default gateway |
| `subnet` | yes | Subnet mask |
| `dns` | no | DNS server (defaults to gateway) |

**Response 200:** `{"ok":true}`  
**Response 400:** `{"ok":false,"message":"Invalid IP, gateway, or subnet"}`

---

### `GET /api/staticip/clear`

Revert to DHCP by removing static IP entries from NVS. Takes effect on next reconnect.

**Response 200:** `{"ok":true}`

---

### `GET /api/ap`

Returns the soft-AP SSID and whether a password is set.

**Response 200:**
```json
{ "ssid": "PowerTelemeter_1A2B", "hasPassword": false }
```

---

### `POST /api/ap`

Set or clear the soft-AP password. Restarts the AP immediately so the new password takes effect without a reboot.

**Body** (form-encoded):

| Field | Constraints | Description |
|-------|-------------|-------------|
| `password` | Empty to open; min 8 chars if non-empty | WPA2 passphrase |

**Response 200:** `{"ok":true}`  
**Response 400:** `{"ok":false,"message":"Minimum 8 characters"}`

---

## 4. REST — Encrypted Builds Only (`PKT_ENCRYPTION`)

These endpoints are registered only in `gateway_enc` / `*_enc` environments.

---

### `POST /api/provision`

Initiates a background MIFARE card write. The task blocks for up to 5 seconds waiting for a card tap on the PN532. Returns immediately with HTTP 202. Poll `/api/provision/status` for the result.

**Auth required:** yes

**Response 202:**
```json
{ "status": "pending", "message": "tap MIFARE card to PN532 within 5 seconds" }
```

**Response 409** (if a write is already in progress):
```json
{ "error": "already in progress" }
```

---

### `GET /api/provision/status`

Poll for the result of a `POST /api/provision` request.

**Auth required:** yes

**Response 200:**
```json
{ "status": "idle" }
```

| `status` value | Meaning |
|----------------|---------|
| `idle` | No provisioning in progress |
| `pending` | Waiting for card tap |
| `ok` | Key written successfully |
| `fail` | PN532 I2C error or card not detected in time |

---

### `POST /api/keygen`

Regenerates the AES-128 key and bumps the network epoch in one atomic sequence. The gateway:
1. Increments `networkEpoch` immediately (next beacon invalidates all registered nodes).
2. Waits one full superframe (~3 s) to ensure at least one beacon with the new epoch is transmitted.
3. Generates and stores the new key.

All registered nodes will de-register after receiving the epoch-incremented beacon and must be re-provisioned.

**Auth required:** yes

**Response 202:** `{"ok":true}`  
**Response 409:** `{"error":"already in progress"}`

---

## 5. WebSocket `/ws`

Connect with `ws://telemeter.local/ws` or `ws://192.168.4.1/ws`.

All messages are UTF-8 JSON text frames. Only complete, unfragmented text frames are processed; binary frames are ignored.

### 5.1 Connection and Auth Handshake

**No password set:**

On connect, the server automatically sends:
```json
{ "type": "auth", "ok": true }
```
The client can immediately send any command.

**Password set:**

The first message from the client must be:
```json
{ "cmd": "auth", "token": "<32-hex-token>" }
```

Server replies:
```json
{ "type": "auth", "ok": true }
```
or closes the connection:
```json
{ "type": "auth", "ok": false }
```

Any command sent before the `auth` exchange completes receives `{"type":"auth","ok":false}` and the connection is closed.

The server also pushes a periodic `nodes` snapshot every 5 seconds to all connected clients regardless of which commands have been sent.

---

### 5.2 Client → Server Commands

All commands share the base shape:
```json
{ "cmd": "<command>", "node": <nodeId>, ...args }
```

`node` is the slot ID (1–8). Omit it for commands that do not target a specific node.

---

#### `relay_manual`

Set a node relay immediately.

```json
{ "cmd": "relay_manual", "node": 3, "state": 1 }
```

| Field | Values | Description |
|-------|--------|-------------|
| `state` | `0` or `1` | Target relay state |

**Ack:** [`relay_ack`](#relay_ack)

---

#### `relay_schedule`

Set a daily recurring relay ON window. The relay turns ON at `startH:startM` and OFF at `endH:endM` each day. Schedules that cross midnight (e.g. 22:00–06:00) are supported.

```json
{
  "cmd": "relay_schedule",
  "node": 3,
  "startH": 8,
  "startM": 0,
  "endH": 17,
  "endM": 30
}
```

**Ack:** [`schedule_ack`](#schedule_ack)

---

#### `relay_clear`

Cancel the active schedule on a node and revert to manual mode.

```json
{ "cmd": "relay_clear", "node": 3 }
```

**Ack:** [`clear_ack`](#clear_ack)

---

#### `set_threshold`

Set the over-power alarm threshold on a node. The node asserts `alarmState=1` when instantaneous power exceeds this value.

```json
{ "cmd": "set_threshold", "node": 3, "watts": 1500 }
```

| Field | Range | Description |
|-------|-------|-------------|
| `watts` | 1–23000 | Threshold in whole watts |

**Ack:** [`threshold_ack`](#threshold_ack)

---

#### `nudge`

Blink the node's LEDs for physical device identification.

```json
{ "cmd": "nudge", "node": 3 }
```

**Ack:** [`nudge_ack`](#nudge_ack)

---

#### `rename`

Rename a node. Persisted to FRAM immediately and broadcast to all WS clients.

```json
{ "cmd": "rename", "node": 3, "name": "Air Conditioner" }
```

| Field | Constraints | Description |
|-------|-------------|-------------|
| `name` | 1–29 characters | New label |

**Broadcast:** [`name_changed`](#name_changed) to all clients (no per-sender ack).

---

#### `set_time`

Set the gateway clock and/or timezone offset.

```json
{
  "cmd": "set_time",
  "hour": 14,
  "minute": 30,
  "second": 0,
  "utcOffset": 28800
}
```

| Field | Type | Description |
|-------|------|-------------|
| `hour` | 0–23 | Used only when NTP is not active |
| `minute` | 0–59 | Used only when NTP is not active |
| `second` | 0–59 | Used only when NTP is not active |
| `utcOffset` | int32, seconds | Offset east of UTC (e.g. UTC+8 = 28800). Always applied and persisted regardless of NTP state |

When NTP is active: the clock is not modified; the server replies only to the requesting client with `{"type":"time_set","source":"ntp",...}`.

When NTP is not active: the clock is updated and `time_set` is broadcast to all clients with `"source":"client"`.

**Ack / Broadcast:** [`time_set`](#time_set)

---

#### `clear_energy`

Reset the accumulated energy counter for one node to zero. Not persisted to FRAM (resets the in-RAM counter only).

```json
{ "cmd": "clear_energy", "node": 3 }
```

**Ack:** [`energy_cleared`](#energy_cleared)

---

#### `clear_all_energy`

Reset accumulated energy counters for all registered nodes.

```json
{ "cmd": "clear_all_energy" }
```

**Ack:** [`all_energy_cleared`](#all_energy_cleared)

---

#### `get_nodes`

Request an immediate `nodes` snapshot. Equivalent to waiting for the 5-second periodic push.

```json
{ "cmd": "get_nodes" }
```

**Response:** [`nodes`](#nodes)

---

#### `transport_test` *(TRANSPORT_TEST builds only)*

Enqueue a GFSK bulk transfer session for protocol validation. Only one session can be active at a time.

```json
{
  "cmd": "transport_test",
  "node": 3,
  "scenario": "echo",
  "size": 0
}
```

| `scenario` | Payload | Description |
|------------|---------|-------------|
| `echo` | 16 bytes, `0xAB` fill | Loopback; gateway verifies CRC-32 of echoed payload |
| `boundary` | `size` bytes (1–4096), sequential `0x00–0xFF` | Tests arbitrary payload sizes |
| `pattern` | 4096 bytes, sequential | Maximum-size transfer |
| `pdr_measure` | 2000 bytes, `0xAA` fill | Counts successfully ACKed fragments to measure PDR |
| `broadcast_500` | 500 bytes, sequential | Broadcast scenario |

**Ack:** `{"type":"transport_test_ack","node":<id>,"scenario":"...","success":true,"size":<n>}`

On completion or failure, the server broadcasts [`bulk_complete`](#bulk_complete) or [`bulk_failed`](#bulk_failed).

---

### 5.3 Server → Client Events

---

#### `telemetry`

Pushed by the server immediately after each uplink packet is received from a node (every ~3 s per node). Contains full `NodeDetail`.

```json
{
  "type": "telemetry",
  "node": { <NodeDetail> },
  "timeSet": true,
  "time": "14:30:00"
}
```

---

#### `nodes`

Pushed every 5 seconds and on `get_nodes` command. Contains `NodeSummary` for all active nodes.

```json
{
  "type": "nodes",
  "nodes": [ <NodeSummary>, ... ],
  "count": 3,
  "timeSet": true,
  "time": "14:30:00",
  "ntpSynced": true
}
```

---

#### `name_changed`

Broadcast to all clients when a node is renamed (via WS `rename` or `POST /api/node/{id}/name`).

```json
{ "type": "name_changed", "node": 3, "name": "Air Conditioner" }
```

---

#### `time_set`

Sent in response to a `set_time` command.

```json
{
  "type": "time_set",
  "timeSet": true,
  "time": "14:30:00",
  "source": "client"
}
```

`source` is `"ntp"` when the clock is NTP-controlled (reply sent only to the requesting client); `"client"` when AP mode / NTP not synced (broadcast to all clients).

---

#### `relay_ack`

```json
{ "type": "relay_ack", "node": 3, "success": true }
```

`success` is `false` if the node is not registered or the DL command queue is full.

---

#### `schedule_ack`

```json
{ "type": "schedule_ack", "node": 3, "success": true }
```

---

#### `clear_ack`

```json
{ "type": "clear_ack", "node": 3, "success": true }
```

---

#### `threshold_ack`

```json
{ "type": "threshold_ack", "node": 3, "success": true }
```

---

#### `nudge_ack`

```json
{ "type": "nudge_ack", "node": 3, "success": true }
```

---

#### `energy_cleared`

```json
{ "type": "energy_cleared", "node": 3, "success": true }
```

---

#### `all_energy_cleared`

```json
{ "type": "all_energy_cleared", "node": 0, "success": true }
```

`node` is `0` (not node-specific).

---

#### `log`

A single serial log line relayed from the async ring buffer on the gateway. The dashboard may use this to display a live console.

```json
{ "type": "log", "line": "[GW-TDMA] Superframe 1234 | ch 3" }
```

---

#### `bulk_complete` *(TRANSPORT_TEST builds only)*

Pushed after a successful GFSK bulk transfer session.

```json
{
  "type": "bulk_complete",
  "node": 3,
  "dir": 0,
  "typeTag": 240,
  "totalLen": 16,
  "fragsSent": 1,
  "fragsAcked": 1,
  "durationMs": 280,
  "crc32ok": true
}
```

| Field | Description |
|-------|-------------|
| `dir` | `0` = DL (GW->Node), `1` = UL (Node->GW) |
| `typeTag` | Application type byte (`0xF0` = echo/loopback) |
| `crc32ok` | DL only — whether echoed payload CRC-32 matched |
| `crc32` | UL only — hex string of received CRC-32 |

When `TRANSPORT_TEST` is defined, additional PDR fields are present: `scenario`, `pdrTotal`, `pdrAcked`, `pdrPct`.

---

#### `bulk_failed` *(TRANSPORT_TEST builds only)*

Pushed after an aborted or timed-out bulk session. Same shape as `bulk_complete`.

---

## 6. Data Models

### NodeSummary

Returned by `GET /api/nodes` and the `nodes` WS event.

```json
{
  "id": 3,
  "label": "Air Conditioner",
  "online": true,
  "rssi": -62,
  "voltage": 220.4,
  "current": 2.345,
  "power": 516.3,
  "energy": 1024,
  "frequency": 60.0,
  "powerFactor": 0.98,
  "relayState": 1,
  "relayMode": 0,
  "schedState": 0,
  "alarmState": 0,
  "alarmThreshold": 2000,
  "age": 2,
  "pending": false,
  "hasSched": false,
  "classifier": {
    "supported": true,
    "pending": false,
    "classId": 0,
    "className": "Resistive",
    "confidence": 7,
    "transient": false
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `id` | int | Slot ID, 1–8 |
| `label` | string | Human-readable name (up to 29 chars) |
| `online` | bool | `true` if `hasData` and missed superframes < 8 |
| `rssi` | int | Last uplink RSSI (dBm) |
| `voltage` | float | AC voltage (V) |
| `current` | float | AC current (A) |
| `power` | float | Active power (W) |
| `energy` | uint32 | Accumulated energy (Wh) since last clear |
| `frequency` | float | AC frequency (Hz) |
| `powerFactor` | float | Power factor (0.00–1.00) |
| `relayState` | int | `0`=OFF, `1`=ON |
| `relayMode` | int | `0`=MANUAL, `1`=SCHEDULED |
| `schedState` | int | `0`=NONE, `1`=WAITING, `2`=ACTIVE |
| `alarmState` | int | `0`=OK, `1`=ALARM (power exceeded threshold) |
| `alarmThreshold` | int | Over-power threshold (W); `0` if not set |
| `age` | uint32 | Seconds since last successful uplink |
| `pending` | bool | `true` while a DL command is awaiting node ACK |
| `hasSched` | bool | `true` when `relayMode==1` and `schedState>0` |
| `classifier` | object | Load classifier result (see below) |

### Classifier sub-object

| Field | Type | Description |
|-------|------|-------------|
| `supported` | bool | `false` for PZEM / fake-meter builds (no AFE) |
| `pending` | bool | `true` while smoothing window is filling (~10 s after relay ON) |
| `classId` | int | 0=Resistive, 1=Capacitive, 2=Motor, 3=Fan, 4=SMPS, 5=Lighting |
| `className` | string | Human-readable class name |
| `confidence` | int | 0–7 confidence score from the classifier |
| `transient` | bool | `true` if the classification is from a transient window |

`pending` and `classId`/`className`/`confidence`/`transient` are mutually exclusive: `pending:true` omits the class fields.

---

### NodeDetail

Returned by `GET /api/node/{id}/live` and the `telemetry` WS event. Extends `NodeSummary` with schedule and clock fields.

Additional fields when `hasSched` is `true`:

| Field | Type | Description |
|-------|------|-------------|
| `schedStart` | string | Schedule start time `"HH:MM"` |
| `schedEnd` | string | Schedule end time `"HH:MM"` |

Additional fields always present:

| Field | Type | Description |
|-------|------|-------------|
| `timeSet` | bool | Whether the gateway clock has been set |
| `time` | string | Current gateway time `"HH:MM:SS"` |

---

## 7. Error Responses

| HTTP Status | Meaning |
|-------------|---------|
| 200 | Success |
| 202 | Accepted (async operation started) |
| 400 | Bad request (missing or invalid parameters) |
| 401 | Unauthorized (token missing, wrong, or expired) |
| 404 | Node not found |
| 409 | Conflict (operation already in progress) |

All error bodies follow:
```json
{ "error": "<description>" }
```

Unauthorized REST responses:
```json
{ "error": "unauthorized" }
```
