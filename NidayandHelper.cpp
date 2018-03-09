#include "Arduino.h"
#include "NidayandHelper.h"

NidayandHelper::NidayandHelper(){
  this->_configfile = "/config.json";
  this->_mqttclientid = ("ESPClient-" + String(ESP.getChipId()));

}

boolean NidayandHelper::loadconfig(){
  File configFile = SPIFFS.open(this->_configfile, "r");
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
  this->_config = jsonBuffer.parseObject(buf.get());

  if (!this->_config.success()) {
    Serial.println("Failed to parse config file");
    return false;
  }
  return true;
}

JsonVariant NidayandHelper::getconfig(){
  return this->_config;
}

boolean NidayandHelper::saveconfig(JsonVariant json){
  File configFile = SPIFFS.open(this->_configfile, "w");
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

String NidayandHelper::mqtt_gettopic(String type) {
  return "/raw/esp8266/" + String(ESP.getChipId()) + "/" + type;
}


void NidayandHelper::mqtt_reconnect(PubSubClient& psclient){
  return mqtt_reconnect(psclient, String(NULL), String(NULL));
}
void NidayandHelper::mqtt_reconnect(PubSubClient& psclient, std::list<const char*> topics){
  return mqtt_reconnect(psclient, String(NULL), String(NULL), topics);
}
void NidayandHelper::mqtt_reconnect(PubSubClient& psclient, String uid, String pwd){
  std::list<const char*> mylist;
  return mqtt_reconnect(psclient, uid, pwd, mylist);
}
void NidayandHelper::mqtt_reconnect(PubSubClient& psclient, String uid, String pwd, std::list<const char*> topics){
  // Loop until we're reconnected
  boolean mqttLogon = false;
  if (uid!=NULL and pwd != NULL){
    mqttLogon = true;
  }
  while (!psclient.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if ((mqttLogon ? psclient.connect(this->_mqttclientid.c_str(), uid.c_str(), pwd.c_str()) : psclient.connect(this->_mqttclientid.c_str()))) {
      Serial.println("connected");

      //Send register MQTT message with JSON of chipid and ip-address
      this->mqtt_publish(psclient, "/raw/esp8266/register", "{ \"id\": \"" + String(ESP.getChipId()) + "\", \"ip\":\"" + WiFi.localIP().toString() +"\"}");

      //Setup subscription
      if (topics.empty()){
        for (const char* t : topics){
           psclient.subscribe(t);
        }
      }

    } else {
      Serial.print("failed, rc=");
      Serial.print(psclient.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      ESP.wdtFeed();
      delay(5000);
    }
  }
  if (psclient.connected()){
    psclient.loop();
  }
}

void NidayandHelper::mqtt_publish(PubSubClient& psclient, String topic, String payload){
  Serial.println("Trying to send msg..."+topic+":"+payload);
  //Send status to MQTT bus if connected
  if (psclient.connected()) {
    psclient.publish(topic.c_str(), payload.c_str());
  } else {
    Serial.println("PubSub client is not connected...");
  }
}

void NidayandHelper::resetsettings(WiFiManager& wifim){
  SPIFFS.format();
  wifim.resetSettings();
}
