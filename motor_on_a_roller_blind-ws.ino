#include <Stepper_28BYJ_48.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include "FS.h"
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>

//--------------- CHANGE PARAMETERS ------------------
//Configure Default Settings for Access Point logon
String APid = "BlindsConnectAP";    //Name of access point
String APpw = "nidayand";           //Hardcoded password for access point

//----------------------------------------------------

// Version number for checking if there are new code releases and notifying the user
String version = "1.2.2";


//Fixed settings for WIFI
WiFiClient espClient;
PubSubClient psclient(espClient);   //MQTT client
String mqttclientid;         //Generated MQTT client id
char mqtt_server[40];             //WIFI config: MQTT server config (optional)
char mqtt_port[6] = "1883";       //WIFI config: MQTT port config (optional)
char mqtt_uid[40];             //WIFI config: MQTT server username (optional)
char mqtt_pwd[40];             //WIFI config: MQTT server password (optional)

String outputTopic;               //MQTT topic for sending messages
String inputTopic;                //MQTT topic for listening
boolean mqttActive = true;
boolean mqttLogon = false;

char config_name[40];             //WIFI config: Bonjour name of device
char config_rotation[40] = "false"; //WIFI config: Detault rotation is CCW

String action;                      //Action manual/auto
int path = 0;                       //Direction of blind (1 = down, 0 = stop, -1 = up)
int setPos = 0;                     //The set position 0-100% by the client
long currentPosition = 0;           //Current position of the blind
long maxPosition = 2000000;         //Max position of the blind. Initial value
boolean loadDataSuccess = false;
boolean saveItNow = false;          //If true will store positions to SPIFFS
bool shouldSaveConfig = false;      //Used for WIFI Manager callback to save parameters
boolean initLoop = true;            //To enable actions first time the loop is run
boolean ccw = true;                 //Turns counter clockwise to lower the curtain

Stepper_28BYJ_48 small_stepper(D1, D3, D2, D4); //Initiate stepper driver

ESP8266WebServer server(80);              // TCP server at port 80 will respond to HTTP requests
WebSocketsServer webSocket = WebSocketsServer(81);  // WebSockets will respond on port 81

bool loadConfig() {
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("Failed to open config file");
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Config file size is too large");
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  // We don't use String here because ArduinoJson library requires the input
  // buffer to be mutable. If you don't use ArduinoJson, you may as well
  // use configFile.readString instead.
  configFile.readBytes(buf.get(), size);

  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());

  if (!json.success()) {
    Serial.println("Failed to parse config file");
    return false;
  }
  json.printTo(Serial);
  Serial.println();

  //Store variables locally
  currentPosition = long(json["currentPosition"]);
  maxPosition = long(json["maxPosition"]);
  strcpy(config_name, json["config_name"]);
  strcpy(mqtt_server, json["mqtt_server"]);
  strcpy(mqtt_port, json["mqtt_port"]);
  strcpy(mqtt_uid, json["mqtt_uid"]);
  strcpy(mqtt_pwd, json["mqtt_pwd"]);
  strcpy(config_rotation, json["config_rotation"]);

  return true;
}

/**
   Save configuration data to a JSON file
   on SPIFFS
*/
bool saveConfig() {
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["currentPosition"] = currentPosition;
  json["maxPosition"] = maxPosition;
  json["config_name"] = config_name;
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["mqtt_uid"] = mqtt_uid;
  json["mqtt_pwd"] = mqtt_pwd;
  json["config_rotation"] = config_rotation;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
    return false;
  }

  json.printTo(configFile);

  Serial.println("Saved JSON to SPIFFS");
  json.printTo(Serial);
  Serial.println();
  return true;
}

