#include <Arduino.h>

#include <ArduinoOTA.h>	 // built-in
#include <IotWebConf.h>	 // https://github.com/prampec/IotWebConf
#include <MQTT.h>		 // https://github.com/256dpi/arduino-mqtt
#include <RemoteDebug.h> // https://github.com/JoaoLopesF/RemoteDebug

#include "LogStream.hpp"
#include "RinnaiSignalDecoder.hpp"
#include "RinnaiMQTTGateway.hpp"

// check (required) parameters passed from the ini
// create your own private_config.ini with the data. See private_config_template.ini
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
#ifndef TX_OUT_RINNAI_PIN	 // the exit fo the proxy, data from the local mcu with optional changes
#define TX_OUT_RINNAI_PIN -1 // default is a read only mode without overriding commands
#endif

// hardcoded settings (consider to move to separate config or to the ini)
// mqtt
const int MQTT_PACKET_MAX_SIZE = 700; // the config message is rather large, keep enough space
// wifi manager - // max configuration paramter length
const int WIFI_CONFIG_PARAM_MAX_LEN = 128;
// wifi manager - Configuration specific key. The value should be modified if config structure was changed.
const char WIFI_CONFIG_VERSION[] = "mqt1";
// wifi manager - Status indicator pin.
//      First it will light up (kept LOW), on Wifi connection it will blink,
//      when connected to the Wifi it will turn off (kept HIGH).
const int WIFI_STATUS_PIN = LED_BUILTIN;

// main objects
DNSServer dnsServer;
WebServer server(80);
IotWebConf iotWebConf(HOST_NAME, &dnsServer, &server, WIFI_INITIAL_AP_PASSWORD, WIFI_CONFIG_VERSION);
WiFiClient net;
MQTTClient mqttClient(MQTT_PACKET_MAX_SIZE);
RinnaiSignalDecoder rxDecoder(RX_RINNAI_PIN, INVALID_PIN, RX_INVERT);
RinnaiSignalDecoder txDecoder(TX_IN_RINNAI_PIN, TX_OUT_RINNAI_PIN, TX_IN_INVERT);
RinnaiMQTTGateway rinnaiMqttGateway(HA_DEVICE_NAME, rxDecoder, txDecoder, mqttClient, MQTT_TOPIC, TEST_PIN);
RemoteDebug remoteDebug;

// state
boolean needReset = false;
boolean needOTAConnect = false;
boolean needMqttConnect = false;
unsigned long lastMqttConnectionAttempt = 0;

char mqttServerValue[WIFI_CONFIG_PARAM_MAX_LEN];
char mqttUserNameValue[WIFI_CONFIG_PARAM_MAX_LEN];
char mqttUserPasswordValue[WIFI_CONFIG_PARAM_MAX_LEN];

IotWebConfParameter mqttServerParam = IotWebConfParameter("MQTT server", "mqttServer", mqttServerValue, WIFI_CONFIG_PARAM_MAX_LEN);
IotWebConfParameter mqttUserNameParam = IotWebConfParameter("MQTT user", "mqttUser", mqttUserNameValue, WIFI_CONFIG_PARAM_MAX_LEN);
IotWebConfParameter mqttUserPasswordParam = IotWebConfParameter("MQTT password", "mqttPass", mqttUserPasswordValue, WIFI_CONFIG_PARAM_MAX_LEN, "password");

// declarations
void handleRoot();
void setupWifiManager();
void setupOTA();
void setupRemoteDebug();
void setupMqtt();
void connectWifi(const char *ssid, const char *password);
void wifiConnected();
void configSaved();
boolean formValidator();
boolean connectMqtt();
boolean connectMqttOptions();
void onMqttMessageReceived(String &topic, String &payload);

// code
void setup()
{
	Serial.begin(SERIAL_BAUD);
	logStream().println();
	logStream().println("Starting up...");

	bool retRx = rxDecoder.setup();
	logStream().printf("Finished setting up rx decoder, %d\n", retRx);
	bool retTx = txDecoder.setup();
	logStream().printf("Finished setting up tx decoder, %d\n", retTx);
	if (!retRx || !retTx)
	{
		for (;;)
			; // hang further execution
	}

	logStream().println("Setting up Wifi and Mqtt");
	setupWifiManager();
	setupMqtt();

	logStream().println("Ready.");
}

