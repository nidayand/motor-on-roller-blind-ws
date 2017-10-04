#include <Stepper_28BYJ_48.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include "FS.h"
#include <WiFiClient.h>

#include <WebSocketsServer.h>

String version = "1.0";

//Configure Default Settings
String APid = "BlindsConnectAP";
String APpw = "nidayand";
String OTAhostname = "blinds";

//Fixed settings for WIFI
bool shouldSaveConfig = false;    //Used for WIFI Manager callback to save parameters
char config_name[40];             //Bonjour name of device

boolean initLoop = true;
boolean debugging = false;          //Debug mode. Toggled by payload "debug". Will send MQTT message at stop with position
String debugTopic;

String action;                      //Action manual/auto
int path = 0;                       //Direction of blind (1 = down, 0 = stop, -1 = up)
int setPos = 0;

//Stored data
long currentPosition = 0;           //Current position of the blind
long maxPosition = 2000000;         //Max position of the blind
boolean loadDataSuccess = false;
boolean saveItNow = false;          //If true will store positions to SPIFFS

Stepper_28BYJ_48 small_stepper(D1, D3, D2, D4);


// TCP server at port 80 will respond to HTTP requests
WiFiServer server(80);
// WebSockets will respond on port 81
WebSocketsServer webSocket = WebSocketsServer(81);


/****************************************************************************************
   Loading configuration that has been saved on SPIFFS.
   Returns false if not successful
*/
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
/****************************************************************************************
*/


void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[%u] Disconnected!\n", num);
            break;
        case WStype_CONNECTED:
            {
                IPAddress ip = webSocket.remoteIP(num);
                Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);

            }
            break;
        case WStype_TEXT:
            Serial.printf("[%u] get Text: %s\n", num, payload);

            {String res = (char*)payload;

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
              int pos = (setPos * 100)/maxPosition;
              webSocket.sendTXT(num, "{ \"type\":\"set\", \"value\":"+String(pos)+"}");
              pos = (currentPosition * 100)/maxPosition;
              webSocket.sendTXT(num, "{ \"type\":\"position\", \"value\":"+String(pos)+"}");
            } else {
              /*
                 Any other message will take the blind to a position
                 Incoming value = 0-100
                 path is now the position
              */
              path = maxPosition * res.toInt() / 100;
              setPos = path; //Copy path for responding to updates
              action = "auto";

              //Send the instruction to all connected devices
              webSocket.broadcastTXT("{ \"type\":\"set\", \"value\":"+String(res.toInt())+"}");
            }}
            break;
        case WStype_BIN:
            Serial.printf("[%u] get binary length: %u\n", num, length);
            hexdump(payload, length);

            // send message to client
            // webSocket.sendBIN(num, payload, length);
            break;
    }
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

