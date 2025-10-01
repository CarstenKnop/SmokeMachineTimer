#pragma once
#include <Arduino.h>
#include <DNSServer.h>

class CaptivePortalDNS {
public:
  bool begin(IPAddress apIP, const char* domain="*") {
    // Using wildcard domain to catch all
    return dns.start(53, domain, apIP);
  }
  void loop() { dns.processNextRequest(); }
private:
  DNSServer dns;
};
