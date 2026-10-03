#include "nkb/client.h"

namespace nkb {

Socket connect_and_handshake(const Address& addr, Role role, const std::string& token,
                             int timeout_ms, std::string* err) {
  Socket s = connect_to(addr, timeout_ms, err);
  if (!s.valid()) return Socket();
  Header h = make_header(MsgType::Hello);
  h.flags = static_cast<uint16_t>(role);
  if (!send_message(s.fd(), h, token.data(), token.size(), err)) return Socket();
  Message ack;
  const RecvResult r = recv_message(s.fd(), ack, timeout_ms, 1 << 20, err);
  if (r != RecvResult::Ok) {
    if (err && err->empty()) *err = r == RecvResult::Timeout ? "handshake timed out" : "connection closed during handshake";
    return Socket();
  }
  if (ack.h.type != static_cast<uint16_t>(MsgType::HelloAck) ||
      ack.h.status != static_cast<uint16_t>(Status::Ok)) {
    if (err) *err = "handshake rejected: " + std::string(ack.payload.begin(), ack.payload.end());
    return Socket();
  }
  return s;
}

bool control_request(const Address& addr, const std::string& token, const std::string& cmd,
                     std::string* reply, std::string* err) {
  Socket s = connect_and_handshake(addr, Role::Tool, token, 3000, err);
  if (!s.valid()) return false;
  Header h = make_header(MsgType::Control);
  if (!send_message(s.fd(), h, cmd.data(), cmd.size(), err)) return false;
  Message m;
  const RecvResult r = recv_message(s.fd(), m, 5000, 1 << 20, err);
  if (r != RecvResult::Ok) {
    if (err && err->empty()) *err = "no reply to control command";
    return false;
  }
  *reply = std::string(m.payload.begin(), m.payload.end());
  return m.h.status == static_cast<uint16_t>(Status::Ok);
}

}  // namespace nkb
