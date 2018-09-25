#define _GNU_SOURCE
#include <openssl/err.h>
#include <openssl/rand.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <net/if.h>
#define flock __flock
#include <fcntl.h>
#undef flock
#include <sched.h>
#include <sys/uio.h>
#include <linux/sched.h>
#include <linux/if_tun.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <net/ethernet.h>
#include <net/if_arp.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/udp.h>

#include "bridge.h"
#include "container.h"
#include "util.h"
#include "persona.h"
#include "application.h"
#include "state.h"
#include "storkd_proto.h"

#define KITE_APPLIANCED_APP_PORT 9998

#define OP_BRIDGE_TAP_PACKETS EVT_CTL_CUSTOM
#define OP_BRIDGE_BPR_FINISHED (EVT_CTL_CUSTOM + 1)

static int bridge_setup_ns(struct brstate *br);
static int bridge_setup_main(void *br_ptr);

static int bridge_enter_network_namespace(struct brstate *br);
static int bridge_move_if_to_ns(struct brstate *br, const char *if_name, int netns);
static int bridge_create_veth(struct brstate *br, const char *in_if_name, const char *out_if_name);
static void bridge_handle_bpr_response(struct brstate *br, struct brpermrequest *bpr);
static int find_hw_addr(const char *if_name, unsigned char *mac_addr);
static void bridgefn(struct eventloop *el, int op, void *arg);

static uint16_t ip_checksum(const void *buf, size_t sz) {
  size_t i;
  uint32_t a;
  uint16_t cs;
  uint8_t carry;

  assert((sz % 2) == 0);

  for ( i = 0, a = 0; i < sz; i += 2 ) {
    uint16_t word;
    memcpy(&word, buf + i, sizeof(word));

    a += htons(word);
  }

  carry = (a >> 16) & 0xF;

  a &= 0xFFFF;

  cs = a;
  cs += carry;

  return ~cs;
}

void bridge_clear(struct brstate *br) {
  br->br_mutexes_initialized = 0;
  br->br_appstate = NULL;
  br->br_iproute_path = NULL;
  br->br_uid = 0;
  br->br_gid = 0;
  br->br_comm_fd[0] = br->br_comm_fd[1] = 0;
  br->br_debug_out = NULL;
  br->br_netns = br->br_userns = br->br_tapfd = 0;
  br->br_bridge_addr.s_addr = 0;
  br->br_arp_table = NULL;
  br->br_sctp_table = NULL;
  memset(&br->br_bridge_mac, 0, sizeof(mac_addr));
  fdsub_clear(&br->br_tap_sub);
  br->br_next_ip = 0x0A000001;
  br->br_eth_ix = 0;
}

int bridge_init(struct brstate *br, struct appstate *as, const char *iproute) {
  char ip_dbg[INET_ADDRSTRLEN], mac_dbg[32];
  int err;

  bridge_clear(br);

  br->br_appstate = as;
  br->br_uid = getuid();
  br->br_gid = getgid();

  bridge_allocate_ip(br, &br->br_bridge_addr);
  random_mac(br->br_bridge_mac);

  fprintf(stderr, "Opening bridge with IP address %s and mac %s\n",
          inet_ntop(AF_INET, &br->br_bridge_addr, ip_dbg, sizeof(ip_dbg)),
          mac_ntop(br->br_bridge_mac, mac_dbg, sizeof(mac_dbg)));

  br->br_iproute_path = iproute;

  err = pthread_rwlock_init(&br->br_arp_mutex, NULL);
  if ( err != 0 ) {
    fprintf(stderr, "Could not initialize arp mutex: %s\n", strerror(err));
    goto error;
  }
  br->br_mutexes_initialized |= BR_ARP_MUTEX_INITIALIZED;

  err = pthread_rwlock_init(&br->br_sctp_mutex, NULL);
  if ( err != 0 ) {
    fprintf(stderr, "Could not initialize sctp mutex: %s\n", strerror(err));
    goto error;
  }
  br->br_mutexes_initialized |= BR_SCTP_MUTEX_INITIALIZED;

  err = pthread_mutex_init(&br->br_tap_write_mutex, NULL);
  if ( err != 0 ) {
    fprintf(stderr, "Could not initialize tap write mutex: %s\n", strerror(err));
    goto error;
  }
  br->br_mutexes_initialized |= BR_TAP_MUTEX_INITIALIZED;

  if ( bridge_setup_ns(br) < 0 ) {
    fprintf(stderr, "bridge_init: bridge_setup_ns failed\n");
    goto error;
  }

  return 0;

 error:
  bridge_release(br);
  return -1;
}

void bridge_release(struct brstate *br) {
  if ( br->br_mutexes_initialized & BR_DEBUG_MUTEX_INITIALIZED )
    pthread_mutex_lock(&br->br_debug_mutex);

  if ( br->br_debug_out ) {
    fclose(br->br_debug_out);
    br->br_debug_out = NULL;
  }

  if ( br->br_mutexes_initialized & BR_DEBUG_MUTEX_INITIALIZED ) {
    pthread_mutex_unlock(&br->br_debug_mutex);
    pthread_mutex_destroy(&br->br_debug_mutex);
    br->br_mutexes_initialized &= ~BR_DEBUG_MUTEX_INITIALIZED;
  }

  if ( br->br_mutexes_initialized & BR_ARP_MUTEX_INITIALIZED ) {
    pthread_rwlock_destroy(&br->br_arp_mutex);
    br->br_mutexes_initialized &= ~BR_ARP_MUTEX_INITIALIZED;
  }

  if ( br->br_mutexes_initialized & BR_SCTP_MUTEX_INITIALIZED ) {
    pthread_rwlock_destroy(&br->br_sctp_mutex);
    br->br_mutexes_initialized &= ~BR_SCTP_MUTEX_INITIALIZED;
  }

  if ( br->br_mutexes_initialized & BR_TAP_MUTEX_INITIALIZED ) {
    pthread_mutex_destroy(&br->br_tap_write_mutex);
    br->br_mutexes_initialized &= ~BR_TAP_MUTEX_INITIALIZED;
  }

  if ( br->br_netns ) {
    close(br->br_netns);
    br->br_netns = 0;
  }

  if ( br->br_userns ) {
    close(br->br_userns);
    br->br_userns = 0;
  }

  if ( br->br_tapfd ) {
    close(br->br_tapfd);
    br->br_tapfd = 0;
  }

  if ( br->br_comm_fd[0] ) {
    close(br->br_comm_fd[0]);
    br->br_comm_fd[0] = 0;
  }

  if ( br->br_comm_fd[1] ) {
    close(br->br_comm_fd[1]);
    br->br_comm_fd[1] = 0;
  }

  // TODO free hash
}

static void log_tap_packet(FILE *out, char dir, const struct iovec *iov, int iovcnt) {
  int iov_ix;
  size_t i;
  struct tm cur_time;
  time_t now;

  time(&now);
  localtime_r(&now, &cur_time);

  fprintf(out, "%c %02d:%02d:%02d.000000 0000", dir, cur_time.tm_hour, cur_time.tm_min, cur_time.tm_sec);
  for ( iov_ix = 0; iov_ix < iovcnt; ++iov_ix ) {
    for ( i = 0; i < iov[iov_ix].iov_len; ++i )
      fprintf(out, " %02x", ((const unsigned char *)iov[iov_ix].iov_base)[i]);
  }
  fprintf(out, "\n");
  fflush(out);
}

