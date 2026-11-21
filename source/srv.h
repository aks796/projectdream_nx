/* srv.h -- no in-process server in this port
 *
 * mkt_nx answered its game's server in the process (srv.c); the generic parts
 * it shares with this port (net_shim.c, java_lang.c, watchdog.c) still ask.
 * Sonic Dream Team needs no server: every answer is "none".
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef SRV_H
#define SRV_H

#include <stddef.h>
#include <stdint.h>

struct MemConn;

// (192.0.2.1, the in-process server's address in mkt_nx: never handed out here,
// srv_port is 0)
#define SRV_FAKE_IP_BE 0x010200c0u

static inline void srv_accept_conn(struct MemConn *m, int tls) {
  (void)m;
  (void)tls;
}
static inline int srv_port(int tls) {
  (void)tls;
  return 0;
}
static inline unsigned srv_thread_handle(void) { return 0; }
static inline unsigned srv_rounds(void) { return 0; }
static inline const uint8_t *srv_ca_der(size_t *len) {
  *len = 0;
  return NULL;
}

#endif
