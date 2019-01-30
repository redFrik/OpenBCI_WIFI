//install esp8266 (version 2.4.2) board manager
//install OpenBCI_Wifi, OSC, ArduinoJson (the older 5.13.4) libraries
//select board 'Generic ESP8266 Module' and set cpu freq to "80 MHz"
//set Builtin Led to "2" and Flash Size to "4M (no SPIFFS)"

//hold PRG (gpio0) on power up to upload new firmware
//press PRG (gpio0) to erase wifi configuration and restart - TODO does not work good - esp bug

//TODO:
//adapt for Ganglion and CytonDaisy

#define ARDUINO_ARCH_ESP8266
#define ESP8266

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include "SPISlave.h"
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <OSCData.h>
#include <Ticker.h>
#include "OpenBCI_Wifi_Definitions.h"
#include "OpenBCI_Wifi.h"

#define DEFAULT_LATENCY 5000  //override OpenBCI_Wifi_Definitions
#define MAX_PACKETS_PER_SEND_OSC 39 //ensure no segmented packages
#define OSCINPORT 13999  //EDIT input osc port
int udpPort = 57120; //EDIT output osc port (supercollider by default)
char *espname = "OpenBCI_WifiShieldOSC";
IPAddress udpAddress;
unsigned long lastSendToClient;
WiFiUDP clientUDP;
Ticker ticker;

void tick() {
  int state = digitalRead(LED_BUILTIN);
  digitalWrite(LED_BUILTIN, !state);
}
void configModeCallback(WiFiManager *myWiFiManager) {
  ticker.attach(0.15, tick);
}

void oscReady() {
  OSCMessage msg("/ready");
  msg.add(OSCINPORT);
  clientUDP.beginPacket(udpAddress, udpPort);
  msg.send(clientUDP);
  clientUDP.endPacket();
  yield();
  msg.empty();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  ticker.attach(0.6, tick);

  pinMode(0, INPUT);
  WiFi.hostname(espname);
  wifi_station_set_hostname(espname);
  WiFiManager wifiManager;
  wifiManager.setAPCallback(configModeCallback);
  if (!wifiManager.autoConnect(espname)) {
    ESP.reset();
    delay(1000);
  }
  MDNS.begin(espname);  //make .local work
  udpAddress = WiFi.localIP();
  udpAddress[3] = 255;  //by default use broadcast ip x.x.x.255
  clientUDP.begin(OSCINPORT);

  SPISlave.onData([](uint8_t * data, size_t len) {
    wifi.spiProcessPacket(data);
  });
  SPISlave.onDataSent([]() {
    wifi.spiOnDataSent();
    SPISlave.setData(wifi.passthroughBuffer, BYTES_PER_SPI_PACKET);
  });
  SPISlave.onStatusSent([]() {
    SPISlave.setStatus(209);
  });
  SPISlave.begin();
  SPISlave.setStatus(209);
  SPISlave.setData(wifi.passthroughBuffer, BYTES_PER_SPI_PACKET);

  wifi.setLatency(DEFAULT_LATENCY);
  oscReady();
  ticker.detach();
  digitalWrite(LED_BUILTIN, HIGH);
}

void oscStart(OSCMessage &msg) {
  wifi.passthroughCommands("b");  //start streaming
  SPISlave.setData(wifi.passthroughBuffer, BYTES_PER_SPI_PACKET);
}
void oscStop(OSCMessage &msg) {
  wifi.passthroughCommands("s");  //stop streaming
  SPISlave.setData(wifi.passthroughBuffer, BYTES_PER_SPI_PACKET);
}
void oscIp(OSCMessage &msg) {
  udpAddress[0] = getIntData(msg, 0);
  udpAddress[1] = getIntData(msg, 1);
  udpAddress[2] = getIntData(msg, 2);
  udpAddress[3] = getIntData(msg, 3);
}
void oscPort(OSCMessage &msg) {
  udpPort = getIntData(msg, 0);
}
void oscLatency(OSCMessage &msg) {
  uint16_t latency = getIntData(msg, 0);
  wifi.setLatency(latency);
}
void oscCommand(OSCMessage &msg) {
  uint8_t len = msg.getDataLength(0);
  char str[len];
  msg.getString(0, str, len);
  uint8_t retVal = wifi.passthroughCommands(str);
  if (retVal < PASSTHROUGH_PASS) {
    OSCMessage rpl("/reply");
    switch (retVal) {
      case PASSTHROUGH_FAIL_TOO_MANY_CHARS:
        rpl.add(501);
        rpl.add("Error: Sent more than 31 chars");
        break;
      case PASSTHROUGH_FAIL_NO_CHARS:
        rpl.add(505);
        rpl.add("Error: No characters found for key 'command'");
        break;
      case PASSTHROUGH_FAIL_QUEUE_FILLED:
        rpl.add(503);
        rpl.add("Error: Queue is full, please wait 20ms and try again.");
        break;
      default:
        rpl.add(504);
        rpl.add("Error: Unknown error");
    }
    sendMsg(rpl);
    rpl.empty();
  }
}
void oscVersion(OSCMessage &msg) {
  OSCMessage rpl("/version");
  rpl.add(wifi.getVersion().c_str());
  sendMsg(rpl);
  rpl.empty();
}
void oscName(OSCMessage &msg) {
  OSCMessage rpl("/name");
  rpl.add(wifi.getName().c_str());
  sendMsg(rpl);
  rpl.empty();
}
void oscBoard(OSCMessage &msg) {
  OSCMessage rpl("/board");
  rpl.add(wifi.getInfoBoard().c_str());
  sendMsg(rpl);
  rpl.empty();
}
void oscAll(OSCMessage &msg) {
  OSCMessage rpl("/all");
  rpl.add(wifi.getInfoAll().c_str());
  sendMsg(rpl);
  rpl.empty();
}
void sendMsg(OSCMessage &msg) {
  clientUDP.beginPacket(udpAddress, udpPort);
  msg.send(clientUDP);
  clientUDP.endPacket();
  yield();
}
int getIntData(OSCMessage &msg, int val) {
  if (msg.isInt(val)) {
    return msg.getInt(val);
  }
  return int(msg.getFloat(val));
}