static void bridge_process_arp(struct brstate *br, int size) {
  struct arphdr hdr;
  uint16_t ether_type;

  if ( size < (sizeof(struct ether_header) + sizeof(struct arphdr)) ) {
    fprintf(stderr, "bridge_process_arp: not enough space in packet\n");
    return;
  }

  memcpy(&hdr, br->br_tap_pkt + sizeof(struct ether_header), sizeof(hdr));

  if ( ntohs(hdr.ar_hrd) != ARPHRD_ETHER ) {
    fprintf(stderr, "bridge_process_arp: dropping ARP of type %04x\n", htons(hdr.ar_hrd));
    return;
  }

  ether_type = ntohs(hdr.ar_pro);
  if ( hdr.ar_hln != ETH_ALEN ||
       (ether_type == ETHERTYPE_IP && hdr.ar_pln != 4) ||
       (ether_type == ETHERTYPE_IPV6 && hdr.ar_pln != 16) ) {
    fprintf(stderr, "bridge_process_arp: drop packet due to addr length mismatch\n");
    return;
  }

  switch ( ntohs(hdr.ar_op) ) {
  case ARPOP_REQUEST:
    fprintf(stderr, "bridge_process_arp: got ARP request\n");
    if ( ether_type == ETHERTYPE_IP ) {
      struct in_addr which_ip;
      struct ether_header rsp_eth;
      struct arphdr rsp_arp;
      uint32_t src_hw_ip;
      mac_addr src_hw_addr;
      int was_found = 0;
      char addr_str[INET_ADDRSTRLEN];

      memset(rsp_eth.ether_dhost, 0xFF, ETH_ALEN);
      rsp_eth.ether_type = htons(ETHERTYPE_ARP);

      rsp_arp.ar_hrd = htons(ARPHRD_ETHER);
      rsp_arp.ar_pro = htons(ETHERTYPE_IP);
      rsp_arp.ar_hln = ETH_ALEN;
      rsp_arp.ar_pln = 4;
      rsp_arp.ar_op = htons(ARPOP_REPLY);

      if ( size < (sizeof(struct ether_header) + sizeof(struct arphdr) +
                   (2 * hdr.ar_hln) + hdr.ar_pln) ) {
        fprintf(stderr, "bridge_process_arp: packet is too small\n");
        return;
      }

      memcpy(&which_ip.s_addr, br->br_tap_pkt + sizeof(struct ether_header) +
             sizeof(struct arphdr) + (2 * hdr.ar_hln) + hdr.ar_pln, sizeof(which_ip.s_addr));

      fprintf(stderr, "Lookup %s\n", inet_ntop(AF_INET, &which_ip, addr_str, sizeof(addr_str)));

      if ( memcmp(&which_ip, &br->br_bridge_addr, sizeof(which_ip)) == 0 ) {
        was_found = 1;
        memcpy(rsp_eth.ether_shost, br->br_bridge_mac, ETH_ALEN);
        memcpy(src_hw_addr, br->br_bridge_mac, ETH_ALEN);
        src_hw_ip = br->br_bridge_addr.s_addr;
      } else {
        fprintf(stderr, "bridge_process_arp: TODO lookup arp\n");
      }

      if ( was_found ) {
        mac_addr tgt_hw_addr = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        struct iovec iov[] = {
          { .iov_base = &rsp_eth, .iov_len = sizeof(rsp_eth) },
          { .iov_base = &rsp_arp, .iov_len = sizeof(rsp_arp) },
          { .iov_base = src_hw_addr, .iov_len = sizeof(src_hw_addr) },
          { .iov_base = &src_hw_ip, .iov_len = sizeof(src_hw_ip) },
          { .iov_base = tgt_hw_addr, .iov_len = sizeof(tgt_hw_addr) },
          { .iov_base = br->br_tap_pkt + sizeof(struct ether_header) + sizeof(struct arphdr) + hdr.ar_pln,
            .iov_len = sizeof(uint32_t) }
        };

        bridge_write_tap_pktv(br, iov, sizeof(iov) / sizeof(iov[0]));
      } else
        fprintf(stderr, "bridge_process_arp: not found\n");
    } else
      fprintf(stderr, "bridge_process_arp: TODO IPV6\n");
    break;

  case ARPOP_REPLY:
    fprintf(stderr, "bridge_process_arp: got ARP response\n");
    break;

  default:
    fprintf(stderr, "bridge_process_arp: unknown op %04x\n", ntohs(hdr.ar_op));
    break;
  }
}

static void bridge_process_udp(struct brstate *br, struct eventloop *el, int sz,
                               struct ether_header *hdr_eth,
                               struct iphdr *hdr_ip) {
  struct udphdr hdr_udp;
  unsigned char *buf = br->br_tap_pkt;
  if ( hdr_ip->daddr == br->br_bridge_addr.s_addr &&
       memcmp(hdr_eth->ether_dhost, br->br_bridge_mac, ETH_ALEN) == 0 ) {

    sz -= sizeof(struct ether_header) + sizeof(struct iphdr);
    if ( sz < sizeof(hdr_udp) ) {
      fprintf(stderr, "bridge_process_udp: not enough data in udp packet\n");
      return;
    }

    buf += sizeof(struct ether_header) + sizeof(struct iphdr);
    memcpy(&hdr_udp, buf, sizeof(hdr_udp));

    sz -= sizeof(hdr_udp);
    buf += sizeof(hdr_udp);

    switch ( ntohs(hdr_udp.uh_dport) ) {
    case KITE_APPLIANCED_APP_PORT:
      if ( sz < 4 ) {
        fprintf(stderr, "bridge_process_udp: not enough bytes in open app request\n");
        return;
      } else {
        uint32_t app_name_len;

        memcpy(&app_name_len, buf, sizeof(app_name_len));
        sz -= 4;
        buf += 4;

        app_name_len = ntohl(app_name_len);
        if ( app_name_len > APP_URL_MAX ) {
          fprintf(stderr, "bridge_process_udp: the application name is too long (%u)\n", app_name_len);
          return;
        }

        if ( sz < app_name_len ) {
          fprintf(stderr, "bridge_process_udp: the application name of length %u is too long (have %d bytes left)\n", app_name_len, sz);
          return;
        }

        fprintf(stderr, "bridge_process_udp: request to open app %.*s\n", app_name_len, buf);

        if ( pthread_rwlock_wrlock(&br->br_arp_mutex) == 0 ) {
          struct arpentry *arp;

          HASH_FIND(ae_hh, br->br_arp_table, &hdr_ip->saddr, sizeof(hdr_ip->saddr), arp);
          if ( arp && arp->ae_permfn ) {
            struct brpermrequest *bpr = malloc(sizeof(*bpr) + app_name_len);
            if ( !bpr ) {
              fprintf(stderr, "bridge_process_udp: could not allocate bpr request\n");
              return;
            }
            bpr->bpr_el = el;
            bpr->bpr_bridge = br;
            bpr->bpr_user_data = NULL;
            bpr->bpr_persona = NULL;
            memcpy(bpr->bpr_srchost, hdr_eth->ether_shost, sizeof(bpr->bpr_srchost));
            bpr->bpr_srcaddr.sin_addr.s_addr = hdr_ip->saddr;
            bpr->bpr_srcaddr.sin_port = hdr_udp.uh_sport;
            bpr->bpr_sts = BPR_ERR_INTERNAL;
            bpr->bpr_perm_size = app_name_len;
            bpr->bpr_perm.bp_type = BR_PERM_APPLICATION;
            memcpy(bpr->bpr_perm.bp_data, buf, app_name_len);
            qdevtsub_init(&bpr->bpr_finished_event, OP_BRIDGE_BPR_FINISHED, bridgefn);
            arp->ae_permfn(arp, bpr);
          }
          pthread_rwlock_unlock(&br->br_arp_mutex);
        }
      }
      break;

    default:
      fprintf(stderr, "bridge_process_udp: unrecognized message on port %d\n", ntohs(hdr_udp.uh_dport));
      break;
    }
  }
}

