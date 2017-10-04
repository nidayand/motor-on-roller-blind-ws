# motor-on-roller-blind-ws
WebSocket based version of [motor-on-roller-blind](https://github.com/nidayand/motor-on-roller-blind). I.e. there is no need of an MQTT server.

3d parts for printing are available on Thingiverse.com: ["motor on a roller blind"](https://www.thingiverse.com/thing:2392856)

 1. A tiny webserver is setup on the esp8266 that will serve one page to the client
 2. Upon powering on the first time WIFI credentials and a hostname is needed to be configured. Connect your computer to a new WIFI hotspot named **BlindsConnectAP**. Password = **nidayand**
 3. Connect to your normal WIFI with your client and go to the IP address of the device - or if you have an mDNS supported device (e.g. iOS, OSX or have Bonjour installed) you can go to http://{hostname}.local. If you don't know the IP-address of the device check your router for the leases (or check the serial console in the Arduino IDE)
 4. As the webpage is loaded it will connect through a websocket directly to the device to progress updates and to control the device. If any other client connects the updates will be in sync.
 5. Go to the Configuration page to calibrate the motor with the start and end positions of the roller blind. Follow the instructions on the page

![enter image description here](https://user-images.githubusercontent.com/2181965/31178217-a5351678-a918-11e7-9611-3e8256c873a4.png) ![enter image description here](https://user-images.githubusercontent.com/2181965/31178216-a4f7194a-a918-11e7-85dd-8e189cfc031c.png)
