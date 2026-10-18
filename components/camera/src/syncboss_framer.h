// SPDX-License-Identifier: MIT
// Complete SyncBoss frames across arbitrary read boundaries. No MCU commands.
#ifndef OQ_SYNCBOSS_FRAMER_H
#define OQ_SYNCBOSS_FRAMER_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
struct oq_sb_framer {
  uint8_t bytes[261];
  size_t used;
};
typedef void (*oq_sb_packet_fn)(void *, uint8_t, const uint8_t *, uint8_t);
static inline void oq_sb_feed(struct oq_sb_framer *f, const uint8_t *data,
                              size_t n, oq_sb_packet_fn callback,
                              void *context) {
  for (size_t i = 0; i < n; i++) {
    f->bytes[f->used++] = data[i];
    while (f->used >= 5 && !(f->bytes[0] == 1 && f->bytes[1] == 3 &&
                             f->bytes[2] == 0 && f->bytes[4] == 0)) {
      memmove(f->bytes, f->bytes + 1, --f->used);
    }
    if (f->used >= 6) {
      size_t total = 6 + f->bytes[5];
      if (f->used == total) {
        callback(context, f->bytes[3], f->bytes + 6, f->bytes[5]);
        f->used = 0;
      }
    }
  }
}
#endif