static int bridge_validate_ip(struct brstate *br, struct ether_header *hdr_eth,
                              struct iphdr *hdr_ip) {
  struct arpentry *arp;
  // Verify that this IP packet was not injected
  if ( pthread_rwlock_rdlock(&br->br_arp_mutex) == 0 ) {
    HASH_FIND(ae_hh, br->br_arp_table, &hdr_ip->saddr, sizeof(hdr_ip->saddr), arp);
    pthread_rwlock_unlock(&br->br_arp_mutex);
  } else return -1;

  if ( !arp ) {
    fprintf(stderr, "bridge_validate_ip: no entry for this IP\n");
    return -1;
  }

  if ( memcmp(hdr_eth->ether_shost, arp->ae_mac, ETH_ALEN) != 0 ) {
    fprintf(stderr, "bridge_validate_ip: IP/MAC mismatch\n");
    return -1;
  }

  return 0;
}

static void bridge_process_ip(struct brstate *br, struct eventloop *el, int sz) {
  struct iphdr hdr_ip;
  struct ether_header hdr_eth;

  if ( sz < (sizeof(struct ether_header) + sizeof(struct iphdr)) ) {
    fprintf(stderr, "bridge_process_ip: packet too small\n");
    return;
  }

  memcpy(&hdr_eth, br->br_tap_pkt, sizeof(struct ether_header));
  memcpy(&hdr_ip, br->br_tap_pkt + sizeof(struct ether_header), sizeof(struct iphdr));

  if ( memcmp(hdr_eth.ether_dhost, br->br_bridge_mac, ETH_ALEN) == 0 &&
       hdr_ip.daddr == br->br_bridge_addr.s_addr ) {
    if ( bridge_validate_ip(br, &hdr_eth, &hdr_ip) < 0 ) {
      fprintf(stderr, "bridge_process_ip: invalid source MAC/IP pair\n");
      return;
    }

    switch ( hdr_ip.protocol ) {
    case IPPROTO_ICMP: {
      struct icmphdr icmp;
      struct ether_header rsp_eth;
      struct iphdr rsp_ip;
      struct icmphdr rsp_icmp;


      if ( sz < (sizeof(struct ether_header) + sizeof(struct iphdr) + sizeof(struct icmphdr)) ) {
        fprintf(stderr, "bridge_process_ip: not enough bytes (ICMP)\n");
        return;
      }

      memcpy(&icmp, br->br_tap_pkt + sizeof(struct ether_header) + sizeof(struct iphdr),
             sizeof(icmp));
      memcpy(rsp_eth.ether_dhost, hdr_eth.ether_shost, ETH_ALEN);
      memcpy(rsp_eth.ether_shost, br->br_bridge_mac, ETH_ALEN);
      rsp_eth.ether_type = htons(ETHERTYPE_IP);

      rsp_ip.version = 4;
      rsp_ip.ihl = 5;
      rsp_ip.tos = 0x00;
      rsp_ip.tot_len = sz - sizeof(rsp_eth);
      rsp_ip.tot_len = 2 * ((rsp_ip.tot_len + 1) / 2);
      rsp_ip.tot_len = htons(rsp_ip.tot_len);
      rsp_ip.id = hdr_ip.id;
      rsp_ip.frag_off = htons(IP_DF);
      rsp_ip.ttl = 64;
      rsp_ip.protocol = IPPROTO_ICMP;
      rsp_ip.check = 0;
      rsp_ip.saddr = br->br_bridge_addr.s_addr;
      rsp_ip.daddr = hdr_ip.saddr;
      rsp_icmp.type = ICMP_ECHOREPLY;
      rsp_icmp.code = 0;
      rsp_icmp.checksum = 0;
      rsp_icmp.un.echo.id = icmp.un.echo.id;
      rsp_icmp.un.echo.sequence = icmp.un.echo.sequence;

      rsp_ip.check = htons(ip_checksum(&rsp_ip, sizeof(rsp_ip)));
      rsp_icmp.checksum = htons(ip_checksum(&rsp_icmp, sizeof(rsp_icmp)));

      switch ( icmp.type ) {
      case ICMP_ECHO: {
        struct iovec iov[] = {
          { .iov_base = &rsp_eth, .iov_len = sizeof(rsp_eth) },
          { .iov_base = &rsp_ip, .iov_len = sizeof(rsp_ip) },
          { .iov_base = &rsp_icmp, .iov_len = sizeof(rsp_icmp) },
          { .iov_base = br->br_tap_pkt + sizeof(struct ether_header) + sizeof(struct iphdr) + sizeof(struct icmphdr),
            .iov_len = sz - sizeof(struct ether_header) - sizeof(struct iphdr) - sizeof(struct icmphdr) }
        };
        bridge_write_tap_pktv(br, iov, sizeof(iov) / sizeof(iov[0]));
        break;
      }

      case ICMP_ECHOREPLY:
        fprintf(stderr, "bridge_process_ip: got ICMP echo reply\n");
        break;

      default:
        fprintf(stderr, "bridge_process_ip: unrecognized ICMP message %02x\n", icmp.type);
        break;
      }

      break;
    }

    case IPPROTO_SCTP:
      //      fprintf(stderr, "bridge_process_ip: got SCTP message\n");
      if ( sz < (sizeof(struct ether_header) + sizeof(struct iphdr) + 2) ) {
        fprintf(stderr, "bridge_process_ip: SCTP packet is too short\n");
      } else {
        if ( pthread_rwlock_rdlock(&br->br_sctp_mutex) == 0 ) {
          struct sctpentry *se;
          struct sockaddr_in source;

          memset(&source, 0, sizeof(source));
          source.sin_addr.s_addr = hdr_ip.saddr;
          memcpy(&source.sin_port,
                 br->br_tap_pkt + sizeof(struct ether_header) + sizeof(struct iphdr),
                 2);

          HASH_FIND(se_hh, br->br_sctp_table, &source, sizeof(source), se);
          if ( se ) {
            se->se_on_packet(se, br->br_tap_pkt + sizeof(struct ether_header) + sizeof(struct iphdr),
                             sz - sizeof(struct ether_header) - sizeof(struct iphdr));
          } else
            fprintf(stderr, "bridge_process_ip: warning: received SCTP to nowhere\n");
          pthread_rwlock_unlock(&br->br_sctp_mutex);
        } else
          fprintf(stderr, "bridge_process_ip: drop SCTP packet because we could not acquire lock\n");
      }
      break;

    case IPPROTO_UDP:
      bridge_process_udp(br, el, sz, &hdr_eth, &hdr_ip);
      break;

    default:
      fprintf(stderr, "bridge_process_ip: got Unknown packet %02x\n", hdr_ip.protocol);
      break;
    }
  } else {
    fprintf(stderr, "bridge_process_ip: Received IP packet somewhere else\n");
  }
}

 static void bridge_process_tap_packet(struct brstate *br, struct eventloop *el, int size) {
  // Check the contained protocol type
  struct ether_header *pkt = (struct ether_header *)br->br_tap_pkt;
  uint16_t ether_type = ntohs(pkt->ether_type);

  switch ( ether_type ) {
  case ETHERTYPE_ARP:
    bridge_process_arp(br, size);
    break;
  case ETHERTYPE_IP:
    bridge_process_ip(br, el, size);
    break;
  case ETHERTYPE_IPV6:
    break;
  default:
    fprintf(stderr, "Dropping ethernet packet with type %d", ether_type);
    break;
  }
}

