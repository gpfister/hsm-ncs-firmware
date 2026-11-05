#ifndef HSM_AT_PARSER_H_
#define HSM_AT_PARSER_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_PARSER_MAX_LINE_LEN 1024

typedef struct {
    char buf[AT_PARSER_MAX_LINE_LEN];
    size_t len;
    bool overflow;
} at_parser_t;

/**
 * Initialize an AT line parser.
 */
void at_parser_init(at_parser_t *parser);

/**
 * Reset internal buffer state.
 */
void at_parser_reset(at_parser_t *parser);

/**
 * Feed a single byte to the parser.
 *
 * @param parser Pointer to parser instance
 * @param byte Input byte
 * @param out_line Buffer where completed line will be copied (must be >= AT_PARSER_MAX_LINE_LEN)
 * @param max_len Size of out_line buffer
 * @param line_ready Set to true if a full line was completed
 * @return 0 on success, -1 on buffer overflow
 */
int at_parser_feed(at_parser_t *parser, uint8_t byte, char *out_line, size_t max_len, bool *line_ready);

#ifdef __cplusplus
}
#endif

#endif /* HSM_AT_PARSER_H_ */
