// Practice tools for the Tekken 5 DR rollback build of RPCS3: frame data and position restart.
//
// The launcher starts RPCS3 with RB_T5_PRACTICE=1 and two localhost UDP ports. This module listens for the
// launcher's commands, measures frame data from game memory and sends the results back.
//
//   launcher -> game:  SAVE_POS | RESTART_POS | FD 1 | FD 0 | PING
//   game -> launcher:  HELLO | FD <json record> | STATUS <json> | ACK <name> | NACK <name>
//
// Wiring in the emulator (see README.md): call start_from_env() once when the game boots, on_fight_start() when a
// round begins, and on_frame(frame) once per simulation frame (never during a rollback resimulation).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "framedata.hpp"
#include "framedata_reader.hpp"
#include "udp.hpp"

namespace t5 {

struct PracticeLayout {
  fd::Layout fields;
  bool complete = false;                                          // both player bases and all six fields are set
  std::vector<std::pair<uint32_t, uint32_t>> restart_ranges;      // guest address, size
};

namespace detail {
inline std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
inline std::vector<std::string> split(const std::string& s, char c) {
  std::vector<std::string> out; std::string cur; std::istringstream in(s);
  while (std::getline(in, cur, c)) out.push_back(trim(cur));
  return out;
}
inline bool num(const std::string& s, uint32_t& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  unsigned long long x = std::strtoull(s.c_str(), &end, 0);   // base 0: 0x.. hex or decimal
  if (*end || x > 0xFFFFFFFFull) return false;
  v = (uint32_t)x;
  return true;
}
inline bool parse_field(const std::string& v, fd::Field& f, std::string& err) {
  auto p = split(v, ',');
  if (p.empty() || p.size() > 4) { err = "expected offset[,size[,mask[,invert]]]"; return false; }
  uint32_t offset = 0, size = 4, mask = 0xFFFFFFFFu, inv = 0;
  if (!num(p[0], offset)) { err = "bad offset '" + p[0] + "'"; return false; }
  if (p.size() > 1 && !num(p[1], size)) { err = "bad size '" + p[1] + "'"; return false; }
  if (size != 1 && size != 2 && size != 4) { err = "size must be 1, 2 or 4"; return false; }
  if (p.size() > 2 && !num(p[2], mask)) { err = "bad mask '" + p[2] + "'"; return false; }
  if (p.size() > 3 && !num(p[3], inv)) { err = "bad invert '" + p[3] + "'"; return false; }
  f.offset = offset; f.size = (uint8_t)size; f.mask = mask; f.invert = inv != 0;
  return true;
}
inline std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; }
  return o;
}
}  // namespace detail

