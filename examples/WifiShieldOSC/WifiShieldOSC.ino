//install OpenBCI_Wifi, OSC, ArduinoJson (the older 5.13.4) libraries
//select board 'Generic ESP8266 Module' and set cpu freq to 160MHz
//set Builtin Led to 2

//hold down PRG (gpio0) on power up to upload new firmware
//press PRG (gpio0) to enter wifi configuration

//TODO:
//wifi config reset
//check udp package max size
//deal with errors in oscCommand
//extract reply and send back in oscCommand
//test more commands http://docs.openbci.com/OpenBCI%20Software/04-OpenBCI_Cyton_SDK

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

#define OSCINPORT 13999  //EDIT input osc port
int udpPort = 57120; //EDIT output osc port (supercollider by default)
char *espname = "OpenBCI_WifiShield";
IPAddress udpAddress;
unsigned long lastSendToClient;
WiFiUDP clientUDP;
Ticker ticker;

void tick() {
  int state = digitalRead(LED_BUILTIN);
  digitalWrite(LED_BUILTIN, !state);
}
void configModeCallback(WiFiManager *myWiFiManager) {
  ticker.attach(0.2, tick);
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
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(0, INPUT);
  ticker.attach(0.6, tick);
  WiFi.hostname(espname);
  wifi_station_set_hostname(espname);
  WiFiManager wifiManager;
  wifiManager.setAPCallback(configModeCallback);
  if (!wifiManager.autoConnect(espname)) {
    ESP.reset();
    delay(2000);
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

  // The master has read the status register
  SPISlave.onStatusSent([]() {
    SPISlave.setStatus(209);
  });

  // Setup SPI Slave registers and pins
  SPISlave.begin();

  // Set the status register (if the master reads it, it will read this value)
  SPISlave.setStatus(209);
  SPISlave.setData(wifi.passthroughBuffer, BYTES_PER_SPI_PACKET);

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
  uint8_t res = wifi.passthroughCommands(str);
  if (res < PASSTHROUGH_PASS) {
    //TODO deal with errors
  } else {
    OSCMessage rpl("/res");
    rpl.add(int(res));
    //TODO extract reply and send back
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
void oscInfo(OSCMessage &msg) {
  OSCMessage rpl("/info");
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
      oscMsg.dispatch("/info", oscInfo);
    }
  }

  int packetsToSend = wifi.rawBufferHead - wifi.rawBufferTail;
  if (packetsToSend < 0) {
    packetsToSend = NUM_PACKETS_IN_RING_BUFFER_RAW + packetsToSend; // for wrap around
  }
  if (packetsToSend > MAX_PACKETS_PER_SEND_TCP) { //TODO test
    packetsToSend = MAX_PACKETS_PER_SEND_TCP;
  }
  if ((micros() > (lastSendToClient + wifi.getLatency()) || packetsToSend == MAX_PACKETS_PER_SEND_TCP) && (packetsToSend > 0)) {

    OSCMessage msg("/data");
    uint32_t taily = wifi.rawBufferTail;
    for (uint8_t i = 0; i < packetsToSend; i++) {
      if (taily >= NUM_PACKETS_IN_RING_BUFFER_RAW) {
        taily = 0;
      }
      uint8_t *buf = wifi.rawBuffer[taily];
      uint8_t stopByte = buf[0];

      msg.add(uint16_t(buf[1])); //sample number

      for (uint8_t j = 0; j < 8; j++) { //eight channels of eeg data
        int16_t msb = buf[j * 3 + 2];
        if (msb & 128) {
          msb = -256 + msb;
        }
        msg.add((msb << 16) + (buf[j * 3 + 3] << 8) + buf[j * 3 + 4]);
      }

      switch (stopByte) { //check which type of aux bytes
        case 0xC0:
          if (buf[26] != 0 || buf[27] != 0 || buf[28] != 0 || buf[29] != 0 || buf[30] != 0 || buf[31] != 0) {
            OSCMessage acc("/acc");
            for (uint8_t k = 0; k < 3; k++) { //accelerometer xyz
              int16_t msb = buf[k * 2 + 26];
              if (msb & 128) {
                msb = -256 + msb;
              }
              acc.add((msb << 8) + buf[k * 2 + 27]);
            }
            sendMsg(acc);
            acc.empty();
          }
          break;
      }

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
    ESP.eraseConfig();  //TODO does not work
    WiFi.disconnect();
    delay(1000);
    ESP.restart();
    delay(1000);
  }
}
