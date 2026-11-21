#pragma once
#include <Arduino.h>
bool outboxBegin();
bool outboxFormat();
bool outboxSave(const char *payload,bool event);
bool outboxNext(String &path,String &payload);
void outboxAcknowledge(const String &path);
void outboxReject(const String &path);  // quarantine a payload the server refuses permanently
unsigned outboxDropped();
bool outboxReady();
