/**
 * main.cpp
 *
 * Unified entry point for the Power Telemetry firmware.
 * Compiles as either a Gateway or a Sensor Node depending on the
 * build flag defined in platformio.ini.
 *
 * -- Usage ---------------------------------------------------------------------
 *
 *   In platformio.ini, set exactly ONE of:
 *
 *     build_flags = -D NODE_GATEWAY
 *     build_flags = -D NODE_TELEMETRY
 *
 *   If BOTH are defined, the build resolves to NODE_GATEWAY (with a warning).
 *   If NEITHER is defined, the build fails with a descriptive #error.
 *
 * -- platformio.ini example ----------------------------------------------------
 *
 *   [env:gateway]
 *   platform  = espressif32
 *   board     = esp32dev
 *   framework = arduino
 *   build_flags =
 *       -D NODE_GATEWAY
 *       -D ARDUINO_USB_CDC_ON_BOOT=0
 *   board_build.filesystem = littlefs
 *   lib_deps =
 *       jgromes/RadioLib @ ^6.0.0
 *       me-no-dev/ESPAsyncWebServer
 *       me-no-dev/AsyncTCP
 *       bblanchon/ArduinoJson @ ^7.0.0
 *
 *   [env:node]
 *   platform  = espressif32
 *   board     = esp32dev
 *   framework = arduino
 *   build_flags =
 *       -D NODE_TELEMETRY
 *       -D ARDUINO_USB_CDC_ON_BOOT=0
 *   lib_deps =
 *       jgromes/RadioLib @ ^6.0.0
 *       olehs/PZEM004Tv30
 *
 * -- Hardware ------------------------------------------------------------------
 *
 *   SX1278 (VSPI - identical on both roles):
 *     NSS  -> GPIO5    SCK  -> GPIO21
 *     MISO -> GPIO19   MOSI -> GPIO18
 *     DIO0 -> GPIO14   RST  -> GPIO13
 *     DIO1 -> GPIO27 (tie to GND if unused)
 *
 *   Gateway only:
 *     WiFi + LittleFS (index.html + wifi_config.html in data/ folder)
 *     Dashboard: http://telemeter.local  or  http://192.168.4.1
 *
 *   Node only:
 *     PZEM-004T v3  -> Serial2  PZEM_RX=GPIO22, PZEM_TX=GPIO23
 *     Relay signal  -> GPIO25 (active HIGH)
 *     LED green     -> GPIO32
 *     LED red       -> GPIO33
 */

// --- Macro resolution ---------------------------------------------------------
#if defined(NODE_GATEWAY) && defined(NODE_TELEMETRY)
  // Both defined - resolve to gateway with a compile-time warning.
  // #warning is a GCC extension supported by the ESP32 toolchain.
  #warning "Both NODE_GATEWAY and NODE_TELEMETRY are defined - building as NODE_GATEWAY."
  #undef NODE_TELEMETRY
#elif !defined(NODE_GATEWAY) && !defined(NODE_TELEMETRY)
  #error "No build role defined. Set exactly one of: -D NODE_GATEWAY  or  -D NODE_TELEMETRY"
#endif
// After resolution: exactly one of NODE_GATEWAY or NODE_TELEMETRY is defined.

// --- Common includes (both roles) --------------------------------------------
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "tdma_protocol.h"
#include "log_async.h"

// --- Role-specific includes ---------------------------------------------------
#ifdef NODE_GATEWAY
  #include <WiFi.h>
  #include <ESPmDNS.h>
  #include <LittleFS.h>
  #include <Wire.h>
  #include "gateway_state.h"
  #include "gateway_tdma_task.h"
  #include "gateway_web.h"
  #include "gateway_wifi_config.h"
  #include "fram_store.h"
#endif
#ifdef NODE_TELEMETRY
  #ifndef PZEM_FAKE
  #include <PZEM004Tv30.h>
  #endif
  #include "node_tdma_task.h"
#endif

#ifdef PKT_ENCRYPTION
  #include "crypto.h"
  #include "rfid_provision.h"
#endif

// --- Pin definitions (shared) ------------------------------------------------
#ifdef BOARD_S3_SUPERMINI
  // ESP32-S3 Super Mini — THT pins only (GPIO 1-13, TX=43, RX=44)
  #define LORA_PIN_NSS    4
  #define LORA_PIN_MOSI   5
  #define LORA_PIN_MISO   6
  #define LORA_PIN_SCK    7
  #define LORA_PIN_DIO0   8
  #define LORA_PIN_RST    9
  #define LORA_PIN_DIO1   RADIOLIB_NC
