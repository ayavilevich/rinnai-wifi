#include <RemoteDebug.h>

#include "LogStream.hpp"

extern RemoteDebug remoteDebug; // defined and configured in main

LogStream::LogStream(Print &destination)
	: destination(&destination)
{
}

Print & LogStream::operator()()
{
	return *destination;
}

void LogStream::SetLogStream(Print &_destination)
{
	destination = &_destination;
}

void LogStream::SetLogStreamTelnet()
{
	SetLogStream(remoteDebug);
}

void LogStream::SetLogStreamSerial()
{
	SetLogStream(Serial);
}

LogStream logStream(Serial); // the global, singleton
