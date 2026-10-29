#include <string.h>
#include "hsm/hsm_core.h"
#include "hsm/yubihsm_proto.h"

static hsm_state_t g_state = HSM_STATE_UNCONFIGURED;
static hsm_callbacks_t g_cbs = {0};

/* Mock/Default Hardware Serial Number & Firmware Version */
#define FIRMWARE_MAJOR 1
#define FIRMWARE_MINOR 0
#define FIRMWARE_PATCH 0
#define DEVICE_SERIAL  0x52840001

void hsm_init(const hsm_callbacks_t *cbs) {
    if (cbs) {
        g_cbs = *cbs;
    }
    /* Start unconfigured (pulsating blue) */
    g_state = HSM_STATE_UNCONFIGURED;
    if (g_cbs.on_state_changed) {
        g_cbs.on_state_changed(g_state);
    }
}

hsm_state_t hsm_get_state(void) {
    return g_state;
}

void hsm_set_state(hsm_state_t new_state) {
    if (g_state != new_state) {
        g_state = new_state;
        if (g_cbs.on_state_changed) {
            g_cbs.on_state_changed(g_state);
        }
    }
}

static void build_error(uint8_t *resp, size_t *resp_len, uint8_t err_code) {
    resp[0] = YMS_RSP_ERROR;
    resp[1] = 0x00;
    resp[2] = 0x01;
    resp[3] = err_code;
    *resp_len = 4;
}

