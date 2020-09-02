#include "LogStream.hpp"
#include "RinnaiSignalDecoder.hpp"

const int PULSES_IN_BIT = 2;
const int BITS_IN_PACKET = RinnaiSignalDecoder::BYTES_IN_PACKET * 8;
const int MAX_PACKETS_IN_QUEUE = 3;

const int TASK_STACK_DEPTH = 2000; // minimum is configMINIMAL_STACK_SIZE
const int BIT_TASK_PRIORITY = 1;   // Each task can have a priority between 0 and 24. The upper limit is defined by configMAX_PRIORITIES. The priority of the main loop is 1.
const int PACKET_TASK_PRIORITY = 1;
const int OVERRIDE_TASK_PRIORITY = 4; // high priority task, will block others while it is running

const int SYMBOL_DURATION_US = 600;

const int INIT_PULSE = 850;
const int SHORT_PULSE = 150;
const int LONG_PULSE = 450;

const int SYMBOL_SHORT_PERIOD_RATIO_MIN = SYMBOL_DURATION_US * 15 / 100;
const int SYMBOL_SHORT_PERIOD_RATIO_MAX = SYMBOL_DURATION_US * 35 / 100;
const int SYMBOL_LONG_PERIOD_RATIO_MIN = SYMBOL_DURATION_US * 65 / 100;
const int SYMBOL_LONG_PERIOD_RATIO_MAX = SYMBOL_DURATION_US * 85 / 100;

const int EXPECTED_PERIOD_BETWEEN_PACKETS_MIN = 160000; // us
const int EXPECTED_PERIOD_BETWEEN_PACKETS_MAX = 180000; // us

enum BitTaskState
{
	WAIT_PRE,
	WAIT_SYMBOL,
};

RinnaiSignalDecoder::RinnaiSignalDecoder(const byte pin, const byte proxyOutPin, const bool invertIn, const bool invertOut)
	: pin(pin), proxyOutPin(proxyOutPin), invertIn(invertIn), invertOut(invertOut)
{
}

// return true is setup is ok
bool RinnaiSignalDecoder::setup()
{
	// setup input pin
	// pinMode(pin, INPUT); // too basic
	gpio_pad_select_gpio(pin);
	gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT); // is this a valid cast to gpio_num_t?
	gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
	gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_ANYEDGE);
	gpio_intr_enable((gpio_num_t)pin);
	// setup output pin
	if (proxyOutPin != INVALID_PIN)
	{
		pinMode(proxyOutPin, OUTPUT);
		digitalWrite(proxyOutPin, digitalRead(pin) ^ invertIn ^ invertOut); // outputting LOW will signal that we are ready to receive
	}

	// create interrupts
	// attachInterrupt(); // too basic
	// use either gpio_isr_register (global ISR for all pins) or gpio_install_isr_service + gpio_isr_handler_add (per pin)
	esp_err_t ret_isr;
	ret_isr = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);	   // ESP_INTR_FLAG_IRAM -> code is in RAM -> allows the interrupt to run even during flash operations
	if (ret_isr != ESP_OK && ret_isr != ESP_ERR_INVALID_STATE) // ESP_ERR_INVALID_STATE -> already initialized
	{
		logStream().printf("Error installing isr, %d\n", ret_isr);
		return false;
	}
	ret_isr = gpio_isr_handler_add((gpio_num_t)pin, &RinnaiSignalDecoder::pulseISRHandler, this);
	if (ret_isr != ESP_OK)
	{
		logStream().printf("Error adding isr handler, %d\n", ret_isr);
		return false;
	}
	// create pulse queue
	pulseQueue = xQueueCreate(MAX_PACKETS_IN_QUEUE * BITS_IN_PACKET * PULSES_IN_BIT, sizeof(PulseQueueItem)); // every bit is two pulses (not including "pre" overhead)
	if (pulseQueue == 0)
	{
		logStream().printf("Error creating queue\n");
		return false;
	}
	// create bit queue
	bitQueue = xQueueCreate(MAX_PACKETS_IN_QUEUE * BITS_IN_PACKET, sizeof(BitQueueItem));
	if (bitQueue == 0)
	{
		logStream().printf("Error creating queue\n");
		return false;
	}
	// create packet queue
	packetQueue = xQueueCreate(MAX_PACKETS_IN_QUEUE, sizeof(PacketQueueItem));
	if (packetQueue == 0)
	{
		logStream().printf("Error creating queue\n");
		return false;
	}
	// log
	logStream().printf("Created queues, now about to create tasks\n");
	// create pulse to bit task
	BaseType_t ret;
	ret = xTaskCreate([](void *o) { static_cast<RinnaiSignalDecoder *>(o)->bitTaskHandler(); },
					  "bit task",
					  TASK_STACK_DEPTH,
					  this,
					  BIT_TASK_PRIORITY,
					  &bitTask);
	if (ret != pdPASS)
	{
		logStream().printf("Error creating task, %d\n", ret);
		return false;
	}
	// create byte to packet task
	ret = xTaskCreate([](void *o) { static_cast<RinnaiSignalDecoder *>(o)->packetTaskHandler(); },
					  "packet task",
					  TASK_STACK_DEPTH,
					  this,
					  PACKET_TASK_PRIORITY,
					  &packetTask);
	if (ret != pdPASS)
	{
		logStream().printf("Error creating task, %d\n", ret);
		return false;
	}
	// create packet override task
	ret = xTaskCreate([](void *o) { static_cast<RinnaiSignalDecoder *>(o)->overrideTaskHandler(); },
					  "override task",
					  TASK_STACK_DEPTH,
					  this,
					  OVERRIDE_TASK_PRIORITY,
					  &overrideTask);
	if (ret != pdPASS)
	{
		logStream().printf("Error creating task, %d\n", ret);
		return false;
	}
	// return
	return true;
}

