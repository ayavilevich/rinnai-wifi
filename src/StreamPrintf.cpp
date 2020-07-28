#include <Arduino.h>
#include <stdarg.h>

#include "StreamPrintf.hpp"

int StreamPrintf(Stream& stream, const char* format, va_list va) {
	// measure
	int n = vsnprintf(NULL, 0, format, va) + 1; // return value is not counting the null terminator
	// allocate buffer
	char buf[n];
	int ret = vsnprintf(buf, n, format, va);
	stream.print(buf);
	return ret;
}

int StreamPrintf(Stream* stream, const char* format, ...) {
	if (stream != NULL) {
		va_list args;
		va_start(args, format);
		int ret = StreamPrintf(*stream, format, args);
		va_end(args);
		return ret;
	}
	return -1;
}

int StreamPrintf(Stream& stream, const char* format, ...) {
	va_list args;
	va_start(args, format);
	int ret = StreamPrintf(stream, format, args);
	va_end(args);
	return ret;
}
