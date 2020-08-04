#include <ArduinoJson.h>

#include "LogStream.hpp"
#include "RinnaiMQTTGateway.hpp"

const char *DEVICE_NAME = "Rinnai Water Heater";
const int MQTT_REPORT_FORCED_FLUSH_INTERVAL_MS = 20000; // ms
const int STATE_JSON_MAX_SIZE = 300;
const int CONFIG_JSON_MAX_SIZE = 500;
const int MAX_OVERRIDE_PERIOD_FROM_ORIGINAL_MS = 500; // ms, only send override if there was an original message lately

RinnaiMQTTGateway::RinnaiMQTTGateway(RinnaiSignalDecoder &rxDecoder, RinnaiSignalDecoder &txDecoder, MQTTClient &mqttClient, String mqttTopic, byte testPin)
	: rxDecoder(rxDecoder), txDecoder(txDecoder), mqttClient(mqttClient), mqttTopic(mqttTopic), mqttTopicState(String(mqttTopic) + "/state"), testPin(testPin)
{
}

void RinnaiMQTTGateway::loop()
{
	// low level rinnai decoding monitoring
	// logStream().printf("rx errors: pulse %d, bit %d, packet %d\n", rxDecoder.getPulseHandlerErrorCounter(), rxDecoder.getBitTaskErrorCounter(), rxDecoder.getPacketTaskErrorCounter());
	/*
	logStream().printf("rx pulse: waiting %d, avail %d\n", uxQueueMessagesWaiting(rxDecoder.getPulseQueue()), uxQueueSpacesAvailable(rxDecoder.getPulseQueue()));
	static unsigned long lastPulseTime = 0;
	while (uxQueueMessagesWaiting(rxDecoder.getPulseQueue()))
	{
		PulseQueueItem item;
		BaseType_t ret = xQueueReceive(rxDecoder.getPulseQueue(), &item, 0); // pdTRUE=1 if an item was successfully received from the queue, otherwise pdFALSE.
		// hack delta
		unsigned long d = clockCyclesToMicroseconds(item.cycle - lastPulseTime);
		lastPulseTime = item.cycle;
		logStream().printf("rx p %d %d, q %d, r %d\n", item.newLevel, d, uxQueueMessagesWaiting(rxDecoder.getPulseQueue()), ret);
	}
	logStream().printf("rx bit: waiting %d, avail %d\n", uxQueueMessagesWaiting(rxDecoder.getBitQueue()), uxQueueSpacesAvailable(rxDecoder.getBitQueue()));
	while (uxQueueMessagesWaiting(rxDecoder.getBitQueue()))
	{
		BitQueueItem item;
		BaseType_t ret = xQueueReceive(rxDecoder.getBitQueue(), &item, 0); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
		logStream().printf("rx b %d %d %d, q %d, r %d\n", item.bit, item.startCycle, item.misc, uxQueueMessagesWaiting(rxDecoder.getBitQueue()), ret);
	}
	*/
	// logStream().printf("rx packet: waiting %d, avail %d\n", uxQueueMessagesWaiting(rxDecoder.getPacketQueue()), uxQueueSpacesAvailable(rxDecoder.getPacketQueue()));
	while (uxQueueMessagesWaiting(rxDecoder.getPacketQueue()))
	{
		PacketQueueItem item;
		BaseType_t ret = xQueueReceive(rxDecoder.getPacketQueue(), &item, 0); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
		if (handleIncomingPacketQueueItem(item, true) == false)
		{
			logStream().printf("rx pkt %d %02x%02x%02x %u %d %d %d, q %d, r %d\n", item.bitsPresent, item.data[0], item.data[1], item.data[2], item.startCycle, item.validPre, item.validParity, item.validChecksum, uxQueueMessagesWaiting(rxDecoder.getPacketQueue()), ret);
		}
	}

	// logStream().printf("tx errors: pulse %d, bit %d, packet %d\n", txDecoder.getPulseHandlerErrorCounter(), txDecoder.getBitTaskErrorCounter(), txDecoder.getPacketTaskErrorCounter());
	// logStream().printf("tx packet: waiting %d, avail %d\n", uxQueueMessagesWaiting(txDecoder.getPacketQueue()), uxQueueSpacesAvailable(txDecoder.getPacketQueue()));
	while (uxQueueMessagesWaiting(txDecoder.getPacketQueue()))
	{
		PacketQueueItem item;
		BaseType_t ret = xQueueReceive(txDecoder.getPacketQueue(), &item, 0); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
		if (handleIncomingPacketQueueItem(item, false) == false)
		{
			logStream().printf("tx pkt %d %02x%02x%02x %u %d %d %d, q %d, r %d\n", item.bitsPresent, item.data[0], item.data[1], item.data[2], item.startCycle, item.validPre, item.validParity, item.validChecksum, uxQueueMessagesWaiting(rxDecoder.getPacketQueue()), ret);
		}
	}

	// MQTT payload generation and flushing
	// render payload
	DynamicJsonDocument doc(STATE_JSON_MAX_SIZE);
	doc["testPin"] = digitalRead(testPin) == LOW ? "ON" : "OFF";
	if (heaterPacketCounter)
	{
		doc["currentTemperature"] = lastHeaterPacketParsed.temperatureCelsius;
		doc["targetTemperature"] = targetTemperatureCelsius;
		doc["activeId"] = lastHeaterPacketParsed.activeId;
		doc["mode"] = lastHeaterPacketParsed.on ? "heating" : "off";
		doc["action"] = lastHeaterPacketParsed.inUse ? "heating" : (lastHeaterPacketParsed.on ? "idle" : "off");
		doc["heaterBytes"] = RinnaiProtocolDecoder::renderPacket(lastHeaterPacketBytes);
	}
	if (localControlPacketCounter)
	{
		doc["controlId"] = lastLocalControlPacketParsed.myId;
		doc["controlBytes"] = RinnaiProtocolDecoder::renderPacket(lastLocalControlPacketBytes);
	}
	String payload;
	serializeJson(doc, payload);
	// check if to send
	unsigned long now = millis();
	if (mqttClient.connected() && (now - lastMqttReportMillis > MQTT_REPORT_FORCED_FLUSH_INTERVAL_MS || payload != lastMqttReportPayload))
	{
		logStream().printf("Sending on MQTT channel '%s': %s\n", mqttTopicState.c_str(), payload.c_str());
		bool ret = mqttClient.publish(mqttTopicState, payload);
		if (!ret)
		{
			logStream().println("Error publishing a state MQTT message");
		}
		lastMqttReportMillis = now;
		lastMqttReportPayload = payload;
	}

	// delay to not over flood the serial interface
	// delay(100);
}

