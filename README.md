# Rinnai Wifi
Firmware for an ESP32 module that interfaces a Rinnai Control Panel to Home Assistant via MQTT

An article about the project: https://blog.yavilevich.com/2020/08/changing-a-dumb-rinnai-water-heater-to-a-smart-one/

Related projects:  
https://github.com/ayavilevich/rinnai-mock/  
https://github.com/ayavilevich/rinnai-control-panel-sigrok-pd  
by others:  
https://github.com/genmeim/rinnai-serial-decoder  

## MQTT protocol and topic syntax

The MQTT topic prefix can be set in your ``private_config.ini`` . The setting is called ``MQTT_TOPIC`` and it defaults to ``homeassistant/climate/rinnai``. Below it will be referred to as just ``~``.

### ~/config
Sent by the device to perform configuration. The message contains a json payload holding the description of the device for the purpose of https://www.home-assistant.io/docs/mqtt/discovery/ as an https://www.home-assistant.io/integrations/climate.mqtt/ device.


Example:

    {
    "~": "homeassistant/climate/rinnai",
    "name": "Rinnai Water Heater",
    "action_topic": "~/state",
    "action_template": "{{ value_json.action }}",
    "current_temperature_topic": "~/state",
    "current_temperature_template": "{{ value_json.currentTemperature }}",
    "max_temp": 48,
    "min_temp": 37,
    "initial": 37,
    "mode_command_topic": "~/mode",
    "mode_state_topic": "~/state",
    "mode_state_template": "{{ value_json.mode }}",
    "modes": [
        "off",
        "heat"
    ],
    "precision": 1,
    "temperature_command_topic": "~/temp",
    "temperature_unit": "C",
    "temperature_state_topic": "~/state",
    "temperature_state_template": "{{ value_json.targetTemperature }}",
    "availability_topic": "~/availability"
    }

### ~/state 
Sent by the device to update its state. The message contains a json topic holding most of the state of the device.

Example (with REPORT\_RESEARCH\_FIELDS == true):

    {
    "ip": "192.168.1.10",
    "testPin": "OFF",
    "enableTemperatureSync": true,
    "currentTemperature": 40,
    "targetTemperature": 40,
    "mode": "off",
    "action": "off",
    "activeId": 0,
    "heaterBytes": "07,01,03,50,20",
    "startupState": 80,
    "locControlId": 0,
    "locControlBytes": "00,00,00,5f,3f",
    "rssi": -83,
    "heaterDelta": 199,
    "locControlTiming": 81,
    "remControlId": 6,
    "remControlBytes": "06,00,00,5f,3f",
    "remControlTiming": 40
    }

### ~/availability
Sent by the device to update its availability. The payload is either "online" or "offline" per HA convention. The offline state is set using MQTT "last will" mechanism.

### ~/temp
Received by the device to set the desired temperature. The payload is a number in the range TEMP\_C\_MIN to TEMP\_C\_MAX and must also be a valid setting for your device.  
Invalid values will set the minimal temperature.

### ~/temperature_sync
Received by the device to enable or disable the temperature syncing functionality. The payload can be "on", "enable", "true" or "1" to enable sync and any other value to disable it. The default is "on".  
You would normally want this "on" except if troubleshooting.

### ~/mode
Received by the device to set the desired operational mode. The payload can be either "off" or "heat".

### ~/priority
Received by the device to request priority for this control panel from the heater. This topic has no payload.

### ~/log_level
Received by the device to set the verbosity of the log. The payload can be either "none", "parsed" or "raw".

### ~/log_destination
Received by the device to set the log medium. The payload can be "telnet" for sending the log using RemoteDebug library or anything else to send the log to the "Serial" device.