static void bridgefn(struct eventloop *el, int op, void *arg) {
  struct brpermrequest *bpr;
  struct brstate *br;
  struct fdevent *fdev;
  struct qdevent *qde;
  int err;

  switch ( op ) {
  case OP_BRIDGE_TAP_PACKETS:
    fdev = (struct fdevent *) arg;
    br = STRUCT_FROM_BASE(struct brstate, br_tap_sub, fdev->fde_sub);

    if ( FD_READ_PENDING(fdev) ) {
      err = read(br->br_tapfd, br->br_tap_pkt, sizeof(br->br_tap_pkt));
      if ( err < 0 ) {
        perror("bridge_tap_fn: read");
        eventloop_subscribe_fd(el, br->br_tapfd, FD_SUB_READ, &br->br_tap_sub);
        return;
      }

      // Attempt to process this packet
      if ( br->br_debug_out ) {
        if ( pthread_mutex_lock(&br->br_debug_mutex) == 0 ) {
          struct iovec iov = { .iov_base = br->br_tap_pkt,
                               .iov_len = err };
          log_tap_packet(br->br_debug_out, 'I', &iov, 1);
          pthread_mutex_unlock(&br->br_debug_mutex);
        } else
          fprintf(stderr, "Could not log tap packet: could not lock mutex\n");
      }

      bridge_process_tap_packet(br, el, err);
    }

    eventloop_subscribe_fd(el, br->br_tapfd, FD_SUB_READ, &br->br_tap_sub);
    break;

  case OP_BRIDGE_BPR_FINISHED:
    qde = arg;
    bpr = STRUCT_FROM_BASE(struct brpermrequest, bpr_finished_event, qde->qde_sub);
    br = bpr->bpr_bridge;
    bridge_handle_bpr_response(br, bpr);
    break;

  default:
    fprintf(stderr, "bridge_tap_fn: Unknown op %d\n", op);
  };
}

void bridge_start(struct brstate *br, struct eventloop *el) {
  fdsub_init(&br->br_tap_sub, el, br->br_tapfd, OP_BRIDGE_TAP_PACKETS, bridgefn);

  eventloop_subscribe_fd(el, br->br_tapfd, FD_SUB_READ, &br->br_tap_sub);
}

int bridge_write_tap_pktv(struct brstate *br, const struct iovec *iov, int iovcnt) {
  int err;

  if ( iovcnt < 0 ) {
    errno = EINVAL;
    return -1;
  }

  // TODO this may not be necessary
  if ( pthread_mutex_lock(&br->br_tap_write_mutex) != 0 ) {
    errno = EBUSY;
    return -1;
  }

  err = writev(br->br_tapfd, iov, iovcnt);
  if ( err < 0 ) {
    int old_err = errno;
    perror("bridge_write_tap_pkt: write");
    err = old_err;
  }

  if ( err > 0 && br->br_debug_out ) {
    if ( pthread_mutex_lock(&br->br_debug_mutex) == 0 ) {
      // Output raw tap pkt
      log_tap_packet(br->br_debug_out, 'O', iov, iovcnt);
      pthread_mutex_unlock(&br->br_debug_mutex);
    } else
      fprintf(stderr, "bridge_write_tap_pkt: Skipping debug packet because the mutex could not be locked\n");
  } else
    fprintf(stderr, "bridge_write_tap_pkt: Skipping packet because there was en error writing\n");

  pthread_mutex_unlock(&br->br_tap_write_mutex);

  if ( err != 0 ) {
    errno = err;
    return -1;
  } else return 0;
}

int bridge_write_tap_pkt(struct brstate *br, const unsigned char *pkt, uint16_t pkt_sz) {
  const struct iovec iov[] = {
    { .iov_base = (void *)pkt,
      .iov_len = pkt_sz }
  };

  return bridge_write_tap_pktv(br, iov, 1);
}

int bridge_write_from_foreign_pkt(struct brstate *br, struct container *dst,
                                  const struct sockaddr *sa, socklen_t sa_sz,
                                  const unsigned char *tap_pkt, uint16_t tap_sz) {
  struct arpentry *arp;
  struct ether_header mac;
  struct iphdr ip;

  struct sockaddr_in *sin;

  struct iovec iov[3] = {
    { .iov_base = &mac, .iov_len = sizeof(mac) },
    { 0 },
    { .iov_base = (void *) tap_pkt, .iov_len = tap_sz }
  };

  if ( pthread_rwlock_rdlock(&br->br_arp_mutex) == 0 ) {
    HASH_FIND(ae_hh, br->br_arp_table, &dst->c_ip, sizeof(dst->c_ip), arp);
    pthread_rwlock_unlock(&br->br_arp_mutex);
  } else return -1;

  if ( !arp ) {
    fprintf(stderr, "bridge_write_from_foreign_pkt: could not arp\n");
    return -1;
  }

  memcpy(mac.ether_dhost, arp->ae_mac, ETH_ALEN);
  memcpy(mac.ether_shost, br->br_bridge_mac, ETH_ALEN);
  mac.ether_type = htons(ETHERTYPE_IP);

  switch ( sa->sa_family ) {
  case AF_INET:
    sin = (struct sockaddr_in *) sa;
    if ( sa_sz < sizeof(*sin) )
      return -1;

    ip.version = 4;
    ip.ihl = 5;
    ip.tos = 0;
    ip.tot_len = htons(tap_sz + sizeof(ip));
    ip.id = 0xBEEF;
    ip.frag_off = htons(IP_DF);
    ip.ttl = 64;
    ip.protocol = IPPROTO_SCTP;
    ip.check = 0;
    ip.saddr = br->br_bridge_addr.s_addr; //sin->sin_addr.s_addr;
    ip.daddr = arp->ae_ip.s_addr;

    ip.check = htons(ip_checksum(&ip, sizeof(ip)));

    iov[1].iov_base = &ip;
    iov[1].iov_len = sizeof(ip);

    return bridge_write_tap_pktv(br, iov, 3);

  default:
    fprintf(stderr, "bridge_write_from_foreign_pkt: unknown family %d\n", sa->sa_family);
    return -1;
  }
}