// forward calls to a member function
void IRAM_ATTR RinnaiSignalDecoder::pulseISRHandler(void *arg)
{
	static_cast<RinnaiSignalDecoder *>(arg)->pulseISRHandler();
}

// https://www.reddit.com/r/esp32/comments/f529hf/results_comparing_the_speeds_of_different_gpio/
int IRAM_ATTR gpio_get_level_IRAM(int gpio_num)
{
	if (gpio_num < 32)
	{
		return (GPIO.in >> gpio_num) & 0x1;
	}
	else
	{
		return (GPIO.in1.data >> (gpio_num - 32)) & 0x1;
	}
}

// https://www.reddit.com/r/esp32/comments/f529hf/results_comparing_the_speeds_of_different_gpio/
void IRAM_ATTR gpio_set_level_IRAM(int gpio_num, int level)
{
	if (gpio_num < 32)
	{
		if (level)
		{
			GPIO.out_w1ts = 1 << gpio_num;
		}
		else
		{
			GPIO.out_w1tc = 1 << gpio_num;
		}
	}
	else
	{
		if (level)
		{
			GPIO.out1_w1ts.data = 1 << (gpio_num - 32);
		}
		else
		{
			GPIO.out1_w1tc.data = 1 << (gpio_num - 32);
		}
	}
}

// handle pulse raise and falls
void IRAM_ATTR RinnaiSignalDecoder::pulseISRHandler()
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	// read state
	PulseQueueItem item;
	item.cycle = xthal_get_ccount();
	//item.newLevel = gpio_get_level((gpio_num_t)pin); // not IRAM safe
	item.newLevel = (bool)gpio_get_level_IRAM(pin) ^ invertIn;
	// track changes to output
	if (proxyOutPin != INVALID_PIN && !isOverriding) // if overriding proxy is enabled and we are not already overriding
	{
		// see if we need to start overriding
		if (overridePacketSet && item.newLevel) // if there is override data and it is a rise
		{
			unsigned int delta = clockCyclesToMicroseconds(item.cycle - lastPulseCycle);
			if (delta > EXPECTED_PERIOD_BETWEEN_PACKETS_MIN && delta < EXPECTED_PERIOD_BETWEEN_PACKETS_MAX) // and if timings match
			{
				isOverriding = true;
				// unblock high priority override task
				// use notifications https://www.freertos.org/RTOS-task-notifications.html, they are faster than semaphores
				vTaskNotifyGiveFromISR(overrideTask, &xHigherPriorityTaskWoken);
			}
		}
		if (!isOverriding) // only if we didn't start an override task above
		{
			gpio_set_level_IRAM(proxyOutPin, item.newLevel ^ invertOut); // mirror
		}
	}
	lastPulseCycle = item.cycle;
	// send pulse to queue
	BaseType_t ret = xQueueSendToBackFromISR(pulseQueue, &item, &xHigherPriorityTaskWoken);
	// ret: pdTRUE = 1; errQUEUE_FULL = 0;
	if (ret != pdTRUE)
	{
		// ets_printf("xQueueSendToBackFromISR %d\n", ret);
		pulseHandlerErrorCounter++;
	}
	// do context switch if it was requested
	if (xHigherPriorityTaskWoken)
	{
		portYIELD_FROM_ISR();
	}
}

