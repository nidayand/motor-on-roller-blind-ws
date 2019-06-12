#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Stepper_28BYJ_48.h>
#include <WebSocketsServer.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include "FS.h"
#include "index_html.h"
#include "NidayandHelper.h"

//--------------- CHANGE PARAMETERS ------------------
//Configure Default Settings for Access Point logon
String APid = "BlindsConnectAP";    //Name of access point
String APpw = "nidayand";           //Hardcoded password for access point

//Set up buttons
const uint8_t btnup = D5; //Up button
const uint8_t btndn = D6; //Down button
const uint8_t btnres = D7; //Reset button

//----------------------------------------------------

// Version number for checking if there are new code releases and notifying the user
String version = "1.3.3";

NidayandHelper helper = NidayandHelper();

//Fixed settings for WIFI
WiFiClient espClient;
PubSubClient psclient(espClient);   //MQTT client
char mqtt_server[40];             //WIFI config: MQTT server config (optional)
char mqtt_port[6] = "1883";       //WIFI config: MQTT port config (optional)
char mqtt_uid[40];             //WIFI config: MQTT server username (optional)
char mqtt_pwd[40];             //WIFI config: MQTT server password (optional)

String outputTopic;               //MQTT topic for sending messages
String inputTopic;                //MQTT topic for listening
boolean mqttActive = true;
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
  if (!helper.loadconfig())
    return false;

  JsonVariant json = helper.getconfig();

  //Useful if you need to see why confing is read incorrectly
  json.printTo(Serial);

  //Store variables locally
  currentPosition = json["currentPosition"].as<long>();
  maxPosition = json["maxPosition"].as<long>();

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
  DynamicJsonBuffer jsonBuffer(300);
  JsonObject& json = jsonBuffer.createObject();
  json["currentPosition"] = currentPosition;
  json["maxPosition"] = maxPosition;
  json["config_name"] = config_name;
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["mqtt_uid"] = mqtt_uid;
  json["mqtt_pwd"] = mqtt_pwd;
  json["config_rotation"] = config_rotation;

  return helper.saveconfig(json);
}

/*
   Connect to MQTT server and publish a message on the bus.
   Finally, close down the connection and radio
*/
void sendmsg(String topic, String payload) {
  if (!mqttActive)
    return;

  helper.mqtt_publish(psclient, topic, payload);
}


