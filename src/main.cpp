#include <Arduino.h>

#include <IotWebConf.h>

// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char thingName[] = "rinnai-wifi";

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "rinnairinnai"; // must be over 8 characters

DNSServer dnsServer;
WebServer server(80);

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword);

// daclarations
void handleRoot();

// code
void setup()
{
	Serial.begin(115200);
	Serial.println();
	Serial.println("Starting up...");

	// -- Initializing the configuration.
	iotWebConf.init();

	// -- Set up required URL handlers on the web server.
	server.on("/", handleRoot);
	server.on("/config", [] { iotWebConf.handleConfig(); });
	server.onNotFound([]() { iotWebConf.handleNotFound(); });

	Serial.println("Ready.");
}

void loop()
{
	// -- doLoop should be called as frequently as possible.
	iotWebConf.doLoop();
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