// Layout file: key=value lines, '#' starts a comment. See launcher/lib/practice-link.js for the template.
inline bool parse_layout(const std::string& text, PracticeLayout& out, std::string& err) {
  out = PracticeLayout{};
  enum { P1 = 1, P2 = 2, MOVE = 4, ACT = 8, STUN = 16, ACTIVE = 32, HIT = 64, BLOCK = 128 };
  int seen = 0, ln = 0;
  std::istringstream in(text);
  std::string line;
  auto fail = [&](const std::string& m) { err = "line " + std::to_string(ln) + ": " + m; return false; };
  while (std::getline(in, line)) {
    ++ln;
    auto hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    line = detail::trim(line);
    if (line.empty()) continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) return fail("expected key=value");
    std::string key = detail::trim(line.substr(0, eq)), val = detail::trim(line.substr(eq + 1));
    uint32_t n = 0;
    std::string e;
    if (key == "big_endian") { if (!detail::num(val, n)) return fail("bad number"); out.fields.big_endian = n != 0; }
    else if (key == "p1_base" || key == "p2_base") {
      if (!detail::num(val, n)) return fail("bad address '" + val + "'");
      out.fields.player_base[key == "p2_base"] = n; seen |= key == "p1_base" ? P1 : P2;
    }
    else if (key == "restart_range") {
      auto p = detail::split(val, ',');
      uint32_t a = 0, sz = 0;
      if (p.size() != 2 || !detail::num(p[0], a) || !detail::num(p[1], sz) || sz == 0 || sz > 64u * 1024 * 1024)
        return fail("restart_range needs address,size (size up to 64 MiB)");
      out.restart_ranges.push_back({a, sz});
    }
    else {
      struct { const char* name; fd::Field* f; int bit; } fields[] = {
        {"move_id", &out.fields.move_id, MOVE}, {"actionable", &out.fields.actionable, ACT},
        {"stunned", &out.fields.stunned, STUN}, {"active", &out.fields.active, ACTIVE},
        {"hit", &out.fields.hit, HIT}, {"block", &out.fields.block, BLOCK}};
      bool found = false;
      for (auto& f : fields) if (key == f.name) {
        if (!detail::parse_field(val, *f.f, e)) return fail(key + ": " + e);
        seen |= f.bit; found = true;
      }
      if (!found) return fail("unknown key '" + key + "'");
    }
  }
  out.complete = seen == 255 && out.fields.player_base[0] != 0 && out.fields.player_base[1] != 0;
  return true;
}

struct PracticeConfig {
  int ctrl_port = 0;       // this module listens here (127.0.0.1)
  int event_port = 0;      // the launcher listens here
  bool frame_data = true;
  std::string layout_path;
};

class Practice {
 public:
  // Guest address -> writable host pointer (in RPCS3: vm::base(addr)). Required.
  std::function<uint8_t*(uint32_t)> ptr;
  // Optional: use the emulator's own snapshot for position restart instead of the layout file's restart_range lines.
  std::function<bool()> save_position, restore_position;

  static bool env_config(PracticeConfig& c) {
    const char* on = std::getenv("RB_T5_PRACTICE");
    if (!on || std::string(on) != "1") return false;
    const char* cp = std::getenv("RB_T5_CTRL_PORT");
    const char* ep = std::getenv("RB_T5_EVENT_PORT");
    if (!cp || !ep) return false;
    c.ctrl_port = std::atoi(cp); c.event_port = std::atoi(ep);
    if (c.ctrl_port < 1 || c.ctrl_port > 65535 || c.event_port < 1 || c.event_port > 65535) return false;
    const char* fdv = std::getenv("RB_T5_FD");
    c.frame_data = !fdv || std::string(fdv) != "0";
    const char* lay = std::getenv("RB_T5_LAYOUT");
    c.layout_path = lay ? lay : "";
    return true;
  }

  bool start_from_env() { PracticeConfig c; return env_config(c) && start(c); }

  bool start(const PracticeConfig& c) {
    udp_ = std::make_unique<net::Udp>((uint16_t)c.ctrl_port, true);
    if (!udp_->bound()) { udp_.reset(); return false; }
    launcher_ = {}; launcher_.sin_family = AF_INET; launcher_.sin_port = htons((uint16_t)c.event_port);
    inet_pton(AF_INET, "127.0.0.1", &launcher_.sin_addr);
    fd_on_ = c.frame_data;
    layout_path_ = c.layout_path;
    load_layout();
    reader_.ptr = [this](uint32_t a) -> const uint8_t* { return ptr ? ptr(a) : nullptr; };
    tracker_.on_record = [this](const fd::Record& r) { send("FD " + fd::to_json(r)); };
    started_ = true;
    send("HELLO");
    send_status();
    return true;
  }

  bool running() const { return started_; }

  // A round begins: the position at this moment is what "restart position" returns to until the player sets another.
  void on_fight_start() {
    if (!started_) return;
    do_save();
    tracker_.reset();
    send_status();
  }

