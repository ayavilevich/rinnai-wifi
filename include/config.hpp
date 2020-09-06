#pragma once

// check (required) parameters passed from the ini
// create your own private_config.ini with the data. See private_config.template.ini
#ifndef SERIAL_BAUD
#error Need to pass SERIAL_BAUD
#endif
// Name of the device in homa-assistant
#ifndef HA_DEVICE_NAME
#define HA_DEVICE_NAME "Rinnai Water Heater"
#endif
// Initial name of the Thing. Used e.g. as SSID of the own Access Point.
#ifndef HOST_NAME // there could be esp-idf bugs setting the DHCP hostname, it will be empty or "espressif", wait for fixes.
#define HOST_NAME "rinnai-wifi"
#endif
// Initial password to connect to the Thing, when it creates an own Access Point.
#ifndef WIFI_INITIAL_AP_PASSWORD
#define WIFI_INITIAL_AP_PASSWORD "rinnairinnai" // must be over 8 characters
#endif
// OTA password
#ifndef OTA_PASSWORD
#error Need to define ota_password / OTA_PASSWORD
#endif
// Thing will stay in AP mode for an amount of time on boot, before retrying to connect to a WiFi network.
#ifndef AP_MODE_TIMEOUT_MS
#define AP_MODE_TIMEOUT_MS 5000
#endif
// Restrict OTA updates based on state of a pin
#ifndef OTA_ENABLE_PIN
#define OTA_ENABLE_PIN -1 // if set to !=-1, drive this pin low to allow OTA updates
#endif
// MQTT topic prefix
#ifndef MQTT_TOPIC
#define MQTT_TOPIC "homeassistant/climate/rinnai"
#endif
// When WIFI_CONFIG_PIN is pulled to ground on startup, the Thing will use the initial password to build an AP. (E.g. in case of lost password)
#ifndef WIFI_CONFIG_PIN
#error Need to define WIFI_CONFIG_PIN
#endif
// Pin whose state to send over mqtt
#ifndef TEST_PIN
#error Need to define TEST_PIN
#endif
// Rinnai rx and tx pins
#ifndef RX_RINNAI_PIN // pin carrying data from the outside (heater, other control panels, etc)
#error Need to define RX_RINNAI_PIN
#endif
#ifndef RX_INVERT // "true" if we need to invert the incoming signal, set when an inverting mosfet is used to level shift the signal from 5v to 3.3V
#define RX_INVERT false
#endif
#ifndef TX_IN_RINNAI_PIN // pin carrying data from the local control panel mcu
#error Need to define TX_IN_RINNAI_PIN
#endif
#ifndef TX_IN_INVERT // "true" if we need to invert the incoming signal, set when an inverting mosfet is used to level shift the signal from 5v to 3.3V
#define TX_IN_INVERT false
#endif
#ifndef TX_OUT_INVERT // "true" if we need to invert the outgoing signal, set when an inverting mosfet is used to level shift the signal from 3.3V to 5V
#define TX_OUT_INVERT false
#endif
#ifndef TX_OUT_RINNAI_PIN	 // the exit of the proxy, data from the local mcu with optional changes
#define TX_OUT_RINNAI_PIN -1 // default is a read only mode without overriding commands
#endif
