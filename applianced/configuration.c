#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <libgen.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "util.h"
#include "configuration.h"

#define VALGRIND_FLAG 0x201
#define WEBRTC_PROXY_OPTION 0x202
#define PERSONA_INIT_OPTION 0x203
#define APP_INSTANCE_INIT_OPTION 0x204

static void usage(const char *msg) {
  if ( msg ) fprintf(stderr, "Error: %s\n", msg);

  fprintf(stderr, "applianced - Kite appliance server\n");
  fprintf(stderr, "Usage: applianced [OPTION]...\n\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr,
          "  -h, --help                    Show this help message\n");
  fprintf(stderr,
          "  -c, --conf-dir <DIR>          Appliance configuration directory\n");
  fprintf(stderr,
          "  --ebroute <EBROUTE>           Path to 'ebroute' executable\n");
  fprintf(stderr,
          "  --iproute <IPROUTE>           Path to 'iproute' executable\n");
  fprintf(stderr,
          "  --webrtc-proxy <PROXY>        Path to 'webrtc-proxy' executable\n");
  fprintf(stderr,
          "  --persona-init <INIT>         Path to 'persona-init' executable\n");
  fprintf(stderr,
          "  --app-instance-init <INIT>    Path to 'app-instance-init' executable\n");
  fprintf(stderr,
          "  --valgrind                    Make things valgrind compatible\n");
}

static const char *nix_build(const char *pkg_name, const char *suffix) {
  int p[2];
  int err;
  pid_t pid;

  fprintf(stderr, "Building nix package %s\n", pkg_name);

  err = pipe(p);
  if ( err == -1 ) {
    perror("nix_build: pipe");
    return NULL;
  }

  pid = fork();
  if ( pid == 0 ) {
    close(p[0]);

    dup2(p[1], STDOUT_FILENO);
    close(STDIN_FILENO);

    execlp("nix-build", "nix-build", "<nixpkgs>", "-A", pkg_name, "--no-out-link", NULL);
    perror("execlp(nix-build)");
    exit(1);
  } else {
    int sts, sz;
    char path[PATH_MAX], *ret;

    close(p[1]);

    // Parent
    err = waitpid(pid, &sts, 0);
    if ( err < 0 ) {
      close(p[0]);
      perror("nix_build: waitpid");
      return NULL;
    } else if ( sts != 0 ) {
      close(p[0]);
      fprintf(stderr, "nix-build returns error %d\n", sts);
      return NULL;
    }

    err = read(p[0], path, PATH_MAX);
    if ( err == -1 ) {
      perror("nix_build: read");
      close(p[0]);
      return NULL;
    }

    if ( err > 0 )
      path[err - 1] = '\0'; // Remove newline

    close(p[0]);

    sz = strlen(path) + strlen(suffix) + 2;
    ret = malloc(sz);
    if ( !ret ) {
      fprintf(stderr, "nix_build: could not build %s\n", pkg_name);
      abort();
    }
    SAFE_ASSERT( snprintf(ret, sz, "%s/%s", path, suffix) == (sz - 1) );
    return ret;
  }
}

void appconf_init(struct appconf *ac) {
  ac->ac_conf_dir = NULL;
  ac->ac_iproute_bin = NULL;
  ac->ac_ebroute_bin = NULL;
  ac->ac_webrtc_proxy_path = NULL;
  ac->ac_persona_init_path = NULL;
  ac->ac_app_instance_init_path = NULL;
  ac->ac_kitepath = NULL;
  ac->ac_flags = 0;
}

static void appconf_attempt_kitepath(struct appconf *ac) {
  FILE *maps;

  maps = fopen("/proc/self/maps", "rt");
  if ( !maps ) {
    fprintf(stderr, "appconf_attempt_kite_path: could not open maps\n");
    return;
  }

  while ( !feof(maps) ) {
    void *start, *end;
    char path[PATH_MAX];
    int n = fscanf(maps, "%p-%p %*s %*s %*s %*s %s", &start, &end, path);

    if ( n != 3 ) {
      fprintf(stderr, "warning could not match items for maps file\n");
    } else {
      if ( start <= (void*)appconf_attempt_kitepath &&
           end > (void*)appconf_attempt_kitepath ) {
        const char *dir = dirname(path);

        char *newdir = malloc(strlen(dir) + 1);
        strcpy(newdir, dir);

        fprintf(stderr, "found KITEPATH %s by examining mappings\n", ac->ac_kitepath);
        ac->ac_kitepath = newdir;

        break;
      }
    }

    while (fgetc(maps) != '\n');
  }

  fclose(maps);
}