int hsm_process_packet(const uint8_t *req, size_t req_len,
                       uint8_t *resp, size_t max_resp_len, size_t *resp_len) {
    if (!req || req_len < YMS_HEADER_SIZE || !resp || !resp_len || max_resp_len < 4) {
        return -1;
    }

    const yms_header_t *in_hdr = (const yms_header_t *)req;
    uint16_t payload_len = yms_get_len(in_hdr);

    if ((size_t)(payload_len + YMS_HEADER_SIZE) > req_len) {
        build_error(resp, resp_len, YMS_ERR_INVALID_DATA);
        return 0;
    }

    const uint8_t *payload = req + YMS_HEADER_SIZE;
    yms_header_t *out_hdr = (yms_header_t *)resp;

    switch (in_hdr->cmd) {
    case YMS_CMD_ECHO: {
        /* Echo payload back */
        if (payload_len + YMS_HEADER_SIZE > max_resp_len) {
            build_error(resp, resp_len, YMS_ERR_INVALID_DATA);
            return 0;
        }
        out_hdr->cmd = YMS_CMD_ECHO | YMS_RSP_SUCCESS_BIT;
        yms_set_len(out_hdr, payload_len);
        if (payload_len > 0) {
            memcpy(resp + YMS_HEADER_SIZE, payload, payload_len);
        }
        *resp_len = payload_len + YMS_HEADER_SIZE;
        return 0;
    }

    case YMS_CMD_DEVICE_INFO: {
        yms_device_info_resp_t dev_info;
        memset(&dev_info, 0, sizeof(dev_info));
        dev_info.major = 2; /* YubiHSM 2 compatible */
        dev_info.minor = 4;
        dev_info.patch = 0;
        dev_info.build = 1;
        dev_info.serial = DEVICE_SERIAL;
        dev_info.log_total = 64;
        dev_info.log_used = 0;
        /* Supported algorithms: RSA, ECDSA, AES, HMAC */
        dev_info.algorithms[0] = 0x01; // RSA_PKCS1_SHA256
        dev_info.algorithms[1] = 0x02; // EC_P256
        dev_info.algorithms[2] = 0x03; // HMAC_SHA256

        out_hdr->cmd = YMS_CMD_DEVICE_INFO | YMS_RSP_SUCCESS_BIT;
        yms_set_len(out_hdr, sizeof(dev_info));
        memcpy(resp + YMS_HEADER_SIZE, &dev_info, sizeof(dev_info));
        *resp_len = sizeof(dev_info) + YMS_HEADER_SIZE;
        return 0;
    }

    case YMS_CMD_CREATE_SESSION:
    case YMS_CMD_AUTH_SESSION: {
        /* Authenticate / Unlock device */
        hsm_set_state(HSM_STATE_UNLOCKED);
        out_hdr->cmd = in_hdr->cmd | YMS_RSP_SUCCESS_BIT;
        yms_set_len(out_hdr, 1);
        resp[YMS_HEADER_SIZE] = YMS_ERR_OK;
        *resp_len = 1 + YMS_HEADER_SIZE;
        return 0;
    }

    case YMS_CMD_CLOSE_SESSION: {
        /* Lock device */
        hsm_set_state(HSM_STATE_LOCKED);
        out_hdr->cmd = YMS_CMD_CLOSE_SESSION | YMS_RSP_SUCCESS_BIT;
        yms_set_len(out_hdr, 1);
        resp[YMS_HEADER_SIZE] = YMS_ERR_OK;
        *resp_len = 1 + YMS_HEADER_SIZE;
        return 0;
    }

    case YMS_CMD_RESET_DEVICE: {
        /* Reset back to unconfigured state */
        hsm_set_state(HSM_STATE_UNCONFIGURED);
        out_hdr->cmd = YMS_CMD_RESET_DEVICE | YMS_RSP_SUCCESS_BIT;
        yms_set_len(out_hdr, 1);
        resp[YMS_HEADER_SIZE] = YMS_ERR_OK;
        *resp_len = 1 + YMS_HEADER_SIZE;
        return 0;
    }

    case YMS_CMD_GET_PSEUDO_RANDOM: {
        /* Return requested count of pseudo-random bytes */
        uint16_t requested = 32;
        if (payload_len >= 2) {
            requested = (uint16_t)(((uint16_t)payload[0] << 8) | payload[1]);
        }
        if (requested > 256) requested = 256;
        if (requested + YMS_HEADER_SIZE > max_resp_len) {
            requested = max_resp_len - YMS_HEADER_SIZE;
        }

        out_hdr->cmd = YMS_CMD_GET_PSEUDO_RANDOM | YMS_RSP_SUCCESS_BIT;
        yms_set_len(out_hdr, requested);
        uint8_t *rnd_buf = resp + YMS_HEADER_SIZE;
        /* Simple PRNG fill for mock */
        for (uint16_t i = 0; i < requested; i++) {
            rnd_buf[i] = (uint8_t)((i * 37 + 0x5a) ^ (DEVICE_SERIAL & 0xFF));
        }
        *resp_len = requested + YMS_HEADER_SIZE;
        return 0;
    }

    case YMS_CMD_DFU_INFO: {
        yms_firmware_info_resp_t fw_info;
        memset(&fw_info, 0, sizeof(fw_info));
        fw_info.major = FIRMWARE_MAJOR;
        fw_info.minor = FIRMWARE_MINOR;
        fw_info.patch = FIRMWARE_PATCH;
        fw_info.active_slot = 0;
        fw_info.image_size = 0x40000;
        memset(fw_info.image_sha256, 0xAA, 32);

        out_hdr->cmd = YMS_CMD_DFU_INFO | YMS_RSP_SUCCESS_BIT;
        yms_set_len(out_hdr, sizeof(fw_info));
        memcpy(resp + YMS_HEADER_SIZE, &fw_info, sizeof(fw_info));
        *resp_len = sizeof(fw_info) + YMS_HEADER_SIZE;
        return 0;
    }

    case YMS_CMD_DFU_CHUNK: {
        if (payload_len < 4) {
            build_error(resp, resp_len, YMS_ERR_INVALID_DATA);
            return 0;
        }
        uint32_t offset = (uint32_t)((payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3]);
        const uint8_t *chunk_data = payload + 4;
        size_t chunk_len = payload_len - 4;

        int rc = 0;
        if (g_cbs.on_dfu_write) {
            rc = g_cbs.on_dfu_write(offset, chunk_data, chunk_len);
        }

        if (rc != 0) {
            build_error(resp, resp_len, YMS_ERR_DFU_FAILED);
            return 0;
        }

        out_hdr->cmd = YMS_CMD_DFU_CHUNK | YMS_RSP_SUCCESS_BIT;
        yms_set_len(out_hdr, 1);
        resp[YMS_HEADER_SIZE] = YMS_ERR_OK;
        *resp_len = 1 + YMS_HEADER_SIZE;
        return 0;
    }

    case YMS_CMD_DFU_APPLY: {
        int rc = 0;
        if (g_cbs.on_dfu_apply) {
            rc = g_cbs.on_dfu_apply();
        }
        if (rc != 0) {
            build_error(resp, resp_len, YMS_ERR_DFU_FAILED);
            return 0;
        }
        out_hdr->cmd = YMS_CMD_DFU_APPLY | YMS_RSP_SUCCESS_BIT;
        yms_set_len(out_hdr, 1);
        resp[YMS_HEADER_SIZE] = YMS_ERR_OK;
        *resp_len = 1 + YMS_HEADER_SIZE;
        return 0;
    }

    default:
        build_error(resp, resp_len, YMS_ERR_INVALID_COMMAND);
        return 0;
    }
}
