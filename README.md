This is an always-on networking monitor based on ESP32. You can set the probes through ping, TCP connection, and local DNS resolution. 
You need to manually add the devices, their IPs, and the define the port numbers that this monitor can listen to.
Based on these probes, data is collected to show the state on the web interface. Whenever a device's state changes,
MQTT publishes a message which can be integrated into Home Assistant or Node-RED.

Along with that the Wi-Fi RSSI values are also shown to estimate signal strength to collerate with Wi-Fi setup. 
That an help discern interference, congestion, and whether you want to improve wireless connectivity. 
You can use that data to optimize router placement. 
