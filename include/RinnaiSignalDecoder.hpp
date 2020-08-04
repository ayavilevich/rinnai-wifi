#pragma once
#include <Arduino.h>

const byte INVALID_PIN = -1;

// this class decodes pulse length encoded Rinnai data coming from a pin and converts it to bytes
// this class is also capable of overwriting a packet with override data (proxy functionality)
class RinnaiSignalDecoder
{
public:
	RinnaiSignalDecoder(const byte pin, const byte proxyOutPin = INVALID_PIN, const bool invertIn = false);
	bool setup();

	// expose properties
	QueueHandle_t getPulseQueue()
	{
		return pulseQueue;
	}
	QueueHandle_t getBitQueue()
	{
		return bitQueue;
	}
	QueueHandle_t getPacketQueue()
	{
		return packetQueue;
	}

	unsigned int getPulseHandlerErrorCounter()
	{
		return pulseHandlerErrorCounter;
	}
	unsigned int getBitTaskErrorCounter()
	{
		return bitTaskErrorCounter;
	}
	unsigned int getPacketTaskErrorCounter()
	{
		return packetTaskErrorCounter;
	}

	bool setOverridePacket(const byte * data, int length);

	static const int BYTES_IN_PACKET = 6;

private:
	// private functions
	void pulseISRHandler();
	static void pulseISRHandler(void *);
	void bitTaskHandler();
	void packetTaskHandler();
	void overrideTaskHandler();
	void writeOverridePacket();
	static void writePacket(const byte pin, const byte * data, const byte len);
	static bool isOddParity(byte b);

	// properties
	byte pin = INVALID_PIN;
	byte proxyOutPin = INVALID_PIN;
	bool invertIn = false;
	QueueHandle_t pulseQueue = NULL;
	QueueHandle_t bitQueue = NULL;
	QueueHandle_t packetQueue = NULL;
	TaskHandle_t bitTask = NULL;
	TaskHandle_t packetTask = NULL;
	TaskHandle_t overrideTask = NULL;
	// packet override props
	byte overridePacket[BYTES_IN_PACKET];
	bool overridePacketSet = false;
	unsigned int lastPulseCycle = 0;
	bool isOverriding = false;

	unsigned int pulseHandlerErrorCounter = 0;
	unsigned int bitTaskErrorCounter = 0;
	unsigned int packetTaskErrorCounter = 0;
};

struct PulseQueueItem
{
	byte newLevel; // raise = 1, fall = 0
	unsigned int cycle; // when did it happen
};

enum BIT // enum for values of the bit queue item
{
	SYM0 = 0, // "0"
	SYM1,	  // "1"
	PRE,
	ERROR,
};

struct BitQueueItem
{
	BIT bit; // what bit is it?
	unsigned int startCycle; // when did it start
	int misc; // for testing
};

struct PacketQueueItem
{
	byte data[RinnaiSignalDecoder::BYTES_IN_PACKET];
	unsigned int startCycle; // when did it start
	byte bitsPresent;
	bool validPre;
	bool validChecksum;
	bool validParity;
};
