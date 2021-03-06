#include <assert.h>
#include <stdio.h>
#include <getopt.h>

#include "local_proto.h"
#include "commands.h"

#define DISPLAY_NAME_ARG 0x200
#define PASSWORD_ARG     0x201
#define SUPERUSER_ARG    0x202

int create_persona_usage() {
  fprintf(stderr, "Usage: appliancectl create-persona [--display-name <name>] [--password <pw>] [--superuser]\n");
  return 1;
}

int create_persona(int argc, char **argv) {
  static const struct option options[] = {
    { "display-name", 1, NULL, DISPLAY_NAME_ARG },
    { "password", 1, NULL, PASSWORD_ARG },
    { "superuser", 0, NULL, SUPERUSER_ARG },
    {0, 0, 0, 0}
  };

  char buf[KITE_MAX_LOCAL_MSG_SZ];
  struct kitelocalmsg *msg = (struct kitelocalmsg *) buf;
  struct kitelocalattr *attr = KLM_FIRSTATTR(msg, sizeof(buf));
  int optind = 0, c, sz = KLM_SIZE_INIT, sk, err;

  char *display_name = NULL;
  char *password = NULL;
  uint32_t flags = 0;

  while ( (c = getopt_long(argc, argv, "h", options, &optind)) ) {
    if ( c == -1 ) break;

    switch ( c ) {
    case DISPLAY_NAME_ARG:
      display_name = optarg;
      break;
    case PASSWORD_ARG:
      password = optarg;
      break;
    case SUPERUSER_ARG:
      flags |= PERSONA_FLAG_SUPERUSER;
      break;
    case 'h':
    default:
      return create_persona_usage();
    }
  }

  // Prompt for anything remaining (TODO)
  if ( !display_name || !password )
    return create_persona_usage();

  msg->klm_req = ntohs(KLM_REQ_CREATE | KLM_REQ_ENTITY_PERSONA);
  msg->klm_req_flags = 0;

  attr->kla_name = ntohs(KLA_PERSONA_DISPLAYNM);
  attr->kla_length = ntohs(KLA_SIZE(strlen(display_name)));
  memcpy(KLA_DATA_UNSAFE(attr, char *), display_name, strlen(display_name));
  KLM_SIZE_ADD_ATTR(sz, attr);

  attr = KLM_NEXTATTR(msg, attr, sizeof(buf));
  assert(attr);
  attr->kla_name = ntohs(KLA_PERSONA_PASSWORD);
  attr->kla_length = ntohs(KLA_SIZE(strlen(password)));
  memcpy(KLA_DATA_UNSAFE(attr, char *), password, strlen(password));
  KLM_SIZE_ADD_ATTR(sz, attr);

  if ( flags ) {
    attr = KLM_NEXTATTR(msg, attr, sizeof(buf));
    assert(attr);
    attr->kla_name = ntohs(KLA_PERSONA_FLAGS);
    attr->kla_length = ntohs(KLA_SIZE(sizeof(uint32_t) * 2));
    memset(KLA_DATA_UNSAFE(attr, void *), 0, sizeof(uint32_t) * 2);
    flags = ntohl(flags);
    memcpy(KLA_DATA_UNSAFE(attr, void *), &flags, sizeof(flags));
    KLM_SIZE_ADD_ATTR(sz, attr);
  }

  sk = mk_api_socket();
  if ( sk < 0 ) {
    fprintf(stderr, "create_persona: mk_api_socket failed\n");
    return 3;
  }

  err = send(sk, buf, sz, 0);
  if ( err < 0 ) {
    perror("create_persona: send");
    close(sk);
    return 3;
  }

  err = recv(sk, buf, sizeof(buf), 0);
  if ( err < 0 ) {
    perror("create_persona: recv");
    close(sk);
    return 2;
  }

  display_stork_response(buf, err, "Successfully created persona");

  return 0;
}

int list_personas(int argc, char **argv) {
  return 1;
}
