// Game-independent frame data tracker (header-only, C++17).
//
// Feed it one Sample per player after every simulation frame; it works out, per move:
//   startup   = the frame number of the first active frame (Tekken's "i10" means 10)
//   active    = number of active frames
//   recovery  = frames after the last active frame until the player can act again
//   total     = frames from the first frame of the move until the player can act again
//   on hit / on block = how many frames earlier the attacker can act than the defender
//                       (positive = attacker is plus), measured directly from when each side is free.
// Do not call update() while resimulating after a rollback (practice mode has no rollback).
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fd {

enum class Contact : uint8_t { None, Hit, Block };   // what this player's attack did THIS frame

struct Sample {
  uint32_t move_id = 0;       // current move / animation id
  bool actionable = true;     // can start a new move (neutral, or recovered)
  bool stunned = false;       // in hitstun / blockstun / knockdown: a reaction, never counted as a move
  bool active = false;        // an attack is currently active (hitbox out)
  Contact contact = Contact::None;
};

struct Record {
  int slot = 0;
  uint32_t move_id = 0;
  int start_frame = 0;
  int startup = 0;            // 0 = the move never became active
  int active = 0, recovery = 0, total = 0;
  bool chained = false;       // started by cancelling a previous move (string), not from neutral
  bool cancelled = false;     // was cancelled into another move before the player recovered
  std::optional<int> on_hit, on_block;
};

class Tracker {
 public:
  std::function<void(const Record&)> on_record;   // called when a record is final
  const std::vector<Record>& records() const { return records_; }

  // Forget any move being measured (after a position restart or a state restore).
  void reset() { slot_[0] = Slot{}; slot_[1] = Slot{}; probe_[0] = Probe{}; probe_[1] = Probe{}; }

  // Call once per simulation frame, AFTER the frame ran. `s` holds both players' samples.
  void update(int frame, const Sample s[2]) {
    for (int p = 0; p < 2; ++p) step(p, frame, s[p]);
    for (int p = 0; p < 2; ++p) probe_step(p, frame, s);
  }

 private:
  struct Slot { bool busy = false, contacted = false, seen_active = false; Record rec; int n = 0, last_active = 0; };
  struct Probe {   // measures advantage after a contact
    bool on = false, ready = false, stunned = false;
    Record rec; Contact kind = Contact::None;
    int contact_frame = 0, atk_free = -1, def_free = -1, age = 0;
  };

  void emit(const Record& r) { records_.push_back(r); if (on_record) on_record(r); }

  void drop_probe(int p) {
    Probe& pr = probe_[p];
    if (pr.ready) emit(pr.rec);   // finished move whose advantage could not be measured
    pr.on = false;
  }

  void step(int p, int frame, const Sample& s) {
    Slot& st = slot_[p];
    bool chained = false;
    if (s.stunned) {                       // got hit: any move in progress ends here, a reaction is not a move
      if (st.busy) finish(p, frame, true);
      return;
    }
    if (st.busy) {
      if (s.actionable) { finish(p, frame, false); return; }
      if (s.move_id != st.rec.move_id) { finish(p, frame, true); chained = true; }
    }
    if (!st.busy) {
      if (s.actionable) return;
      st = Slot{};
      st.busy = true; st.rec.slot = p; st.rec.move_id = s.move_id; st.rec.start_frame = frame; st.rec.chained = chained;
    }
    ++st.n;
    if (s.active) {
      if (!st.seen_active) { st.seen_active = true; st.rec.startup = st.n; }
      ++st.rec.active; st.last_active = st.n;
    }
    if (s.contact != Contact::None && !st.contacted) {
      st.contacted = true;
      if (probe_[p].on) drop_probe(p);
      probe_[p] = Probe{};
      probe_[p].on = true; probe_[p].kind = s.contact; probe_[p].contact_frame = frame;
    }
  }

  void finish(int p, int frame, bool cancelled) {
    Slot& st = slot_[p];
    Record r = st.rec;
    r.total = st.n;
    r.cancelled = cancelled;
    r.recovery = (!cancelled && st.seen_active) ? st.n - st.last_active : 0;
    st.busy = false;
    Probe& pr = probe_[p];
    if (st.contacted && pr.on && !cancelled) { pr.ready = true; pr.rec = r; pr.atk_free = frame; }
    else { if (st.contacted && pr.on) pr.on = false; emit(r); }
  }

  void probe_step(int p, int frame, const Sample s[2]) {
    Probe& pr = probe_[p];
    if (!pr.on) return;
    const Sample& d = s[1 - p];
    ++pr.age;
    if (!d.actionable && frame >= pr.contact_frame) pr.stunned = true;
    else if (d.actionable && pr.stunned && pr.def_free < 0) pr.def_free = frame;
    if (pr.ready && pr.def_free >= 0) {
      int adv = pr.def_free - pr.atk_free;
      (pr.kind == Contact::Hit ? pr.rec.on_hit : pr.rec.on_block) = adv;
      emit(pr.rec);
      pr.on = false;
    } else if (pr.age > 400) {
      drop_probe(p);
    }
  }

  Slot slot_[2];
  Probe probe_[2];
  std::vector<Record> records_;
};

inline std::string format(const Record& r) {
  char b[256];
  int n = snprintf(b, sizeof b, "P%d move %u: ", r.slot + 1, r.move_id);
  if (r.startup == 0) n += snprintf(b + n, sizeof b - n, "no active frames, total %d", r.total);
  else n += snprintf(b + n, sizeof b - n, "i%d, active %d, recovery %d, total %d", r.startup, r.active, r.recovery, r.total);
  if (r.on_hit) n += snprintf(b + n, sizeof b - n, " | on hit %+d", *r.on_hit);
  if (r.on_block) n += snprintf(b + n, sizeof b - n, " | on block %+d", *r.on_block);
  if (r.cancelled) n += snprintf(b + n, sizeof b - n, " | cancelled into next move");
  return b;
}

inline std::string to_json(const Record& r) {   // one line, for the launcher to read
  char b[320];
  int n = snprintf(b, sizeof b, "{\"slot\":%d,\"move\":%u,\"startup\":%d,\"active\":%d,\"recovery\":%d,\"total\":%d,\"chained\":%s,\"cancelled\":%s",
                   r.slot, r.move_id, r.startup, r.active, r.recovery, r.total, r.chained ? "true" : "false", r.cancelled ? "true" : "false");
  if (r.on_hit) n += snprintf(b + n, sizeof b - n, ",\"on_hit\":%d", *r.on_hit);
  if (r.on_block) n += snprintf(b + n, sizeof b - n, ",\"on_block\":%d", *r.on_block);
  snprintf(b + n, sizeof b - n, "}");
  return b;
}

}  // namespace fd
