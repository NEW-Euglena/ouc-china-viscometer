#include "network_config.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NETWORK_CONFIG_HOST_TEST
#include "main.h"

static void FlashRead(uint32_t address, void *output, size_t size) {
  memcpy(output, (const void *)(uintptr_t)address, size);
}

static uint8_t FlashErase(uint32_t address) {
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t error;
  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.Banks = FLASH_BANK_1;
  erase.Page = (address - FLASH_BASE) / FLASH_PAGE_SIZE;
  erase.NbPages = 1U;
  if (HAL_FLASH_Unlock() != HAL_OK) return 0U;
  const HAL_StatusTypeDef result = HAL_FLASHEx_Erase(&erase, &error);
  HAL_FLASH_Lock();
  return result == HAL_OK;
}

static uint8_t FlashWrite(uint32_t address, uint64_t value) {
  if (HAL_FLASH_Unlock() != HAL_OK) return 0U;
  const HAL_StatusTypeDef result =
      HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, address, value);
  HAL_FLASH_Lock();
  return result == HAL_OK;
}
#else
/* 主机测试注入 Flash 后端，生产固件直接使用 HAL。 */
extern void FlashRead(uint32_t address, void *output, size_t size);
extern uint8_t FlashErase(uint32_t address);
extern uint8_t FlashWrite(uint32_t address, uint64_t value);
#endif

#define CONFIG_VERSION 0x4E434601U
#define CONFIG_COMMIT UINT64_C(0x314746434746434E)

typedef struct {
  uint32_t version;
  uint32_t sequence;
  NetworkConfig config;
  uint32_t crc;
  uint64_t commit;
} ConfigRecord;

_Static_assert(offsetof(ConfigRecord, commit) % 8U == 0U, "Flash alignment");
_Static_assert(sizeof(ConfigRecord) <= NETWORK_CONFIG_PAGE_SIZE, "Flash page capacity");

static uint32_t Crc32(const void *input, size_t size) {
  const uint8_t *bytes = input;
  uint32_t crc = UINT32_MAX;
  for (size_t index = 0U; index < size; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit)
      crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
  }
  return ~crc;
}

static uint8_t ValidText(const char *text, size_t capacity, uint8_t allow_empty) {
  size_t length = 0U;
  while (length < capacity && text[length] != '\0') ++length;
  if (length == capacity || (!allow_empty && length == 0U)) return 0U;
  /* 检查 UTF-8，拒绝过长编码、代理项及换行/空字节。 */
  for (size_t index = 0U; index < length;) {
    const uint8_t first = (uint8_t)text[index++];
    if (first == '\r' || first == '\n') return 0U;
    if (first < 0x80U) continue;
    uint8_t following;
    uint32_t codepoint, minimum;
    if (first >= 0xC2U && first <= 0xDFU) {
      following = 1U; codepoint = first & 0x1FU; minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      following = 2U; codepoint = first & 0x0FU; minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      following = 3U; codepoint = first & 0x07U; minimum = 0x10000U;
    } else return 0U;
    if (index + following > length) return 0U;
    while (following-- != 0U) {
      const uint8_t byte = (uint8_t)text[index++];
      if ((byte & 0xC0U) != 0x80U) return 0U;
      codepoint = (codepoint << 6U) | (byte & 0x3FU);
    }
    if (codepoint < minimum || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) return 0U;
  }
  return 1U;
}

