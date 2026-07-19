// ota.h -- firmware update: the raw-body upload handlers + mDNS init.

#ifndef SFGW_OTA_H
#define SFGW_OTA_H

#include "common.h"
#include <esp_http_server.h>

void otaInit();
void wifiSetApActive(bool up);
esp_err_t handleOTAPage(httpd_req_t* r);
esp_err_t handleOTAUpload(httpd_req_t* r);

#endif // SFGW_OTA_H
