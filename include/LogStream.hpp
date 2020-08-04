#pragma once
#include <Arduino.h>

class LogStream
{
public:
	LogStream(Print &destination);
	Print & operator()();
	void SetLogStreamTelnet();
	void SetLogStreamSerial();

private:
	Print *destination;

	void SetLogStream(Print & _destination);
};

extern LogStream logStream; // external reference for the global, singleton
