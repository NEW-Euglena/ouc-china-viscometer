#ifndef NETWORK_CONFIG_H
#define NETWORK_CONFIG_H

#include <stdint.h>

/* 最后两个 2KB 页专用于配置，必须与链接脚本保持一致。 */
#define NETWORK_CONFIG_BASE 0x0801F000U
#define NETWORK_CONFIG_PAGE_SIZE 2048U

typedef struct {
  char ssid[33];
  char password[65];
  char server_ip[16];
  uint16_t server_port;
} NetworkConfig;

uint8_t NetworkConfig_Validate(const NetworkConfig *config);
uint8_t NetworkConfig_Parse(const char *frame, NetworkConfig *config, char request_id[9]);
uint8_t NetworkConfig_Load(NetworkConfig *config);
uint8_t NetworkConfig_Save(const NetworkConfig *config);

#endif
