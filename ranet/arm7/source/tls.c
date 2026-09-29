// TLS for ranet: mbedTLS over an sgIP TCP connection, resuming the session
// RA Sync saved (tls.h).  Runs on a cooperative thread: every "would block"
// sleeps a little and tries again.
#include <calico/types.h>
#include <calico/system/thread.h>
#include <calico/system/tick.h>
#include <calico/system/dietprint.h>
#include <string.h>
#include "sgIP.h"
#include "ranet_host.h"
#include "tls.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"

#define ERR_CONN_RESET -0x0050 // MBEDTLS_ERR_NET_CONN_RESET (net_sockets.c isn't built)

extern void* sgIP_malloc(int size);
extern void sgIP_free(void* ptr);

static void* tlsCalloc(size_t n, size_t size)
{
	size_t total = n * size;
	if (size && total / size != n) return NULL;
	void* p = sgIP_malloc((int)total);
	if (p) memset(p, 0, total);
	return p;
}

// Only the ClientHello random and nonces come from here (a resumed
// handshake makes no keys of its own)
int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen)
{
	(void)data;
	for (size_t i = 0; i < len; i ++) {
		u32 v = ranetHostTicks() ^ ranetHostEntropy() ^ (u32)(tickGetCount() >> 3);
		output[i] = (u8)(v ^ (v >> 8) ^ (v >> 16) ^ (v >> 24));
	}
	*olen = len;
	return 0;
}

static bool s_ready;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static mbedtls_ssl_config s_conf;
static mbedtls_ssl_context s_ssl;

static int bioSend(void* ctx, const unsigned char* buf, size_t len)
{
	sgIP_Record_TCP* rec = (sgIP_Record_TCP*)ctx;
	if (rec->tcpstate != SGIP_TCP_STATE_ESTABLISHED) return ERR_CONN_RESET;
	SGIP_INTR_PROTECT();
	int r = sgIP_TCP_Send(rec, (const char*)buf, (int)len, 0);
	SGIP_INTR_UNPROTECT();
	return r > 0 ? r : MBEDTLS_ERR_SSL_WANT_WRITE;
}

static int bioRecv(void* ctx, unsigned char* buf, size_t len)
{
	sgIP_Record_TCP* rec = (sgIP_Record_TCP*)ctx;
	SGIP_INTR_PROTECT();
	int r = sgIP_TCP_Recv(rec, (char*)buf, (int)len, 0);
	int state = rec->tcpstate;
	SGIP_INTR_UNPROTECT();
	if (r > 0) return r;
	if (r == 0 || state != SGIP_TCP_STATE_ESTABLISHED) return 0; // closed
	return MBEDTLS_ERR_SSL_WANT_READ;
}

static bool setup(void)
{
	if (s_ready) return true;
	mbedtls_platform_set_calloc_free(tlsCalloc, sgIP_free);
	mbedtls_entropy_init(&s_entropy);
	mbedtls_ctr_drbg_init(&s_drbg);
	mbedtls_ssl_config_init(&s_conf);
	static const unsigned char pers[] = "ranet";
	int r = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy, pers, sizeof(pers) - 1);
	if (!r) r = mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	if (r) {
		dietPrint("[tls] setup -%04x\n", -r);
		return false;
	}
	mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_drbg);
	// No trust anchors: only a resumed session can succeed
	mbedtls_ssl_conf_authmode(&s_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	s_ready = true;
	return true;
}

int tlsOpen(void* rec, const RaTlsSession* saved, const char* host, u32 timeout_ms)
{
	if (!setup()) return -1;
	mbedtls_ssl_init(&s_ssl);
	int r = mbedtls_ssl_setup(&s_ssl, &s_conf);
	if (!r) r = mbedtls_ssl_set_hostname(&s_ssl, host);
	if (r) {
		dietPrint("[tls] ssl setup -%04x\n", -r);
		mbedtls_ssl_free(&s_ssl);
		return -1;
	}

	mbedtls_ssl_session s;
	mbedtls_ssl_session_init(&s);
	s.ciphersuite = saved->ciphersuite;
	s.id_len = saved->id_len;
	memcpy(s.id, saved->id, sizeof(s.id));
	memcpy(s.master, saved->master, sizeof(s.master));
	s.encrypt_then_mac = saved->encrypt_then_mac;
	s.mfl_code = saved->mfl_code;
	s.ticket = (unsigned char*)tlsCalloc(1, saved->ticket_len);
	if (s.ticket) {
		memcpy(s.ticket, saved->ticket, saved->ticket_len);
		s.ticket_len = saved->ticket_len;
		s.ticket_lifetime = saved->ticket_lifetime;
	}
	r = mbedtls_ssl_set_session(&s_ssl, &s);
	mbedtls_ssl_session_free(&s); // wipes the master secret copy
	if (r) {
		dietPrint("[tls] set session -%04x\n", -r);
		mbedtls_ssl_free(&s_ssl);
		return -1;
	}

	mbedtls_ssl_set_bio(&s_ssl, rec, bioSend, bioRecv, NULL);
	u64 start = tickGetCount();
	u64 limit = ticksFromUsec(timeout_ms * 1000);
	while ((r = mbedtls_ssl_handshake(&s_ssl)) != 0) {
		if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) break;
		if (tickGetCount() - start > limit) {
			r = MBEDTLS_ERR_SSL_TIMEOUT;
			break;
		}
		threadSleep(5000);
	}
	if (r) {
		// -0x2700 (certificate) means RA wanted a full handshake: the saved
		// session was refused or has expired
		dietPrint("[tls] handshake -%04x\n", -r);
		mbedtls_ssl_free(&s_ssl);
		return -1;
	}
	dietPrint("[tls] resumed, %s\n", mbedtls_ssl_get_ciphersuite(&s_ssl));
	return 0;
}

int tlsWrite(const void* data, u32 len)
{
	int r = mbedtls_ssl_write(&s_ssl, (const unsigned char*)data, len);
	if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
	return r;
}

int tlsRead(void* buf, u32 size)
{
	int r = mbedtls_ssl_read(&s_ssl, (unsigned char*)buf, size);
	if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return TLS_WOULD_BLOCK;
	if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
	return r;
}

void tlsClose(void)
{
	mbedtls_ssl_close_notify(&s_ssl);
	mbedtls_ssl_free(&s_ssl);
}
