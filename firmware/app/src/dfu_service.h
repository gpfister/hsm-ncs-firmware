#ifndef DFU_SERVICE_H_
#define DFU_SERVICE_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int dfu_service_init(void);
int dfu_service_write_chunk(uint32_t offset, const uint8_t *data, size_t len);
int dfu_service_apply(void);
int dfu_service_get_info(uint8_t *major, uint8_t *minor, uint8_t *patch,
                         uint32_t *active_slot, uint32_t *image_size, uint8_t sha256[32]);

#ifdef __cplusplus
}
#endif

#endif /* DFU_SERVICE_H_ */
