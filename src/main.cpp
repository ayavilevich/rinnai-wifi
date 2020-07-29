#include <Arduino.h>

#include <ArduinoOTA.h> // built-in
#include <IotWebConf.h> // https://github.com/prampec/IotWebConf
#include <MQTT.h>		// https://github.com/256dpi/arduino-mqtt

// hardcoded settings (TODO, move to separate config or to the ini)
// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char thingName[] = "rinnai-wifi";
// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "rinnairinnai"; // must be over 8 characters
// OTA password
const char OTAPassword[] = "rinnairinnai";
// MQTT topic prefix
const char mqttTopic[] = "homeassistant/climate/rinnai";

// max configuration paramter length
#define CONFIG_PARAM_MAX_LEN 128
// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "mqt1"
// -- When CONFIG_PIN is pulled to ground on startup, the Thing will use the initial
//      password to build an AP. (E.g. in case of lost password)
#define CONFIG_PIN 4
// Pin whose state to send over mqtt
#define MQTT_PIN 0
// -- Status indicator pin.
//      First it will light up (kept LOW), on Wifi connection it will blink,
//      when connected to the Wifi it will turn off (kept HIGH).
#define STATUS_PIN LED_BUILTIN

// main objects
DNSServer dnsServer;
WebServer server(80);
IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
WiFiClient net;
MQTTClient mqttClient;

// state
boolean needReset = false;
boolean needOTAConnect = false;
boolean needMqttConnect = false;
int pinState = HIGH;
unsigned long lastReport = 0;
unsigned long lastMqttConnectionAttempt = 0;

char mqttServerValue[CONFIG_PARAM_MAX_LEN];
char mqttUserNameValue[CONFIG_PARAM_MAX_LEN];
char mqttUserPasswordValue[CONFIG_PARAM_MAX_LEN];

IotWebConfParameter mqttServerParam = IotWebConfParameter("MQTT server", "mqttServer", mqttServerValue, CONFIG_PARAM_MAX_LEN);
IotWebConfParameter mqttUserNameParam = IotWebConfParameter("MQTT user", "mqttUser", mqttUserNameValue, CONFIG_PARAM_MAX_LEN);
IotWebConfParameter mqttUserPasswordParam = IotWebConfParameter("MQTT password", "mqttPass", mqttUserPasswordValue, CONFIG_PARAM_MAX_LEN, "password");

String mqttTopicState(String(mqttTopic) + "/status");
String mqttTopicSubscribe(String(mqttTopic) + "/#");

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
void mqttMessageReceived(String &topic, String &payload);

// code
void setup()
{
	Serial.begin(115200);
	Serial.println();
	Serial.println("Starting up...");

	setupWifiManager();
	setupMqtt();

	Serial.println("Ready.");
}

void setupWifiManager()
{
	// -- Initializing the configuration.
	iotWebConf.setStatusPin(STATUS_PIN);
	iotWebConf.setConfigPin(CONFIG_PIN);
	iotWebConf.addParameter(&mqttServerParam);
	iotWebConf.addParameter(&mqttUserNameParam);
	iotWebConf.addParameter(&mqttUserPasswordParam);
	iotWebConf.setConfigSavedCallback(&configSaved);
	iotWebConf.setFormValidator(&formValidator);
	iotWebConf.setWifiConnectionCallback(&wifiConnected);

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
	ArduinoOTA.setHostname(thingName);

	// No authentication by default
	ArduinoOTA.setPassword(OTAPassword);

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
	mqttClient.onMessage(mqttMessageReceived);
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

	// send pin status if changed
	unsigned long now = millis();
	if ((500 < now - lastReport) && (pinState != digitalRead(MQTT_PIN)))
	{
		pinState = 1 - pinState; // invert pin state as it is changed
		lastReport = now;
		Serial.print("Sending on MQTT channel '" + mqttTopicState + "' :");
		Serial.println(pinState == LOW ? "ON" : "OFF");
		mqttClient.publish(mqttTopicState, pinState == LOW ? "ON" : "OFF");
	}
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

	mqttClient.subscribe(mqttTopicSubscribe);

	// TODO, send a '/config' topic to achieve MQTT discovery - https://www.home-assistant.io/docs/mqtt/discovery/
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

void mqttMessageReceived(String &topic, String &payload)
{
	Serial.println("Incoming: " + topic + " - " + payload);

	// Note: Do not use the client in the callback to publish, subscribe or
	// unsubscribe as it may cause deadlocks when other things arrive while
	// sending and receiving acknowledgments. Instead, change a global variable,
	// or push to a queue and handle it in the loop after calling `client.loop()`.
}