void setup(void)
{
  Serial.begin(115200);
  delay(100);
  Serial.print("Starting now\n");

  action = "";

  WiFi.hostname(config_name);

  //Define customer parameters for WIFI Manager
  WiFiManagerParameter custom_config_name("Name", "Bonjour name", config_name, 40);

  //Setup WIFI Manager
  WiFiManager wifiManager;

  //reset settings - for testing
  //clean FS, for testing
  //SPIFFS.format();
  //wifiManager.resetSettings();

  wifiManager.setSaveConfigCallback(saveConfigCallback);
  //add all your parameters here
  wifiManager.addParameter(&custom_config_name);
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
  if (!MDNS.begin(config_name)) {
    Serial.println("Error setting up MDNS responder!");
    while(1) {
      delay(1000);
    }
  }
  Serial.println("mDNS responder started");

  // Start TCP (HTTP) server
  server.begin();
  Serial.println("TCP server started");

  // Add service to MDNS-SD
  MDNS.addService("http", "tcp", 80);

  //Start websocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  //Setup OTA
  {

    // Authentication to avoid unauthorized updates
    //ArduinoOTA.setPassword((const char *)"nidayand");

    //ArduinoOTA.setHostname((OTAhostname + "-" + String(ESP.getChipId())).c_str());
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

int tripCounter = 0;
void loop(void)
{
  //OTA client code
  ArduinoOTA.handle();

  //Websocket listner
  webSocket.loop();

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
      small_stepper.step(-1);
      currentPosition = currentPosition - 1;

      //Avoid flooding the websocket
      if (tripCounter > 40){
        int pos = (currentPosition * 100)/maxPosition;
        webSocket.broadcastTXT("{ \"type\":\"position\", \"value\":"+String(pos)+"}");
        tripCounter = 0;
      }
      tripCounter++;

    } else if (currentPosition < path){
      small_stepper.step(1);
      currentPosition = currentPosition + 1;
      //Avoid flooding the websocket
      if (tripCounter > 40){
        int pos = (currentPosition * 100)/maxPosition;
        webSocket.broadcastTXT("{ \"type\":\"position\", \"value\":"+String(pos)+"}");
        tripCounter = 0;
      }
      tripCounter++;
    } else {
      path = 0;
      action = "";
      tripCounter = 0;
      int pos = (currentPosition * 100)/maxPosition;
      webSocket.broadcastTXT("{ \"type\":\"position\", \"value\":"+String(pos)+"}");
      Serial.println("Stopped. Reached wanted position");
      saveItNow = true;
    }

 } else if (action == "manual" && path != 0) {
    /*
       Manually running the blind
    */
    small_stepper.step(path);
    currentPosition = currentPosition + path;
  }


  /**
    Serving the webpage
  */
  {
    // Check if a client has connected
    WiFiClient client = server.available();
    if (!client) {
      return;
    }
    Serial.println("New client");

    // Wait for data from client to become available
    while(client.connected() && !client.available()){
      delay(1);
    }

    // Read the first line of HTTP request
    String req = client.readStringUntil('\r');

    // First line of HTTP request looks like "GET /path HTTP/1.1"
    // Retrieve the "/path" part by finding the spaces
    int addr_start = req.indexOf(' ');
    int addr_end = req.indexOf(' ', addr_start + 1);
    if (addr_start == -1 || addr_end == -1) {
      Serial.print("Invalid request: ");
      Serial.println(req);
      return;
    }
    req = req.substring(addr_start + 1, addr_end);
    Serial.print("Request: ");
    Serial.println(req);
    client.flush();

    String s;
    if (req == "/")
    {
      s = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE html><html><head> <link rel=\"stylesheet\" href=\"https://unpkg.com/onsenui/css/onsenui.css\"> <link rel=\"stylesheet\" href=\"https://unpkg.com/onsenui/css/onsen-css-components.min.css\"> <script src=\"https://unpkg.com/onsenui/js/onsenui.min.js\"></script> <script src=\"https://unpkg.com/jquery/dist/jquery.min.js\"></script> <script>var cversion=\"{VERSION}\"; var wsUri=\"ws://\"+location.host+\":81/\"; var repo=\"motor-on-roller-blind-ws\"; window.fn={}; window.fn.open=function(){var menu=document.getElementById('menu'); menu.open();}; window.fn.load=function(page){var content=document.getElementById('content'); var menu=document.getElementById('menu'); content.load(page) .then(menu.close.bind(menu)).then(setActions());}; var gotoPos=function(percent){doSend(percent);}; var instr=function(action){doSend(\"(\"+action+\")\");}; var setActions=function(){doSend(\"(update)\"); $.get(\"https://api.github.com/repos/nidayand/\"+repo+\"/releases\", function(data){if (data.length>0 && data[0].tag_name !==cversion){$(\"#cversion\").text(cversion); $(\"#nversion\").text(data[0].tag_name); $(\"#update-card\").show();}}); setTimeout(function(){$(\"#arrow-close\").on(\"click\", function(){$(\"#setrange\").val(100);gotoPos(100);}); $(\"#arrow-open\").on(\"click\", function(){$(\"#setrange\").val(0);gotoPos(0);}); $(\"#setrange\").on(\"change\", function(){gotoPos($(\"#setrange\").val())}); $(\"#arrow-up-man\").on(\"click\", function(){instr(\"-1\")}); $(\"#arrow-down-man\").on(\"click\", function(){instr(\"1\")}); $(\"#arrow-stop-man\").on(\"click\", function(){instr(\"0\")}); $(\"#set-start\").on(\"click\", function(){instr(\"start\")}); $(\"#set-max\").on(\"click\", function(){instr(\"max\");});}, 200);}; $(document).ready(function(){setActions();}); var websocket; function init(){websocket=null; ons.notification.toast({message: 'Connecting...', timeout: 1000}); try{websocket=new WebSocket(wsUri); websocket.onerror=function(evt){ons.notification.toast({message: 'Cannot connect to device', timeout: 2000}); setTimeout(function(){init();},5000);}; websocket.onopen=function(evt){ons.notification.toast({message: 'Connected to device', timeout: 2000}); doSend(\"(update)\");}; websocket.onclose=function(evt){ons.notification.toast({message: 'Disconnected. Retrying', timeout: 2000}); setTimeout(function(){init();},5000);}; websocket.onmessage=function(evt){try{var msg=JSON.parse(evt.data); if (msg.type==\"position\"){$(\"#pbar\").attr(\"value\", msg.value);}if (msg.type==\"set\"){$(\"#setrange\").val(msg.value);}}catch(err){}};}catch (e){ons.notification.toast({message: 'Cannot connect to device. Retrying...', timeout: 2000}); setTimeout(function(){init();},5000);};}; function doSend(msg){if (websocket && websocket.readyState==1){websocket.send(msg);}}; window.addEventListener(\"load\", init, false); </script></head><body><ons-splitter> <ons-splitter-side id=\"menu\" side=\"left\" width=\"220px\" collapse swipeable> <ons-page> <ons-list> <ons-list-item onclick=\"fn.load('home.html')\" tappable> Home </ons-list-item> <ons-list-item onclick=\"fn.load('settings.html')\" tappable> Settings </ons-list-item> <ons-list-item onclick=\"fn.load('about.html')\" tappable> About </ons-list-item> </ons-list> </ons-page> </ons-splitter-side> <ons-splitter-content id=\"content\" page=\"home.html\"></ons-splitter-content></ons-splitter><template id=\"home.html\"> <ons-page> <ons-toolbar> <div class=\"left\"> <ons-toolbar-button onclick=\"fn.open()\"> <ons-icon icon=\"md-menu\"></ons-icon> </ons-toolbar-button> </div><div class=\"center\">{NAME}</div></ons-toolbar><ons-card> <div class=\"title\">Adjust position</div><div class=\"content\"><p>Move the slider to the wanted position or use the arrows to open/close to the max positions</p></div><ons-row> <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\"> </ons-col> <ons-col> <ons-progress-bar id=\"pbar\" value=\"75\"></ons-progress-bar> </ons-col> <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\"> </ons-col> </ons-row> <ons-row> <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\"> <ons-icon id=\"arrow-open\" icon=\"fa-arrow-up\" size=\"2x\"></ons-icon> </ons-col> <ons-col> <ons-range id=\"setrange\" style=\"width: 100%;\" value=\"25\"></ons-range> </ons-col> <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\"> <ons-icon id=\"arrow-close\" icon=\"fa-arrow-down\" size=\"2x\"></ons-icon> </ons-col> </ons-row> </ons-card> <ons-card id=\"update-card\" style=\"display:none\"> <div class=\"title\">Update available</div><div class=\"content\">You are running <span id=\"cversion\"></span> and <span id=\"nversion\"></span> is the latest. Go to <a href=\"https://github.com/nidayand/motor-on-roller-blind-ws/releases\">the repo</a> to download</div></ons-card> </ons-page></template><template id=\"settings.html\"> <ons-page> <ons-toolbar> <div class=\"left\"> <ons-toolbar-button onclick=\"fn.open()\"> <ons-icon icon=\"md-menu\"></ons-icon> </ons-toolbar-button> </div><div class=\"center\"> Settings </div></ons-toolbar> <ons-card> <div class=\"title\">Instructions</div><div class=\"content\"> <p> <ol> <li>Use the arrows and stop button to navigate to the top position i.e. the blind is opened</li><li>Click the START button</li><li>Use the down arrow to navigate to the max closed position</li><li>Click the MAX button</li><li>Calibration is completed!</li></ol> </p></div></ons-card> <ons-card> <div class=\"title\">Control</div><ons-row style=\"width:100%\"> <ons-col style=\"text-align:center\"><ons-icon id=\"arrow-up-man\" icon=\"fa-arrow-up\" size=\"2x\"></ons-icon></ons-col> <ons-col style=\"text-align:center\"><ons-icon id=\"arrow-stop-man\" icon=\"fa-stop\" size=\"2x\"></ons-icon></ons-col> <ons-col style=\"text-align:center\"><ons-icon id=\"arrow-down-man\" icon=\"fa-arrow-down\" size=\"2x\"></ons-icon></ons-col> </ons-row> </ons-card> <ons-card> <div class=\"title\">Store</div><ons-row style=\"width:100%\"> <ons-col style=\"text-align:center\"><ons-button id=\"set-start\">Set Start</ons-button></ons-col> <ons-col style=\"text-align:center\">&nbsp;</ons-col> <ons-col style=\"text-align:center\"><ons-button id=\"set-max\">Set Max</ons-button></ons-col> </ons-row> </ons-card> </ons-page></template><template id=\"about.html\"> <ons-page> <ons-toolbar> <div class=\"left\"> <ons-toolbar-button onclick=\"fn.open()\"> <ons-icon icon=\"md-menu\"></ons-icon> </ons-toolbar-button> </div><div class=\"center\"> About </div></ons-toolbar> <ons-card> <div class=\"title\">Motor on a roller blind</div><div class=\"content\"> <p> <ul> <li>3d print files and instructions: <a href=\"https://www.thingiverse.com/thing:2392856\">https://www.thingiverse.com/thing:2392856</a></li><li>MQTT based version on Github: <a href=\"https://github.com/nidayand/motor-on-roller-blind\">https://github.com/nidayand/motor-on-roller-blind</a></li><li>WebSocket based version on Github: <a href=\"https://github.com/nidayand/motor-on-roller-blind-ws\">https://github.com/nidayand/motor-on-roller-blind-ws</a></li><li>Licensed unnder <a href=\"https://creativecommons.org/licenses/by/3.0/\">Creative Commons</a></li></ul> </p></div></ons-card> </ons-page></template></body></html>\r\n\r\n";
      s.replace("{VERSION}","V"+version);
      s.replace("{NAME}",String(config_name));
      Serial.println("Sending 200");
    }
    else
    {
      s = "HTTP/1.1 404 Not Found\r\n\r\n";
      Serial.println("Sending 404");
    }

    //Print page but as max package is 2048 we need to break it down
    while(s.length()>2000){
      String d = s.substring(0,2000);
      client.print(d);
      s.replace(d,"");
    }
    client.print(s);

    Serial.println("Done with client");

  }
}
