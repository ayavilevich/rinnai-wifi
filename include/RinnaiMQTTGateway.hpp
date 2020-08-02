#pragma once
#include <Arduino.h>

#include "RinnaiSignalDecoder.hpp"
#include "RinnaiProtocolDecoder.hpp"

// this class will handle the logic of converting between MQTT commands and Rinnai packets
class RinnaiMQTTGateway
{
public:
	RinnaiMQTTGateway(RinnaiSignalDecoder & rxDecoder, RinnaiSignalDecoder & txDecoder);

	void loop();

private:
	// private functions
	bool handleIncomingPacketQueueItem(const PacketQueueItem & item, bool remote);

	// properties
	RinnaiSignalDecoder & rxDecoder;
	RinnaiSignalDecoder & txDecoder;
};
