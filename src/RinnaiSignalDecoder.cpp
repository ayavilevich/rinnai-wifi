#include "RinnaiSignalDecoder.hpp"
#include "StreamPrintf.hpp"

const int PULSES_IN_BIT = 2;
const int BITS_IN_PACKET = RinnaiSignalDecoder::BYTES_IN_PACKET * 8;
const int MAX_PACKETS_IN_QUEUE = 3;

const int TASK_STACK_DEPTH = 2000; // minimum is configMINIMAL_STACK_SIZE
const int BIT_TASK_PRIORITY = 1;   // Each task can have a priority between 0 and 24. The upper limit is defined by configMAX_PRIORITIES. The priority of the main loop is 1.
const int PACKET_TASK_PRIORITY = 1;

const int SYMBOL_DURATION_US = 600;

const int SYMBOL_SHORT_PERIOD_RATIO_MIN = SYMBOL_DURATION_US * 15 / 100;
const int SYMBOL_SHORT_PERIOD_RATIO_MAX = SYMBOL_DURATION_US * 35 / 100;
const int SYMBOL_LONG_PERIOD_RATIO_MIN = SYMBOL_DURATION_US * 65 / 100;
const int SYMBOL_LONG_PERIOD_RATIO_MAX = SYMBOL_DURATION_US * 85 / 100;

enum BitTaskState
{
	WAIT_PRE,
	WAIT_SYMBOL,
};

RinnaiSignalDecoder::RinnaiSignalDecoder(const byte pin)
	: pin(pin)
{
}

// return true is setup is ok
bool RinnaiSignalDecoder::setup()
{
	// setup pin
	// pinMode(pin, INPUT);
	gpio_pad_select_gpio(pin);
	gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT); // is this a valid cast to gpio_num_t?
	gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
	gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_ANYEDGE);
	gpio_intr_enable((gpio_num_t)pin);
	// create interrupts
	// attachInterrupt(); // too basic
	// use either gpio_isr_register (global ISR for all pins) or gpio_install_isr_service + gpio_isr_handler_add (per pin)
	esp_err_t ret_isr;
	ret_isr = gpio_install_isr_service(ESP_INTR_FLAG_IRAM); // ESP_INTR_FLAG_IRAM -> code is in RAM -> allows the interrupt to run even during flash operations
	if (ret_isr != ESP_OK && ret_isr != ESP_ERR_INVALID_STATE) // ESP_ERR_INVALID_STATE -> already initialized
	{
		return false;
	}
	ret_isr = gpio_isr_handler_add((gpio_num_t)pin, &RinnaiSignalDecoder::pulseISRHandler, this);
	if (ret_isr != ESP_OK)
	{
		return false;
	}
	/*
	esp_err_t ret_isr = esp_intr_alloc(ETS_GPIO_INTR_SOURCE, (int)ESP_INTR_FLAG_IRAM, gpio_isr_handler, this, NULL); // don't save handle
	if (ret_isr != ESP_OK)
	{
		return false;
	}
	*/
	// create pulse queue
	pulseQueue = xQueueCreate(MAX_PACKETS_IN_QUEUE * BITS_IN_PACKET * PULSES_IN_BIT, sizeof(PulseQueueItem)); // every bit is two pulses (not including "pre" overhead)
	if (pulseQueue == 0)
	{
		return false;
	}
	// create bit queue
	bitQueue = xQueueCreate(MAX_PACKETS_IN_QUEUE * BITS_IN_PACKET, sizeof(BitQueueItem));
	if (bitQueue == 0)
	{
		return false;
	}
	// create packet queue
	packetQueue = xQueueCreate(MAX_PACKETS_IN_QUEUE, sizeof(PacketQueueItem));
	if (packetQueue == 0)
	{
		return false;
	}
	// log
	StreamPrintf(Serial, "Created queues, now about to create tasks\n");
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
		StreamPrintf(Serial, "Error creating task, %d\n", ret);
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
		StreamPrintf(Serial, "Error creating task, %d\n", ret);
		return false;
	}
	// return
	return true;
}

// forward calls to a member function
void IRAM_ATTR RinnaiSignalDecoder::pulseISRHandler(void *arg)
{
	static_cast<RinnaiSignalDecoder *>(arg)->pulseISRHandler();
	// ets_printf("isr %d\n", arg);
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

// handle pulse raise and falls
void IRAM_ATTR RinnaiSignalDecoder::pulseISRHandler()
{
	PulseQueueItem item;
	item.cycle = xthal_get_ccount();
	//item.newLevel = gpio_get_level((gpio_num_t)pin); // not IRAM safe
	item.newLevel = gpio_get_level_IRAM(pin);
	BaseType_t ret = xQueueSendToBackFromISR(pulseQueue, &item, NULL);
	// ret: pdTRUE = 1; errQUEUE_FULL = 0;
	if (ret != pdTRUE)
	{
		// ets_printf("xQueueSendToBackFromISR %d\n", ret);
		pulseHandlerErrorCounter++;
	}
}

// start each iteration assuming pin is low
// wait for switch to HIGH, then to LOW. measure length.
// have timeouts in place
void RinnaiSignalDecoder::bitTaskHandler()
{
	Serial.println("bitTaskHandler started");
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
	Serial.println("packetTaskHandler started");
	BitQueueItem bit; // we read these, process and push data to the packet queue

	PacketQueueItem packet; // current state
	packet.bitsPresent = 0;
	packet.startCycle = 0;
	packet.validPre = false;
	packet.validChecksum = false;
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
			case PRE:
			case ERROR:
			default:
				// flush current (invalid packet) ?
				// reset state
				packet.bitsPresent = 0;
				packet.startCycle = bit.startCycle;
				packet.validPre = bit.bit == PRE;
				packet.validChecksum = false;
				memset(packet.data, 0, sizeof(packet.data));
			}
			// see if we completed a packet
			if (packet.bitsPresent == BITS_IN_PACKET)
			{
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
				memset(packet.data, 0, sizeof(packet.data));
			}
		}
	}
}