// start each iteration assuming pin is low
// wait for switch to HIGH, then to LOW. measure length.
// have timeouts in place
void RinnaiSignalDecoder::bitTaskHandler()
{
	logStream().println("bitTaskHandler started");
	PulseQueueItem pulse; // we read these, process and push data to the bit queue
	unsigned int lastEndCycle = 0;
	for (;;)
	{
		// assume next edge is rise
		BaseType_t ret = xQueueReceive(pulseQueue, &pulse, portMAX_DELAY); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
		if (ret != pdTRUE || pulse.newLevel != 1)						   // if not rise or can't pull from queue
		{
			bitTaskErrorCounter++;
		}
		else
		{
			unsigned int risingCycle = pulse.cycle;
			// wait for fall
			ret = xQueueReceive(pulseQueue, &pulse, portMAX_DELAY); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
			if (ret != pdTRUE || pulse.newLevel != 0)
			{
				bitTaskErrorCounter++;
			}
			else
			{
				unsigned int fallingCycle = pulse.cycle;
				// we have 3 relevant timings: lastEndCycle, risingCycle and fallingCycle
				// convert
				unsigned long pulseLengthLow = clockCyclesToMicroseconds(risingCycle - lastEndCycle);
				unsigned long pulseLengthHigh = clockCyclesToMicroseconds(fallingCycle - risingCycle);
				// decide on what to register
				BitQueueItem value;																		 // what to register?
				if (pulseLengthHigh > SYMBOL_DURATION_US && SYMBOL_DURATION_US < SYMBOL_DURATION_US * 2) // if valid pre pulse
				{
					// register a "pre"
					value.bit = PRE;
					value.startCycle = risingCycle;
					value.misc = pulseLengthHigh;
				}
				else if (pulseLengthLow > SYMBOL_SHORT_PERIOD_RATIO_MIN && pulseLengthLow < SYMBOL_SHORT_PERIOD_RATIO_MAX && pulseLengthHigh > SYMBOL_LONG_PERIOD_RATIO_MIN && pulseLengthHigh < SYMBOL_LONG_PERIOD_RATIO_MAX)
				{
					value.bit = SYM1;
					value.startCycle = lastEndCycle;
					value.misc = pulseLengthLow;
				}
				else if (pulseLengthLow > SYMBOL_LONG_PERIOD_RATIO_MIN && pulseLengthLow < SYMBOL_LONG_PERIOD_RATIO_MAX && pulseLengthHigh > SYMBOL_SHORT_PERIOD_RATIO_MIN && pulseLengthHigh < SYMBOL_SHORT_PERIOD_RATIO_MAX)
				{
					value.bit = SYM0;
					value.startCycle = lastEndCycle;
					value.misc = pulseLengthLow;
				}
				else
				{
					value.bit = ERROR;
					value.startCycle = lastEndCycle;
					value.misc = pulseLengthLow;
				}
				// register
				BaseType_t ret = xQueueSendToBack(bitQueue, &value, 0); // no wait
				if (ret != pdTRUE)
				{
					// inc error counter
					bitTaskErrorCounter++;
				}
			}
			// save last for next iteration
			lastEndCycle = pulse.cycle;
		}
	}
}