uint8_t NetworkConfig_Validate(const NetworkConfig *config) {
  if (!ValidText(config->ssid, sizeof(config->ssid), 0U) ||
      !ValidText(config->password, sizeof(config->password), 1U) ||
      !ValidText(config->server_ip, sizeof(config->server_ip), 0U) ||
      config->server_port == 0U) return 0U;
  unsigned a, b, c, d;
  char extra, canonical[16];
  if (sscanf(config->server_ip, "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &extra) != 4 ||
      a > 255U || b > 255U || c > 255U || d > 255U) return 0U;
  snprintf(canonical, sizeof(canonical), "%u.%u.%u.%u", a, b, c, d);
  return strcmp(canonical, config->server_ip) == 0;
}

static int HexDigit(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
  return -1;
}

static uint8_t DecodeHex(char *output, size_t capacity, const char *input) {
  const size_t length = strlen(input);
  if (length % 2U != 0U || length / 2U >= capacity) return 0U;
  for (size_t index = 0U; index < length; index += 2U) {
    const int high = HexDigit(input[index]), low = HexDigit(input[index + 1U]);
    if (high < 0 || low < 0 || (high == 0 && low == 0)) return 0U;
    output[index / 2U] = (char)((high << 4) | low);
  }
  output[length / 2U] = '\0';
  return 1U;
}

uint8_t NetworkConfig_Parse(const char *frame, NetworkConfig *config, char request_id[9]) {
  request_id[0] = '\0';
  const size_t length = strlen(frame);
  if (length < 21U || length > 255U || strncmp(frame, "NETCFG1 SET ", 12U) != 0 ||
      frame[20] != ' ') return 0U;
  memcpy(request_id, frame + 12, 8U);
  request_id[8] = '\0';
  for (size_t index = 0U; index < 8U; ++index)
    if (HexDigit(request_id[index]) < 0) { request_id[0] = '\0'; return 0U; }
  /* 明确检查字段分隔，不能把超长标识或拼接字段截成合法参数。 */
  char payload[235];
  strcpy(payload, frame + 21);
  char *parts[4] = {payload, NULL, NULL, NULL};
  for (uint8_t index = 0U; index < 3U; ++index) {
    char *separator = strchr(parts[index], ' ');
    if (separator == NULL || separator == parts[index]) return 0U;
    *separator = '\0';
    parts[index + 1U] = separator + 1;
  }
  if (parts[3][0] == '\0' || strchr(parts[3], ' ') != NULL ||
      strlen(parts[2]) >= sizeof(config->server_ip) || strlen(parts[3]) > 5U) return 0U;
  NetworkConfig decoded = {0};
  if (!DecodeHex(decoded.ssid, sizeof(decoded.ssid), parts[0])) return 0U;
  if (strcmp(parts[1], "-") != 0 &&
      !DecodeHex(decoded.password, sizeof(decoded.password), parts[1])) return 0U;
  strcpy(decoded.server_ip, parts[2]);
  for (const char *digit = parts[3]; *digit != '\0'; ++digit)
    if (*digit < '0' || *digit > '9') return 0U;
  const unsigned long number = strtoul(parts[3], NULL, 10);
  if (number == 0U || number > 65535U) return 0U;
  decoded.server_port = (uint16_t)number;
  if (!NetworkConfig_Validate(&decoded)) return 0U;
  *config = decoded;
  return 1U;
}

static uint8_t ReadRecord(uint8_t slot, ConfigRecord *record) {
  FlashRead(NETWORK_CONFIG_BASE + slot * NETWORK_CONFIG_PAGE_SIZE, record, sizeof(*record));
  return record->commit == CONFIG_COMMIT && record->version == CONFIG_VERSION &&
         record->crc == Crc32(record, offsetof(ConfigRecord, crc)) &&
         NetworkConfig_Validate(&record->config);
}

static int LatestRecord(ConfigRecord records[2]) {
  const uint8_t first = ReadRecord(0U, &records[0]);
  const uint8_t second = ReadRecord(1U, &records[1]);
  if (!first) return second ? 1 : -1;
  if (!second) return 0;
  return (int32_t)(records[1].sequence - records[0].sequence) > 0 ? 1 : 0;
}

uint8_t NetworkConfig_Load(NetworkConfig *config) {
  ConfigRecord records[2];
  const int latest = LatestRecord(records);
  if (latest < 0) return 0U;
  *config = records[latest].config;
  return 1U;
}

uint8_t NetworkConfig_Save(const NetworkConfig *config) {
  if (!NetworkConfig_Validate(config)) return 0U;
  ConfigRecord records[2], record;
  const int latest = LatestRecord(records);
  /* 相同配置不重复擦写；保存失败时有效旧页始终保留。 */
  if (latest >= 0 && memcmp(config, &records[latest].config, sizeof(*config)) == 0) return 1U;
  memset(&record, 0, sizeof(record));
  record.version = CONFIG_VERSION;
  record.sequence = latest < 0 ? 1U : records[latest].sequence + 1U;
  record.config = *config;
  record.crc = Crc32(&record, offsetof(ConfigRecord, crc));
  const uint8_t slot = latest == 0 ? 1U : 0U;
  const uint32_t address = NETWORK_CONFIG_BASE + slot * NETWORK_CONFIG_PAGE_SIZE;
  if (!FlashErase(address)) return 0U;
  for (size_t offset = 0U; offset < offsetof(ConfigRecord, commit); offset += 8U) {
    uint64_t value;
    memcpy(&value, (const uint8_t *)&record + offset, sizeof(value));
    if (!FlashWrite(address + (uint32_t)offset, value)) return 0U;
  }
  /* 有效标记最后写入，未完整提交的页不能参与启动选择。 */
  if (!FlashWrite(address + offsetof(ConfigRecord, commit), CONFIG_COMMIT)) return 0U;
  ConfigRecord verified;
  return ReadRecord(slot, &verified) && memcmp(&verified.config, config, sizeof(*config)) == 0;
}
