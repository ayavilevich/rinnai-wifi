#include <ArduinoJson.h>

#include "RinnaiMQTTGateway.hpp"
#include "StreamPrintf.hpp"

const byte OVERRIDE_TEST_DATA[RinnaiSignalDecoder::BYTES_IN_PACKET] = {0x02, 0x00, 0x00, 0x7f, 0xbf, 0xc2};
const int MQTT_REPORT_FORCED_FLUSH_INTERVAL_MS = 20000; // ms
const int STATE_JSON_MAX_SIZE = 300;

RinnaiMQTTGateway::RinnaiMQTTGateway(RinnaiSignalDecoder &rxDecoder, RinnaiSignalDecoder &txDecoder, MQTTClient &mqttClient, String mqttTopicState, byte testPin)
	: rxDecoder(rxDecoder), txDecoder(txDecoder), mqttClient(mqttClient), mqttTopicState(mqttTopicState), testPin(testPin)
{
}

void RinnaiMQTTGateway::loop()
{
	// low level rinnai decoding monitoring
	// StreamPrintf(Serial, "rx errors: pulse %d, bit %d, packet %d\n", rxDecoder.getPulseHandlerErrorCounter(), rxDecoder.getBitTaskErrorCounter(), rxDecoder.getPacketTaskErrorCounter());
	/*
	StreamPrintf(Serial, "rx pulse: waiting %d, avail %d\n", uxQueueMessagesWaiting(rxDecoder.getPulseQueue()), uxQueueSpacesAvailable(rxDecoder.getPulseQueue()));
	static unsigned long lastPulseTime = 0;
	while (uxQueueMessagesWaiting(rxDecoder.getPulseQueue()))
	{
		PulseQueueItem item;
		BaseType_t ret = xQueueReceive(rxDecoder.getPulseQueue(), &item, 0); // pdTRUE=1 if an item was successfully received from the queue, otherwise pdFALSE.
		// hack delta
		unsigned long d = clockCyclesToMicroseconds(item.cycle - lastPulseTime);
		lastPulseTime = item.cycle;
		StreamPrintf(Serial, "rx p %d %d, q %d, r %d\n", item.newLevel, d, uxQueueMessagesWaiting(rxDecoder.getPulseQueue()), ret);
	}
	StreamPrintf(Serial, "rx bit: waiting %d, avail %d\n", uxQueueMessagesWaiting(rxDecoder.getBitQueue()), uxQueueSpacesAvailable(rxDecoder.getBitQueue()));
	while (uxQueueMessagesWaiting(rxDecoder.getBitQueue()))
	{
		BitQueueItem item;
		BaseType_t ret = xQueueReceive(rxDecoder.getBitQueue(), &item, 0); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
		StreamPrintf(Serial, "rx b %d %d %d, q %d, r %d\n", item.bit, item.startCycle, item.misc, uxQueueMessagesWaiting(rxDecoder.getBitQueue()), ret);
	}
	*/
	// StreamPrintf(Serial, "rx packet: waiting %d, avail %d\n", uxQueueMessagesWaiting(rxDecoder.getPacketQueue()), uxQueueSpacesAvailable(rxDecoder.getPacketQueue()));
	while (uxQueueMessagesWaiting(rxDecoder.getPacketQueue()))
	{
		PacketQueueItem item;
		BaseType_t ret = xQueueReceive(rxDecoder.getPacketQueue(), &item, 0); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
		if (handleIncomingPacketQueueItem(item, true) == false)
		{
			StreamPrintf(Serial, "rx pkt %d %02x%02x%02x %u %d %d %d, q %d, r %d\n", item.bitsPresent, item.data[0], item.data[1], item.data[2], item.startCycle, item.validPre, item.validParity, item.validChecksum, uxQueueMessagesWaiting(rxDecoder.getPacketQueue()), ret);
		}
	}

	// StreamPrintf(Serial, "tx errors: pulse %d, bit %d, packet %d\n", txDecoder.getPulseHandlerErrorCounter(), txDecoder.getBitTaskErrorCounter(), txDecoder.getPacketTaskErrorCounter());
	// StreamPrintf(Serial, "tx packet: waiting %d, avail %d\n", uxQueueMessagesWaiting(txDecoder.getPacketQueue()), uxQueueSpacesAvailable(txDecoder.getPacketQueue()));
	while (uxQueueMessagesWaiting(txDecoder.getPacketQueue()))
	{
		PacketQueueItem item;
		BaseType_t ret = xQueueReceive(txDecoder.getPacketQueue(), &item, 0); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
		if (handleIncomingPacketQueueItem(item, false) == false)
		{
			StreamPrintf(Serial, "tx pkt %d %02x%02x%02x %u %d %d %d, q %d, r %d\n", item.bitsPresent, item.data[0], item.data[1], item.data[2], item.startCycle, item.validPre, item.validParity, item.validChecksum, uxQueueMessagesWaiting(rxDecoder.getPacketQueue()), ret);
		}
	}

	// MQTT payload generation and flushing
	// render payload
	DynamicJsonDocument doc(STATE_JSON_MAX_SIZE);
	doc["testPin"] = digitalRead(testPin) == LOW ? "ON" : "OFF";
	if (heaterPacketCounter)
	{
		doc["temperature"] = lastHeaterPacketParsed.temperatureCelsius;
		doc["activeId"] = lastHeaterPacketParsed.activeId;
		doc["inUse"] = lastHeaterPacketParsed.inUse;
		doc["on"] = lastHeaterPacketParsed.on;
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
		StreamPrintf(Serial, "Sending on MQTT channel '%s': %s\n", mqttTopicState.c_str(), payload.c_str());
		bool ret = mqttClient.publish(mqttTopicState, payload);
		if (!ret)
		{
			Serial.println("Error publishing a MQTT message");
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
			StreamPrintf(Serial, "Heater packet: a=%d o=%d u=%d t=%d\n", packet.activeId, packet.on, packet.inUse, packet.temperatureCelsius);
		}
		memcpy(&lastHeaterPacketParsed, &packet, sizeof(RinnaiHeaterPacket));
		memcpy(lastHeaterPacketBytes, item.data, RinnaiProtocolDecoder::BYTES_IN_PACKET);
		heaterPacketCounter++;
		lastHeaterPacketMillis = millis(); // not, this is not cycle/us accurate timing info, this is rough ms level timing
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
			StreamPrintf(Serial, "Control packet: r=%d i=%d o=%d p=%d td=%d tu=%d\n", remote, packet.myId, packet.onOffPressed, packet.priorityPressed, packet.temperatureDownPressed, packet.temperatureUpPressed);
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

void RinnaiMQTTGateway::mqttMessageReceived(String &fullTopic, String &payload)
{
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

	StreamPrintf(Serial, "Incoming: %s %s - %s\n", fullTopic.c_str(), topic.c_str(), payload.c_str());

	if (topic == "status")
	{
		// ignore
	}
	else if (topic == "override")
	{
		bool overRet = txDecoder.setOverridePacket(OVERRIDE_TEST_DATA, RinnaiSignalDecoder::BYTES_IN_PACKET);
		if (overRet == false)
		{
			Serial.println("Error setting override");
		}
	}
	else if (topic == "debug")
	{
		if (debugLevel == NONE)
		{
			debugLevel = PARSED;
		}
		else
		{
			debugLevel = NONE;
		}
	}
	else
	{
		StreamPrintf(Serial, "Unknown topic: %s\n", topic.c_str());
	}
}