/*
   Connect to MQTT server and publish a message on the bus.
   Finally, close down the connection and radio
*/
void sendmsg(String topic, String payload) {
  if (!mqttActive)
    return;

  Serial.println("Trying to send msg..."+topic+":"+payload);
  //Send status to MQTT bus if connected
  if (psclient.connected()) {
    psclient.publish(topic.c_str(), payload.c_str());
  } else {
    Serial.println("PubSub client is not connected...");
  }
}
/*
   Connect the MQTT client to the
   MQTT server
*/
void reconnect() {
  if (!mqttActive)
    return;

  // Loop until we're reconnected
  while (!psclient.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if ((mqttLogon ? psclient.connect(mqttclientid.c_str(), mqtt_uid, mqtt_pwd) : psclient.connect(mqttclientid.c_str()))) {
      Serial.println("connected");

      //Send register MQTT message with JSON of chipid and ip-address
      sendmsg("/raw/esp8266/register", "{ \"id\": \"" + String(ESP.getChipId()) + "\", \"ip\":\"" + WiFi.localIP().toString() + "\", \"type\":\"roller blind\", \"name\":\""+config_name+"\"}");

      //Setup subscription
      psclient.subscribe(inputTopic.c_str());

    } else {
      Serial.print("failed, rc=");
      Serial.print(psclient.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      ESP.wdtFeed();
      delay(5000);
    }
  }
}
/*
   Common function to get a topic based on the chipid. Useful if flashing
   more than one device
*/
String getMqttTopic(String type) {
  return "/raw/esp8266/" + String(ESP.getChipId()) + "/" + type;
}

/****************************************************************************************
*/
void processMsg(String res, uint8_t clientnum){
  /*
     Check if calibration is running and if stop is received. Store the location
  */
  if (action == "set" && res == "(0)") {
    maxPosition = currentPosition;
    saveItNow = true;
  }

  /*
     Below are actions based on inbound MQTT payload
  */
  if (res == "(start)") {
    /*
       Store the current position as the start position
    */
    currentPosition = 0;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "(max)") {
    /*
       Store the max position of a closed blind
    */
    maxPosition = currentPosition;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "(0)") {
    /*
       Stop
    */
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "(1)") {
    /*
       Move down without limit to max position
    */
    path = 1;
    action = "manual";
  } else if (res == "(-1)") {
    /*
       Move up without limit to top position
    */
    path = -1;
    action = "manual";
  } else if (res == "(update)") {
    //Send position details to client
    int set = (setPos * 100)/maxPosition;
    int pos = (currentPosition * 100)/maxPosition;
    sendmsg(outputTopic, "{ \"set\":"+String(set)+", \"position\":"+String(pos)+" }");
    webSocket.sendTXT(clientnum, "{ \"set\":"+String(set)+", \"position\":"+String(pos)+" }");
  } else if (res == "(ping)") {
    //Do nothing
  } else {
    /*
       Any other message will take the blind to a position
       Incoming value = 0-100
       path is now the position
    */
    path = maxPosition * res.toInt() / 100;
    setPos = path; //Copy path for responding to updates
    action = "auto";

    int set = (setPos * 100)/maxPosition;
    int pos = (currentPosition * 100)/maxPosition;

    //Send the instruction to all connected devices
    sendmsg(outputTopic, "{ \"set\":"+String(set)+", \"position\":"+String(pos)+" }");
    webSocket.broadcastTXT("{ \"set\":"+String(set)+", \"position\":"+String(pos)+" }");
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_TEXT:
            Serial.printf("[%u] get Text: %s\n", num, payload);

            String res = (char*)payload;

            //Send to common MQTT and websocket function
            processMsg(res, num);
            break;
    }
}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  String res = "";
  for (int i = 0; i < length; i++) {
    res += String((char) payload[i]);
  }
  processMsg(res, NULL);
}

/**
  Turn of power to coils whenever the blind
  is not moving
*/
void stopPowerToCoils() {
  digitalWrite(D1, LOW);
  digitalWrite(D2, LOW);
  digitalWrite(D3, LOW);
  digitalWrite(D4, LOW);
}