void bridge_enable_debug(struct brstate *br, const char *pkts_out) {
  int err;

  if ( br->br_debug_out ) return;

  err = pthread_mutex_init(&br->br_debug_mutex, NULL);
  if ( err != 0 ) {
    fprintf(stderr, "bridge_enable_debug: Could not initialize debug lock: %s\n", strerror(err));
    return;
  }
  br->br_mutexes_initialized |= BR_DEBUG_MUTEX_INITIALIZED;

  br->br_debug_out = fopen(pkts_out, "wt");
  if ( !br->br_debug_out )
    perror("bridge_enable_debug");
}

void bridge_allocate_ip(struct brstate *br, struct in_addr *new_ip) {
  new_ip->s_addr = htonl(__sync_fetch_and_add(&br->br_next_ip, 1));
}

void bridge_allocate(struct brstate *br, struct in_addr *new_ip, int *new_eth_ix) {
  bridge_allocate_ip(br, new_ip);
  *new_eth_ix = __sync_fetch_and_add(&br->br_eth_ix, 1);
}

int bridge_add_arp(struct brstate *br, struct arpentry *arp) {
  struct arpentry *old;
  if ( pthread_rwlock_wrlock(&br->br_arp_mutex) == 0 ) {
    int ret = 0;
    HASH_FIND(ae_hh, br->br_arp_table, &arp->ae_ip, sizeof(arp->ae_ip), old);
    if ( old ) {
      fprintf(stderr, "bridge_add_arp: already have arp\n");
      ret = -1;
    } else {
      HASH_ADD(ae_hh, br->br_arp_table, ae_ip, sizeof(arp->ae_ip), arp);
    }
    pthread_rwlock_unlock(&br->br_arp_mutex);
    return ret;
  } else return -1;
}

int bridge_del_arp(struct brstate *br, struct arpentry *arp) {
  struct arpentry *old;
  if ( pthread_rwlock_wrlock(&br->br_arp_mutex) == 0 ) {
    int ret = 0;
    HASH_FIND(ae_hh, br->br_arp_table, &arp->ae_ip, sizeof(arp->ae_ip), old);
    if ( old != arp ) {
      fprintf(stderr, "bridge_del_arp: not in table\n");
      ret = -1;
    } else {
      HASH_DELETE(ae_hh, br->br_arp_table, arp);
    }
    pthread_rwlock_unlock(&br->br_arp_mutex);
    return ret;
  } else return -1;
}

int bridge_register_sctp(struct brstate *br, struct sctpentry *se) {
  struct sctpentry *old;
  if ( pthread_rwlock_wrlock(&br->br_sctp_mutex) == 0 ) {
    int ret = 0;
    HASH_FIND(se_hh, br->br_sctp_table, &se->se_source, sizeof(se->se_source), old);
    if ( old ) {
      fprintf(stderr, "bridge_register_sctp: already have this assocation\n");
      ret = -1;
    } else {
      HASH_ADD(se_hh, br->br_sctp_table, se_source, sizeof(se->se_source), se);
    }
    pthread_rwlock_unlock(&br->br_sctp_mutex);
    return ret;
  } else return -1;
}

int bridge_unregister_sctp(struct brstate *br, struct sctpentry *se) {
  struct sctpentry *old;
  if ( pthread_rwlock_wrlock(&br->br_sctp_mutex) == 0 ) {
    int ret = 0;
    HASH_FIND(se_hh, br->br_sctp_table, &se->se_source, sizeof(se->se_source), old);
    if ( old != se ) {
      fprintf(stderr, "bridge_unregister_sctp: not in table\n");
      ret = -1;
    } else {
      HASH_DELETE(se_hh, br->br_sctp_table, se);
    }
    pthread_rwlock_unlock(&br->br_sctp_mutex);
    return ret;
  } else return -1;
}

static void setup_namespace_users(uid_t uid, gid_t gid) {
  FILE *deny_setgroups, *gid_map, *uid_map;
  int err;

  deny_setgroups = fopen("/proc/self/setgroups", "wt");
  if ( !deny_setgroups ) {
    perror("Could not open /proc/self/setgroups");
    exit(1);
  }
  fprintf(deny_setgroups, "deny");
  fclose(deny_setgroups);

  gid_map = fopen("/proc/self/gid_map", "wt");
  if ( !gid_map ) {
    perror("Could not open /proc/self/gid_map");
    exit(1);
  }
  fprintf(gid_map, "0 %d 1\n", gid);
  fclose(gid_map);

  uid_map = fopen("/proc/self/uid_map", "wt");
  if ( !uid_map ) {
    perror("Could not open /proc/self/uid_map");
    exit(1);
  }
  fprintf(uid_map, "0 %d 1\n", uid);
  fclose(uid_map);

  err = setreuid(0, 0);
  if ( err < 0 ) {
    perror("setup_namespace_users: setreuid(0, 0)");
    exit(1);
  }

  err = setregid(0, 0);
  if ( err < 0 ) {
    perror("setup_namespace_users: setregid(0, 0)");
    exit(1);
  }
}

static int bridge_setup_tap(struct brstate *br, char *tap_nm) {
  int fd, err;
  struct ifreq ifr;

  fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
  if ( fd < 0 ) {
    perror("bridge_setup_tap: open(/dev/net/tun)");
    exit(4);
  }

  memset(&ifr, 0, sizeof(ifr));
  ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

  err = ioctl(fd, TUNSETIFF, (void *) &ifr);
  if ( err < 0 ) {
    perror("bridge_setup_tap: ioctl(TUNSETIFF)");
    exit(5);
  }

  strncpy(tap_nm, ifr.ifr_name, IFNAMSIZ);

  return fd;
}

static void bridge_create_bridge(struct brstate *br, const char *tap_nm) {
  char cmd_buf[512];
  int err;

  err = snprintf(cmd_buf, sizeof(cmd_buf), "%s link add bridge type bridge", br->br_iproute_path);
  if ( err >= sizeof(cmd_buf) ) goto nospc;
  err = system(cmd_buf);
  if ( err != 0 ) goto cmdfailed;

  err = snprintf(cmd_buf, sizeof(cmd_buf), "%s link set dev lo up", br->br_iproute_path);
  if ( err >= sizeof(cmd_buf) ) goto nospc;
  err = system(cmd_buf);
  if ( err != 0 ) goto cmdfailed;

  err = snprintf(cmd_buf, sizeof(cmd_buf), "%s link set dev %s master bridge", br->br_iproute_path, tap_nm);
  if ( err >= sizeof(cmd_buf) ) goto nospc;

  err = system(cmd_buf);
  if ( err != 0 ) goto cmdfailed;

  err = snprintf(cmd_buf, sizeof(cmd_buf), "%s link set dev %s up multicast off", br->br_iproute_path, tap_nm);
  if ( err >= sizeof(cmd_buf) ) goto nospc;
  err = system(cmd_buf);
  if ( err != 0 ) goto cmdfailed;

  err = snprintf(cmd_buf, sizeof(cmd_buf), "%s link set dev bridge up multicast off", br->br_iproute_path);
  if ( err >= sizeof(cmd_buf) ) goto nospc;
  err = system(cmd_buf);
  if ( err != 0 ) goto cmdfailed;

  err = system("ifconfig -a");

  return;

 cmdfailed:
  fprintf(stderr, "bridge_create_bridge: '%s' failed with %d\n", cmd_buf, err);
  exit(err);

 nospc:
  fprintf(stderr, "bridge_create_bridge: Not enough space in cmd_buf\n");
  exit(3);
}

