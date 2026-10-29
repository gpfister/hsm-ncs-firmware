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

#ifdef __cplusplus
}
#endif

#endif /* DFU_SERVICE_H_ */
