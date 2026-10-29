#ifndef HSM_YUBIHSM_PROTO_H_
#define HSM_YUBIHSM_PROTO_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YMS_RSP_SUCCESS_BIT          0x80
#define YMS_RSP_ERROR                0x7f

/* YubiHSM 2 Command Opcodes */
#define YMS_CMD_ECHO                 0x01
#define YMS_CMD_CREATE_SESSION       0x03
#define YMS_CMD_AUTH_SESSION         0x04
#define YMS_CMD_SESSION_MSG          0x05
#define YMS_CMD_DEVICE_INFO          0x06
#define YMS_CMD_RESET_DEVICE         0x07
#define YMS_CMD_CLOSE_SESSION        0x08
#define YMS_CMD_GET_STORAGE_INFO     0x09
#define YMS_CMD_GET_PSEUDO_RANDOM    0x1f

/* Extension DFU / Firmware Update Opcodes */
#define YMS_CMD_DFU_INFO             0x70
#define YMS_CMD_DFU_CHUNK            0x71
#define YMS_CMD_DFU_APPLY            0x72

/* Error Codes */
#define YMS_ERR_OK                   0x00
#define YMS_ERR_INVALID_COMMAND      0x01
#define YMS_ERR_INVALID_DATA         0x02
#define YMS_ERR_INVALID_SESSION      0x03
#define YMS_ERR_AUTH_FAILED          0x04
#define YMS_ERR_SESSION_FULL         0x05
#define YMS_ERR_INSUFFICIENT_PERM    0x06
#define YMS_ERR_STORAGE_FULL         0x07
#define YMS_ERR_DEVICE_LOCKED        0x08
#define YMS_ERR_DFU_FAILED           0x09

#define YMS_MAX_PAYLOAD_SIZE         1024
#define YMS_HEADER_SIZE              3

#pragma pack(push, 1)
typedef struct {
    uint8_t cmd;
    uint8_t len_msb;
    uint8_t len_lsb;
} yms_header_t;

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    uint8_t build;
    uint32_t serial;
    uint8_t log_total;
    uint8_t log_used;
    uint8_t algorithms[8];
} yms_device_info_resp_t;

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    uint32_t active_slot;
    uint32_t image_size;
    uint8_t image_sha256[32];
} yms_firmware_info_resp_t;
#pragma pack(pop)

static inline uint16_t yms_get_len(const yms_header_t *hdr) {
    return (uint16_t)(((uint16_t)hdr->len_msb << 8) | hdr->len_lsb);
}

static inline void yms_set_len(yms_header_t *hdr, uint16_t len) {
    hdr->len_msb = (uint8_t)((len >> 8) & 0xFF);
    hdr->len_lsb = (uint8_t)(len & 0xFF);
}

#ifdef __cplusplus
}
#endif

#endif /* HSM_YUBIHSM_PROTO_H_ */
