// Reads fd::Sample for both players straight out of emulated game memory.
// You fill in a Layout once you have found the player structs and their fields with the memory viewer.
#pragma once
#include <cstdint>
#include <functional>
#include "framedata.hpp"

namespace fd {

struct Field {
  uint32_t offset = 0;        // byte offset inside the player struct
  uint8_t size = 4;           // 1, 2 or 4 bytes
  uint32_t mask = 0xFFFFFFFFu;
  bool invert = false;        // for flags: true if the game stores "busy" and you want "actionable"
};

struct Layout {
  uint32_t player_base[2] = {0, 0};   // guest address of each player's state struct
  Field move_id, actionable, stunned, active, hit, block;
  bool big_endian = true;             // PS3 guest memory is big-endian
};

class MemoryReader {
 public:
  Layout layout;
  std::function<const uint8_t*(uint32_t)> ptr;   // guest address -> host pointer (in RPCS3: vm::base)

  void read(Sample out[2]) const {
    for (int p = 0; p < 2; ++p) {
      uint32_t base = layout.player_base[p];
      Sample& s = out[p];
      s.move_id = load(base, layout.move_id) & layout.move_id.mask;
      s.actionable = flag(base, layout.actionable);
      s.stunned = flag(base, layout.stunned);
      s.active = flag(base, layout.active);
      s.contact = flag(base, layout.hit) ? Contact::Hit : flag(base, layout.block) ? Contact::Block : Contact::None;
    }
  }

 private:
  uint32_t load(uint32_t base, const Field& f) const {
    const uint8_t* q = ptr(base + f.offset);
    uint32_t v = 0;
    for (int i = 0; i < f.size; ++i) v = layout.big_endian ? (v << 8) | q[i] : v | ((uint32_t)q[i] << (8 * i));
    return v;
  }
  bool flag(uint32_t base, const Field& f) const { return ((load(base, f) & f.mask) != 0) != f.invert; }
};

}  // namespace fd