/*
   Callback from WIFI Manager for saving configuration
*/
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void handleRoot() {
  server.send(200, "text/html", "<!DOCTYPE html><html><head> <meta http-equiv=\"Cache-Control\" content=\"no-cache, no-store, must-revalidate\"/> <meta http-equiv=\"Pragma\" content=\"no-cache\"/> <meta http-equiv=\"Expires\" content=\"0\"/> <title>{NAME}</title> <link rel=\"stylesheet\" href=\"https://unpkg.com/onsenui/css/onsenui.css\"> <link rel=\"stylesheet\" href=\"https://unpkg.com/onsenui/css/onsen-css-components.min.css\"> <script src=\"https://unpkg.com/onsenui/js/onsenui.min.js\"></script> <script src=\"https://unpkg.com/jquery/dist/jquery.min.js\"></script> <script>var cversion=\"{VERSION}\"; var wsUri=\"ws://\"+location.host+\":81/\"; var repo=\"motor-on-roller-blind-ws\"; window.fn={}; window.fn.open=function(){var menu=document.getElementById('menu'); menu.open();}; window.fn.load=function(page){var content=document.getElementById('content'); var menu=document.getElementById('menu'); content.load(page) .then(menu.close.bind(menu)).then(setActions());}; var gotoPos=function(percent){doSend(percent);}; var instr=function(action){doSend(\"(\"+action+\")\");}; var setActions=function(){doSend(\"(update)\"); $.get(\"https://api.github.com/repos/nidayand/\"+repo+\"/releases\", function(data){if (data.length>0 && data[0].tag_name !==cversion){$(\"#cversion\").text(cversion); $(\"#nversion\").text(data[0].tag_name); $(\"#update-card\").show();}}); setTimeout(function(){$(\"#arrow-close\").on(\"click\", function(){$(\"#setrange\").val(100);gotoPos(100);}); $(\"#arrow-open\").on(\"click\", function(){$(\"#setrange\").val(0);gotoPos(0);}); $(\"#setrange\").on(\"change\", function(){gotoPos($(\"#setrange\").val())}); $(\"#arrow-up-man\").on(\"click\", function(){instr(\"-1\")}); $(\"#arrow-down-man\").on(\"click\", function(){instr(\"1\")}); $(\"#arrow-stop-man\").on(\"click\", function(){instr(\"0\")}); $(\"#set-start\").on(\"click\", function(){instr(\"start\")}); $(\"#set-max\").on(\"click\", function(){instr(\"max\");});}, 200);}; $(document).ready(function(){setActions();}); var websocket; var timeOut; function retry(){clearTimeout(timeOut); timeOut=setTimeout(function(){websocket=null; init();},5000);}; function init(){ons.notification.toast({message: 'Connecting...', timeout: 1000}); try{websocket=new WebSocket(wsUri); websocket.onclose=function (){}; websocket.onerror=function(evt){ons.notification.toast({message: 'Cannot connect to device', timeout: 2000}); retry();}; websocket.onopen=function(evt){ons.notification.toast({message: 'Connected to device', timeout: 2000}); setTimeout(function(){doSend(\"(update)\");}, 1000);}; websocket.onclose=function(evt){ons.notification.toast({message: 'Disconnected. Retrying', timeout: 2000}); retry();}; websocket.onmessage=function(evt){try{var msg=JSON.parse(evt.data); if (typeof msg.position !=='undefined'){$(\"#pbar\").attr(\"value\", msg.position);}; if (typeof msg.set !=='undefined'){$(\"#setrange\").val(msg.set);};}catch(err){}};}catch (e){ons.notification.toast({message: 'Cannot connect to device. Retrying...', timeout: 2000}); retry();};}; function doSend(msg){if (websocket && websocket.readyState==1){websocket.send(msg);}}; window.addEventListener(\"load\", init, false); window.onbeforeunload=function(){if (websocket && websocket.readyState==1){websocket.close();};}; </script></head><body><ons-splitter> <ons-splitter-side id=\"menu\" side=\"left\" width=\"220px\" collapse swipeable> <ons-page> <ons-list> <ons-list-item onclick=\"fn.load('home.html')\" tappable> Home </ons-list-item> <ons-list-item onclick=\"fn.load('settings.html')\" tappable> Settings </ons-list-item> <ons-list-item onclick=\"fn.load('about.html')\" tappable> About </ons-list-item> </ons-list> </ons-page> </ons-splitter-side> <ons-splitter-content id=\"content\" page=\"home.html\"></ons-splitter-content></ons-splitter><template id=\"home.html\"> <ons-page> <ons-toolbar> <div class=\"left\"> <ons-toolbar-button onclick=\"fn.open()\"> <ons-icon icon=\"md-menu\"></ons-icon> </ons-toolbar-button> </div><div class=\"center\">{NAME}</div></ons-toolbar><ons-card> <div class=\"title\">Adjust position</div><div class=\"content\"><p>Move the slider to the wanted position or use the arrows to open/close to the max positions</p></div><ons-row> <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\"> </ons-col> <ons-col> <ons-progress-bar id=\"pbar\" value=\"75\"></ons-progress-bar> </ons-col> <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\"> </ons-col> </ons-row> <ons-row> <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\"> <ons-icon id=\"arrow-open\" icon=\"fa-arrow-up\" size=\"2x\"></ons-icon> </ons-col> <ons-col> <ons-range id=\"setrange\" style=\"width: 100%;\" value=\"25\"></ons-range> </ons-col> <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\"> <ons-icon id=\"arrow-close\" icon=\"fa-arrow-down\" size=\"2x\"></ons-icon> </ons-col> </ons-row> </ons-card> <ons-card id=\"update-card\" style=\"display:none\"> <div class=\"title\">Update available</div><div class=\"content\">You are running <span id=\"cversion\"></span> and <span id=\"nversion\"></span> is the latest. Go to <a href=\"https://github.com/nidayand/motor-on-roller-blind-ws/releases\">the repo</a> to download</div></ons-card> </ons-page></template><template id=\"settings.html\"> <ons-page> <ons-toolbar> <div class=\"left\"> <ons-toolbar-button onclick=\"fn.open()\"> <ons-icon icon=\"md-menu\"></ons-icon> </ons-toolbar-button> </div><div class=\"center\"> Settings </div></ons-toolbar> <ons-card> <div class=\"title\">Instructions</div><div class=\"content\"> <p> <ol> <li>Use the arrows and stop button to navigate to the top position i.e. the blind is opened</li><li>Click the START button</li><li>Use the down arrow to navigate to the max closed position</li><li>Click the MAX button</li><li>Calibration is completed!</li></ol> </p></div></ons-card> <ons-card> <div class=\"title\">Control</div><ons-row style=\"width:100%\"> <ons-col style=\"text-align:center\"><ons-icon id=\"arrow-up-man\" icon=\"fa-arrow-up\" size=\"2x\"></ons-icon></ons-col> <ons-col style=\"text-align:center\"><ons-icon id=\"arrow-stop-man\" icon=\"fa-stop\" size=\"2x\"></ons-icon></ons-col> <ons-col style=\"text-align:center\"><ons-icon id=\"arrow-down-man\" icon=\"fa-arrow-down\" size=\"2x\"></ons-icon></ons-col> </ons-row> </ons-card> <ons-card> <div class=\"title\">Store</div><ons-row style=\"width:100%\"> <ons-col style=\"text-align:center\"><ons-button id=\"set-start\">Set Start</ons-button></ons-col> <ons-col style=\"text-align:center\">&nbsp;</ons-col> <ons-col style=\"text-align:center\"><ons-button id=\"set-max\">Set Max</ons-button></ons-col> </ons-row> </ons-card> </ons-page></template><template id=\"about.html\"> <ons-page> <ons-toolbar> <div class=\"left\"> <ons-toolbar-button onclick=\"fn.open()\"> <ons-icon icon=\"md-menu\"></ons-icon> </ons-toolbar-button> </div><div class=\"center\"> About </div></ons-toolbar> <ons-card> <div class=\"title\">Motor on a roller blind</div><div class=\"content\"> <p> <ul> <li>3d print files and instructions: <a href=\"https://www.thingiverse.com/thing:2392856\">https://www.thingiverse.com/thing:2392856</a></li><li>MQTT based version on Github: <a href=\"https://github.com/nidayand/motor-on-roller-blind\">https://github.com/nidayand/motor-on-roller-blind</a></li><li>WebSocket based version on Github: <a href=\"https://github.com/nidayand/motor-on-roller-blind-ws\">https://github.com/nidayand/motor-on-roller-blind-ws</a></li><li>Licensed unnder <a href=\"https://creativecommons.org/licenses/by/3.0/\">Creative Commons</a></li></ul> </p></div></ons-card> </ons-page></template></body></html>");
}
void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

