#include <Arduino.h>

// this class decodes pulse length encoded Rinnai data coming from a pin and converts it to bytes
class RinnaiSignalDecoder
{
public:
	RinnaiSignalDecoder(const byte pin);
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

	static const int BYTES_IN_PACKET = 6;

private:
	// private functions
	void pulseISRHandler();
	static void pulseISRHandler(void *);
	void bitTaskHandler();
	void packetTaskHandler();

	// properties
	byte pin;
	QueueHandle_t pulseQueue;
	QueueHandle_t bitQueue;
	QueueHandle_t packetQueue;
	TaskHandle_t bitTask;
	TaskHandle_t packetTask;

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
};