/****************************************************************************************
*/
void processMsg(String res, uint8_t clientnum) {
  /*
     Check if calibration is running and if stop is received. Store the location
  */
  if (action == "set" && res == "(0)") {
    maxPosition = currentPosition;
    saveItNow = true;
  }

  //Below are actions based on inbound MQTT payload
  if (res == "(start)") {

    //Store the current position as the start position
    currentPosition = 0;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "(max)") {

    //Store the max position of a closed blind
    maxPosition = currentPosition;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "(0)") {

    //Stop
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "(1)") {

    //Move down without limit to max position
    path = 1;
    action = "manual";
  } else if (res == "(-1)") {

    //Move up without limit to top position
    path = -1;
    action = "manual";
  } else if (res == "(update)") {
    //Send position details to client
    int set = (setPos * 100) / maxPosition;
    int pos = (currentPosition * 100) / maxPosition;
    sendmsg(outputTopic, "{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
    webSocket.sendTXT(clientnum, "{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
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

    int set = (setPos * 100) / maxPosition;
    int pos = (currentPosition * 100) / maxPosition;

    //Send the instruction to all connected devices
    sendmsg(outputTopic, "{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
    webSocket.broadcastTXT("{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_TEXT:
      Serial.printf("[%u] get Text: %s\n", num, payload);

      String res = (char*)payload;

      //Send to common MQTT and websocket function
      processMsg(res, num);
      break;
  }
}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print(F("Message arrived ["));
  Serial.print(topic);
  Serial.print(F("] "));
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
  Serial.println(F("Motor stopped"));
}

/*
   Callback from WIFI Manager for saving configuration
*/
void saveConfigCallback () {
  shouldSaveConfig = true;
}

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}
void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

void setup(void)
{
  Serial.begin(115200);
  delay(100);
  Serial.print(F("Starting now\n"));

  pinMode(btnup, INPUT_PULLUP);
  pinMode(btndn, INPUT_PULLUP);
  pinMode(btnres, INPUT_PULLUP);

  //Reset the action
  action = "";

  //Set MQTT properties
  outputTopic = helper.mqtt_gettopic("out");
  inputTopic = helper.mqtt_gettopic("in");

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
  //helper.resetsettings(wifiManager);

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
    Serial.println(F("Failed to mount file system"));
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
    Serial.println(F("Unable to load saved data"));
    currentPosition = 0;
    maxPosition = 2000000;
  }
  /*
    Setup multi DNS (Bonjour)
  */
  if (MDNS.begin(config_name)) {
    Serial.println(F("MDNS responder started"));
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("ws", "tcp", 81);

  } else {
    Serial.println(F("Error setting up MDNS responder!"));
    while (1) {
      delay(1000);
    }
  }
  Serial.print("Connect to http://" + String(config_name) + ".local or http://");
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
  if (String(mqtt_server) != "") {
    Serial.println(F("Registering MQTT server"));
    psclient.setServer(mqtt_server, String(mqtt_port).toInt());
    psclient.setCallback(mqttCallback);

  } else {
    mqttActive = false;
    Serial.println(F("NOTE: No MQTT server address has been registered. Only using websockets"));
  }

  //Set rotation direction of the blinds
  if (String(config_rotation) == "false")
    ccw = true;
  else
    ccw = false;


  //Update webpage
  INDEX_HTML.replace("{VERSION}", "V" + version);
  INDEX_HTML.replace("{NAME}", String(config_name));


  //Setup OTA
  //helper.ota_setup(config_name);
  {
    // Authentication to avoid unauthorized updates
    //ArduinoOTA.setPassword(OTA_PWD);

    ArduinoOTA.setHostname(config_name);

    ArduinoOTA.onStart([]() {
      Serial.println(F("Start"));
    });
    ArduinoOTA.onEnd([]() {
      Serial.println(F("\nEnd"));
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
      else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
      else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
      else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
      else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
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

  //Serving the webpage
  server.handleClient();

  //MQTT client
  if (mqttActive)
    helper.mqtt_reconnect(psclient, mqtt_uid, mqtt_pwd, { inputTopic.c_str() });

  if (digitalRead(btnres)) {
    bool pres_cont = false;
    while (!digitalRead(btndn) && currentPosition > 0) {
      Serial.println(F("Moving down"));
      small_stepper.step(ccw ? -1 : 1);
      currentPosition = currentPosition - 1;
      yield();
      delay(1);
      pres_cont = true;
    }
    while (!digitalRead(btnup) && currentPosition < maxPosition) {
      Serial.println(F("Moving up"));
      small_stepper.step(ccw ? 1 : -1);
      currentPosition = currentPosition + 1;
      yield();
      delay(1);
      pres_cont = true;
    }
    if (pres_cont) {
      int set = (setPos * 100) / maxPosition;
      int pos = (currentPosition * 100) / maxPosition;
      webSocket.broadcastTXT("{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
      sendmsg(outputTopic, "{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
      Serial.println(F("Stopped. Reached wanted position"));
      saveItNow = true;
    }
  }

  if (!(digitalRead(btnres) || digitalRead(btndn) || digitalRead(btnup))) {
    Serial.println(F("Hold to reset..."));
    uint32_t restime = millis();
    while (!(digitalRead(btnres) || digitalRead(btndn) || digitalRead(btnup)))
      yield(); //Prevent watchdog trigger

    if (millis() - restime >= 2500) {
      stopPowerToCoils();
      Serial.println(F("Removing configs..."));

      WiFi.disconnect(true);
      WiFiManager wifiManager;
      helper.resetsettings(wifiManager);

      Serial.println(F("Reboot"));
      ESP.wdtFeed();
      yield();
      ESP.restart();
    }
  }

  //Storing positioning data and turns off the power to the coils
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

  //Manage actions. Steering of the blind
  if (action == "auto") {

    //Automatically open or close blind
    if (currentPosition > path) {
      Serial.println(F("Moving down"));
      small_stepper.step(ccw ? -1 : 1);
      currentPosition = currentPosition - 1;
    } else if (currentPosition < path) {
      Serial.println(F("Moving up"));
      small_stepper.step(ccw ? 1 : -1);
      currentPosition = currentPosition + 1;
    } else {
      path = 0;
      action = "";
      int set = (setPos * 100) / maxPosition;
      int pos = (currentPosition * 100) / maxPosition;
      webSocket.broadcastTXT("{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
      sendmsg(outputTopic, "{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
      Serial.println(F("Stopped. Reached wanted position"));
      saveItNow = true;
    }

  } else if (action == "manual" && path != 0) {

    //Manually running the blind
    small_stepper.step(ccw ? path : -path);
    currentPosition = currentPosition + path;
    Serial.println(F("Moving motor manually"));
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