static int bridge_setup_main(void *br_ptr) {
  struct brstate *br = (struct brstate *)br_ptr;
  char tap_nm[IFNAMSIZ];
  int fds[3], tap, netns, userns, err;

  close(br->br_comm_fd[1]);

  setup_namespace_users(br->br_uid, br->br_gid);
  fprintf(stderr, "Setup namespace users\n");

  tap = bridge_setup_tap(br, tap_nm);
  fprintf(stderr, "Created tap device %s\n", tap_nm);

  bridge_create_bridge(br, tap_nm);

  netns = open("/proc/self/ns/net", O_CLOEXEC);
  if ( netns < 0 )
    perror("open(/proc/self/ns/net)");

  userns = open("/proc/self/ns/user", O_CLOEXEC);
  if ( userns < 0 )
    perror("open(/proc/self/ns/user)");

  fds[0] = netns;
  fds[1] = userns;
  fds[2] = tap;

  err = send_fd(br->br_comm_fd[0], sizeof(fds) / sizeof(fds[0]), fds);
  if ( err < 0 ) {
    perror("bridge_setup_main: send_fd");
    return 1;
  }

  close(br->br_comm_fd[0]);
  return 0;
}

static int bridge_setup_ns(struct brstate *br) {
  char stack[8192];
  int err, new_proc, ns_fds[3], sts;

  fprintf(stderr, "bridge_setup_ns: stack is %p\n", stack);

  err = socketpair(AF_UNIX, SOCK_DGRAM, 0, br->br_comm_fd);
  if ( err < 0 ) {
    perror("bridge_setup_ns: socketpair");
    return -1;
  }

  new_proc =
    clone(&bridge_setup_main, stack + sizeof(stack),
          CLONE_NEWUSER | CLONE_NEWNET | CLONE_VFORK | SIGCHLD, br);
  if ( new_proc == -1 ) {
    perror("bridge_setup_ns: clone");
    return -1;
  }

  close(br->br_comm_fd[0]);
  br->br_comm_fd[0] = 0;

  err = recv_fd(br->br_comm_fd[1], sizeof(ns_fds) / sizeof(ns_fds[0]), ns_fds);
  if ( err < 0 ) {
    fprintf(stderr, "bridge_setup_ns: could not fetch namespace fds\n");
    return -1;
  }

  br->br_netns = ns_fds[0];
  br->br_userns = ns_fds[1];
  br->br_tapfd = ns_fds[2];

  close(br->br_comm_fd[1]);
  br->br_comm_fd[1] = 0;

  fprintf(stderr, "Got net ns(%d), user ns(%d), and tap fd(%d)\n",
          br->br_netns, br->br_userns, br->br_tapfd);

  fcntl(br->br_netns, F_SETFD, FD_CLOEXEC);
  fcntl(br->br_userns, F_SETFD, FD_CLOEXEC);
  fcntl(br->br_tapfd, F_SETFD, FD_CLOEXEC);

  if ( set_socket_nonblocking(br->br_tapfd) < 0 )
    fprintf(stderr, "Could not set TAP non blocking\n");

  err = waitpid(new_proc, &sts, 0);
  if ( err < 0 ) {
    perror("bridge_setup_ns: waitpid");
    return -1;
  }

  if ( sts != 0 ) {
    fprintf(stderr, "bridge_setup_ns: namespace child returned %d\n", sts);
    return -1;
  }

  return 0;
}

// Utilities

void random_mac(unsigned char *mac) {
  // Taken from linux eth_random_addr;
  int err = RAND_bytes((unsigned char *)mac, ETH_ALEN);
  if ( !err ) {
    fprintf(stderr, "random_mac fails\n");
    ERR_print_errors_fp(stderr);
  }

  mac[0] &= 0xFE; // Clear multicast bit
  mac[1] |= 0x02; // Local assignment bit
}

