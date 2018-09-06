#ifndef __kite_local_proto_H__
#define __kite_local_proto_H__

#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

#include "util.h"

#define KITE_MAX_LOCAL_MSG_SZ 4096
#define KITE_LOCAL_API_SOCK "applianced-control"

struct kitelocalmsg {
  uint16_t klm_req;
  uint16_t klm_req_flags;
} KITE_PACKED;

struct kitelocalattr {
  uint16_t kla_name;
  uint16_t kla_length; // Includes sizeof (kitelocalattr)
} KITE_PACKED;

#define KLM_REQ_OP(msg)        (ntohs((msg)->klm_req) & 0x00FF)
#define KLM_REQ_GET    0x00
#define KLM_REQ_CREATE 0x01
#define KLM_REQ_DELETE 0x02
#define KLM_REQ_UPDATE 0x03
#define KLM_REQ_STOP   0x04 // Kills running containers
#define KLM_REQ_SUB    0x05 // Runs a command within a container

#define KLM_REQ_ENTITY(msg)    (ntohs((msg)->klm_req) & 0x7F00)
#define KLM_REQ_ENTITY_PERSONA 0x0100
#define KLM_REQ_ENTITY_APP     0x0200
#define KLM_REQ_ENTITY_FLOCK   0x0300

#define KLM_RESPONSE           0x8000

#define KLM_IS_END(msg)        (ntohs((msg)->klm_req_flags) & KLM_IS_LAST)
#define KLM_RETURN_MULTIPLE    0x0001
#define KLM_IS_LAST            0x0002

#define KLA_RESPONSE_CODE      0x0000
#define KLA_PERSONA_ID         0x0001
#define KLA_APP_URL            0x0002
#define KLA_APP_MANIFEST_URL   0x0003
#define KLA_REQUEST_ATTRIBUTE  0x0004
#define KLA_APP_PERMISSION     0x0005
#define KLA_FLOCK_URL          0x0006
#define KLA_ENTITY             0x0007
#define KLA_MESSAGE            0x0008
#define KLA_OPERATION          0x0009
#define KLA_SYSTEM_ERROR       0x000A
#define KLA_FLOCK_SIGNATURE    0x000B
#define KLA_FLOCK_STATUS       0x000C
#define KLA_PERSONA_DISPLAYNM  0x000D
#define KLA_PERSONA_PASSWORD   0x000E

#define KLE_SUCCESS            0x0000
#define KLE_NOT_IMPLEMENTED    0x0001
#define KLE_BAD_ENTITY         0x0002
#define KLE_BAD_OP             0x0003
#define KLE_MISSING_ATTRIBUTES 0x0004
#define KLE_INVALID_URL        0x0005
#define KLE_SYSTEM_ERROR       0x0006

#define KLM_SIZE_INIT sizeof(struct kitelocalmsg)
#define KLA_PAYLOAD_SIZE(a) (ntohs((a)->kla_length) - sizeof(struct kitelocalattr))
#define KLA_SIZE(data_sz) (sizeof(struct kitelocalattr) + data_sz)
#define KLM_SIZE_ADD_ATTR(sz, attr) (sz) += 4 * (((htons((attr)->kla_length)) + 3) / 4)
#define KLM_FIRSTATTR(msg, sz) ((sz) <= sizeof(struct kitelocalmsg) ? NULL : ((struct kitelocalattr *) (((uintptr_t) msg) + sizeof(struct kitelocalmsg))))
#define KLM_NEXTATTR_(attr) (((uintptr_t) attr) + 4 * ((htons((attr)->kla_length) + 3) / 4))
#define KLM_NEXTATTR(msg, attr, sz) (((KLM_NEXTATTR_(attr) - ((uintptr_t) msg)) < sz) ? (struct kitelocalattr *) KLM_NEXTATTR_(attr) : NULL)
#define KLA_DATA(attr, buf, sz)                                         \
  ((((((uintptr_t) attr) + sizeof(struct kitelocalattr)) - ((uintptr_t) buf)) < sz) ? \
   (char *) (((uintptr_t) attr) + sizeof(struct kitelocalattr)) : NULL)
#define KLA_DATA_AS(attr, buf, sz, type) ((type) (KLA_DATA(attr, buf, sz)))
#define KLA_DATA_UNSAFE(attr, type) ((type) (((uintptr_t)attr) + sizeof(struct kitelocalattr)))
#define KLA_NAME(attr) ntohs((attr)->kla_name)

#endif