#else
  #define LORA_PIN_NSS    5
  #define LORA_PIN_MOSI   18
  #define LORA_PIN_MISO   19
  #define LORA_PIN_SCK    21
  #define LORA_PIN_DIO0   14
  #define LORA_PIN_RST    13
  #define LORA_PIN_DIO1   27   // Tie to GND if unused
#endif

// --- Role-specific pin definitions -------------------------------------------
#ifdef NODE_GATEWAY
  #define GW_BTN_PIN 34   // "The button" — active LOW, external pull-up required
                          //   short press (< 3 s): reboot
                          //   long  press (≥ 3 s): forget STA credentials + reboot
#endif

#ifdef NODE_TELEMETRY
  #ifdef BOARD_S3_SUPERMINI
    #define PZEM_TX_PIN     43   // ESP32-S3 physical TX -> PZEM RX
    #define PZEM_RX_PIN     44   // ESP32-S3 physical RX <- PZEM TX
    #define RELAY_PIN_      3    // active HIGH; GPIO3 is JTAG strapping — LOW at boot is safe
    #define LED_GREEN_PIN_  11
    #define LED_RED_PIN_    12
    #define NODE_BTN_PIN    13
  #else
    #define PZEM_RX_PIN     22   // PZEM RX  -> ESP32 TX2
    #define PZEM_TX_PIN     23   // PZEM TX  <- ESP32 RX2
    #define RELAY_PIN_      25   // Relay signal (active HIGH)
    #define LED_GREEN_PIN_  32   // Two-color LED - green channel
    #define LED_RED_PIN_    33   // Two-color LED - red channel
    #define NODE_BTN_PIN    34   // "The button" — active LOW, external pull-up required
                                 //   short press (< 3 s): reboot (forces re-contend)
                                 //   long  press (≥ 3 s): clear encryption key + reboot
  #endif

  // Exported - referenced by node_tdma_task.cpp
  uint8_t RELAY_PIN     = RELAY_PIN_;
  uint8_t LED_GREEN_PIN = LED_GREEN_PIN_;
  uint8_t LED_RED_PIN   = LED_RED_PIN_;
  uint8_t DIO0_PIN      = LORA_PIN_DIO0;
#endif

// --- Hardware instances -------------------------------------------------------
// Radio instance - extern-referenced by both gateway_tdma_task.cpp and node_tdma_task.cpp
SX1278 radio = new Module(LORA_PIN_NSS, LORA_PIN_DIO0, LORA_PIN_RST, LORA_PIN_DIO1);

#ifdef NODE_TELEMETRY
  #ifndef PZEM_FAKE
  // PZEM-004T v3 on Serial2 - extern-referenced by node_tdma_task.cpp
  PZEM004Tv30 pzem(Serial2, PZEM_TX_PIN, PZEM_RX_PIN);
  #endif
#endif

// =============================================================================
// GATEWAY
// =============================================================================
#ifdef NODE_GATEWAY

