#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "local_proto.h"
#include "commands.h"

const char *kite_error_code_str(uint16_t code) {
  switch ( code ) {
  case KLE_SUCCESS: return "Success";
  case KLE_NOT_IMPLEMENTED: return "Not implemented";
  case KLE_BAD_ENTITY: return "Bad entity";
  case KLE_BAD_OP: return "Bad operation";
  case KLE_MISSING_ATTRIBUTES: return "Missing attributes";
  case KLE_INVALID_URL: return "Invalid URL";
  case KLE_SYSTEM_ERROR: return "System error";
  default: return "Unknown";
  }
}

const char *kite_entity_str(uint16_t entity) {
  entity &= 0x7F00;

  switch ( entity ) {
  case KLM_REQ_ENTITY_PERSONA: return "Persona";
  case KLM_REQ_ENTITY_APP: return "Application";
  case KLM_REQ_ENTITY_FLOCK: return "Flock";
  default: return "Unknown";
  }
}

const char *kite_operation_str(uint16_t otype) {
  otype &= 0x00FF;

  switch ( otype ) {
  case KLM_REQ_GET: return "GET";
  case KLM_REQ_CREATE: return "CREATE";
  case KLM_REQ_DELETE: return "DELETE";
  case KLM_REQ_UPDATE: return "UPDATE";
  case KLM_REQ_STOP: return "STOP";
  case KLM_REQ_SUB: return "SUB";
  default: return "Unknown";
  }
}

void kite_print_attr_data(FILE *out, struct kitelocalattr *attr) {
  uint16_t attr_type = ntohs(attr->kla_name), attr_len = ntohs(attr->kla_length);
  //  uint16_t *attr_d16;

  attr_len -= sizeof(*attr);

  switch ( attr_type ) {
  case KLA_RESPONSE_CODE:
    if ( ntohs(attr->kla_length) == KLA_SIZE(sizeof(uint16_t)) ) {
      fprintf(out, "  Response code: %d\n", ntohs(*KLA_DATA_UNSAFE(attr, uint16_t *)));
      return;
    } else goto unknown;
  case KLA_ENTITY:
    if ( ntohs(attr->kla_length) == KLA_SIZE(sizeof(uint16_t)) ) {
      uint16_t etype = ntohs(*KLA_DATA_UNSAFE(attr, uint16_t *));
      fprintf(out, "  Entity: %s (%x)\n", kite_entity_str(etype), etype);
      return;
    } else goto unknown;
  case KLA_SYSTEM_ERROR:
    if ( ntohs(attr->kla_length) == KLA_SIZE(sizeof(uint32_t)) ) {
      fprintf(out, "   Error: %s\n", strerror(ntohl(*KLA_DATA_UNSAFE(attr, uint32_t *))));
      return;
    } else goto unknown;
  case KLA_OPERATION:
    if ( ntohs(attr->kla_length) == KLA_SIZE(sizeof(uint16_t)) ) {
      uint16_t otype = ntohs(*KLA_DATA_UNSAFE(attr, uint16_t *));
      fprintf(out, "  Operation: %s (%x)\n", kite_operation_str(otype), otype);
      return;
    } else goto unknown;
  case KLA_PERSONA_DISPLAYNM:
    fprintf(out, "  Display Name: %.*s\n", (int) KLA_PAYLOAD_SIZE(attr),
            KLA_DATA_UNSAFE(attr, char *));
    return;
  case KLA_PERSONA_ID:
    if ( KLA_PAYLOAD_SIZE(attr) <= 100 ) {
      char hex_str[201];
      fprintf(out, "  Persona ID: %s\n", hex_digest_str(KLA_DATA_UNSAFE(attr, unsigned char *),
                                                        hex_str, KLA_PAYLOAD_SIZE(attr)));
      return;
    } else goto unknown;
  default: goto unknown;
  }
  unknown:
    fprintf(out, "  Unknown Attribute(%d) with %d bytes of data: \n", attr_type, attr_len);
}

int display_stork_response(char *buf, int size, const char *success_msg) {
  struct kitelocalmsg *msg;
  struct kitelocalattr *attr;
  int found_response = 0, response_code = 0, is_error = 1;

  if ( size < sizeof(*msg) ) {
    fprintf(stderr, "Response did not contain enough bytes\n");
    exit(1);
  }

  msg = (struct kitelocalmsg *) buf;
  if ( (ntohs(msg->klm_req) & KLM_RESPONSE) == 0 ) {
    fprintf(stderr, "Response was not a response (%04x)\n", ntohs(msg->klm_req));
    exit(1);
  }

  // Go over each attribute
  for ( attr = KLM_FIRSTATTR(msg, size); attr; attr = KLM_NEXTATTR(msg, attr, size) ) {
    if ( ntohs(attr->kla_name) == KLA_RESPONSE_CODE ) {
      uint16_t *code = KLA_DATA_AS(attr, msg, size, uint16_t *);
      found_response = 1;

      if ( !code ) {
        fprintf(stderr, "Not enough data in response\n");
        exit(1);
      }
      response_code = ntohs(*code);
      break;
    }
  }

  if ( !found_response ) {
    fprintf(stderr, "No KLA_RESPONSE_CODE attribute in response\n");
    exit(1);
  }

  is_error = response_code != KLE_SUCCESS;
  if ( !is_error ) {
    if ( success_msg )
      fprintf(stderr, "%s\n", success_msg);

    return 0;
  } else {
    fprintf(stderr, "Got error code: %s\n", kite_error_code_str(response_code));
    for ( attr = KLM_FIRSTATTR(msg, size); attr; attr = KLM_NEXTATTR(msg, attr, size) ) {
      kite_print_attr_data(stderr, attr);
    }

    return -1;
  }
}

int mk_api_socket() {
  struct sockaddr_un addr;
  int err, sk;
  char *stork_path = getenv("KITE_APPLIANCE_DIR");

  addr.sun_family = AF_UNIX;

  if ( stork_path )
    err = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/" KITE_LOCAL_API_SOCK, stork_path);
  else
    err = strnlen(strncpy(addr.sun_path, KITE_LOCAL_API_SOCK, sizeof(addr.sun_path)),
                  sizeof(addr.sun_path));
  assert(err < sizeof(addr.sun_path));

  sk = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if ( sk < 0 ) {
    perror("mk_api_socket: socket");
    return -1;
  }

  err = connect(sk, (struct sockaddr *)&addr, sizeof(addr));
  if ( err < 0 ) {
    perror("mk_api_socket: connect");
    close(sk);
    return -1;
  }

  return sk;
}