bool RinnaiMQTTGateway::handleIncomingPacketQueueItem(const PacketQueueItem &item, bool remote)
{
	if (!item.validPre || !item.validParity || !item.validChecksum)
	{
		return false;
	}
	RinnaiPacketSource source = RinnaiProtocolDecoder::getPacketSource(item.data, RinnaiSignalDecoder::BYTES_IN_PACKET);
	if (source == INVALID || source == UNKNOWN)
	{
		return false;
	}
	if (source == HEATER && remote)
	{
		RinnaiHeaterPacket packet;
		bool ret = RinnaiProtocolDecoder::decodeHeaterPacket(item.data, packet);
		if (!ret)
		{
			return false;
		}
		if (debugLevel == PARSED)
		{
			logStream().printf("Heater packet: a=%d o=%d u=%d t=%d\n", packet.activeId, packet.on, packet.inUse, packet.temperatureCelsius);
		}
		memcpy(&lastHeaterPacketParsed, &packet, sizeof(RinnaiHeaterPacket));
		memcpy(lastHeaterPacketBytes, item.data, RinnaiProtocolDecoder::BYTES_IN_PACKET);
		heaterPacketCounter++;
		lastHeaterPacketMillis = millis(); // not, this is not cycle/us accurate timing info, this is rough ms level timing
		// init target temperature once we have reports from the heater
		if (targetTemperatureCelsius == -1)
		{
			targetTemperatureCelsius = lastHeaterPacketParsed.temperatureCelsius; 
		}
		// act on temperature info
		handleTemperatureSync();
	}
	else if (source == CONTROL)
	{
		RinnaiControlPacket packet;
		bool ret = RinnaiProtocolDecoder::decodeControlPacket(item.data, packet);
		if (!ret)
		{
			return false;
		}
		if (debugLevel == PARSED)
		{
			logStream().printf("Control packet: r=%d i=%d o=%d p=%d td=%d tu=%d\n", remote, packet.myId, packet.onOffPressed, packet.priorityPressed, packet.temperatureDownPressed, packet.temperatureUpPressed);
		}
		if (remote)
		{
			remoteControlPacketCounter++;
			lastRemoteControlPacketMillis = millis();
		}
		else
		{
			memcpy(&lastLocalControlPacketParsed, &packet, sizeof(RinnaiControlPacket));
			memcpy(lastLocalControlPacketBytes, item.data, RinnaiProtocolDecoder::BYTES_IN_PACKET);
			localControlPacketCounter++;
			lastLocalControlPacketMillis = millis();
		}
	}
	else
	{
		return false;
	}
	return true;
}

void RinnaiMQTTGateway::handleTemperatureSync()
{
	if (heaterPacketCounter && localControlPacketCounter && targetTemperatureCelsius != -1 &&
		lastHeaterPacketParsed.temperatureCelsius != targetTemperatureCelsius && millis() - lastHeaterPacketMillis < MAX_OVERRIDE_PERIOD_FROM_ORIGINAL_MS)
	{
		override(lastHeaterPacketParsed.temperatureCelsius < targetTemperatureCelsius ? TEMPERATURE_UP : TEMPERATURE_DOWN);
	}
}