// GPIO 34 is input-only on ESP32 — no internal pull-up; requires external resistor.
static void btnTask(void*) {
  pinMode(GW_BTN_PIN, INPUT);
  vTaskDelay(pdMS_TO_TICKS(500));  // skip transients during boot
  for (;;) {
    if (digitalRead(GW_BTN_PIN) == LOW) {
      uint32_t t0 = millis();
      while (digitalRead(GW_BTN_PIN) == LOW) {
        if (millis() - t0 >= 3100) break;  // long-press threshold reached
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      uint32_t held = millis() - t0;
      if (held < 80) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }  // debounce glitch

      if (held >= 3000) {
        logAsync("[GW] Button long-press — forgetting STA credentials, rebooting\n");
        wifiClearCredentials();
      } else {
        logAsync("[GW] Button press — rebooting\n");
      }
      vTaskDelay(pdMS_TO_TICKS(200));  // let log drain
      ESP.restart();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup() {
  Serial.begin(115200);
  logDrainTaskStart();
  delay(500);
  logAsync("\n===== Power Telemetry Gateway =====\n");

  // -- LittleFS --------------------------------------------------------------
  // Mounts the web SPA (index.html, app.js, etc.) served by the web task.
  // LittleFS.begin(true) formats on mount failure.
  // NVS (WiFi credentials) is unaffected by format.
  if (!LittleFS.begin(true)) {
    logAsync("[FS] LittleFS mount FAILED - dashboard SPA not available\n");
  } else {
    logAsync("[FS] LittleFS OK\n");
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    while (f) {
      logAsync("  /%-30s  %lu B\n", f.name(), (unsigned long)f.size());
      f = root.openNextFile();
    }
  }

  // -- FRAM (MB85RC256V, SDA=32, SCL=33, addr=0x50) -------------------------
  // framLoadAll() is called before the TDMA task starts so no mutex is needed.
  // framInit() internally calls Wire.begin(32, 33).
  if (!framInit(32, 33, 0x50)) {
    logAsync("[FRAM] Init FAILED - energy/history persistence disabled\n");
  } else {
    logAsync("[FRAM] OK\n");
    framLoadAll();
    framTaskStart();
  }

  // -- Packet encryption (encrypted builds only) ----------------------------
  // cryptoLoadKey() tries NVS; if missing, generate a fresh random key so the
  // gateway is self-contained out of the box.  rfidProvisionInit() prepares the
  // PN532 for web-triggered card writes (POST /api/provision).
  // Wire (SDA=32, SCL=33) is already up from framInit(); rfidProvisionInit()
  // calls Wire.begin() again with the same pins — safe and idempotent.
#ifdef PKT_ENCRYPTION
  if (!cryptoLoadKey()) {
    cryptoGenerateKey();
  }
  rfidProvisionInit();
  logAsync("[CRYPTO] Encrypted mode active\n");
#endif

  // -- WiFi - non-blocking state machine ------------------------------------
  // AP starts immediately (last-resort access route). STA attempted in
  // parallel if credentials are saved in NVS.
  //   AP on  -> while STA unavailable (no creds, connecting, or dropped)
  //   AP off -> only while STA actively connected
  // wifiConfigLoop() drives all transitions from loop().
  wifiConfigBegin();

  // -- SX1278 radio ----------------------------------------------------------
  SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_NSS);
  logAsync("[LORA] Initialising SX1278 ... ");
  int16_t loraState = radio.begin(
    LORA_CHANNELS[0],   // Ch 0 - beacon channel
    LORA_BANDWIDTH,
    LORA_SF,
    LORA_CR,
    LORA_SYNC_WORD,
    LORA_TX_POWER,
    LORA_PREAMBLE_LEN
  );
  if (loraState == RADIOLIB_ERR_NONE) {
    logAsync("OK\n");
  } else {
    logAsync("FAILED (err=%d) - check wiring!\n", loraState);
  }
  radio.setCRC(true);

  // -- Web server ------------------------------------------------------------
  webServerSetup();
  webBroadcastTaskStart();

  // -- TDMA task (Core 1, priority 2) ---------------------------------------
  gatewayTdmaTaskStart();
  xTaskCreate(btnTask, "BTN", 2048, nullptr, 1, nullptr);

  logAsync("[GW] Setup complete.\n");
}

void loop() {
  // Drive the WiFi state machine and DNS server.
  wifiConfigLoop();

  // Periodic 5 s "nodes" WebSocket push + ws.cleanupClients().
  webBroadcastAllNodes();

  delay(10);
}

#endif // NODE_GATEWAY

// =============================================================================
// SENSOR NODE
// =============================================================================
#ifdef NODE_TELEMETRY

// NODE_BTN_PIN is active LOW; external pull-up required (GPIO34 on ESP32 is input-only).
static void btnTask(void*) {
  pinMode(NODE_BTN_PIN, INPUT);
  vTaskDelay(pdMS_TO_TICKS(500));  // skip transients during boot
  for (;;) {
    if (digitalRead(NODE_BTN_PIN) == LOW) {
      uint32_t t0 = millis();
      while (digitalRead(NODE_BTN_PIN) == LOW) {
        if (millis() - t0 >= 3100) break;  // long-press threshold reached
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      uint32_t held = millis() - t0;
      if (held < 80) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }  // debounce glitch

      if (held >= 3000) {
        // Rapid red blink — visual confirmation before clearing key
        for (int i = 0; i < 6; i++) {
          digitalWrite(LED_RED_PIN_, i & 1 ? HIGH : LOW);
          vTaskDelay(pdMS_TO_TICKS(80));
        }
        logAsync("[NODE] Button long-press — clearing encryption key, rebooting\n");
#ifdef PKT_ENCRYPTION
        cryptoClearKey();
#endif
      } else {
        logAsync("[NODE] Button press — rebooting\n");
      }
      vTaskDelay(pdMS_TO_TICKS(200));  // let log drain
      ESP.restart();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup() {
  Serial.begin(115200);
  logDrainTaskStart();
  delay(500);
  Serial.println("\n===== Power Telemetry Node =====");

  // -- GPIO init -------------------------------------------------------------
#ifndef PZEM_FAKE
  pinMode(RELAY_PIN_,      OUTPUT);
  digitalWrite(RELAY_PIN_, LOW);       // Relay OFF at boot
#endif
  pinMode(LED_GREEN_PIN_, OUTPUT);
  digitalWrite(LED_GREEN_PIN_, HIGH);
  pinMode(LED_RED_PIN_,   OUTPUT);
  digitalWrite(LED_RED_PIN_,   HIGH);

  // -- Packet encryption (encrypted builds only) ----------------------------
  // On first boot (no NVS key) the node blocks here waiting for the operator
  // to tap a pre-written MIFARE card to the PN532 (SDA=PN532_SDA, SCL=PN532_SCL).
  // The red LED blinks while waiting.  After a successful read the node reboots
  // so setup() runs cleanly with the key resident in NVS.
#ifdef PKT_ENCRYPTION
  if (!cryptoLoadKey()) {
    Serial.println("[NODE] NOT PROVISIONED - tap provisioned RFID card to PN532");
    rfidProvisionInit();
    uint8_t rfidKey[16];
    while (true) {
      // Red blink: ON
      digitalWrite(LED_RED_PIN_, LOW);
      if (rfidProvisionRead(rfidKey)) {
        digitalWrite(LED_RED_PIN_, HIGH);
        // Key was already stored by rfidProvisionRead() via cryptoSetKey()
        Serial.println("[NODE] Key loaded from RFID card - rebooting...");
        delay(200);
        ESP.restart();
      }
      // Red blink: OFF
      digitalWrite(LED_RED_PIN_, HIGH);
      delay(400);
    }
  }
  Serial.println("[CRYPTO] Encrypted mode active");
#endif

  // -- SX1278 radio ----------------------------------------------------------
  SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_NSS);
#ifdef BOARD_S3_SUPERMINI
  // SPI.begin() resets GPIO11/12 (default FSPI pins) to INPUT, clobbering the pinMode above.
  pinMode(LED_GREEN_PIN_, OUTPUT);
  pinMode(LED_RED_PIN_,   OUTPUT);
#endif

  // 3 fast green blinks - boot indicator
  for (int i = 0; i < 6; i++) {
    digitalWrite(LED_GREEN_PIN_, i & 1 ? HIGH : LOW);
    delay(120);
  }
  digitalWrite(LED_GREEN_PIN_, HIGH);
  Serial.print("[LORA] Initialising SX1278 ... ");
  int16_t loraState = radio.begin(
    LORA_CHANNELS[0],   // Ch 0 - listen for beacon
    LORA_BANDWIDTH,
    LORA_SF,
    LORA_CR,
    LORA_SYNC_WORD,
    LORA_TX_POWER,
    LORA_PREAMBLE_LEN
  );
  if (loraState == RADIOLIB_ERR_NONE) {
    Serial.println("OK");
  } else {
    Serial.printf("FAILED (err=%d)\n", loraState);
  }
  radio.setCRC(true);

  // -- PZEM sanity check -----------------------------------------------------
#ifdef PZEM_FAKE
  Serial.println("[PZEM-FAKE] Net-test mode — skipping PZEM hardware check");
#else
  Serial.print("[PZEM] Checking connection ... ");
  delay(200);
  float testV = pzem.voltage();
  if (!isnan(testV)) {
    Serial.printf("OK (%.1f V)\n", testV);
  } else {
    Serial.println("No response - check wiring (TX/RX swapped?)");
    // pzemTask will keep retrying - non-fatal
  }
#endif

  // -- Launch FreeRTOS tasks -------------------------------------------------
  nodeTdmaTaskStart();
  xTaskCreate(btnTask, "BTN", 2048, nullptr, 1, nullptr);

  Serial.println("[NODE] Setup complete - waiting for beacon.\n");
}

void loop() {
  // All work is in FreeRTOS tasks on Core 1 (TDMA) and Core 0 (PZEM, sched, nudge).
  // loop() runs as the idle context - just yield.
  vTaskDelay(pdMS_TO_TICKS(1000));
}

#endif // NODE_TELEMETRY
