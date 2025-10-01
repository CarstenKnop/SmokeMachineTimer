#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <lwip/dns.h>

// Minimal synchronous UDP-based DNS responder (fallback if no Async libs)
// We'll implement a tiny captive DNS that responds with the AP IP to any A query.
class SimpleDNSCaptive {
public:
  bool begin(IPAddress ip, uint16_t port=53) {
    apIP = ip;
    return udp.begin(port);
  }
  void loop() {
    int pktSize = udp.parsePacket();
    if (!pktSize) return;
    if (pktSize > sizeof(buf)) { udp.flush(); return; }
    int len = udp.read(buf, sizeof(buf));
    if (len < 12) return; // header
    uint8_t id0=buf[0], id1=buf[1];
    uint16_t flags = 0x8180; // standard query response, no error
    uint16_t qd = (buf[4]<<8)|buf[5];
    // Only handle 1 question
    if (qd==0) return;
    // Parse QNAME
    int idx=12; while(idx < len && buf[idx]!=0) { idx += buf[idx]+1; }
    if (idx+5 >= len) return; // need null label + qtype + qclass
    // Extract qtype,qclass
    uint16_t qtype = (buf[idx+1]<<8)|buf[idx+2];
    uint16_t qclass = (buf[idx+3]<<8)|buf[idx+4];
    // Build response
    int respLen=0;
    // Copy header
    out[0]=id0; out[1]=id1; out[2]=flags>>8; out[3]=flags & 0xFF; out[4]=0; out[5]=1; // QDCOUNT
    out[6]=0; out[7]= (qtype==1 && qclass==1)?1:0; // ANCOUNT
    out[8]=0; out[9]=0; out[10]=0; out[11]=0; // NS/AR
    // Copy question
    int qlen = idx+5+1 - 12; // up to qclass inclusive plus null label
    memcpy(out+12, buf+12, qlen);
    respLen = 12 + qlen;
    if (qtype==1 && qclass==1) {
      // Answer: pointer to name (0xC00C)
      out[respLen++] = 0xC0; out[respLen++] = 0x0C; // name ptr
      out[respLen++] = 0x00; out[respLen++] = 0x01; // TYPE A
      out[respLen++] = 0x00; out[respLen++] = 0x01; // CLASS IN
      out[respLen++] = 0x00; out[respLen++] = 0x00; out[respLen++] = 0x00; out[respLen++] = 60; // TTL 60s
      out[respLen++] = 0x00; out[respLen++] = 0x04; // RDLENGTH
      out[respLen++] = apIP[0]; out[respLen++] = apIP[1]; out[respLen++] = apIP[2]; out[respLen++] = apIP[3];
    }
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write(out, respLen);
    udp.endPacket();
  }
private:
  WiFiUDP udp;
  IPAddress apIP;
  uint8_t buf[512];
  uint8_t out[512];
};
