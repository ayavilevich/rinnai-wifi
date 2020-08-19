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

enum OverrideCommand
{
	ON_OFF,
	PRIORITY,
	TEMPERATURE_UP,
	TEMPERATURE_DOWN,
};

// this class will handle the logic of converting between MQTT commands and Rinnai packets
class RinnaiMQTTGateway
{
public:
	RinnaiMQTTGateway(String haDeviceName, RinnaiSignalDecoder & rxDecoder, RinnaiSignalDecoder & txDecoder, MQTTClient & mqttClient, String mqttTopic, byte testPin);

	void loop();
	void onMqttMessageReceived(String &topic, String &payload);
	void onMqttConnected();

private:
	// private functions
	bool handleIncomingPacketQueueItem(const PacketQueueItem & item, bool remote);
	void handleTemperatureSync();
	bool override(OverrideCommand command);
	long millisDelta(unsigned long t1, unsigned long t2);

	// properties
	String haDeviceName;
	RinnaiSignalDecoder & rxDecoder;
	RinnaiSignalDecoder & txDecoder;
	MQTTClient & mqttClient;
	String mqttTopic;
	String mqttTopicState;
	byte testPin;
	DebugLevel logLevel = NONE;
	int targetTemperatureCelsius = -1;

	unsigned long lastMqttReportMillis = 0;
	String lastMqttReportPayload;

	byte lastHeaterPacketBytes[RinnaiProtocolDecoder::BYTES_IN_PACKET];
	byte lastLocalControlPacketBytes[RinnaiProtocolDecoder::BYTES_IN_PACKET];
	byte lastRemoteControlPacketBytes[RinnaiProtocolDecoder::BYTES_IN_PACKET];
	byte lastUnknownPacketBytes[RinnaiProtocolDecoder::BYTES_IN_PACKET];
	RinnaiHeaterPacket lastHeaterPacketParsed;
	RinnaiControlPacket lastLocalControlPacketParsed;
	RinnaiControlPacket lastRemoteControlPacketParsed;
	int heaterPacketCounter = 0;
	int localControlPacketCounter = 0;
	int remoteControlPacketCounter = 0;
	int unknownPacketCounter = 0;
	unsigned long lastHeaterPacketMillis = 0;
	unsigned long lastHeaterPacketDeltaMillis = 0;
	unsigned long lastLocalControlPacketMillis = 0;
	unsigned long lastRemoteControlPacketMillis = 0;
	unsigned long lastUnknownPacketMillis = 0;
};