void setupWifiManager()
{
	// setup CONFIG pin ourselves otherwise pullup wasn't ready by the time iotWebConf tried to use it
	pinMode(WIFI_CONFIG_PIN, INPUT_PULLUP);
	delay(1);
	// -- Initializing the configuration.
	iotWebConf.setStatusPin(WIFI_STATUS_PIN);
	iotWebConf.setConfigPin(WIFI_CONFIG_PIN);
	iotWebConf.addParameter(&mqttServerParam);
	iotWebConf.addParameter(&mqttUserNameParam);
	iotWebConf.addParameter(&mqttUserPasswordParam);
	iotWebConf.setConfigSavedCallback(&configSaved);
	iotWebConf.setFormValidator(&formValidator);
	iotWebConf.setWifiConnectionCallback(&wifiConnected);
	iotWebConf.setWifiConnectionHandler(&connectWifi);
	iotWebConf.setApTimeoutMs(AP_MODE_TIMEOUT_MS); // try to shorten the time the device is in AP mode on boot

	// -- Initializing the configuration.
	boolean validConfig = iotWebConf.init();
	if (!validConfig)
	{
		mqttServerValue[0] = '\0';
		mqttUserNameValue[0] = '\0';
		mqttUserPasswordValue[0] = '\0';
	}

	// -- Set up required URL handlers on the web server.
	server.on("/", handleRoot);
	server.on("/config", [] { iotWebConf.handleConfig(); });
	server.onNotFound([]() { iotWebConf.handleNotFound(); });
}

// need to call once wifi is connected
void setupOTA()
{
	// allow pin
	if (OTA_ENABLE_PIN != -1)
	{
		pinMode(OTA_ENABLE_PIN, INPUT_PULLUP);
	}

	// Port defaults to 3232
	// ArduinoOTA.setPort(3232);

	// Hostname defaults to esp3232-[MAC]
	ArduinoOTA.setHostname(HOST_NAME);

	// No authentication by default
	ArduinoOTA.setPassword(OTA_PASSWORD);

	// Password can be set with it's md5 value as well
	// MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
	// ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

	ArduinoOTA.setMdnsEnabled(false); // we already have DNS from the wifi manager

	ArduinoOTA
		.onStart([]() {
			String type;
			if (ArduinoOTA.getCommand() == U_FLASH)
				type = "sketch";
			else // U_SPIFFS
				type = "filesystem";

			// NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
			logStream().println("Start updating " + type);
		})
		.onEnd([]() {
			logStream().println("\nEnd");
		})
		.onProgress([](unsigned int progress, unsigned int total) {
			logStream().printf("Progress: %u%%\r", (progress / (total / 100)));
		})
		.onError([](ota_error_t error) {
			logStream().printf("Error[%u]: ", error);
			if (error == OTA_AUTH_ERROR)
				logStream().println("Auth Failed");
			else if (error == OTA_BEGIN_ERROR)
				logStream().println("Begin Failed");
			else if (error == OTA_CONNECT_ERROR)
				logStream().println("Connect Failed");
			else if (error == OTA_RECEIVE_ERROR)
				logStream().println("Receive Failed");
			else if (error == OTA_END_ERROR)
				logStream().println("End Failed");
		});

	ArduinoOTA.begin();
}

// need to call once wifi is connected
void setupRemoteDebug()
{
	// Initialize RemoteDebug
	remoteDebug.begin(HOST_NAME); // Initialize the WiFi server. Can pass port but telnet port 23 is the default
	// remoteDebug.setResetCmdEnabled(true); // Enable the reset command
	// remoteDebug.showProfiler(true); // Profiler (Good to measure times, to optimize codes)
	remoteDebug.showColors(true); // Colors need ANSI supporting terminal
}

void setupMqtt()
{
	mqttClient.begin(mqttServerValue, net); // use default port = 1883
	mqttClient.onMessage(onMqttMessageReceived);
}