char *mac_ntop(const unsigned char *mac, char *str, int str_sz) {
  snprintf(str, str_sz, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return str;
}

// Container utils

int bridge_setup_root_uid_gid(struct brstate *br) {
  int err = setreuid(0, 0);
  if ( err < 0 ) {
    perror("brudge_setup_root_uid_gid: setreuid");
    return -1;
  }

  err = setregid(0, 0);
  if ( err < 0 ) {
    perror("bridge_setup_root_uid_gid: setregid");
    return -1;
  }

  return 0;
}

int bridge_set_up_networking(struct brstate *br) {
  char cmd_buf[512];
  int err;

  err = snprintf(cmd_buf, sizeof(cmd_buf), "%s link set dev lo up", br->br_iproute_path);
  if ( err >= sizeof(cmd_buf) ) {
    fprintf(stderr, "bridge_set_up_networking: no space in command buffer\n");
    return -1;
  }

  err = system(cmd_buf);
  if ( err != 0 ) {
    fprintf(stderr, "bridge_set_up_networking: '%s' command fails: %d\n", cmd_buf, err);
    return -1;
  }

  return 0;
}

int bridge_create_veth_to_ns(struct brstate *br, int port_ix, int this_netns,
                             struct in_addr *this_ip, const char *if_name,
                             struct arpentry *arp) {
  int err;
  pid_t new_proc;

  char in_if_name[IFNAMSIZ], out_if_name[IFNAMSIZ], ip_addr_str[INET6_ADDRSTRLEN];

  fprintf(stderr, "bridge_create_veth_to_ns: create veth to %d: %s\n", br->br_netns, if_name);

  memset(arp, 0, sizeof(*arp));

  err = snprintf(in_if_name, sizeof(in_if_name), "in%d", port_ix);
  if ( err >= sizeof(in_if_name) ) {
    fprintf(stderr, "bridge_create_veth_to_ns: in_if_name overflow\n");
    return -1;
  }

  err = snprintf(out_if_name, sizeof(out_if_name), "out%d", port_ix);
  if ( err >= sizeof(out_if_name) ) {
    fprintf(stderr, "bridge_create_veth_to_ns: out_if_name overflow\n");
    return -1;
  }

  new_proc = fork();
  if ( new_proc < 0 ) {
    perror("bridge_create_veth_to_ns: fork");
    return -1;
  }

  if ( new_proc == 0 ) {
    err = bridge_enter_network_namespace(br);
    if ( err < 0 ) {
      fprintf(stderr, "bridge_create_veth_to_ns: could not enter bridge network namespace\n");
      exit(1);
    }

    err = bridge_create_veth(br, in_if_name, out_if_name);
    if ( err < 0 ) {
      fprintf(stderr, "bridge_create_veth_to_ns: could not create veth\n");
      exit(2);
    }

    err = bridge_move_if_to_ns(br, out_if_name, this_netns);
    if ( err < 0 ) {
      fprintf(stderr, "bridge_create_veth_to_ns: could not move interface\n");
      exit(3);
    }

    exit(EXIT_SUCCESS);
  } else {
    int sts = 0;
    char cmd_buf[512];

    fprintf(stderr, "Going to wait for pid %d\n", new_proc);
    err = waitpid(new_proc, &sts, 0);
    if ( err < 0 ) {
      perror("bridge_create_veth_to_ns: waitpid");
      return -1;
    }

    fprintf(stderr, "bridge_create_veth_to_ns: got status %d\n", sts);

    if ( sts != 0 ) {
      fprintf(stderr, "bridge_create_veth_to_ns: namespace child exited with %d\n", sts);
      return -1;
    }

    err = snprintf(cmd_buf, sizeof(cmd_buf), "%s link set %s name %s multicast off",
                   br->br_iproute_path, out_if_name, if_name);
    if ( err >= sizeof(cmd_buf) ) goto overflow;

    err = system(cmd_buf);
    if ( err != 0 ) goto cmd_error;

    err = snprintf(cmd_buf, sizeof(cmd_buf), "%s address add %s/8 broadcast 10.255.255.255 dev %s",
                   br->br_iproute_path,
                   inet_ntop(AF_INET, this_ip, ip_addr_str, sizeof(ip_addr_str)),
                   if_name);
    if ( err >= sizeof(cmd_buf) ) goto overflow;

    err = system(cmd_buf);
    if ( err != 0 ) goto cmd_error;

    err = snprintf(cmd_buf, sizeof(cmd_buf), "%s link set dev %s up",
                   br->br_iproute_path, if_name);
    if ( err >= sizeof(cmd_buf) ) goto overflow;

    err = system(cmd_buf);
    if ( err != 0 ) goto cmd_error;


    err = find_hw_addr(if_name, arp->ae_mac);
    if ( err < 0 ) {
      fprintf(stderr, "bridge_create_veth_to_ns: could not find hardware address of %s\n", if_name);
      return -1;
    }

    memcpy(&arp->ae_ip, this_ip, sizeof(arp->ae_ip));

    return 0;
  overflow:
    fprintf(stderr, "bridge_create_veth_to_ns: no space for commmand '%s'\n", cmd_buf);
    return -1;

  cmd_error:
    fprintf(stderr, "bridge_create_veth_to_ns: %s failed: %d\n", cmd_buf, err);
    return -1;
  }
}

static int bridge_enter_network_namespace(struct brstate *br) {
  int err;

  err = setns(br->br_netns, CLONE_NEWNET);
  if ( err < 0 ) {
    perror("bridge_enter_network_namespace: setns");
    return -1;
  }

  fprintf(stderr, "Entered bridge network namespace\n");

  return 0;
}

static int bridge_move_if_to_ns(struct brstate *br, const char *if_name, int netns) {
  int nl_sk, err;
  struct ifreq ifr;

  struct {
    struct nlmsghdr nl;
    struct ifinfomsg ifi;
    struct rtattr ns_fd_a;
    int ns_fd;
  } KITE_PACKED nl_msg;
  struct nlmsghdr rsp;
  struct nlmsgerr nl_err;
  char recv_buf[512];

  nl_sk = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
  if ( nl_sk < 0 ) {
    perror("bridge_move_if_to_ns: socket");
    return -1;
  }

  strncpy(ifr.ifr_name, if_name, sizeof(ifr.ifr_name));

  if ( ioctl(nl_sk, SIOCGIFINDEX, &ifr, sizeof(ifr)) < 0 ) {
    perror("bridge_move_if_to_ns: ioctl(SIOCGIFINDEX)");
    close(nl_sk);
    return -1;
  }

  fprintf(stderr, "bridge_move_if_to_ns: index of iface %s is %d\n", if_name, ifr.ifr_ifindex);

  // Set up netlink message

  nl_msg.nl.nlmsg_len = sizeof(nl_msg);
  nl_msg.nl.nlmsg_type = RTM_SETLINK;
  nl_msg.nl.nlmsg_flags = NLM_F_ACK | NLM_F_REQUEST;
  nl_msg.nl.nlmsg_seq = 0;
  nl_msg.nl.nlmsg_pid = getpid();
  nl_msg.ifi.ifi_family = AF_UNSPEC;
  nl_msg.ifi.ifi_type = ARPHRD_ETHER;
  nl_msg.ifi.ifi_index = ifr.ifr_ifindex;
  nl_msg.ifi.ifi_flags = 0;
  nl_msg.ifi.ifi_change = 0xFFFFFFFF;
  nl_msg.ns_fd_a.rta_type = IFLA_NET_NS_FD;
  nl_msg.ns_fd_a.rta_len = RTA_LENGTH(sizeof(int));
  nl_msg.ns_fd = netns;

  err = setregid(0, 0);
  if ( err < 0 ) {
    perror("bridge_move_if_to_ns: setregid");
    close(nl_sk);
    return -1;
  }

  err = send(nl_sk, (void *) &nl_msg, sizeof(nl_msg), 0);
  if ( err < 0 ) {
    perror("bridge_move_if_to_ns: send(nl_sk)");
    close(nl_sk);
    return -1;
  }

  err = recv(nl_sk, (void *)recv_buf, sizeof(recv_buf), 0);
  if ( err < 0 ) {
    perror("bridge_move_if_to_ns: recv(nl_sk)");
    close(nl_sk);
    return -1;
  }
  close(nl_sk);

  if ( err < sizeof(rsp) ) {
    fprintf(stderr, "bridge_move_if_to_ns: not enough bytes in response: %d < %ld\n",
            err, sizeof(rsp));
    return -1;
  }

  memcpy(&rsp, recv_buf, sizeof(rsp));
  if ( err < rsp.nlmsg_len ) {
    fprintf(stderr, "bridge_move_if_to_ns: did not receive enough bytes in response\n");
    return -1;
  }

  if ( rsp.nlmsg_type != NLMSG_ERROR ) {
    fprintf(stderr, "bridge_move_if_to_ns: did not get ack from kernel\n");
    return -1;
  }

  if ( rsp.nlmsg_len < (sizeof(rsp) + sizeof(nl_err)) ) {
    fprintf(stderr, "bridge_move_if_to_ns: invalid netlink response\n");
    return -1;
  }

  memcpy(&nl_err, recv_buf + sizeof(rsp), sizeof(nl_err));
  if ( nl_err.error != 0 ) {
    fprintf(stderr, "bridge_move_if_to_ns: could not set namespace: %s\n", strerror(nl_err.error));
    return -1;
  }

  fprintf(stderr, "bridge_move_if_to_ns: moved %s to %d\n", if_name, netns);
  err = system("ifconfig -a");

  return 0;
}

static int bridge_create_veth(struct brstate *br, const char *in_if_name, const char *out_if_name) {
  char cmd_buf[512];
  int err;

  err = snprintf(cmd_buf, sizeof(cmd_buf), "%s link add %s type veth peer name %s",
                 br->br_iproute_path, in_if_name, out_if_name);
  if ( err >= sizeof(cmd_buf) ) goto overflow;

  err = system(cmd_buf);
  if ( err != 0 ) goto cmd_fails;

  err = snprintf(cmd_buf, sizeof(cmd_buf), "%s link set %s master bridge",
                 br->br_iproute_path, in_if_name);
  if ( err >= sizeof(cmd_buf) ) goto overflow;

  err = system(cmd_buf);
  if ( err != 0 ) goto cmd_fails;

  err = snprintf(cmd_buf, sizeof(cmd_buf), "%s link set %s up",
                 br->br_iproute_path, in_if_name);
  if ( err >= sizeof(cmd_buf) ) goto overflow;

  err = system(cmd_buf);
  if ( err != 0 ) goto cmd_fails;

  return 0;

 overflow:
  fprintf(stderr, "bridge_create_veth: '%s' overflowed command buffer\n", cmd_buf);
  return -1;

 cmd_fails:
  fprintf(stderr, "bridge_create_veth: '%s' fails with %d\n", cmd_buf, err);
  return -1;
}

static int find_hw_addr(const char *if_name, unsigned char *mac_addr) {
  int sk;
  struct ifreq ifr;

  sk = socket(AF_UNIX, SOCK_DGRAM, 0);
  if ( sk < 0 ) {
    perror("find_hw_addr: socket");
    return -1;
  }

  if ( strlen(if_name) >= sizeof(ifr.ifr_name) ) {
    close(sk);
    fprintf(stderr, "find_hw_addr: interface name too long\n");
    return -1;
  }

  strncpy(ifr.ifr_name, if_name, sizeof(ifr.ifr_name));

  if ( ioctl(sk, SIOCGIFHWADDR, &ifr, sizeof(ifr)) < 0 ) {
    close(sk);
    return -1;
  }

  close(sk);

  if ( ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER ) {
    fprintf(stderr, "find_hw_addr: invalid hardware address returned\n");
    return -1;
  }

  memcpy(mac_addr, ifr.ifr_hwaddr.sa_data, ETH_ALEN);

  return 0;
}

static void bpr_release(struct brpermrequest *bpr) {
  if ( bpr->bpr_persona )
    PERSONA_UNREF(bpr->bpr_persona);

  free(bpr);
}

static void bridge_respond(struct brstate *br, struct brpermrequest *bpr,
                           struct stkdmsg *rsp, size_t rspsz) {
  struct ether_header rsp_eth;
  struct iphdr rsp_ip;
  struct udphdr rsp_udp;

  struct iovec iov[] = {
    { .iov_base = &rsp_eth, .iov_len = sizeof(rsp_eth) },
    { .iov_base = &rsp_ip, .iov_len = sizeof(rsp_ip) },
    { .iov_base = &rsp_udp, .iov_len = sizeof(rsp_udp) },
    { .iov_base = rsp, .iov_len = rspsz }
  };

  memcpy(rsp_eth.ether_shost, br->br_bridge_mac, ETH_ALEN);
  memcpy(rsp_eth.ether_dhost, bpr->bpr_srchost, ETH_ALEN);
  rsp_eth.ether_type = htons(ETHERTYPE_IP);

  rsp_ip.version = 4;
  rsp_ip.ihl = 5;
  rsp_ip.tos = 0x00;
  rsp_ip.tot_len = htons(sizeof(rsp_ip) + sizeof(rsp_udp) + rspsz);
  rsp_ip.id = 0xBEEF;
  rsp_ip.frag_off = htons(IP_DF);
  rsp_ip.ttl = 64;
  rsp_ip.protocol = IPPROTO_UDP;
  rsp_ip.check = 0;
  rsp_ip.saddr = br->br_bridge_addr.s_addr;
  rsp_ip.daddr = bpr->bpr_srcaddr.sin_addr.s_addr;

  rsp_ip.check = htons(ip_checksum(&rsp_ip, sizeof(rsp_ip)));

  memset(&rsp_udp, 0, sizeof(rsp_udp));
  rsp_udp.uh_sport = htons(KITE_APPLIANCED_APP_PORT);
  rsp_udp.uh_dport = bpr->bpr_srcaddr.sin_port;
  rsp_udp.uh_ulen = htons(sizeof(rsp_udp) + rspsz);

  bridge_write_tap_pktv(br, iov, sizeof(iov) / sizeof(iov[0]));
}

static void bridge_respond_error(struct brstate *br, struct brpermrequest *bpr, int err) {
  struct stkdmsg rsp;
  rsp.sm_flags = STKD_MKFLAGS(STKD_RSP | STKD_ERROR, STKD_OPEN_APP_REQUEST);
  rsp.sm_data.sm_error = htonl(err);

  bridge_respond(br, bpr, &rsp, STKD_ERROR_MSG_SZ);
}

static void bridge_handle_bpr_response(struct brstate *br, struct brpermrequest *bpr) {
  if ( bpr->bpr_sts < 0 ) {
    fprintf(stderr, "bridge_handle_bpr_response: brpermrequest fails with %d\n", bpr->bpr_sts);
    bridge_respond_error(br, bpr, STKD_ERROR_SYSTEM_ERROR);
  } else {
    switch ( bpr->bpr_perm.bp_type ) {
    case BR_PERM_APPLICATION:
      if ( !bpr->bpr_persona ) {
        fprintf(stderr, "bridge_handle_bpr_response: expected bpr_persona to be filled for BR_PERM_APPLICATION\n");
        bridge_respond_error(br, bpr, STKD_ERROR_PERSONA_DOES_NOT_EXIST);
      } else {
        struct app *a = appstate_get_app_by_url_ex(br->br_appstate,
                                                   (const char *)bpr->bpr_perm.bp_data,
                                                   bpr->bpr_perm_size);
        if ( !a ) {
          fprintf(stderr, "bridge_handle_bpr_response: could not find app %.*s\n",
                  bpr->bpr_perm_size, bpr->bpr_perm.bp_data);
          bridge_respond_error(br, bpr, STKD_ERROR_APP_DOES_NOT_EXIST);
        } else {
          struct appinstance *ai = persona_launch_app_instance(bpr->bpr_persona, a);
          APPLICATION_UNREF(a);
          if ( !ai ) {
            fprintf(stderr, "bridge_handle_bpr_response: could not launch app instance\n");
          } else {
            struct stkdmsg rsp;
            container_release_running(&ai->inst_container, bpr->bpr_el);
            APPINSTANCE_UNREF(ai);

            fprintf(stderr, "bridge_handle_bpr_response: launched application\n");

            rsp.sm_flags = STKD_MKFLAGS(STKD_RSP, STKD_OPEN_APP_REQUEST);
            rsp.sm_data.sm_opened_app.sm_family = htonl(AF_INET);
            rsp.sm_data.sm_opened_app.sm_addr = ai->inst_container.c_ip.s_addr;
            bridge_respond(br, bpr, &rsp, STKD_OPENED_APP_RSP_SZ);
          }
        }
      }
      break;

    default:
      fprintf(stderr, "bridge_handle_bpr_response: unknown type %d\n", bpr->bpr_perm.bp_type);
      break;
    }
  }

  bpr_release(bpr);
}