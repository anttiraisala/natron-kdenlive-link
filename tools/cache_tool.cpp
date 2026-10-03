// natron-kdenlive-cache - inspect and clear the daemon's output cache.
//   --status   print cache and queue statistics (key=value lines)
//   --clear    drop all cached frames
//   --ping     check that the daemon answers
//   --address A   override filter_address
#include "common.h"

using namespace nkb;

int main(int argc, char** argv) {
  nkbtools::Args args(argc, argv);
  std::string cmd;
  if (args.has("status")) cmd = "stats";
  else if (args.has("clear")) cmd = "clear";
  else if (args.has("ping")) cmd = "ping";
  else {
    std::puts("usage: natron-kdenlive-cache --status | --clear | --ping [--address ADDR]");
    return args.has("help") ? 0 : 2;
  }
  nkbtools::Env env;
  std::string err;
  if (!nkbtools::load_env(&env, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
  Address addr;
  if (!Address::parse(args.str("address", env.cfg.get("daemon", "filter_address")), &addr, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  std::string reply;
  if (!control_request(addr, env.token, cmd, &reply, &err)) {
    std::fprintf(stderr, "error: %s\n", err.empty() ? reply.c_str() : err.c_str());
    return 1;
  }
  std::printf("%s\n", reply.c_str());
  return 0;
}
