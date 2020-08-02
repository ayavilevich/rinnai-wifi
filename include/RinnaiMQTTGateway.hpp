#pragma once
#include <Arduino.h>

#include <MQTT.h>

#include "RinnaiSignalDecoder.hpp"
#include "RinnaiProtocolDecoder.hpp"

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

	unsigned long lastMqttReport = 0;
	int testPinState = HIGH;

	RinnaiHeaterPacket lastHeaterPacket;
	RinnaiControlPacket lastLocalControlPacket;
	int heaterPacketCounter = 0;
	int localControlPacketCounter = 0;
	int remoteControlPacketCounter = 0;
	unsigned long lastHeaterPacketMillis = 0;
	unsigned long lastLocalControlPacketMillis = 0;
	unsigned long lastRemoteControlPacketMillis = 0;
};