  // Once per simulation frame, after the frame ran. Do not call while resimulating after a rollback.
  void on_frame(int frame) {
    if (!started_) return;
    poll();
    if (fd_on_ && layout_.complete && ptr) {
      fd::Sample s[2];
      reader_.read(s);
      tracker_.update(frame, s);
    }
    if (frame % 60 == 0) send("HELLO");           // heartbeat: the launcher shows "connected" while these arrive
    if (frame % 120 == 0) send_status();
  }

  std::string status_json() const {
    const char* f = !fd_on_ ? "off" : layout_state_;
    return std::string("{\"frameData\":\"") + f + "\",\"position\":\"" + (have_saved_ ? "saved" : "none") +
           "\",\"note\":\"" + detail::json_escape(note_) + "\"}";
  }

  const fd::Tracker& tracker() const { return tracker_; }
  const PracticeLayout& layout() const { return layout_; }

 private:
  void send(const std::string& s) { if (udp_) udp_->send(launcher_, s.data(), s.size()); }
  void send_status() { send("STATUS " + status_json()); }

  void load_layout() {
    layout_ = PracticeLayout{};
    note_.clear();
    std::ifstream f(layout_path_);
    if (layout_path_.empty() || !f) { layout_state_ = "layout missing"; return; }
    std::stringstream ss; ss << f.rdbuf();
    std::string err;
    if (!parse_layout(ss.str(), layout_, err)) { layout_ = PracticeLayout{}; layout_state_ = "layout error"; note_ = err; return; }
    reader_.layout = layout_.fields;
    layout_state_ = layout_.complete ? "on" : "layout missing";
  }

  void poll() {
    char buf[256]; sockaddr_in from{}; int n;
    while ((n = udp_->recv(buf, sizeof buf - 1, from)) >= 0) {
      if (!net::same(from, launcher_)) continue;
      buf[n] = 0;
      std::string cmd = detail::trim(buf);
      handle(cmd);
    }
  }

  void handle(const std::string& cmd) {
    if (cmd == "SAVE_POS") { send(do_save() ? "ACK save" : "NACK save"); send_status(); }
    else if (cmd == "RESTART_POS") {
      bool ok = do_restore();
      if (ok) tracker_.reset();
      send(ok ? "ACK restart" : "NACK restart");
      send_status();
    }
    else if (cmd == "FD 1" || cmd == "FD 0") { fd_on_ = cmd == "FD 1"; if (fd_on_) tracker_.reset(); send_status(); }
    else if (cmd == "PING") send("HELLO");
  }

  bool do_save() {
    if (save_position) { have_saved_ = save_position(); return have_saved_; }
    if (layout_.restart_ranges.empty() || !ptr) {
      note_ = "position restart needs restart_range lines in the layout file";
      return false;
    }
    saved_.clear();
    for (auto& r : layout_.restart_ranges) {
      uint8_t* p = ptr(r.first);
      if (!p) { note_ = "restart_range is not readable"; saved_.clear(); return false; }
      saved_.emplace_back(p, p + r.second);
    }
    note_.clear();
    have_saved_ = true;
    return true;
  }

  bool do_restore() {
    if (restore_position) return restore_position();
    if (!have_saved_ || saved_.size() != layout_.restart_ranges.size()) { note_ = "no position saved yet"; return false; }
    for (size_t i = 0; i < saved_.size(); ++i) {
      uint8_t* p = ptr(layout_.restart_ranges[i].first);
      if (!p) return false;
      std::memcpy(p, saved_[i].data(), saved_[i].size());
    }
    note_.clear();
    return true;
  }

  std::unique_ptr<net::Udp> udp_;
  sockaddr_in launcher_{};
  bool started_ = false, fd_on_ = true, have_saved_ = false;
  std::string layout_path_, note_;
  const char* layout_state_ = "layout missing";
  PracticeLayout layout_;
  fd::MemoryReader reader_;
  fd::Tracker tracker_;
  std::vector<std::vector<uint8_t>> saved_;
};

}  // namespace t5
