#include <getopt.h>

#include "local_proto.h"
#include "commands.h"

#define MANIFEST_URL_ARG 0x200
#define IDENTIFIER_ARG   0x201

int register_app_usage() {
  fprintf(stderr, "Usage: appliancectl register-app [-h] [-f] <app-manifest-url>\n");
  return 1;
}

int register_app(int argc, char **argv) {
  char buf[KITE_MAX_LOCAL_MSG_SZ];
  char *app_manifest;
  struct kitelocalmsg *msg = (struct kitelocalmsg *)buf;
  struct kitelocalattr *attr = KLM_FIRSTATTR(msg, sizeof(buf));
  int sz = KLM_SIZE_INIT, sk, err, c, do_force = 0;

  while ( (c = getopt(argc, argv, "hf")) ) {
    if ( c == -1 ) break;

    switch ( c ) {
    case 'f':
      do_force = 1;
      break;

    case 'h':
    default:
      return register_app_usage();
    }
  }

  if ( optind >= argc ) {
    fprintf(stderr, "Expected app manifest url\n");
    return register_app_usage();
  }


  app_manifest = argv[optind];

  msg->klm_req = ntohs(KLM_REQ_CREATE | KLM_REQ_ENTITY_APP);
  msg->klm_req_flags = 0;

  attr->kla_name = ntohs(KLA_APP_MANIFEST_URL);
  attr->kla_length = ntohs(KLA_SIZE(strlen(app_manifest)));
  memcpy(KLA_DATA_UNSAFE(attr, char*), app_manifest, strlen(app_manifest));
  KLM_SIZE_ADD_ATTR(sz, attr);

  if ( do_force ) {
    attr = KLM_NEXTATTR(msg, attr, sizeof(buf));
    assert(attr);
    attr->kla_name = ntohs(KLA_FORCE);
    attr->kla_length = ntohs(KLA_SIZE(0));
    KLM_SIZE_ADD_ATTR(sz, attr);
  }

  sk = mk_api_socket();
  if ( sk < 0 ) {
    fprintf(stderr, "register_app: mk_api_socket failed\n");
    return 3;
  }

  err = send(sk, buf, sz, 0);
  if ( err < 0 ) {
    perror("register_app: send");
    close(sk);
    return 3;
  }

  err = recv(sk, buf, sizeof(buf), 0);
  if ( err < 0 ) {
    perror("register_app: recv");
    close(sk);
    return 2;
  }

  display_stork_response(buf, err, "Successfully registered application\n");

  return EXIT_SUCCESS;
}