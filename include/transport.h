#pragma once
#include <Arduino.h>
void transportBegin();
// Returns the HTTP status (reply filled on 200), or a negative value when no
// HTTP exchange happened (no network, TLS prerequisites missing, modem error).
int transportPost(const String &baseUrl,const char *payload,String &reply);
void transportTestTls(const String &url, Print &out);
void transportShutdown();
int transportSignalDbm();  // 0 when unknown
// The server rejects this exact payload; retrying it would block the outbox forever.
inline bool httpPermanentReject(int status){return status==400||status==413||status==422;}
