#include <Arduino.h>

#include <ArduinoOTA.h> // built-in
#include <IotWebConf.h> // https://github.com/prampec/IotWebConf
#include <MQTT.h>		// https://github.com/256dpi/arduino-mqtt

#include "RinnaiSignalDecoder.hpp"
#include "RinnaiMQTTGateway.hpp"

// confirm required parameters passed from the ini
#ifndef SERIAL_BAUD
#error Need to pass SERIAL_BAUD
#endif

// hardcoded settings (TODO, move to separate config or to the ini)
// Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char THING_NAME[] = "rinnai-wifi";
// Initial password to connect to the Thing, when it creates an own Access Point.
const char WIFI_INITIAL_AP_PASSWORD[] = "rinnairinnai"; // must be over 8 characters
// OTA password
const char OTA_PASSWORD[] = "rinnairinnai";
// Thing will stay in AP mode for an amount of time on boot, before retrying to connect to a WiFi network.
const int AP_MODE_TIMEOUT_MS = 5000;

// MQTT topic prefix
const char MQTT_TOPIC[] = "homeassistant/climate/rinnai";
const int MQTT_PACKET_MAX_SIZE = 700; // the config message is rather large, keep enough space

// max configuration paramter length
#define CONFIG_PARAM_MAX_LEN 128
// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "mqt1"
// -- When CONFIG_PIN is pulled to ground on startup, the Thing will use the initial
//      password to build an AP. (E.g. in case of lost password)
#define CONFIG_PIN 4
// Pin whose state to send over mqtt
#define TEST_PIN 0
// -- Status indicator pin.
//      First it will light up (kept LOW), on Wifi connection it will blink,
//      when connected to the Wifi it will turn off (kept HIGH).
#define STATUS_PIN LED_BUILTIN
#define RX_RINNAI_PIN 25
#define TX_IN_RINNAI_PIN 26
#define TX_OUT_RINNAI_PIN 12

// main objects
DNSServer dnsServer;
WebServer server(80);
IotWebConf iotWebConf(THING_NAME, &dnsServer, &server, WIFI_INITIAL_AP_PASSWORD, CONFIG_VERSION);
WiFiClient net;
MQTTClient mqttClient(MQTT_PACKET_MAX_SIZE);
RinnaiSignalDecoder rxDecoder(RX_RINNAI_PIN);
RinnaiSignalDecoder txDecoder(TX_IN_RINNAI_PIN, TX_OUT_RINNAI_PIN);
RinnaiMQTTGateway rinnaiMqttGateway(rxDecoder, txDecoder, mqttClient, MQTT_TOPIC, TEST_PIN);

// state
boolean needReset = false;
boolean needOTAConnect = false;
boolean needMqttConnect = false;
unsigned long lastMqttConnectionAttempt = 0;

char mqttServerValue[CONFIG_PARAM_MAX_LEN];
char mqttUserNameValue[CONFIG_PARAM_MAX_LEN];
char mqttUserPasswordValue[CONFIG_PARAM_MAX_LEN];

IotWebConfParameter mqttServerParam = IotWebConfParameter("MQTT server", "mqttServer", mqttServerValue, CONFIG_PARAM_MAX_LEN);
IotWebConfParameter mqttUserNameParam = IotWebConfParameter("MQTT user", "mqttUser", mqttUserNameValue, CONFIG_PARAM_MAX_LEN);
IotWebConfParameter mqttUserPasswordParam = IotWebConfParameter("MQTT password", "mqttPass", mqttUserPasswordValue, CONFIG_PARAM_MAX_LEN, "password");

// declarations
void handleRoot();
void setupWifiManager();
void setupOTA();
void setupMqtt();
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
	Serial.println();
	Serial.println("Starting up...");

	bool retRx = rxDecoder.setup();
	Serial.printf("Finished setting up rx decoder, %d\n", retRx);
	bool retTx = txDecoder.setup();
	Serial.printf("Finished setting up tx decoder, %d\n", retTx);
	if (!retRx || !retTx)
	{
		for (;;)
			; // hang further execution
	}

	Serial.println("Setting up Wifi and Mqtt");
	setupWifiManager();
	setupMqtt();

	Serial.println("Ready.");
}

void setupWifiManager()
{
	//Serial.printf("Config pin: %d\n", digitalRead(CONFIG_PIN));
	// setup CONFIG pin ourselves otherwise pullup wasn't ready by the time iotWebConf tried to use it
	pinMode(CONFIG_PIN, INPUT_PULLUP);
	delay(1);
	// -- Initializing the configuration.
	iotWebConf.setStatusPin(STATUS_PIN);
	iotWebConf.setConfigPin(CONFIG_PIN);
	iotWebConf.addParameter(&mqttServerParam);
	iotWebConf.addParameter(&mqttUserNameParam);
	iotWebConf.addParameter(&mqttUserPasswordParam);
	iotWebConf.setConfigSavedCallback(&configSaved);
	iotWebConf.setFormValidator(&formValidator);
	iotWebConf.setWifiConnectionCallback(&wifiConnected);
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
	// Port defaults to 3232
	// ArduinoOTA.setPort(3232);

	// Hostname defaults to esp3232-[MAC]
	ArduinoOTA.setHostname(THING_NAME);

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
			Serial.println("Start updating " + type);
		})
		.onEnd([]() {
			Serial.println("\nEnd");
		})
		.onProgress([](unsigned int progress, unsigned int total) {
			Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
		})
		.onError([](ota_error_t error) {
			Serial.printf("Error[%u]: ", error);
			if (error == OTA_AUTH_ERROR)
				Serial.println("Auth Failed");
			else if (error == OTA_BEGIN_ERROR)
				Serial.println("Begin Failed");
			else if (error == OTA_CONNECT_ERROR)
				Serial.println("Connect Failed");
			else if (error == OTA_RECEIVE_ERROR)
				Serial.println("Receive Failed");
			else if (error == OTA_END_ERROR)
				Serial.println("End Failed");
		});

	ArduinoOTA.begin();
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
	// OTA loop, consider to call only if a certain button pin is enabled
	ArduinoOTA.handle(); // ok to call if not initialized yet, does nothing
	// MQTT loop
	mqttClient.loop();

	// need to setup OTA after wifi connection
	if (needOTAConnect)
	{
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
		Serial.println("MQTT reconnect");
		connectMqtt();
	}

	// need to reset after config change
	if (needReset)
	{
		Serial.println("Rebooting after 1 second.");
		iotWebConf.delay(1000);
		ESP.restart();
	}

	// mqtt and rinnai business logic
	rinnaiMqttGateway.loop();
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

// callback when wifi is connected
void wifiConnected()
{
	needOTAConnect = true;
	needMqttConnect = true;
}

void configSaved()
{
	Serial.println("Configuration was updated.");
	needReset = true;
}

boolean formValidator()
{
	Serial.println("Validating form.");
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
	Serial.println("Connecting to MQTT server...");
	if (!connectMqttOptions())
	{
		lastMqttConnectionAttempt = now;
		return false;
	}
	Serial.println("Connected!");

	rinnaiMqttGateway.onMqttConnected();
	return true;
}

boolean connectMqttOptions()
{
	boolean result;

	/*
	Serial.println("mqtt params:");
	Serial.println(iotWebConf.getThingName());
	Serial.println(mqttUserNameValue);
	Serial.println(mqttUserPasswordValue);
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
