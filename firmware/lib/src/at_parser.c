#include "hsm/at_parser.h"
#include <string.h>

void at_parser_init(at_parser_t *parser) {
    if (parser) {
        memset(parser->buf, 0, sizeof(parser->buf));
        parser->len = 0;
        parser->overflow = false;
    }
}

void at_parser_reset(at_parser_t *parser) {
    if (parser) {
        parser->len = 0;
        parser->overflow = false;
        parser->buf[0] = '\0';
    }
}

int at_parser_feed(at_parser_t *parser, uint8_t byte, char *out_line, size_t max_len, bool *line_ready) {
    if (!parser || !out_line || !line_ready || max_len == 0) {
        return -1;
    }

    *line_ready = false;

    if (byte == '\r' || byte == '\n') {
        if (parser->overflow) {
            parser->len = 0;
            parser->overflow = false;
            parser->buf[0] = '\0';
            return -1; /* Signal overflow */
        }

        if (parser->len > 0) {
            parser->buf[parser->len] = '\0';
            size_t copy_len = (parser->len < max_len - 1) ? parser->len : max_len - 1;
            memcpy(out_line, parser->buf, copy_len);
            out_line[copy_len] = '\0';
            *line_ready = true;

            parser->len = 0;
            parser->buf[0] = '\0';
            return 0;
        }

        /* Empty line / extra CR/LF -> ignore */
        return 0;
    }

    if (parser->overflow) {
        /* Still waiting for \r or \n to recover */
        return -1;
    }

    if (parser->len >= sizeof(parser->buf) - 1) {
        parser->overflow = true;
        return -1;
    }

    parser->buf[parser->len++] = (char)byte;
    return 0;
}
