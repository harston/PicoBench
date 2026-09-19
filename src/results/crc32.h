#ifndef PICOBENCH_CRC32_H
#define PICOBENCH_CRC32_H

#include <stdint.h>
#include <stddef.h>

// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), the same as zlib's crc32().
// Start with crc = 0; feed the previous result to continue over more data.
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

#endif