void loop()
{
	// -- doLoop should be called as frequently as possible.
	iotWebConf.doLoop();
	// OTA loop, allow only if a certain button pin is enabled
	if (OTA_ENABLE_PIN == -1 || digitalRead(OTA_ENABLE_PIN) == LOW)
	{
		ArduinoOTA.handle(); // ok to call if not initialized yet, does nothing
	}
	// RemoteDebug handle
	remoteDebug.handle();
	// MQTT loop
	mqttClient.loop();

	// need to setup OTA after wifi connection
	if (needOTAConnect)
	{
		setupRemoteDebug();
		setupOTA();
		needOTAConnect = false;
	}

	// need to setup mqtt after wifi connection
	if (needMqttConnect)
	{
		if (connectMqtt())
		{
			needMqttConnect = false;
		}
	}
	else if ((iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE) && (!mqttClient.connected())) // keep mqtt connection
	{
		logStream().println("MQTT reconnect");
		connectMqtt();
	}

	// need to reset after config change
	if (needReset)
	{
		logStream().println("Rebooting after 1 second.");
		iotWebConf.delay(1000);
		ESP.restart();
	}

	// mqtt and rinnai business logic
	rinnaiMqttGateway.loop();
	// see if others want to do some work
	yield();
}

/**
 * Handle web requests to "/" path.
 */
void handleRoot()
{
	// -- Let IotWebConf test and handle captive portal requests.
	if (iotWebConf.handleCaptivePortal())
	{
		// -- Captive portal request were already served.
		return;
	}
	String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
	s += "<title>Rinnai Wifi</title></head><body>";
	s += "Go to <a href='config'>configure page</a> to change settings.";
	s += "</body></html>\n";

	server.send(200, "text/html", s);
}

// callback to connect to wifi
void connectWifi(const char *ssid, const char *password)
{
	// Attempt to fix issue setting the DHCP hostname. Unfortunately this just seems to set the hostname to "espressif" possibly due to:
	// https://github.com/espressif/esp-idf/issues/4737
	// If have working solution, report at: https://github.com/prampec/IotWebConf/issues/40
	/*
	WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE); // https://github.com/espressif/arduino-esp32/issues/2537
	WiFi.setHostname(HOST_NAME);
	*/
	WiFi.begin(ssid, password);
}

// callback when wifi is connected
void wifiConnected()
{
	needOTAConnect = true;
	needMqttConnect = true;
}

void configSaved()
{
	logStream().println("Configuration was updated.");
	needReset = true;
}

boolean formValidator()
{
	logStream().println("Validating form.");
	boolean valid = true;

	int l = server.arg(mqttServerParam.getId()).length();
	if (l < 3)
	{
		mqttServerParam.errorMessage = "Please provide at least 3 characters!";
		valid = false;
	}

	return valid;
}

boolean connectMqtt()
{
	unsigned long now = millis();
	if (1000 > now - lastMqttConnectionAttempt)
	{
		// Do not repeat within 1 sec.
		return false;
	}
	logStream().println("Connecting to MQTT server...");
	if (!connectMqttOptions())
	{
		lastMqttConnectionAttempt = now;
		return false;
	}
	logStream().println("Connected!");

	rinnaiMqttGateway.onMqttConnected();
	return true;
}

boolean connectMqttOptions()
{
	boolean result;

	/*
	logStream().println("mqtt params:");
	logStream().println(iotWebConf.getThingName());
	logStream().println(mqttUserNameValue);
	logStream().println(mqttUserPasswordValue);
	*/

	if (mqttUserPasswordValue[0] != '\0')
	{
		result = mqttClient.connect(iotWebConf.getThingName(), mqttUserNameValue, mqttUserPasswordValue);
	}
	else if (mqttUserNameValue[0] != '\0')
	{
		result = mqttClient.connect(iotWebConf.getThingName(), mqttUserNameValue);
	}
	else
	{
		result = mqttClient.connect(iotWebConf.getThingName());
	}
	return result;
}

void onMqttMessageReceived(String &topic, String &payload)
{
	rinnaiMqttGateway.onMqttMessageReceived(topic, payload);

	// Note: Do not use the client in the callback to publish, subscribe or
	// unsubscribe as it may cause deadlocks when other things arrive while
	// sending and receiving acknowledgments. Instead, change a global variable,
	// or push to a queue and handle it in the loop after calling `client.loop()`.
}
