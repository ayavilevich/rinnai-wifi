#pragma once
#include <Arduino.h>

#include <MQTT.h>

#include "RinnaiSignalDecoder.hpp"
#include "RinnaiProtocolDecoder.hpp"

enum DebugLevel
{
	NONE,
	PARSED,
	RAW,
};

// this class will handle the logic of converting between MQTT commands and Rinnai packets
class RinnaiMQTTGateway
{
public:
	RinnaiMQTTGateway(RinnaiSignalDecoder & rxDecoder, RinnaiSignalDecoder & txDecoder, MQTTClient & mqttClient, String mqttTopicState, byte testPin);

	void loop();
	void mqttMessageReceived(String &topic, String &payload);

private:
	// private functions
	bool handleIncomingPacketQueueItem(const PacketQueueItem & item, bool remote);

	// properties
	RinnaiSignalDecoder & rxDecoder;
	RinnaiSignalDecoder & txDecoder;
	MQTTClient & mqttClient;
	String mqttTopicState;
	byte testPin;
	DebugLevel debugLevel = NONE;

	unsigned long lastMqttReportMillis = 0;
	String lastMqttReportPayload;

	byte lastHeaterPacketBytes[RinnaiProtocolDecoder::BYTES_IN_PACKET];
	byte lastLocalControlPacketBytes[RinnaiProtocolDecoder::BYTES_IN_PACKET];
	RinnaiHeaterPacket lastHeaterPacketParsed;
	RinnaiControlPacket lastLocalControlPacketParsed;
	int heaterPacketCounter = 0;
	int localControlPacketCounter = 0;
	int remoteControlPacketCounter = 0;
	unsigned long lastHeaterPacketMillis = 0;
	unsigned long lastLocalControlPacketMillis = 0;
	unsigned long lastRemoteControlPacketMillis = 0;
};
