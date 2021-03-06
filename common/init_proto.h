#ifndef __stork_init_proto_H__
#define __stork_init_proto_H__

#include <stdint.h>

#define STK_MAX_PKT_SZ (2 * 1024 * 1024)
#define STK_ARG_MAX (64 * 1024)
#define ENV_ARG_MAX (64 * 1024)

struct stkinitmsg {
  uint16_t sim_req;
  uint32_t sim_flags;
  union {
    struct {
      int argc, envc;
    } run;
    struct {
      pid_t which;
      int sig;
    } kill;
    struct {
      int dir;
      uint16_t dom_len, tgt_len;
    } modhost;
  } un;
  char after[];
};

#define STK_ARGS(msg) ((msg)->after)

#define STK_REQ_RUN  0x0001
#define STK_REQ_KILL 0x0002
#define STK_REQ_MOD_HOST_ENTRY 0x0003

// The process follows the kite initialization protocol. Set this flag
// to wait for the process to really start
#define STK_RUN_FLAG_KITE   0x00000001

#define STK_RUN_FLAG_STDIN  0x00000002
#define STK_RUN_FLAG_STDOUT 0x00000004
#define STK_RUN_FLAG_STDERR 0x00000008

// Causes the process to send its return code on a pipe when it exits
#define STK_RUN_FLAG_WAIT   0x00000010

#endif