void setup(void)
{
  Serial.begin(115200);
  delay(100);
  Serial.print("Starting now\n");

  //Reset the action
  action = "";

  //Set MQTT properties
  outputTopic = getMqttTopic("out");
  inputTopic = getMqttTopic("in");
  mqttclientid = ("ESPClient-" + String(ESP.getChipId()));
  Serial.println("Sending to Mqtt-topic: "+outputTopic);
  Serial.println("Listening to Mqtt-topic: "+inputTopic);
  //Setup MQTT Client ID
  Serial.println("MQTT Client ID: "+String(mqttclientid));

  //Set the WIFI hostname
  WiFi.hostname(config_name);

  //Define customer parameters for WIFI Manager
  WiFiManagerParameter custom_config_name("Name", "Bonjour name", config_name, 40);
  WiFiManagerParameter custom_rotation("Rotation", "Clockwise rotation", config_rotation, 40);
  WiFiManagerParameter custom_text("<p><b>Optional MQTT server parameters:</b></p>");
  WiFiManagerParameter custom_mqtt_server("server", "MQTT server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT port", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_uid("uid", "MQTT username", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_pwd("pwd", "MQTT password", mqtt_server, 40);
  WiFiManagerParameter custom_text2("<script>t = document.createElement('div');t2 = document.createElement('input');t2.setAttribute('type', 'checkbox');t2.setAttribute('id', 'tmpcheck');t2.setAttribute('style', 'width:10%');t2.setAttribute('onclick', \"if(document.getElementById('Rotation').value == 'false'){document.getElementById('Rotation').value = 'true'} else {document.getElementById('Rotation').value = 'false'}\");t3 = document.createElement('label');tn = document.createTextNode('Clockwise rotation');t3.appendChild(t2);t3.appendChild(tn);t.appendChild(t3);document.getElementById('Rotation').style.display='none';document.getElementById(\"Rotation\").parentNode.insertBefore(t, document.getElementById(\"Rotation\"));</script>");
  //Setup WIFI Manager
  WiFiManager wifiManager;

  //reset settings - for testing
  //clean FS, for testing
  //SPIFFS.format();
  //wifiManager.resetSettings();

  wifiManager.setSaveConfigCallback(saveConfigCallback);
  //add all your parameters here
  wifiManager.addParameter(&custom_config_name);
  wifiManager.addParameter(&custom_rotation);
  wifiManager.addParameter(&custom_text);
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_uid);
  wifiManager.addParameter(&custom_mqtt_pwd);
  wifiManager.addParameter(&custom_text2);

  wifiManager.autoConnect(APid.c_str(), APpw.c_str());

  //Load config upon start
  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount file system");
    return;
  }

  /* Save the config back from WIFI Manager.
      This is only called after configuration
      when in AP mode
  */
  if (shouldSaveConfig) {
    //read updated parameters
    strcpy(config_name, custom_config_name.getValue());
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(mqtt_uid, custom_mqtt_uid.getValue());
    strcpy(mqtt_pwd, custom_mqtt_pwd.getValue());
    strcpy(config_rotation, custom_rotation.getValue());

    //Save the data
    saveConfig();
  }

  /*
     Try to load FS data configuration every time when
     booting up. If loading does not work, set the default
     positions
  */
  loadDataSuccess = loadConfig();
  if (!loadDataSuccess) {
    currentPosition = 0;
    maxPosition = 2000000;
  }

  /*
    Setup multi DNS (Bonjour)
    */
  if (MDNS.begin(config_name)) {
    Serial.println("MDNS responder started");
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("ws", "tcp", 81);

  } else {
    Serial.println("Error setting up MDNS responder!");
    while(1) {
      delay(1000);
    }
  }
  Serial.print("Connect to http://"+String(config_name)+".local or http://");
  Serial.println(WiFi.localIP());

  //Start HTTP server
  server.on("/", handleRoot);
  server.onNotFound(handleNotFound);
  server.begin();

  //Start websocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  /* Setup connection for MQTT and for subscribed
    messages IF a server address has been entered
  */
  if (String(mqtt_server) != ""){
    Serial.println("Registering MQTT server");
    psclient.setServer(mqtt_server, String(mqtt_port).toInt());
    psclient.setCallback(mqttCallback);
    if (String(mqtt_uid) != "" and String(mqtt_pwd) != ""){
      mqttLogon = true;
    }
  } else {
    mqttActive = false;
    Serial.println("NOTE: No MQTT server address has been registered. Only using websockets");
  }

  /* Set rotation direction of the blinds */
  if (String(config_rotation) == "false"){
    ccw = true;
  } else {
    ccw = false;
  }


  //Setup OTA
  {

    // Authentication to avoid unauthorized updates
    //ArduinoOTA.setPassword((const char *)"nidayand");

    ArduinoOTA.setHostname(config_name);

    ArduinoOTA.onStart([]() {
      Serial.println("Start");
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("\nEnd");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    ArduinoOTA.begin();
  }
}

void loop(void)
{
  //OTA client code
  ArduinoOTA.handle();

  //Websocket listner
  webSocket.loop();

  /**
    Serving the webpage
  */
  server.handleClient();

  //MQTT client
  if (mqttActive){
    if (!psclient.connected())
      reconnect();
    psclient.loop();
  }


  /**
    Storing positioning data and turns off the power to the coils
  */
  if (saveItNow) {
    saveConfig();
    saveItNow = false;

    /*
      If no action is required by the motor make sure to
      turn off all coils to avoid overheating and less energy
      consumption
    */
    stopPowerToCoils();

  }

  /**
    Manage actions. Steering of the blind
  */
  if (action == "auto") {
    /*
       Automatically open or close blind
    */
    if (currentPosition > path){
      small_stepper.step(ccw ? -1: 1);
      currentPosition = currentPosition - 1;
    } else if (currentPosition < path){
      small_stepper.step(ccw ? 1 : -1);
      currentPosition = currentPosition + 1;
    } else {
      path = 0;
      action = "";
      int set = (setPos * 100)/maxPosition;
      int pos = (currentPosition * 100)/maxPosition;
      webSocket.broadcastTXT("{ \"set\":"+String(set)+", \"position\":"+String(pos)+" }");
      sendmsg(outputTopic, "{ \"set\":"+String(set)+", \"position\":"+String(pos)+" }");
      Serial.println("Stopped. Reached wanted position");
      saveItNow = true;
    }

 } else if (action == "manual" && path != 0) {
    /*
       Manually running the blind
    */
    small_stepper.step(ccw ? path : -path);
    currentPosition = currentPosition + path;
  }

  /*
     After running setup() the motor might still have
     power on some of the coils. This is making sure that
     power is off the first time loop() has been executed
     to avoid heating the stepper motor draining
     unnecessary current
  */
  if (initLoop) {
    initLoop = false;
    stopPowerToCoils();
  }
}
