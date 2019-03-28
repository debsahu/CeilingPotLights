# CeilingPotLights

ESP32 controlling 8 Lights using LEDC and communicates with Home Assistant

**To-Do**
- [ ] Update Wiki
- [x] WebSocket and MQTT API
- [x] Web-Interface served on ESP32
- [x] Save previous state in EEPROM
- [X] Station and AP mode (AP mode works trun off LTE OR 4G)
- [x] Home Assistant MQTT Template Light
- [x] Home Assistant Light Auto-discovery (uncomment `//#define HA_AUTO_DISCOVERY`)
- [x] MQTT Auto-reconnect (non-blocking)
- [ ] Switch topic for MQTT
- [ ] Add Temperature Sensor

__________________________________________________________________________________________________________________________________________
__________________________________________________________________________________________________________________________________________

# Home Assistant Yaml File

## Temperature sensor
- platform: mqtt  
  state_topic: "ceiling/Your_Mac_Address/temp"  
  name: "SN3 Temperature"  
  device_class: "temperature"
  unit_of_measurement: "°F"  
  Have to pick one 
  One will round up One will not
  value_template: '{{ value_json.tempF}}'
  value_template: '{{ value_json.tempF | round(1) }}' 