void RinnaiSignalDecoder::packetTaskHandler()
{
	logStream().println("packetTaskHandler started");
	BitQueueItem bit; // we read these, process and push data to the packet queue

	PacketQueueItem packet; // current state
	packet.bitsPresent = 0;
	packet.startCycle = 0;
	packet.startMicros = 0;
	packet.startMillis = 0;
	packet.validPre = false;
	packet.validChecksum = false;
	packet.validParity = false;
	memset(packet.data, 0, sizeof(packet.data));

	for (;;)
	{
		BaseType_t ret = xQueueReceive(bitQueue, &bit, portMAX_DELAY); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
		if (ret != pdTRUE)											   // if can't pull from queue
		{
			packetTaskErrorCounter++;
		}
		else
		{
			switch (bit.bit)
			{
			case SYM0:
				// nothing to store, just move forward
				packet.bitsPresent++;
				break;
			case SYM1:
				// store bit in packet data
				if (packet.bitsPresent < BITS_IN_PACKET)
				{
					packet.data[packet.bitsPresent / 8] |= (1 << (packet.bitsPresent % 8));
				}
				packet.bitsPresent++;
				break;
			case ERROR:
			default:
				packetTaskErrorCounter++;
			case PRE:
				// flush current (invalid packet) ?
				// reset state
				packet.bitsPresent = 0;
				packet.startCycle = bit.startCycle;
				packet.startMicros = micros(); // this is the time of processing the bit queue item and not exact time of the pulse in the ISR. it was accurate to a ms level most of the time.
				// it is not possible to compensate for the difference using clockCyclesToMicroseconds(xthal_get_ccount() - bit.startCycle) because "xthal_get_ccount" is core specific and this task is not pinned to a specific core.
				// the difference is about 1ms, though, assuming one of the cores can run this task and we are not "stuck" on high priority tasks. This can be observed using xPortGetCoreID() and the expression above.
				packet.startMillis = millis(); // millis and micros come from the same 64bit counter (esp_timer_get_time()) but they overflow/wrap differently.
				packet.validPre = bit.bit == PRE;
				packet.validChecksum = false;
				packet.validParity = false;
				memset(packet.data, 0, sizeof(packet.data));
			}
			// see if we completed a packet
			if (packet.bitsPresent == BITS_IN_PACKET)
			{
				// check parity (each data byte has “odd parity bit” as the MSB bit)
				packet.validParity = true; // be optimistic
				for (int i = 0; i < BYTES_IN_PACKET - 1; i++)
				{
					if (!isOddParity(packet.data[i]))
					{
						packet.validParity = false;
					}
				}
				// check checksum (last byte is xor of first 5 bytes)
				byte checksum = 0;
				for (int i = 0; i < BYTES_IN_PACKET; i++)
				{
					checksum ^= packet.data[i];
				}
				packet.validChecksum = checksum == 0;
				// send
				BaseType_t ret = xQueueSendToBack(packetQueue, &packet, 0); // no wait
				if (ret != pdTRUE)
				{
					// inc error counter
					packetTaskErrorCounter++;
				}
				// reset state, so if we keep getting bytes then we result in error packets
				packet.bitsPresent = 0;
				packet.validPre = false;
				packet.validChecksum = false;
				packet.validParity = false;
				memset(packet.data, 0, sizeof(packet.data));
			}
		}
	}
}

bool RinnaiSignalDecoder::isOddParity(byte b)
{
	// https://stackoverflow.com/questions/21617970/how-to-check-if-value-has-even-parity-of-bits-or-odd
	b ^= b >> 4;
	b ^= b >> 2;
	b ^= b >> 1;
	return b & 1;
}

// wait for signals to override then flush previously set bytes
void RinnaiSignalDecoder::overrideTaskHandler()
{
	logStream().println("overrideTaskHandler started");
	for (;;)
	{
		/* Wait to be notified that we need to do work. Note the first
		parameter is pdTRUE, which has the effect of clearing the task's notification
		value back to 0, making the notification value act like a binary (rather than
		a counting) semaphore.  */
		uint32_t ulNotificationValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		if (ulNotificationValue == 1)
		{
			// we got a notification, write data
			writeOverridePacket();
			delayMicroseconds(EXPECTED_PERIOD_BETWEEN_PACKETS_MAX - EXPECTED_PERIOD_BETWEEN_PACKETS_MIN); // delay to make sure we cover the original changes
			// we finished, clear state
			overridePacketSet = false; // this makes sure a packet is only sent once
			isOverriding = false;
		}
		else
		{
			// timeout
		}
	}
}

void RinnaiSignalDecoder::writeOverridePacket()
{
	writePacket(proxyOutPin, overridePacket, BYTES_IN_PACKET, invertOut);
}

// this is a bit-bang blocking function, consider other options to send pulses without blocking
void RinnaiSignalDecoder::writePacket(const byte pin, const byte *data, const byte len, const bool invert)
{
	// send init
	gpio_set_level_IRAM(pin, HIGH ^ invert);
	delayMicroseconds(INIT_PULSE);
	gpio_set_level_IRAM(pin, LOW ^ invert);
	// send bytes
	for (byte i = 0; i < len; i++)
	{
		// send byte
		for (byte bit = 0; bit < 8; bit++)
		{
			// send bit
			const byte value = data[i] & (1 << bit);
			if (value)
			{
				delayMicroseconds(SHORT_PULSE);
				gpio_set_level_IRAM(pin, HIGH ^ invert);
				delayMicroseconds(LONG_PULSE);
				gpio_set_level_IRAM(pin, LOW ^ invert);
			}
			else
			{
				delayMicroseconds(LONG_PULSE);
				gpio_set_level_IRAM(pin, HIGH ^ invert);
				delayMicroseconds(SHORT_PULSE);
				gpio_set_level_IRAM(pin, LOW ^ invert);
			}
		}
	}
}

bool RinnaiSignalDecoder::setOverridePacket(const byte *data, int length)
{
	if (length != BYTES_IN_PACKET)
	{
		return false;
	}
	// wait for high priority override task to complete
	while (isOverriding)
		;

	if (overridePacketSet) // if we already set a packet but it didn't flush
	{
		return false;
	}

	memcpy(overridePacket, data, length);
	overridePacketSet = true; // turn on flag
	return true;
}
