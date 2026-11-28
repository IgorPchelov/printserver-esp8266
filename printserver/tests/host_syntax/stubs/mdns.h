// mdns.h stub for host syntax checking: printserver.ino calls the IDF
// mdns_service_instance_name_set() directly (the Arduino ESPmDNS wrapper
// only sets ONE global instance name; the per-service socket name needs
// the IDF API). Behaviour-free here - syntax check only.
#pragma once

inline int mdns_service_instance_name_set(const char*, const char*, const char*) {
  return 0;
}