bool RinnaiMQTTGateway::override(OverrideCommand command)
{
	// check if state is valid for sending
	unsigned long originalControlPacketAge = millis() - lastLocalControlPacketMillis;
	if (originalControlPacketAge > MAX_OVERRIDE_PERIOD_FROM_ORIGINAL_MS) // if we have no recent original packet. can happen because we send too many overrides or because no panel signal is available
	{
		logStream().printf("No fresh original data for override command %d, age %lu\n", command, originalControlPacketAge);
		return false;
	}
	// logStream().printf("Attempting override command %d, age %d\n", command, originalControlPacketAge);
	// prep buffer
	byte buf[RinnaiSignalDecoder::BYTES_IN_PACKET];
	memcpy(buf, lastLocalControlPacketBytes, RinnaiSignalDecoder::BYTES_IN_PACKET);
	switch (command)
	{
	case ON_OFF:
		RinnaiProtocolDecoder::setOnOffPressed(buf);
		break;
	case PRIORITY:
		RinnaiProtocolDecoder::setPriorityPressed(buf);
		break;
	case TEMPERATURE_UP:
		RinnaiProtocolDecoder::setTemperatureUpPressed(buf);
		break;
	case TEMPERATURE_DOWN:
		RinnaiProtocolDecoder::setTemperatureDownPressed(buf);
		break;
	default:
		logStream().println("Unknown command for override");
		return false;
	}
	bool overRet = txDecoder.setOverridePacket(buf, RinnaiSignalDecoder::BYTES_IN_PACKET);
	if (overRet == false)
	{
		logStream().println("Error setting override");
		return false;
	}
	return true;
}

void RinnaiMQTTGateway::onMqttMessageReceived(String &fullTopic, String &payload)
{
	// parse topic
	String topic;
	int index = fullTopic.lastIndexOf('/');
	if (index != -1)
	{
		topic = fullTopic.substring(index + 1);
	}
	else
	{
		topic = fullTopic;
	}

	// ignore what we send
	if (topic == "config" || topic == "state")
	{
		return;
	}

	// log
	logStream().printf("Incoming: %s %s - %s\n", fullTopic.c_str(), topic.c_str(), payload.c_str());

	// handle command
	if (topic == "temp")
	{
		// parse and verify targetTemperature
		int temp = atoi(payload.c_str());
		temp = min(temp, (int)RinnaiProtocolDecoder::TEMP_C_MAX);
		temp = max(temp, (int)RinnaiProtocolDecoder::TEMP_C_MIN);
		logStream().printf("Setting %d as target temperature\n", temp);
		targetTemperatureCelsius = temp;
	}
	else if (topic == "mode")
	{
		if ((payload == "off" && lastHeaterPacketParsed.on) || (payload == "heat" && !lastHeaterPacketParsed.on))
		{
			override(ON_OFF);
		}
	}
	else if (topic == "override")
	{
		override(PRIORITY);
	}
	else if (topic == "logLevel")
	{
		if (payload == "NONE")
		{
			debugLevel = NONE;
		}
		else if (payload == "PARSED")
		{
			debugLevel = PARSED;
		}
		else if (payload == "RAW")
		{
			debugLevel = RAW;
		}
	}
	else if (topic == "logDestination")
	{
		if (payload == "TELNET")
		{
			logStream().println("Telnet log set");
			logStream.SetLogStreamTelnet();
		}
		else
		{
			logStream().println("Serial log set");
			logStream.SetLogStreamSerial();
		}
	}
	else
	{
		logStream().printf("Unknown topic: %s\n", topic.c_str());
	}
}

void RinnaiMQTTGateway::onMqttConnected()
{
	// subscribe
	bool ret = mqttClient.subscribe(mqttTopic + "/#");
	if (!ret)
	{
		logStream().println("Error doing a MQTT subscribe");
	}

	// send a '/config' topic to achieve MQTT discovery - https://www.home-assistant.io/docs/mqtt/discovery/
	DynamicJsonDocument doc(CONFIG_JSON_MAX_SIZE);
	doc["~"] = mqttTopic;
	doc["name"] = DEVICE_NAME;
	doc["action_topic"] = "~/state";
	doc["action_template"] = "{{ value_json.action }}";
	doc["current_temperature_topic"] = "~/state";
	doc["current_temperature_template"] = "{{ value_json.currentTemperature }}";
	doc["max_temp"] = RinnaiProtocolDecoder::TEMP_C_MAX;
	doc["min_temp"] = RinnaiProtocolDecoder::TEMP_C_MIN;
	doc["initial"] = RinnaiProtocolDecoder::TEMP_C_MIN;
	doc["mode_command_topic"] = "~/mode";
	doc["mode_state_topic"] = "~/state";
	doc["mode_state_template"] = "{{ value_json.mode }}";
	doc["modes"][0] = "off";
	doc["modes"][1] = "heat";
	doc["precision"] = 1;
	doc["temperature_command_topic"] = "~/temp";
	doc["temperature_unit"] = "C";
	doc["temperature_state_topic"] = "~/state";
	doc["temperature_state_template"] = "{{ value_json.targetTemperature }}";
	String payload;
	serializeJson(doc, payload);
	logStream().printf("Sending on MQTT channel '%s/config': %d bytes, %s\n", mqttTopic.c_str(), payload.length(), payload.c_str());
	ret = mqttClient.publish(mqttTopic + "/config", payload);
	if (!ret)
	{
		logStream().println("Error publishing a config MQTT message");
	}
}
