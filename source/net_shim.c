/* net_shim.c -- bionic(Linux)<->libnx(BSD) socket ABI conversion
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <poll.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <switch.h>

#include "config.h"
#include "log.h"
#include "paths.h"
#include "srv.h"
#include "libc_shim.h"
#include "net_shim.h"
#include "so_util.h"
#include "vfs.h"

static int net_up = 0;
struct pollfd;
static int poll_mixed(struct pollfd *pfd, int n, int timeout); // pipes and sockets
// in-memory connections (defined with the pipes, below)
typedef struct MemConn MemConn;
static MemConn *mem_of(int fd);
static void mem_attach(int fd, MemConn *m);
static long mem_fd_read(int fd, MemConn *m, void *dst, size_t n, int flags);
static long mem_fd_write(int fd, MemConn *m, const void *src, size_t n, int flags);
static int mem_select(int nfds, void *rd, void *wr, void *ex, const void *tv, int *handled);
static uint8_t mem_nonblock[1024];
static int pipe_fcntl(int fd, int cmd, long arg, int *ret);
static int nifm_up = 0;
static int cached_available = -1;
static u64 availability_tick = 0;
static Mutex availability_lock;

int net_init(void) {
  // Socket buffers come out of one pool the app hands the network service,
  // sb_efficiency times one socket's largest buffers: libnx's default (4) is
  // ~2.2 MB, and with GD's curl, Geode's, NONG downloads and Globed's central
  // connection open, Globed's game-server socket got ENOBUFS (OS error 105:
  // kicked out of a room). 8 (the top of the standard range) doubles it. And
  // 8 sessions, not 3: a thread waiting in poll() holds one, and with three
  // of them taken (curl, c-ares, Globed) every other socket call queued.
  SocketInitConfig cfg = *socketGetDefaultInitConfig();
  cfg.sb_efficiency = 8;
  cfg.num_bsd_sessions = 8;
  Result rc = socketInitialize(&cfg);
  if (R_FAILED(rc)) {
    log_printf("[net] the bigger socket pool was refused (0x%x): the default one", rc);
    rc = socketInitializeDefault();
  }
  if (R_FAILED(rc))
    return 0;
  net_up = 1;

  rc = nifmInitialize(NifmServiceType_User);
  if (R_SUCCEEDED(rc))
    nifm_up = 1;
  return 1;
}

void net_exit(void) {
  if (nifm_up) {
    nifmExit();
    nifm_up = 0;
  }
  if (net_up) {
    socketExit();
    net_up = 0;
  }
}

int net_is_available(void) {
  if (!net_up)
    return 0;
  if (!nifm_up)
    return 1;

  mutexLock(&availability_lock);
  const u64 now = armGetSystemTick();
  const u64 freq = armGetSystemTickFreq();
  if (cached_available < 0 || !availability_tick || now - availability_tick >= freq) {
    NifmInternetConnectionType type = 0;
    NifmInternetConnectionStatus status = 0;
    u32 strength = 0;
    Result rc = nifmGetInternetConnectionStatus(&type, &strength, &status);
    cached_available = R_FAILED(rc) || status == NifmInternetConnectionStatus_Connected;
    availability_tick = now;
  }
  const int available = cached_available;
  mutexUnlock(&availability_lock);
  return available;
}


// ---------------------------------------------------------------------------
// sockaddr conversion: Linux {u16 family, ...} <-> BSD {u8 len, u8 family, ...}
// Payload past the first two bytes is identical for AF_INET/AF_INET6.
// ---------------------------------------------------------------------------

#define LINUX_AF_UNSPEC 0
#define LINUX_AF_UNIX   1
#define LINUX_AF_INET   2
#define LINUX_AF_INET6  10

static unsigned char family_to_bsd(unsigned short fam) {
  switch (fam) {
    case LINUX_AF_INET:  return AF_INET;
    case LINUX_AF_INET6: return AF_INET6;
    case LINUX_AF_UNIX:  return AF_UNIX;
    default:             return (unsigned char)fam;
  }
}

static unsigned short family_to_linux(unsigned char fam) {
  switch (fam) {
    case AF_INET:  return LINUX_AF_INET;
    case AF_INET6: return LINUX_AF_INET6;
    case AF_UNIX:  return LINUX_AF_UNIX;
    default:       return fam;
  }
}

// game sockaddr (linux) -> temp BSD copy
static struct sockaddr *sa_to_bsd(const void *in, unsigned len, struct sockaddr_storage *tmp) {
  if (!in || len < 2 || len > sizeof(*tmp))
    return (struct sockaddr *)in;
  memcpy(tmp, in, len);
  const unsigned short fam = *(const unsigned short *)in;
  ((struct sockaddr *)tmp)->sa_len = (unsigned char)len;
  ((struct sockaddr *)tmp)->sa_family = family_to_bsd(fam);
  return (struct sockaddr *)tmp;
}

// BSD sockaddr -> game (linux) layout, in place
static void sa_to_linux_inplace(void *sa, unsigned len) {
  if (!sa || len < 2)
    return;
  const unsigned char fam = ((struct sockaddr *)sa)->sa_family;
  *(unsigned short *)sa = family_to_linux(fam);
}

// ---------------------------------------------------------------------------
// IPv6 sockets, over IPv4. Horizon has no IPv6 (socket(AF_INET6) fails with
// EAFNOSUPPORT), but dual-stack code opens one and reaches IPv4 hosts through
// IPv4-mapped addresses (::ffff:a.b.c.d) -- Globed's pinger does (arc's
// UdpSocket::bindAny(), "Failed to create UDP socket for pinger: (OS error
// 97)"). Such a socket is an IPv4 one here, remembered as IPv6: addresses
// going in are made IPv4 (:: is any, ::1 loopback, a mapped one its IPv4
// address; any other IPv6 address is unreachable), addresses coming out are
// made IPv4-mapped IPv6 again, and IPv6 socket options are accepted.
// ---------------------------------------------------------------------------

#define MAX_V6_FDS 4096
static uint8_t v6_fd[MAX_V6_FDS];

static int is_v6(int fd) {
  return fd >= 0 && fd < MAX_V6_FDS && v6_fd[fd];
}

static void census_remove(int fd);

// log_net: what the game does on its sockets to the offline server
static uint8_t s_trace[1024];
static int s_trace_events;

void net_trace(int fd, const char *what, long n, long r) {
  if (fd < 0 || fd >= (int)sizeof s_trace || !s_trace[fd] || !config.log_net || s_trace_events >= 400)
    return;
  s_trace_events++;
  log_printf("[net] fd %d %s %ld -> %ld%s%d", fd, what, n, r, r < 0 ? " errno " : " ", r < 0 ? errno : 0);
}

void net_forget_fd(int fd) {
  MemConn *m = mem_of(fd);
  if (m) {
    mem_attach(fd, NULL);
    net_mem_close(m, 0);
  }
  if (fd >= 0 && fd < (int)sizeof s_trace && s_trace[fd]) {
    net_trace(fd, "close", 0, 0);
    s_trace[fd] = 0;
  }
  if (fd >= 0 && fd < MAX_V6_FDS)
    v6_fd[fd] = 0;
  census_remove(fd);
}

// a Linux sockaddr_in6 given to an emulated IPv6 socket -> a Linux
// sockaddr_in in *out; 0, or -1 with errno (an address IPv4 cannot reach)
static int v6_to_v4(const void *in, unsigned len, uint8_t out[16], int for_bind) {
  const uint8_t *a = in;
  if (!in || len < 24 || *(const uint16_t *)a != LINUX_AF_INET6) {
    errno = 22; // EINVAL
    return -1;
  }
  static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
  static const uint8_t zero[16] = { 0 };
  const uint8_t *ip6 = a + 8;
  uint8_t ip4[4];
  if (!memcmp(ip6, zero, 16)) {
    memset(ip4, 0, 4); // ::
  } else if (!memcmp(ip6, zero, 15) && ip6[15] == 1) {
    ip4[0] = 127, ip4[1] = 0, ip4[2] = 0, ip4[3] = 1; // ::1
  } else if (!memcmp(ip6, mapped, 12)) {
    memcpy(ip4, ip6 + 12, 4);
  } else {
    errno = for_bind ? 99 : 101; // EADDRNOTAVAIL, ENETUNREACH
    return -1;
  }
  memset(out, 0, 16);
  *(uint16_t *)out = LINUX_AF_INET;
  memcpy(out + 2, a + 2, 2); // port
  memcpy(out + 4, ip4, 4);
  return 0;
}

// a BSD sockaddr_in result for an emulated IPv6 socket -> Linux sockaddr_in6
// (IPv4-mapped) in the caller's buffer, when it has room: 1 when done (the
// result is then in Linux layout already)
static int v4_result_to_v6(void *addr, unsigned *addrlen, unsigned cap) {
  if (!addr || !addrlen || cap < 28 || ((struct sockaddr *)addr)->sa_family != AF_INET)
    return 0;
  const struct sockaddr_in in = *(const struct sockaddr_in *)addr;
  uint8_t *o = addr;
  memset(o, 0, 28);
  *(uint16_t *)o = LINUX_AF_INET6;
  memcpy(o + 2, &in.sin_port, 2);
  o[18] = 0xff, o[19] = 0xff;
  memcpy(o + 20, &in.sin_addr, 4);
  *addrlen = 28;
  return 1;
}

// ---------------------------------------------------------------------------
// Socket census: who made each open socket and when, for debug.log when the
// network service runs out of buffer space (ENOBUFS) -- which sockets hold
// the pool -- and as a leak check: the count is logged each time it passes a
// multiple of 8 for the first time.
// ---------------------------------------------------------------------------

typedef struct {
  uintptr_t from; // where socket()/accept() was called from; 0: not open
  u64 tick;
  uint8_t type;   // SOCK_STREAM, SOCK_DGRAM
} SockInfo;

static SockInfo s_sock[MAX_V6_FDS];
static Mutex s_sock_lock;
static int s_open, s_peak;

static void census_add(int fd, int type, uintptr_t from) {
  if (fd < 0 || fd >= MAX_V6_FDS)
    return;
  mutexLock(&s_sock_lock);
  if (!s_sock[fd].from)
    s_open++;
  s_sock[fd].from = from ? from : 1;
  s_sock[fd].tick = armGetSystemTick();
  s_sock[fd].type = (uint8_t)type;
  int log_it = 0;
  if (s_open > s_peak) {
    s_peak = s_open;
    log_it = s_peak % 8 == 0;
  }
  const int open = s_open;
  mutexUnlock(&s_sock_lock);
  if (log_it)
    log_printf("[net] %d sockets open", open);
}

static void census_remove(int fd) {
  if (fd < 0 || fd >= MAX_V6_FDS)
    return;
  mutexLock(&s_sock_lock);
  if (s_sock[fd].from) {
    s_sock[fd].from = 0;
    s_open--;
  }
  mutexUnlock(&s_sock_lock);
}

static int is_udp(int fd) {
  return fd >= 0 && fd < MAX_V6_FDS && s_sock[fd].from && s_sock[fd].type == SOCK_DGRAM;
}

// every open socket: what it is, who made it, its buffers and unread bytes
// (at most every 30 s; errno kept)
static void census_dump(const char *why) {
  static u64 last;
  const int saved = errno;
  const u64 now = armGetSystemTick();
  mutexLock(&s_sock_lock);
  const int due = !last || armTicksToNs(now - last) >= 30000000000ull;
  if (due)
    last = now;
  const int open = s_open, peak = s_peak;
  mutexUnlock(&s_sock_lock);
  if (!due) {
    errno = saved;
    return;
  }
  log_printf("[net] %s: %d sockets open (at most %d so far)", why, open, peak);
  for (int fd = 0; fd < MAX_V6_FDS; fd++) {
    const SockInfo si = s_sock[fd];
    if (!si.from)
      continue;
    char from[96] = "?";
    so_dl_info info;
    if (si.from > 1 && so_dladdr((void *)si.from, &info) && info.dli_fbase && info.dli_fname) {
      const char *base = strrchr(info.dli_fname, '/');
      snprintf(from, sizeof from, "%s+0x%lx", base ? base + 1 : info.dli_fname,
               (unsigned long)(si.from - (uintptr_t)info.dli_fbase));
    }
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    char addr[32] = "none";
    memset(&peer, 0, sizeof peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0 && peer.sin_family == AF_INET) {
      const uint8_t *a = (const uint8_t *)&peer.sin_addr;
      snprintf(addr, sizeof addr, "%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], ntohs(peer.sin_port));
    }
    int snd = -1, rcv = -1, unread = -1;
    socklen_t ol = sizeof snd;
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, &ol);
    ol = sizeof rcv;
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, &ol);
    ioctl(fd, FIONREAD, &unread);
    log_printf("[net]   fd %d %s%s, %llu s old, from %s, peer %s, buffers %d out %d in, %d unread", fd,
               si.type == SOCK_DGRAM ? "UDP" : si.type == SOCK_STREAM ? "TCP" : "other",
               is_v6(fd) ? " (as IPv6)" : "", (unsigned long long)(armTicksToNs(now - si.tick) / 1000000000ull), from,
               addr, snd, rcv, unread);
  }
  errno = saved;
}

// ---------------------------------------------------------------------------
// msg flags: OOB/PEEK/DONTROUTE match; DONTWAIT is 0x40 on Linux but 0x80 on
// BSD (and Linux 0x40 == BSD MSG_WAITALL, which would BLOCK a non-blocking
// read); WAITALL is 0x100 -> 0x40. Everything else (NOSIGNAL &c.) is dropped.
// ---------------------------------------------------------------------------

static int msg_flags_to_bsd(int flags) {
  int out = flags & (MSG_OOB | MSG_PEEK | MSG_DONTROUTE); // 1|2|4 match
  if (flags & 0x40)  out |= MSG_DONTWAIT; // Linux MSG_DONTWAIT
  if (flags & 0x100) out |= MSG_WAITALL;  // Linux MSG_WAITALL
  return out;
}

// The game is compiled against bionic's Linux errno numbers while libnx uses
// newlib/FreeBSD numbers for most socket errors. Curl checks these values
// directly, especially SO_ERROR after a non-blocking connect.
int net_errno_to_linux(int value) {
  switch (value) {
    case EAFNOSUPPORT:   return 97;
    case EPROTOTYPE:     return 91;
    case ENOTSOCK:       return 88;
    case ENOPROTOOPT:    return 92;
#ifdef ESHUTDOWN
    case ESHUTDOWN:      return 108;
#endif
    case EADDRINUSE:     return 98;
    case ECONNABORTED:   return 103;
    case ENETUNREACH:    return 101;
    case ENETDOWN:       return 100;
    case ETIMEDOUT:      return 110;
    case EHOSTDOWN:      return 112;
    case EHOSTUNREACH:   return 113;
    case EINPROGRESS:    return 115;
    case EALREADY:       return 114;
    case EDESTADDRREQ:   return 89;
    case EMSGSIZE:       return 90;
    case EPROTONOSUPPORT:return 93;
#ifdef ESOCKTNOSUPPORT
    case ESOCKTNOSUPPORT:return 94;
#endif
    case EADDRNOTAVAIL:  return 99;
    case ENETRESET:      return 102;
    case EISCONN:        return 106;
    case ENOTCONN:       return 107;
    case ETOOMANYREFS:   return 109;
    // non-socket values that differ between newlib and Linux
    case ENOSYS:         return 38;
    case ENOTEMPTY:      return 39;
    case ELOOP:          return 40;
    case ENAMETOOLONG:   return 36;
    case EDEADLK:        return 35;
    case ENOLCK:         return 37;
    case EOVERFLOW:      return 75;
    case EILSEQ:         return 84;
    case ENOTSUP:        return 95;
    case ECANCELED:      return 125;
    default:             return value; // common/POSIX values already match
  }
}

static void fixup_socket_errno(void) {
  if (errno == ENOBUFS)
    census_dump("the network service is out of buffer space (ENOBUFS)");
  errno = net_errno_to_linux(errno);
}

static int socket_result(int result) {
  if (result < 0)
    fixup_socket_errno();
  return result;
}

static long socket_result_long(long result) {
  if (result < 0)
    fixup_socket_errno();
  return result;
}

// ---------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------

#define LINUX_SOCK_TYPE_MASK 0xf // strip SOCK_NONBLOCK/SOCK_CLOEXEC

// ---------------------------------------------------------------------------
// the offline server (srv.h): every host name the game looks up is the
// server's made-up address; a connection to it on 443 or 80 goes to the
// server's loopback port, any other is refused, and datagrams to it vanish
// ---------------------------------------------------------------------------

#define OFFLINE_AI_MAGIC 0x4941454e494c464full // "OFLINEAI"


typedef struct {
  uint64_t magic;
  struct addrinfo ai;   // Linux values, as getaddrinfo_fake hands out
  uint8_t sa[28];       // sockaddr_in or sockaddr_in6, Linux layout
  char name[256];
} OfflineAI;

// a host name (not an address literal or localhost) while the server runs
static int offline_name(const char *node) {
  if (!node || !node[0] || !srv_port(1) || !strcasecmp(node, "localhost"))
    return 0;
  struct in_addr a4;
  struct in6_addr a6;
  if (inet_pton(AF_INET, node, &a4) == 1 || inet_pton(AF_INET6, node, &a6) == 1)
    return 0;
  // each name logged once (a small table of hashes)
  static uint32_t seen[128];
  static int nseen;
  uint32_t h = 2166136261u;
  for (const char *p = node; *p; p++)
    h = (h ^ (uint8_t)tolower((unsigned char)*p)) * 16777619u;
  int known = 0;
  for (int i = 0; i < nseen; i++)
    known |= seen[i] == h;
  if (!known && nseen < 128) {
    seen[nseen++] = h;
    log_printf("[net] %s -> the offline server", node);
  }
  return 1;
}

static int offline_getaddrinfo(const char *node, const char *service, const void *hints, void **res) {
  const struct addrinfo *h = hints;
  int port = 0;
  if (service) {
    if (!strcasecmp(service, "https"))
      port = 443;
    else if (!strcasecmp(service, "http"))
      port = 80;
    else
      port = atoi(service);
  }
  OfflineAI *o = calloc(1, sizeof(*o));
  o->magic = OFFLINE_AI_MAGIC;
  const int fam = h ? h->ai_family : LINUX_AF_UNSPEC;
  if (fam == LINUX_AF_INET6) {
    // ::ffff:192.0.2.1
    *(uint16_t *)o->sa = LINUX_AF_INET6;
    *(uint16_t *)(o->sa + 2) = htons((uint16_t)port);
    o->sa[18] = 0xff, o->sa[19] = 0xff;
    *(uint32_t *)(o->sa + 20) = SRV_FAKE_IP_BE;
    o->ai.ai_family = LINUX_AF_INET6;
    o->ai.ai_addrlen = 28;
  } else {
    *(uint16_t *)o->sa = LINUX_AF_INET;
    *(uint16_t *)(o->sa + 2) = htons((uint16_t)port);
    *(uint32_t *)(o->sa + 4) = SRV_FAKE_IP_BE;
    o->ai.ai_family = LINUX_AF_INET;
    o->ai.ai_addrlen = 16;
  }
  o->ai.ai_socktype = h && h->ai_socktype ? (h->ai_socktype & LINUX_SOCK_TYPE_MASK) : SOCK_STREAM;
  o->ai.ai_protocol = h && h->ai_protocol ? h->ai_protocol
                                          : (o->ai.ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
  o->ai.ai_addr = (struct sockaddr *)o->sa;
  if (h && (h->ai_flags & AI_CANONNAME)) {
    snprintf(o->name, sizeof o->name, "%s", node);
    o->ai.ai_canonname = o->name;
  }
  *res = &o->ai;
  return 0;
}

// for a Linux sockaddr: 0 when it is not the offline server, else the port
// it serves (443 or 80), or -1 when that port is not served
static int offline_port(const void *addr, unsigned addrlen) {
  const uint8_t *a = addr;
  if (!a || addrlen < 8 || *(const uint16_t *)a != LINUX_AF_INET ||
      *(const uint32_t *)(a + 4) != SRV_FAKE_IP_BE)
    return 0;
  const int port = ntohs(*(const uint16_t *)(a + 2));
  const int to = (port == 443 && srv_port(1)) || (port == 80 && srv_port(0)) ? port : 0;
  if (!to) {
    static int told;
    if (told++ < 20)
      log_printf("[net] the offline server has no port %d: refused", port);
    return -1;
  }
  return to;
}


int socket_fake(int domain, int type, int protocol) {
  if (!net_up) { errno = EACCES; return -1; }
  const int nonblock = type & 0x800; // Linux SOCK_NONBLOCK
  // IPv6: an IPv4 socket remembered as IPv6 (above)
  const int v6 = domain == LINUX_AF_INET6 &&
                 ((type & LINUX_SOCK_TYPE_MASK) == SOCK_DGRAM || (type & LINUX_SOCK_TYPE_MASK) == SOCK_STREAM);
  if (v6)
    domain = LINUX_AF_INET;
  int fd = socket(family_to_bsd((unsigned short)domain), type & LINUX_SOCK_TYPE_MASK, protocol);
  // out of buffer space: sockets closing elsewhere may give some back soon
  for (int i = 0; fd < 0 && errno == ENOBUFS && i < 5; i++) {
    census_dump("socket(): the network service is out of buffer space (ENOBUFS)");
    svcSleepThread(20000000ull);
    fd = socket(family_to_bsd((unsigned short)domain), type & LINUX_SOCK_TYPE_MASK, protocol);
  }
  if (fd >= 0 && fd < MAX_V6_FDS)
    v6_fd[fd] = (uint8_t)v6;
  if (fd >= 0)
    census_add(fd, type & LINUX_SOCK_TYPE_MASK, (uintptr_t)__builtin_return_address(0));
  if (fd >= 0 && nonblock) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  if (fd < 0)
    fixup_socket_errno();
  return fd;
}

int connect_fake(int fd, const void *addr, unsigned addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  uint8_t v4[16];
  if (is_v6(fd) && addr && addrlen >= 2 && *(const uint16_t *)addr == LINUX_AF_INET6) {
    if (v6_to_v4(addr, addrlen, v4, 0) < 0)
      return -1;
    addr = v4, addrlen = sizeof(v4);
  }
  const int to = offline_port(addr, addrlen);
  if (to < 0) {
    errno = 111; // ECONNREFUSED
    return -1;
  }
  if (to > 0) {
    // connected at once: the stream is in this process (no BSD socket
    // carries it; the socket only keeps the fd number)
    if (fd >= 0 && fd < (int)sizeof s_trace)
      s_trace[fd] = 1;
    MemConn *m = net_mem_new();
    mem_attach(fd, m);
    srv_accept_conn(m, to == 443);
    if (config.log_net)
      log_printf("[net] connect %d to the offline server: port %d, in memory", fd, to);
    return 0;
  }
  struct sockaddr_storage tmp;
  int r = connect(fd, sa_to_bsd(addr, addrlen, &tmp), addrlen);
  if (r < 0)
    fixup_socket_errno();
  // a non-blocking connect under way: Linux says EINPROGRESS, and EAGAIN
  // (which Ryujinx gives) means a failure to the game's code
  if (r < 0 && errno == 11)
    errno = 115;
  return r;
}

int bind_fake(int fd, const void *addr, unsigned addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  uint8_t v4[16];
  if (is_v6(fd) && addr && addrlen >= 2 && *(const uint16_t *)addr == LINUX_AF_INET6) {
    if (v6_to_v4(addr, addrlen, v4, 1) < 0)
      return -1;
    addr = v4, addrlen = sizeof(v4);
  }
  struct sockaddr_storage tmp;
  return socket_result(bind(fd, sa_to_bsd(addr, addrlen, &tmp), addrlen));
}

int accept_fake(int fd, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  const unsigned cap = addrlen ? *addrlen : 0;
  int r = socket_result(accept(fd, (struct sockaddr *)addr, (socklen_t *)addrlen));
  if (r >= 0)
    census_add(r, SOCK_STREAM, (uintptr_t)__builtin_return_address(0));
  if (r >= 0 && is_v6(fd)) {
    if (r < MAX_V6_FDS)
      v6_fd[r] = 1;
    if (v4_result_to_v6(addr, addrlen, cap))
      return r;
  }
  if (r >= 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

int listen_fake(int fd, int backlog) {
  if (!net_up) { errno = EBADF; return -1; }
  return socket_result(listen(fd, backlog));
}

int shutdown_fake(int fd, int how) {
  if (!net_up) { errno = EBADF; return -1; }
  MemConn *m = mem_of(fd);
  if (m) {
    if (how != 0) // SHUT_WR or SHUT_RDWR: nothing more from the game
      net_mem_shutdown(m, 0);
    return 0;
  }
  return socket_result(shutdown(fd, how));
}

// A UDP datagram the service found no buffer for (ENOBUFS): tried again for
// a few ms, then dropped and reported sent, as Linux does when a queue is
// full -- UDP promises nothing and the protocol above resends (Globed's
// handshake does), where an error ended Globed's connection.
static long udp_nobufs(long r, size_t len, long (*again)(const void *), const void *ctx) {
  for (int i = 0; r < 0 && errno == ENOBUFS && i < 5; i++) {
    svcSleepThread(2000000ull);
    r = again(ctx);
  }
  if (r < 0 && errno == ENOBUFS) {
    census_dump("a UDP datagram found no buffer space (dropped, as Linux does)");
    return (long)len;
  }
  return r;
}

typedef struct {
  int fd;
  const void *buf;
  size_t len;
  int flags;
  const struct sockaddr *to;
  socklen_t tolen;
} SendArgs;

static long send_again(const void *ctx) {
  const SendArgs *a = ctx;
  return a->to ? sendto(a->fd, a->buf, a->len, a->flags, a->to, a->tolen) : send(a->fd, a->buf, a->len, a->flags);
}

long send_fake(int fd, const void *buf, size_t len, int flags) {
  if (!net_up) { errno = EBADF; return -1; }
  MemConn *m = mem_of(fd);
  if (m)
    return mem_fd_write(fd, m, buf, len, flags);
  const SendArgs a = { fd, buf, len, msg_flags_to_bsd(flags), NULL, 0 };
  long r = send_again(&a);
  if (r < 0 && errno == ENOBUFS && is_udp(fd))
    r = udp_nobufs(r, len, send_again, &a);
  r = socket_result_long(r);
  net_trace(fd, "send", (long)len, r);
  return r;
}

long recv_fake(int fd, void *buf, size_t len, int flags) {
  if (!net_up) { errno = EBADF; return -1; }
  MemConn *m = mem_of(fd);
  if (m)
    return mem_fd_read(fd, m, buf, len, flags);
  const long r = socket_result_long(recv(fd, buf, len, msg_flags_to_bsd(flags)));
  net_trace(fd, "recv", (long)len, r);
  return r;
}

long sendto_fake(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  // no destination (a connected socket, as c-ares sends DNS queries): that
  // is send(), and Ryujinx's bsd:u crashes on a sendto without an address
  if (!addr || !addrlen)
    return send_fake(fd, buf, len, flags);
  uint8_t v4[16];
  if (is_v6(fd) && addrlen >= 2 && *(const uint16_t *)addr == LINUX_AF_INET6) {
    if (v6_to_v4(addr, addrlen, v4, 0) < 0)
      return -1;
    addr = v4, addrlen = sizeof(v4);
  }
  if (offline_port(addr, addrlen) != 0)
    return (long)len; // a datagram for a game server: there is none offline
  struct sockaddr_storage tmp;
  const SendArgs a = { fd, buf, len, msg_flags_to_bsd(flags), sa_to_bsd(addr, addrlen, &tmp), addrlen };
  long r = send_again(&a);
  if (r < 0 && errno == ENOBUFS && is_udp(fd))
    r = udp_nobufs(r, len, send_again, &a);
  return socket_result_long(r);
}

long recvfrom_fake(int fd, void *buf, size_t len, int flags, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  MemConn *m = mem_of(fd);
  if (m) {
    if (addrlen)
      *addrlen = 0;
    return mem_fd_read(fd, m, buf, len, flags);
  }
  const unsigned cap = addrlen ? *addrlen : 0;
  long r = socket_result_long(recvfrom(fd, buf, len, msg_flags_to_bsd(flags),
                                      (struct sockaddr *)addr,
                                      (socklen_t *)addrlen));
  if (r >= 0 && is_v6(fd) && v4_result_to_v6(addr, addrlen, cap))
    return r;
  if (r >= 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

int getsockname_fake(int fd, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  const unsigned cap = addrlen ? *addrlen : 0;
  if (mem_of(fd) && addr && addrlen && cap >= 16) {
    // a connection to the offline server has no address of its own (its
    // socket never connected; Ryujinx's GetSockName crashes on one):
    // 127.0.0.1 and a port per descriptor
    struct sockaddr_in in = { 0 };
    in.sin_family = AF_INET;
    in.sin_port = htons((uint16_t)(49152 + fd));
    in.sin_addr.s_addr = htonl(0x7f000001);
    memcpy(addr, &in, sizeof in);
    *addrlen = sizeof in;
    if (is_v6(fd) && v4_result_to_v6(addr, addrlen, cap))
      return 0;
    sa_to_linux_inplace(addr, *addrlen);
    return 0;
  }
  int r = socket_result(getsockname(fd, (struct sockaddr *)addr,
                                   (socklen_t *)addrlen));
  if (r == 0 && is_v6(fd) && v4_result_to_v6(addr, addrlen, cap))
    return r;
  if (r == 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

int getpeername_fake(int fd, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  if (mem_of(fd) && addr && addrlen && *addrlen >= 16) {
    uint8_t *a = addr;
    memset(a, 0, 16);
    *(uint16_t *)a = LINUX_AF_INET;
    *(uint16_t *)(a + 2) = htons(443);
    *(uint32_t *)(a + 4) = SRV_FAKE_IP_BE;
    *addrlen = 16;
    return 0;
  }
  const unsigned cap = addrlen ? *addrlen : 0;
  int r = socket_result(getpeername(fd, (struct sockaddr *)addr,
                                   (socklen_t *)addrlen));
  if (r == 0 && is_v6(fd) && v4_result_to_v6(addr, addrlen, cap))
    return r;
  if (r == 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

// Linux SOL_SOCKET=1, option numbers differ from BSD across the board
#define LINUX_SOL_SOCKET 1

static int sockopt_to_bsd(int level, int optname, int *out_level, int *out_name) {
  if (level == IPPROTO_TCP) { // 6 == 6
    *out_level = IPPROTO_TCP;
    switch (optname) {
      case 1: *out_name = TCP_NODELAY;  return 0;
      case 4: *out_name = TCP_KEEPIDLE; return 0;
      case 5: *out_name = TCP_KEEPINTVL;return 0;
      case 6: *out_name = TCP_KEEPCNT;  return 0;
      default: return -1;
    }
  }
  if (level != LINUX_SOL_SOCKET)
    return -1;
  *out_level = SOL_SOCKET;
  switch (optname) {
    case 2:  *out_name = SO_REUSEADDR; return 0;
    case 4:  *out_name = SO_ERROR;     return 0;
    case 6:  *out_name = SO_BROADCAST; return 0;
    case 7:  *out_name = SO_SNDBUF;    return 0;
    case 8:  *out_name = SO_RCVBUF;    return 0;
    case 9:  *out_name = SO_KEEPALIVE; return 0;
    case 10: *out_name = SO_OOBINLINE; return 0;
    case 13: *out_name = SO_LINGER;    return 0;
    case 20: *out_name = SO_RCVTIMEO;  return 0;
    case 21: *out_name = SO_SNDTIMEO;  return 0;
    default: return -1;
  }
}

int getsockopt_fake(int fd, int level, int optname, void *optval, unsigned *optlen) {
  if (!net_up) { errno = EBADF; return -1; }
  int l, n;
  if (mem_of(fd) && sockopt_to_bsd(level, optname, &l, &n) == 0 && l == SOL_SOCKET && n == SO_ERROR &&
      optval && optlen && *optlen >= 4) {
    *(int *)optval = 0;
    *optlen = 4;
    return 0;
  }
  if (sockopt_to_bsd(level, optname, &l, &n) != 0) {
    // unknown option: report "no error/zero" rather than failing the caller
    if (optval && optlen && *optlen >= 4)
      memset(optval, 0, 4);
    return 0;
  }
  int r = getsockopt(fd, l, n, optval, (socklen_t *)optlen);
  if (r < 0) {
    fixup_socket_errno();
    return r;
  }
  if (l == SOL_SOCKET && n == SO_ERROR && optval && optlen &&
      *optlen >= sizeof(int)) {
    int *error = optval;
    *error = net_errno_to_linux(*error);
    net_trace(fd, "SO_ERROR", 0, *error);
  }
  return r;
}

int setsockopt_fake(int fd, int level, int optname, const void *optval, unsigned optlen) {
  if (!net_up) { errno = EBADF; return -1; }
  int l, n;
  if (sockopt_to_bsd(level, optname, &l, &n) != 0)
    return 0; // ignore unmapped options
  return socket_result(setsockopt(fd, l, n, optval, optlen));
}

// struct addrinfo and AI_* values match Android's BSD-derived bionic ABI; only
// socket type flags, address families and the embedded sockaddr need changes.
int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res) {
  if (!net_up)
    return EAI_FAIL;
  if (offline_name(node))
    return offline_getaddrinfo(node, service, hints, res);
  struct addrinfo h, *bsd_hints = NULL;
  if (hints) {
    memcpy(&h, hints, sizeof(h));
    h.ai_flags &= AI_MASK;
    h.ai_family = family_to_bsd((unsigned short)h.ai_family);
    h.ai_socktype &= LINUX_SOCK_TYPE_MASK;
    h.ai_addrlen = 0;
    h.ai_canonname = NULL;
    h.ai_addr = NULL;
    h.ai_next = NULL;
    bsd_hints = &h;
  }
  int r = getaddrinfo(node, service, bsd_hints, (struct addrinfo **)res);
  if (r == EAI_SYSTEM)
    fixup_socket_errno();
  if (r == 0 && res) {
    for (struct addrinfo *ai = *res; ai; ai = ai->ai_next) {
      ai->ai_family = family_to_linux((unsigned char)ai->ai_family);
      if (ai->ai_addr)
        sa_to_linux_inplace(ai->ai_addr, ai->ai_addrlen);
    }
  }
  return r;
}

void freeaddrinfo_fake(void *res) {
  if (!res)
    return;
  OfflineAI *o = (OfflineAI *)((char *)res - offsetof(OfflineAI, ai));
  if (o->magic == OFFLINE_AI_MAGIC) {
    o->magic = 0;
    free(o);
    return;
  }
  // undo the in-place family rewrite before handing the chain back
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    if (ai->ai_addr) {
      unsigned short fam = *(unsigned short *)ai->ai_addr;
      ((struct sockaddr *)ai->ai_addr)->sa_len = (unsigned char)ai->ai_addrlen;
      ((struct sockaddr *)ai->ai_addr)->sa_family = family_to_bsd(fam);
    }
  }
  freeaddrinfo(res);
}

void *gethostbyname_fake(const char *name) {
  if (!net_up)
    return NULL;
  if (offline_name(name)) {
    // struct hostent, the offline server's address (Linux AF_INET = 2)
    static __thread struct { char *name, **aliases; int type, len; char **addrs; } h;
    static __thread char hname[256], *aliases[1], *addrs[2];
    static __thread uint32_t ip;
    snprintf(hname, sizeof hname, "%s", name);
    ip = SRV_FAKE_IP_BE;
    addrs[0] = (char *)&ip, addrs[1] = NULL, aliases[0] = NULL;
    h.name = hname, h.aliases = aliases, h.type = 2, h.len = 4, h.addrs = addrs;
    return &h;
  }
  return gethostbyname(name); // struct hostent layout matches
}

int getnameinfo_fake(const void *sa, unsigned salen, char *host, unsigned hostlen,
                     char *serv, unsigned servlen, int flags) {
  if (!net_up)
    return EAI_FAIL;
  struct sockaddr_storage tmp;
  return getnameinfo(sa_to_bsd(sa, salen, &tmp), salen, host, hostlen, serv,
                     servlen, flags);
}

const char *inet_ntop_fake(int af, const void *src, char *dst, unsigned size) {
  return inet_ntop(family_to_bsd((unsigned short)af), src, dst, size);
}

int inet_pton_fake(int af, const char *src, void *dst) {
  int r = inet_pton(family_to_bsd((unsigned short)af), src, dst);
  if (r < 0)
    fixup_socket_errno();
  return r;
}

// struct pollfd and the POLLIN/OUT/ERR/HUP/NVAL bits match between ABIs
int poll_fake(void *fds, unsigned nfds, int timeout) {
  if (!net_up) { errno = EINVAL; return -1; }
  struct pollfd *pollfds = fds;
  int result;
  if (nfds == 0) {
    // POSIX uses poll(NULL, 0, timeout) as a millisecond sleep. The BSD
    // service rejects the null array with EFAULT, so perform that sleep here.
    if (timeout > 0)
      svcSleepThread((s64)timeout * 1000000ll);
    result = 0;
  } else if (nfds == 1 && pollfds && is_urandom_fd_fake(pollfds[0].fd)) {
    // OpenSSL polls /dev/urandom once before its first entropy read. Its fd is
    // virtual in this port, so forwarding it to BSD produces EBADF and aborts
    // TLS before certificate setup. Horizon's randomGet is always readable.
    pollfds[0].revents = pollfds[0].events & (POLLIN | POLLRDNORM);
    result = pollfds[0].revents ? 1 : 0;
  } else {
    result = poll_mixed(pollfds, (int)nfds, timeout);
  }
  for (unsigned i = 0; i < nfds && pollfds; i++)
    if (pollfds[i].fd >= 0 && pollfds[i].fd < (int)sizeof s_trace && s_trace[pollfds[i].fd]) {
      char what[64];
      snprintf(what, sizeof what, "poll ev %x rev %x of %u, timeout", pollfds[i].events,
               pollfds[i].revents, nfds);
      net_trace(pollfds[i].fd, what, timeout, result);
    }
  return result;
}

int select_fake(int nfds, void *rd, void *wr, void *ex, void *tv) {
  if (!net_up) { errno = EINVAL; return -1; }
  int handled;
  const int mr = mem_select(nfds, rd, wr, ex, tv, &handled);
  if (handled)
    return mr;
  // which traced sockets asked for what (bit i of the u64 words = fd i)
  uint64_t want[3] = { 0, 0, 0 };
  void *sets[3] = { rd, wr, ex };
  for (int k = 0; k < 3; k++)
    if (sets[k])
      for (int fd = 0; fd < nfds && fd < 64; fd++)
        if ((((const uint64_t *)sets[k])[0] >> fd & 1) && fd < (int)sizeof s_trace && s_trace[fd])
          want[k] |= 1ull << fd;
  const long ms = tv ? ((const long *)tv)[0] * 1000 + ((const long *)tv)[1] / 1000 : -1;
  const int r = socket_result(select(nfds, (fd_set *)rd, (fd_set *)wr,
                                     (fd_set *)ex, (struct timeval *)tv));
  for (int fd = 0; fd < 64; fd++)
    if ((want[0] | want[1] | want[2]) >> fd & 1) {
      char what[96];
      snprintf(what, sizeof what, "select r%d w%d e%d -> r%d w%d e%d (nfds %d), ms", (int)(want[0] >> fd & 1),
               (int)(want[1] >> fd & 1), (int)(want[2] >> fd & 1),
               rd ? (int)(((const uint64_t *)rd)[0] >> fd & 1) : 0, wr ? (int)(((const uint64_t *)wr)[0] >> fd & 1) : 0,
               ex ? (int)(((const uint64_t *)ex)[0] >> fd & 1) : 0, nfds);
      net_trace(fd, what, ms, r);
    }
  return r;
}

// fcntl: F_GETFL/F_SETFL match (3/4); O_NONBLOCK is 0x800 on bionic vs
// newlib's 0x4000. F_SETFD/F_GETFD are absorbed.
#define LINUX_O_NONBLOCK 0x800

int fcntl_fake(int fd, int cmd, ...) {
  long arg = 0;
  if (cmd == F_SETFL || cmd == 2) {
    va_list va;
    va_start(va, cmd);
    arg = va_arg(va, long);
    va_end(va);
  }
  if (vfd_is(fd))
    return 0; // F_GETFL: O_RDONLY; descriptor flags: none
  int pret;
  if (pipe_fcntl(fd, cmd, arg, &pret))
    return pret;
  if (mem_of(fd) && fd < 1024 && cmd == F_SETFL)
    mem_nonblock[fd] = (arg & LINUX_O_NONBLOCK) != 0;
  switch (cmd) {
    case F_GETFL: {
      int fl = socket_result(fcntl(fd, F_GETFL, 0));
      if (fl < 0) return fl;
      int out = fl & 3;
      if (fl & O_NONBLOCK) out |= LINUX_O_NONBLOCK;
      return out;
    }
    case F_SETFL: {
      int fl = 0;
      if (arg & LINUX_O_NONBLOCK) fl |= O_NONBLOCK;
      return socket_result(fcntl(fd, F_SETFL, fl));
    }
    case 1: // F_GETFD
      return 0;
    case 2: // F_SETFD (FD_CLOEXEC)
      return 0;
    default:
      return 0;
  }
}

#define LINUX_FIONBIO 0x5421
#define LINUX_FIONREAD 0x541B

int ioctl_fake(int fd, unsigned long request, ...) {
  va_list va;
  va_start(va, request);
  void *arg = va_arg(va, void *);
  va_end(va);
  if (mem_of(fd)) {
    if (request == LINUX_FIONBIO && arg && fd < 1024)
      mem_nonblock[fd] = *(int *)arg != 0;
    if (request == LINUX_FIONREAD && arg) {
      *(int *)arg = (int)net_mem_avail(mem_of(fd), 0);
      return 0;
    }
  }
  switch (request) {
    case LINUX_FIONBIO:
      return socket_result(ioctl(fd, FIONBIO, arg));
    case LINUX_FIONREAD:
      return socket_result(ioctl(fd, FIONREAD, arg));
    default:
      return 0;
  }
}

// ---------------------------------------------------------------------------
// pipes and epoll
//
// Horizon has neither. c-ares' event thread (Globed's resolver) needs both:
// epoll to wait on its DNS sockets and a pipe it writes to wake itself, and
// fails to start without them ("ares_init_options failed with code 3":
// ARES_ESERVFAIL, so no hostname ever resolved). A pipe here is an
// in-process buffer behind two fake descriptors (read/write/close/fcntl know
// them); epoll keeps its interest list, answers pipes itself and waits on
// real sockets with poll() -- in 10 ms slices while pipes are in the list
// too, so a write to one wakes the waiter within a slice. (Sockets on
// 127.0.0.1 as pipes were tried first: Ryujinx's poll never saw their data.)
// eventfd and socketpair stay unavailable: the game's and Geode's curl work
// without them, and stay exactly as they were.
// ---------------------------------------------------------------------------

#define LINUX_O_NONBLOCK_FL 0x800
#define MAX_EPOLL_FDS_POLL 256
#define PIPEFD_BASE 0x7c000000
#define MAX_PIPES 32
#define PIPE_BUF_SIZE 4096

typedef struct {
  int used;
  int open[2];     // read end, write end
  int nonblock[2];
  size_t len;
  uint8_t buf[PIPE_BUF_SIZE];
} Pipe;

static Pipe pipes[MAX_PIPES];
static Mutex pipe_lock;
static CondVar pipe_cv; // any pipe changed (data, space, an end closed)

// the pipe and end of fd, or NULL (lock held)
static Pipe *pipe_of(int fd, int *end) {
  const int i = (fd - PIPEFD_BASE) >> 1;
  if (fd < PIPEFD_BASE || i >= MAX_PIPES || !pipes[i].used)
    return NULL;
  *end = (fd - PIPEFD_BASE) & 1;
  return pipes[i].open[*end] ? &pipes[i] : NULL;
}

int is_pipe_fd_fake(int fd) {
  int end;
  mutexLock(&pipe_lock);
  const int r = pipe_of(fd, &end) != NULL;
  mutexUnlock(&pipe_lock);
  return r;
}

int pipe2_fake(int fds[2], int flags) {
  mutexLock(&pipe_lock);
  for (int i = 0; i < MAX_PIPES; i++) {
    if (pipes[i].used)
      continue;
    memset(&pipes[i], 0, sizeof(pipes[i]));
    pipes[i].used = 1;
    pipes[i].open[0] = pipes[i].open[1] = 1;
    pipes[i].nonblock[0] = pipes[i].nonblock[1] = (flags & LINUX_O_NONBLOCK_FL) != 0;
    mutexUnlock(&pipe_lock);
    fds[0] = PIPEFD_BASE + 2 * i;
    fds[1] = PIPEFD_BASE + 2 * i + 1;
    return 0;
  }
  mutexUnlock(&pipe_lock);
  errno = 24; // EMFILE
  return -1;
}

int pipe_fake(int fds[2]) {
  return pipe2_fake(fds, 0);
}

// read/write/close/fcntl on a pipe end: 1 when fd is one (result in *ret)
int pipe_read_fake(int fd, void *dst, size_t count, long *ret) {
  int end;
  mutexLock(&pipe_lock);
  Pipe *p = pipe_of(fd, &end);
  if (!p) {
    mutexUnlock(&pipe_lock);
    return 0;
  }
  if (end != 0) {
    mutexUnlock(&pipe_lock);
    errno = 9; // EBADF: the write end
    *ret = -1;
    return 1;
  }
  while (p->len == 0 && p->open[1]) {
    if (p->nonblock[0]) {
      mutexUnlock(&pipe_lock);
      errno = 11; // EAGAIN
      *ret = -1;
      return 1;
    }
    condvarWait(&pipe_cv, &pipe_lock);
  }
  const size_t n = count < p->len ? count : p->len;
  memcpy(dst, p->buf, n);
  memmove(p->buf, p->buf + n, p->len - n);
  p->len -= n;
  condvarWakeAll(&pipe_cv);
  mutexUnlock(&pipe_lock);
  *ret = (long)n; // 0: the write end is closed and all was read
  return 1;
}

int pipe_write_fake(int fd, const void *src, size_t count, long *ret) {
  int end;
  mutexLock(&pipe_lock);
  Pipe *p = pipe_of(fd, &end);
  if (!p) {
    mutexUnlock(&pipe_lock);
    return 0;
  }
  if (end != 1 || !p->open[0]) {
    mutexUnlock(&pipe_lock);
    errno = end != 1 ? 9 : 32; // EBADF, EPIPE
    *ret = -1;
    return 1;
  }
  size_t done = 0;
  while (done < count) {
    while (p->len == PIPE_BUF_SIZE && p->open[0]) {
      if (p->nonblock[1]) {
        mutexUnlock(&pipe_lock);
        if (done) {
          *ret = (long)done;
        } else {
          errno = 11;
          *ret = -1;
        }
        return 1;
      }
      condvarWait(&pipe_cv, &pipe_lock);
    }
    if (!p->open[0])
      break;
    size_t n = PIPE_BUF_SIZE - p->len;
    if (n > count - done)
      n = count - done;
    memcpy(p->buf + p->len, (const uint8_t *)src + done, n);
    p->len += n;
    done += n;
    condvarWakeAll(&pipe_cv);
  }
  mutexUnlock(&pipe_lock);
  *ret = (long)done;
  return 1;
}

int pipe_close_fake(int fd) {
  int end;
  mutexLock(&pipe_lock);
  Pipe *p = pipe_of(fd, &end);
  if (p) {
    p->open[end] = 0;
    if (!p->open[0] && !p->open[1])
      p->used = 0;
    condvarWakeAll(&pipe_cv);
  }
  mutexUnlock(&pipe_lock);
  return p != NULL;
}

static int pipe_fcntl(int fd, int cmd, long arg, int *ret) {
  int end;
  mutexLock(&pipe_lock);
  Pipe *p = pipe_of(fd, &end);
  if (p) {
    if (cmd == F_GETFL)
      *ret = (end ? 1 : 0) | (p->nonblock[end] ? LINUX_O_NONBLOCK_FL : 0); // O_WRONLY / O_RDONLY
    else if (cmd == F_SETFL)
      p->nonblock[end] = (arg & LINUX_O_NONBLOCK_FL) != 0, *ret = 0;
    else
      *ret = 0;
  }
  mutexUnlock(&pipe_lock);
  return p != NULL;
}

// readiness of a pipe end, as poll events (lock held)
static short pipe_revents(int fd, short events) {
  int end;
  Pipe *p = pipe_of(fd, &end);
  if (!p)
    return POLLNVAL;
  short r = 0;
  if (end == 0) {
    if (p->len)
      r |= events & POLLIN;
    if (!p->open[1])
      r |= POLLHUP;
  } else {
    if (!p->open[0])
      r |= POLLERR;
    else if (p->len < PIPE_BUF_SIZE)
      r |= events & POLLOUT;
  }
  return r;
}

// ---------------------------------------------------------------------------
// in-memory connections to the offline server (srv.h): connect() to its
// address makes the game's socket one end of a byte stream in this process,
// and the server holds the other end. No BSD socket carries the bytes (the
// socket only keeps its fd number), so it works the same in Ryujinx (whose
// loopback sockets report readiness late or never) and on a console without
// Wi-Fi, and it holds no BSD session. Under the pipes' lock and condition
// variable: poll(), epoll and select() wait on both alike.
// ---------------------------------------------------------------------------

#define MEM_MAX (64u << 20) // per direction, unread

struct MemConn {
  uint8_t *buf[2];       // [0] game -> server, [1] server -> game
  size_t len[2], cap[2];
  int open[2];           // [0] the game's end, [1] the server's
  int shut[2];           // that end writes no more
};

static MemConn *mem_fds[1024]; // the game's sockets that are one (pipe_lock)
static uint8_t mem_nonblock[1024];

static MemConn *mem_of(int fd) {
  return fd >= 0 && fd < 1024 ? mem_fds[fd] : NULL;
}

static void mem_attach(int fd, MemConn *m) {
  if (fd < 0 || fd >= 1024)
    return;
  mutexLock(&pipe_lock);
  mem_fds[fd] = m;
  if (m)
    mem_nonblock[fd] = (fcntl(fd, F_GETFL, 0) & O_NONBLOCK) != 0;
  mutexUnlock(&pipe_lock);
}

MemConn *net_mem_new(void) {
  MemConn *m = calloc(1, sizeof(*m));
  m->open[0] = m->open[1] = 1;
  return m;
}

// end reads what the other end wrote; peek leaves it there
static long mem_read_locked(MemConn *m, int end, void *dst, size_t n, int nonblock, int peek) {
  const int d = 1 - end; // the direction toward this end
  while (m->len[d] == 0) {
    if (!m->open[1 - end] || m->shut[1 - end])
      return 0; // the other end is done
    if (nonblock) {
      errno = 11; // EAGAIN
      return -1;
    }
    condvarWait(&pipe_cv, &pipe_lock);
  }
  const size_t k = n < m->len[d] ? n : m->len[d];
  memcpy(dst, m->buf[d], k);
  if (!peek) {
    memmove(m->buf[d], m->buf[d] + k, m->len[d] - k);
    m->len[d] -= k;
    condvarWakeAll(&pipe_cv);
  }
  return (long)k;
}

static long mem_write_locked(MemConn *m, int end, const void *src, size_t n, int nonblock) {
  const int d = end; // the direction away from this end
  if (!m->open[1 - end] || m->shut[end]) {
    errno = 32; // EPIPE
    return -1;
  }
  while (m->len[d] + n > MEM_MAX) {
    if (nonblock) {
      errno = 11;
      return -1;
    }
    condvarWait(&pipe_cv, &pipe_lock);
    if (!m->open[1 - end]) {
      errno = 32;
      return -1;
    }
  }
  if (m->len[d] + n > m->cap[d]) {
    size_t cap = m->cap[d] ? m->cap[d] : 16384;
    while (cap < m->len[d] + n)
      cap *= 2;
    m->buf[d] = realloc(m->buf[d], cap);
    m->cap[d] = cap;
  }
  memcpy(m->buf[d] + m->len[d], src, n);
  m->len[d] += n;
  condvarWakeAll(&pipe_cv);
  return (long)n;
}

long net_mem_read(MemConn *m, int end, void *dst, size_t n, int nonblock) {
  mutexLock(&pipe_lock);
  const long r = mem_read_locked(m, end, dst, n, nonblock, 0);
  mutexUnlock(&pipe_lock);
  return r;
}

long net_mem_write(MemConn *m, int end, const void *src, size_t n, int nonblock) {
  mutexLock(&pipe_lock);
  const long r = mem_write_locked(m, end, src, n, nonblock);
  mutexUnlock(&pipe_lock);
  return r;
}

size_t net_mem_avail(MemConn *m, int end) {
  mutexLock(&pipe_lock);
  const size_t n = m->len[1 - end];
  mutexUnlock(&pipe_lock);
  return n;
}

void net_mem_shutdown(MemConn *m, int end) {
  mutexLock(&pipe_lock);
  m->shut[end] = 1;
  condvarWakeAll(&pipe_cv);
  mutexUnlock(&pipe_lock);
}

int net_mem_closed(MemConn *m, int end) {
  mutexLock(&pipe_lock);
  const int r = !m->open[1 - end] || m->shut[1 - end];
  mutexUnlock(&pipe_lock);
  return r;
}

void net_mem_close(MemConn *m, int end) {
  mutexLock(&pipe_lock);
  m->open[end] = 0;
  const int last = !m->open[0] && !m->open[1];
  condvarWakeAll(&pipe_cv);
  mutexUnlock(&pipe_lock);
  if (last) {
    free(m->buf[0]);
    free(m->buf[1]);
    free(m);
  }
}

void net_mem_wait(int ms) {
  mutexLock(&pipe_lock);
  condvarWaitTimeout(&pipe_cv, &pipe_lock, (u64)ms * 1000000ull);
  mutexUnlock(&pipe_lock);
}

// the game's end, for poll() (pipe_lock held)
static short mem_revents(MemConn *m, short events) {
  short r = 0;
  if (m->len[1] || !m->open[1] || m->shut[1])
    r |= events & POLLIN;
  if (!m->open[1])
    r |= POLLHUP;
  else if (!m->shut[0])
    r |= events & POLLOUT;
  return r;
}

#define LINUX_MSG_PEEK 0x2
#define LINUX_MSG_DONTWAIT 0x40

static long mem_fd_read(int fd, MemConn *m, void *dst, size_t n, int flags) {
  mutexLock(&pipe_lock);
  const int nb = mem_nonblock[fd] || (flags & LINUX_MSG_DONTWAIT);
  const long r = mem_read_locked(m, 0, dst, n, nb, (flags & LINUX_MSG_PEEK) != 0);
  mutexUnlock(&pipe_lock);
  net_trace(fd, "recv (memory)", (long)n, r);
  return r;
}

static long mem_fd_write(int fd, MemConn *m, const void *src, size_t n, int flags) {
  mutexLock(&pipe_lock);
  const int nb = mem_nonblock[fd] || (flags & LINUX_MSG_DONTWAIT);
  const long r = mem_write_locked(m, 0, src, n, nb);
  mutexUnlock(&pipe_lock);
  net_trace(fd, "send (memory)", (long)n, r);
  return r;
}

int net_mem_fd_read(int fd, void *dst, size_t n, long *ret) {
  MemConn *m = mem_of(fd);
  if (!m)
    return 0;
  *ret = mem_fd_read(fd, m, dst, n, 0);
  return 1;
}

int net_mem_fd_write(int fd, const void *src, size_t n, long *ret) {
  MemConn *m = mem_of(fd);
  if (!m)
    return 0;
  *ret = mem_fd_write(fd, m, src, n, 0);
  return 1;
}

// select() with a memory connection among its sockets: through poll_mixed
// (bionic's fd_set: bit fd of 64-bit words)
static int mem_select(int nfds, void *rd, void *wr, void *ex, const void *tv, int *handled) {
  *handled = 0;
  if (nfds > 1024)
    nfds = 1024;
  void *sets[3] = { rd, wr, ex };
  int any = 0;
  for (int fd = 0; fd < nfds && !any; fd++)
    if (mem_of(fd))
      for (int k = 0; k < 3; k++)
        any |= sets[k] && (((uint64_t *)sets[k])[fd >> 6] >> (fd & 63) & 1);
  if (!any)
    return 0;
  *handled = 1;
  struct pollfd pfd[MAX_EPOLL_FDS_POLL];
  int n = 0;
  for (int fd = 0; fd < nfds && n < MAX_EPOLL_FDS_POLL; fd++) {
    short ev = 0;
    if (rd && (((uint64_t *)rd)[fd >> 6] >> (fd & 63) & 1)) ev |= POLLIN;
    if (wr && (((uint64_t *)wr)[fd >> 6] >> (fd & 63) & 1)) ev |= POLLOUT;
    if (ex && (((uint64_t *)ex)[fd >> 6] >> (fd & 63) & 1)) ev |= POLLPRI;
    if (ev) {
      pfd[n].fd = fd;
      pfd[n].events = ev;
      pfd[n].revents = 0;
      n++;
    }
  }
  const int ms = tv ? (int)(((const long *)tv)[0] * 1000 + ((const long *)tv)[1] / 1000) : -1;
  const int r = poll_mixed(pfd, n, ms);
  if (r < 0)
    return r;
  for (int k = 0; k < 3; k++)
    if (sets[k])
      memset(sets[k], 0, (size_t)((nfds + 63) / 64) * 8);
  int count = 0;
  for (int i = 0; i < n; i++) {
    const int fd = pfd[i].fd;
    const short re = pfd[i].revents;
    if (rd && (re & (POLLIN | POLLHUP | POLLERR))) { ((uint64_t *)rd)[fd >> 6] |= 1ull << (fd & 63); count++; }
    if (wr && (re & (POLLOUT | POLLERR))) { ((uint64_t *)wr)[fd >> 6] |= 1ull << (fd & 63); count++; }
    if (ex && (re & POLLPRI)) { ((uint64_t *)ex)[fd >> 6] |= 1ull << (fd & 63); count++; }
  }
  return count;
}

// poll() over sockets and pipe ends alike. Without pipes: poll() itself.
// With: pipes answered here; sockets polled in 10 ms slices (or not at all
// when there are none: then waiting on the pipes' condition variable).
static int is_virtual_fd(int fd) {
  return (fd >= PIPEFD_BASE && fd < PIPEFD_BASE + 2 * MAX_PIPES) || mem_of(fd);
}

// a pipe end's or memory connection's poll() events (pipe_lock held)
static short virtual_revents(int fd, short events) {
  MemConn *m = mem_of(fd);
  return m ? mem_revents(m, events) : pipe_revents(fd, events);
}

static int poll_mixed(struct pollfd *pfd, int n, int timeout) {
  int npipes = 0;
  for (int i = 0; i < n; i++)
    npipes += is_virtual_fd(pfd[i].fd);
  if (!npipes)
    return socket_result(poll(pfd, (nfds_t)n, timeout));
  struct pollfd socks[MAX_EPOLL_FDS_POLL];
  int map[MAX_EPOLL_FDS_POLL], ns = 0;
  for (int i = 0; i < n && ns < MAX_EPOLL_FDS_POLL; i++) {
    pfd[i].revents = 0;
    if (!is_virtual_fd(pfd[i].fd)) {
      socks[ns] = pfd[i];
      socks[ns].revents = 0;
      map[ns++] = i;
    }
  }
  const u64 start = armGetSystemTick();
  for (;;) {
    int ready = 0;
    mutexLock(&pipe_lock);
    for (int i = 0; i < n; i++)
      if (is_virtual_fd(pfd[i].fd))
        ready += (pfd[i].revents = virtual_revents(pfd[i].fd, pfd[i].events)) != 0;
    mutexUnlock(&pipe_lock);
    const s64 left = timeout < 0 ? 10 : timeout - (s64)(armTicksToNs(armGetSystemTick() - start) / 1000000);
    if (ns) {
      const int slice = ready || left <= 0 ? 0 : (left < 10 ? (int)left : 10);
      const int r = socket_result(poll(socks, (nfds_t)ns, slice));
      if (r < 0)
        return r;
      for (int k = 0; k < ns; k++) {
        pfd[map[k]].revents = socks[k].revents;
        ready += socks[k].revents != 0;
      }
    }
    if (ready || (timeout >= 0 && left <= 0))
      return ready;
    if (!ns) {
      // only pipes: sleep until one changes
      mutexLock(&pipe_lock);
      int any = 0;
      for (int i = 0; i < n; i++)
        any |= virtual_revents(pfd[i].fd, pfd[i].events) != 0;
      if (!any)
        condvarWaitTimeout(&pipe_cv, &pipe_lock, (u64)(timeout < 0 ? 100 : (left < 100 ? left : 100)) * 1000000ull);
      mutexUnlock(&pipe_lock);
    }
  }
}

// Linux struct epoll_event on arm64 (not packed there): events, pad, data
struct linux_epoll_event {
  uint32_t events;
  uint64_t data;
};
#define L_EPOLLIN 0x001
#define L_EPOLLPRI 0x002
#define L_EPOLLOUT 0x004
#define L_EPOLLERR 0x008
#define L_EPOLLHUP 0x010
#define L_EPOLLRDHUP 0x2000
#define L_EPOLLONESHOT (1u << 30)
#define L_EPOLLET (1u << 31)

#define EPFD_BASE 0x7d000000
#define MAX_EPOLL 16
#define MAX_EPOLL_FDS 256

typedef struct {
  int used;
  int n;
  struct {
    int fd;
    uint32_t events; // wanted (0 after a one-shot fired)
    uint32_t last;   // readiness last seen, for edge-triggered entries
    uint64_t data;
  } e[MAX_EPOLL_FDS];
} Epoll;

static Epoll epolls[MAX_EPOLL];
static Mutex epoll_lock;

static Epoll *epoll_of(int epfd) {
  const int i = epfd - EPFD_BASE;
  return i >= 0 && i < MAX_EPOLL && epolls[i].used ? &epolls[i] : NULL;
}

int is_epoll_fd_fake(int fd) {
  mutexLock(&epoll_lock);
  const int r = epoll_of(fd) != NULL;
  mutexUnlock(&epoll_lock);
  return r;
}

int epoll_close_fake(int fd) {
  mutexLock(&epoll_lock);
  Epoll *ep = epoll_of(fd);
  if (ep)
    ep->used = 0;
  mutexUnlock(&epoll_lock);
  return ep != NULL;
}

int epoll_create1_fake(int flags) {
  (void)flags;
  mutexLock(&epoll_lock);
  for (int i = 0; i < MAX_EPOLL; i++) {
    if (!epolls[i].used) {
      memset(&epolls[i], 0, sizeof(epolls[i]));
      epolls[i].used = 1;
      mutexUnlock(&epoll_lock);
      return EPFD_BASE + i;
    }
  }
  mutexUnlock(&epoll_lock);
  errno = 24; // EMFILE
  return -1;
}

int epoll_ctl_fake(int epfd, int op, int fd, void *event) {
  const struct linux_epoll_event *ev = event;
  mutexLock(&epoll_lock);
  Epoll *ep = epoll_of(epfd);
  int r = -1, err = 9; // EBADF
  if (ep) {
    int k = -1;
    for (int i = 0; i < ep->n; i++)
      if (ep->e[i].fd == fd)
        k = i;
    switch (op) {
      case 1: // EPOLL_CTL_ADD
        if (k >= 0) { err = 17; break; } // EEXIST
        if (ep->n >= MAX_EPOLL_FDS || !ev) { err = ev ? 28 : 14; break; }
        k = ep->n++;
        ep->e[k].fd = fd;
        // fall through
      case 3: // EPOLL_CTL_MOD
        if (k < 0) { err = 2; break; } // ENOENT
        if (!ev) { err = 14; break; }
        ep->e[k].events = ev->events;
        ep->e[k].data = ev->data;
        ep->e[k].last = 0;
        r = 0;
        break;
      case 2: // EPOLL_CTL_DEL
        if (k < 0) { err = 2; break; }
        ep->e[k] = ep->e[--ep->n];
        r = 0;
        break;
      default:
        err = 22; // EINVAL
    }
  }
  mutexUnlock(&epoll_lock);
  if (r < 0)
    errno = err;
  return r;
}

int epoll_wait_fake(int epfd, void *events, int max, int timeout) {
  struct linux_epoll_event *out = events;
  if (max <= 0) { errno = 22; return -1; }
  struct pollfd pfd[MAX_EPOLL_FDS];
  uint64_t data[MAX_EPOLL_FDS];
  uint32_t want[MAX_EPOLL_FDS];
  int n = 0;
  mutexLock(&epoll_lock);
  Epoll *ep = epoll_of(epfd);
  if (!ep) {
    mutexUnlock(&epoll_lock);
    errno = 9;
    return -1;
  }
  for (int i = 0; i < ep->n; i++) {
    const uint32_t w = ep->e[i].events;
    if (!(w & (L_EPOLLIN | L_EPOLLOUT | L_EPOLLPRI | L_EPOLLRDHUP)) && !(w & ~L_EPOLLONESHOT))
      continue; // a fired one-shot, until it is re-armed
    pfd[n].fd = ep->e[i].fd;
    pfd[n].events = (short)(((w & (L_EPOLLIN | L_EPOLLRDHUP)) ? POLLIN : 0) |
                            ((w & L_EPOLLOUT) ? POLLOUT : 0) | ((w & L_EPOLLPRI) ? POLLPRI : 0));
    pfd[n].revents = 0;
    data[n] = ep->e[i].data;
    want[n] = w;
    n++;
  }
  mutexUnlock(&epoll_lock);
  if (!net_up) { errno = 22; return -1; }
  int r;
  if (n == 0) {
    if (timeout > 0)
      svcSleepThread((s64)timeout * 1000000ll);
    r = 0;
  } else {
    r = poll_mixed(pfd, n, timeout);
  }
  if (r <= 0)
    return r;

  int got = 0;
  mutexLock(&epoll_lock);
  for (int i = 0; i < n && got < max; i++) {
    if (!pfd[i].revents)
      continue;
    uint32_t e = 0;
    if (pfd[i].revents & POLLIN) e |= L_EPOLLIN;
    if (pfd[i].revents & POLLPRI) e |= L_EPOLLPRI;
    if (pfd[i].revents & POLLOUT) e |= L_EPOLLOUT;
    if (pfd[i].revents & POLLERR) e |= L_EPOLLERR;
    if (pfd[i].revents & POLLHUP) e |= L_EPOLLHUP | (want[i] & L_EPOLLRDHUP);
    e &= want[i] | L_EPOLLERR | L_EPOLLHUP;
    // the entry as it is now (another thread may have changed it)
    int k = -1;
    for (int j = 0; ep->used && j < ep->n; j++)
      if (ep->e[j].fd == pfd[i].fd)
        k = j;
    if (k < 0)
      continue;
    if (pfd[i].revents & POLLNVAL) {
      ep->e[k] = ep->e[--ep->n]; // closed while registered: gone, as on Linux
      continue;
    }
    if (want[i] & L_EPOLLET) {
      // edge-triggered: only what was not ready the time before
      const uint32_t fresh = e & ~ep->e[k].last;
      ep->e[k].last = e;
      e = fresh;
    }
    if (!e)
      continue;
    if (want[i] & L_EPOLLONESHOT)
      ep->e[k].events = L_EPOLLONESHOT; // disarmed until EPOLL_CTL_MOD
    out[got].events = e;
    out[got].data = data[i];
    got++;
  }
  mutexUnlock(&epoll_lock);
  return got;
}

int socketpair_fake(int domain, int type, int protocol, int sv[2]) {
  (void)domain; (void)type; (void)protocol; (void)sv;
  errno = 38;
  return -1;
}

int gethostname_fake(char *name, size_t len) {
  snprintf(name, len, "switch");
  return 0;
}

unsigned if_nametoindex_fake(const char *ifname) {
  (void)ifname;
  return 0;
}

const char *gai_strerror_fake(int ecode) {
  return gai_strerror(ecode);
}

int dup2_fake(int oldfd, int newfd) {
  (void)oldfd; (void)newfd;
  errno = 38;
  return -1;
}

int dup_fake(int fd) {
  if (vfd_is(fd))
    return vfd_dup(fd);
  const int r = dup(fd);
  if (r < 0)
    errno = net_errno_to_linux(errno);
  return r;
}
