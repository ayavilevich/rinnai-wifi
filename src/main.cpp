#include <Arduino.h>

#include <ArduinoOTA.h>
#include <IotWebConf.h>

// hardcoded settings (TODO, move to separate config or to the ini)
// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char thingName[] = "rinnai-wifi";
// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "rinnairinnai"; // must be over 8 characters
// OTA password
const char OTAPassword[] = "rinnairinnai";

// main objects
DNSServer dnsServer;
WebServer server(80);

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword);

// state
boolean needPostWifiConnect = false;

// declarations
void handleRoot();
void setupWifiManager();
void setupOTA();
void wifiConnected();

// code
void setup()
{
	Serial.begin(115200);
	Serial.println();
	Serial.println("Starting up...");

	setupWifiManager();

	Serial.println("Ready.");
}

void setupWifiManager()
{
	// -- Initializing the configuration.
	iotWebConf.setWifiConnectionCallback(&wifiConnected);
	iotWebConf.init();
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

	ArduinoOTA.setMdnsEnabled(false); // we already have DNS from the wifi manager

	// Password can be set with it's md5 value as well
	// MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
	// ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

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

void loop()
{
	// -- doLoop should be called as frequently as possible.
	iotWebConf.doLoop();
	// OTA loop, consider to call only if a certain button pin is enabled
	ArduinoOTA.handle(); // ok to call if not initialized yet, does nothing

	if (needPostWifiConnect)
	{
		setupOTA();
		needPostWifiConnect = false;
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
	needPostWifiConnect = true;
}