void loop() {

  int packetSize = clientUDP.parsePacket();
  if (packetSize) {
    OSCMessage oscMsg;
    while (packetSize--) {
      oscMsg.fill(clientUDP.read());
    }
    if (!oscMsg.hasError()) {
      oscMsg.dispatch("/start", oscStart);
      oscMsg.dispatch("/stop", oscStop);
      oscMsg.dispatch("/ip", oscIp);
      oscMsg.dispatch("/port", oscPort);
      oscMsg.dispatch("/latency", oscLatency);
      oscMsg.dispatch("/command", oscCommand);
      oscMsg.dispatch("/version", oscVersion);
      oscMsg.dispatch("/name", oscName);
      oscMsg.dispatch("/board", oscBoard);
      oscMsg.dispatch("/all", oscAll);
    }
  }

  if (wifi.clientWaitingForResponseFullfilled) {
    wifi.clientWaitingForResponseFullfilled = false;
    OSCMessage rpl("/reply");
    switch (wifi.curClientResponse) {
      case wifi.CLIENT_RESPONSE_OUTPUT_STRING:
        rpl.add(wifi.outputString.c_str());
        wifi.outputString = "";
        break;
      case wifi.CLIENT_RESPONSE_NONE:
      default:
        rpl.add("");
        break;
    }
    sendMsg(rpl);
    rpl.empty();
  }

  if (wifi.clientWaitingForResponse && (millis() > (wifi.timePassthroughBufferLoaded + 2000))) {
    wifi.clientWaitingForResponse = false;
    OSCMessage rpl("/reply");
    rpl.add(502);
    rpl.add("Error: timeout getting command response, be sure board is fully connected");
    sendMsg(rpl);
    rpl.empty();
    wifi.outputString = "";
  }

  int packetsToSend = wifi.rawBufferHead - wifi.rawBufferTail;
  if (packetsToSend < 0) {
    packetsToSend = NUM_PACKETS_IN_RING_BUFFER_RAW + packetsToSend; // for wrap around
  }
  if (packetsToSend > MAX_PACKETS_PER_SEND_OSC) {
    packetsToSend = MAX_PACKETS_PER_SEND_OSC;
    digitalWrite(LED_BUILTIN, LOW);
  } else {
    digitalWrite(LED_BUILTIN, HIGH);
  }

  if ((micros() > (lastSendToClient + wifi.getLatency()) || packetsToSend == MAX_PACKETS_PER_SEND_OSC) && (packetsToSend > 0)) {

    OSCMessage msg("/data");
    uint32_t taily = wifi.rawBufferTail;
    for (uint8_t i = 0; i < packetsToSend; i++) {
      if (taily >= NUM_PACKETS_IN_RING_BUFFER_RAW) {
        taily = 0;
      }
      msg.add(wifi.rawBuffer[taily], BYTES_PER_SPI_PACKET);
      taily += 1;
    }
    sendMsg(msg);
    msg.empty();
    lastSendToClient = micros();
    wifi.rawBufferTail = taily;
  }

  if (digitalRead(0) == LOW) {
    wifi.passthroughCommands("s");  //stop streaming
    SPISlave.setData(wifi.passthroughBuffer, BYTES_PER_SPI_PACKET);
    while (digitalRead(0) == LOW) {}
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    ESP.reset();
  }
}
