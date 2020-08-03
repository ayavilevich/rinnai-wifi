#pragma once
#include <Arduino.h>

enum RinnaiPacketSource
{
	INVALID, // packet is invalid
	UNKNOWN, // unable to identify device
	HEATER,	 // the main device
	CONTROL, // one of control panels
};

struct RinnaiHeaterPacket
{
	byte activeId;
	bool on;
	bool inUse;
	byte temperatureCelsius;
	byte startupState;
};

struct RinnaiControlPacket
{
	byte myId;
	bool onOffPressed;
	bool priorityPressed;
	bool temperatureUpPressed;
	bool temperatureDownPressed;
};

// this class is able to decode and modify Rinnai packets
// decoding is based on observations so it is not full
// specifications for constructing a valid packet are not known
class RinnaiProtocolDecoder
{
public:
	static const int BYTES_IN_PACKET = 6;

	static RinnaiPacketSource getPacketSource(const byte * data, int length);
	static bool decodeHeaterPacket(const byte * data, RinnaiHeaterPacket &packet);
	static bool decodeControlPacket(const byte * data, RinnaiControlPacket &packet);
	static String renderPacket(const byte * data);

	static void setOnOffPressed(byte * data);

private:
	static bool temperatureCodeToTemperatureCelsius(byte code, byte & temperature);
	static void calcAndSetChecksum(byte * data);
	static bool isOddParity(byte b);
};