int appconf_parse_options(struct appconf *ac, int argc, char **argv) {
  int help_flag = 0, option_index = 0, err;
  struct option long_options[] = {
    { "help", no_argument, &help_flag, 1 },
    { "conf-dir", required_argument, 0, 'c' },
    { "iproute", required_argument, 0, 'I' },
    { "ebroute", required_argument, 0, 'E' },
    { "valgrind", no_argument, 0, VALGRIND_FLAG },
    { "webrtc-proxy", required_argument, 0, WEBRTC_PROXY_OPTION },
    { "persona-init", required_argument, 0, PERSONA_INIT_OPTION },
    { "app-instance-init", required_argument, 0, APP_INSTANCE_INIT_OPTION },
    { 0, 0, 0, 0 }
  };

  ac->ac_kitepath = getenv("KITEPATH");
  appconf_attempt_kitepath(ac); // Attempts to get the kite path in other ways

  while ( 1 ) {
    err = getopt_long(argc, argv, "hc:", long_options, &option_index);
    if ( err == -1 ) break;

    switch ( err ) {
    default: case 0: break;

    case VALGRIND_FLAG:
      ac->ac_flags |= AC_FLAG_VALGRIND_COMPAT;
      break;

    case WEBRTC_PROXY_OPTION:
      ac->ac_webrtc_proxy_path = optarg;
      break;

    case PERSONA_INIT_OPTION:
      ac->ac_persona_init_path = optarg;
      break;

    case APP_INSTANCE_INIT_OPTION:
      ac->ac_app_instance_init_path = optarg;
      break;

    case 'h':
      help_flag = 1;
      break;

    case 'c':
      ac->ac_conf_dir = optarg;
      break;

    case 'I':
      ac->ac_iproute_bin = optarg;
      break;

    case 'E':
      ac->ac_ebroute_bin = optarg;
      break;
    }
  }

  if ( help_flag ) {
    usage(NULL);
    return -1;
  } else {
    return appconf_validate(ac, 1);
  }
}

const char *appconf_get_default_executable(struct appconf *ac, const char *nm) {
  if ( ac->ac_kitepath ) {
    int err;
    char *path;
    struct stat stinfo;

    err = snprintf(NULL, 0, "%s/%s", ac->ac_kitepath, nm);

    path = malloc(err + 1);
    if ( !path ) return NULL;

    snprintf(path, err + 1, "%s/%s", ac->ac_kitepath, nm);

    err = stat(path, &stinfo);
    if ( err < 0 ) {
      if ( errno == ENOENT ) {
        fprintf(stderr, "appconf_get_default_executable: %s does not exist\n", path);
      } else
        perror("appconf_get_default_executable: stat");
      free(path);
      return NULL;
    }

    if ( (stinfo.st_mode & S_IFMT) != S_IFREG ) {
      fprintf(stderr, "appconf_get_default_executable: %s is not a regular file\n", path);
      free(path);
      return NULL;
    }

    return path;
  } else
    return NULL;
}

#define APPCONF_ENSURE_EXECUTABLE(fd, nm) do {  \
    if ( !ac->fd ) {                                                    \
      ac->fd = appconf_get_default_executable(ac, nm);                  \
      if ( !ac->fd ) {                                                  \
        fprintf(stderr, "Could not get " nm " (Use --" nm " or KITEPATH)\n"); \
        return -1;                                                      \
      }                                                                 \
    }                                                                   \
  } while (0)

int appconf_validate(struct appconf *ac, int do_debug) {
  if ( !ac->ac_conf_dir ) {
    usage("No configuration directory provided");
    return -1;
  }

  if ( !ac->ac_iproute_bin ) {
    // Attempt to get iproute information using nix-build
    ac->ac_iproute_bin = nix_build("iproute", "bin/ip");
    if ( !ac->ac_iproute_bin ) {
      fprintf(stderr, "Could not build iproute via nix\n");
      return -1;
    }
  }

  if ( !ac->ac_ebroute_bin ) {
    // Attempt to get ebroute information using nix-build
    ac->ac_ebroute_bin = nix_build("ebtables", "bin/ebtables");
    if ( !ac->ac_ebroute_bin ) {
      fprintf(stderr, "Could not build ebroute via nix\n");
      return -1;
    }
  }

  APPCONF_ENSURE_EXECUTABLE(ac_webrtc_proxy_path, "webrtc-proxy");
  APPCONF_ENSURE_EXECUTABLE(ac_persona_init_path, "persona-init");
  APPCONF_ENSURE_EXECUTABLE(ac_app_instance_init_path, "app-instance-init");

  if ( do_debug ) {
    fprintf(stderr, "Using %s as configuration directory\n", ac->ac_conf_dir);
    fprintf(stderr, "Using %s as iproute path\n", ac->ac_iproute_bin);
    fprintf(stderr, "Using %s as ebtables path\n", ac->ac_ebroute_bin);
    fprintf(stderr, "Using %s as webrtc-proxy path\n", ac->ac_webrtc_proxy_path);
    fprintf(stderr, "Using %s as persona-init path\n", ac->ac_persona_init_path);
    fprintf(stderr, "Using %s as app-instance-init path\n", ac->ac_app_instance_init_path);
  }

  return 0;
}

