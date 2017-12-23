//
// Copyright 2017 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"
#include "supplemental/base64/base64.h"
#include "supplemental/http/http.h"
#include "supplemental/sha1/sha1.h"

#include "websocket.h"

// Pre-defined types for some prototypes.  These are from other subsystems.
typedef struct ws_frame ws_frame;
typedef struct ws_msg   ws_msg;

struct nni_ws {
	int           mode; // NNI_EP_MODE_DIAL or NNI_EP_MODE_LISTEN
	nni_list_node node;
	bool          closed;
	bool          ready;
	nni_mtx       mtx;
	nni_list      txmsgs;
	nni_list      rxmsgs;
	ws_frame *    txframe;
	ws_frame *    rxframe;
	nni_aio *     txaio; // physical aios
	nni_aio *     rxaio;
	nni_aio *     closeaio;
	nni_aio *     httpaio; // server only, HTTP reply pending
	nni_http *    http;
	nni_http_req *req;
	nni_http_res *res;
	size_t        maxframe;
	size_t        fragsize;
};

struct nni_ws_listener {
	nni_tls_config *   tls;
	nni_http_server *  server;
	char *             proto;
	char *             url;
	char *             host;
	char *             serv;
	char *             path;
	nni_mtx            mtx;
	nni_list           pend;
	nni_list           reply;
	nni_list           aios;
	bool               started;
	bool               closed;
	void *             hp; // handler pointer
	nni_http_handler   handler;
	nni_ws_listen_hook hookfn;
	void *             hookarg;
};

// The dialer tracks user aios in two lists. The first list is for aios
// waiting for the http connection to be established, while the second
// are waiting for the HTTP negotiation to complete.  We keep two lists
// so we know whether to initiate another outgoing connection after the
// completion of an earlier connection.  (We don't want to establish
// requests when we already have connects negotiating.)
struct nni_ws_dialer {
	nni_tls_config * tls;
	nni_http_req *   req;
	nni_http_res *   res;
	nni_http_client *client;
	nni_mtx          mtx;
	nni_aio *        conaio;
	char *           proto;
	char *           host;
	char *           serv;
	char *           path;
	char *           qinfo;
	char *           addr;     // full address (a URL really)
	char *           uri;      // path + query
	nni_list         conaios;  // user aios waiting for connect.
	nni_list         httpaios; // user aios waiting for HTTP nego.
	bool             started;
	bool             closed;
	nng_sockaddr     sa;
};

typedef enum ws_type {
	WS_CONT   = 0x0,
	WS_TEXT   = 0x1,
	WS_BINARY = 0x2,
	WS_CLOSE  = 0x8,
	WS_PING   = 0x9,
	WS_PONG   = 0xA,
} ws_type;

typedef enum ws_reason {
	WS_CLOSE_NORMAL_CLOSE  = 1000,
	WS_CLOSE_GOING_AWAY    = 1001,
	WS_CLOSE_PROTOCOL_ERR  = 1002,
	WS_CLOSE_UNSUPP_FORMAT = 1003,
	WS_CLOSE_INVALID_DATA  = 1007,
	WS_CLOSE_POLICY        = 1008,
	WS_CLOSE_TOO_BIG       = 1009,
	WS_CLOSE_NO_EXTENSION  = 1010,
	WS_CLOSE_INTERNAL      = 1011,
} ws_reason;

struct ws_frame {
	nni_list_node node;
	uint8_t       head[14];   // maximum header size
	uint8_t       mask[4];    // read by server, sent by client
	uint8_t       sdata[125]; // short data (for short frames only)
	size_t        hlen;       // header length
	size_t        len;        // payload length
	enum ws_type  op;
	bool          final;
	bool          masked;
	size_t        bufsz; // allocated size
	uint8_t *     buf;
	ws_msg *      wmsg;
};

struct ws_msg {
	nni_list      frames;
	nni_list_node node;
	nni_ws *      ws;
	nni_msg *     msg;
	nni_aio *     aio;
};

static void ws_send_close(nni_ws *ws, uint16_t code);

// This looks, case independently for a word in a list, which is either
// space or comma separated.
static bool
ws_contains_word(const char *phrase, const char *word)
{
	size_t len = strlen(word);

	while ((phrase != NULL) && (*phrase != '\0')) {
		if ((nni_strncasecmp(phrase, word, len) == 0) &&
		    ((phrase[len] == 0) || (phrase[len] == ' ') ||
		        (phrase[len] == ','))) {
			return (true);
		}
		// Skip to next word.
		if ((phrase = strchr(phrase, ' ')) != NULL) {
			while ((*phrase == ' ') || (*phrase == ',')) {
				phrase++;
			}
		}
	}
	return (false);
}

// input is base64 challenge, output is the accepted.  input should be
// 24 character base 64, output is 28 character base64 reply.  (output
// must be large enough to hold 29 bytes to allow for termination.)
// Returns 0 on success, NNG_EINVAL if the input is malformed somehow.
static int
ws_make_accept(const char *key, char *accept)
{
	uint8_t      rawkey[16];
	uint8_t      digest[20];
	char         resp[29];
	nni_sha1_ctx ctx;

#define WS_KEY_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_KEY_GUIDLEN 36

	if ((strlen(key) != 24) ||
	    (nni_base64_decode(key, 24, rawkey, 16) != 16)) {
		return (NNG_EINVAL);
	}

	nni_sha1_init(&ctx);
	nni_sha1_update(&ctx, rawkey, 16);
	nni_sha1_update(&ctx, (uint8_t *) WS_KEY_GUID, WS_KEY_GUIDLEN);
	nni_sha1_final(&ctx, digest);

	nni_base64_encode(digest, 20, accept, 28);
	accept[28] = '\0';
	return (0);
}

static void
ws_frame_fini(ws_frame *frame)
{
	if (frame->bufsz) {
		nni_free(frame->buf, frame->bufsz);
	}
	NNI_FREE_STRUCT(frame);
}

static void
ws_msg_fini(ws_msg *wm)
{
	ws_frame *frame;

	while ((frame = nni_list_first(&wm->frames)) != NULL) {
		nni_list_remove(&wm->frames, frame);
		ws_frame_fini(frame);
	}

	if (wm->msg != NULL) {
		nni_msg_free(wm->msg);
	}
	NNI_FREE_STRUCT(wm);
}

static void
ws_mask_frame(ws_frame *frame)
{
	uint32_t r;
	// frames sent by client need mask.
	if (frame->masked) {
		return;
	}
	NNI_PUT32(frame->mask, r);
	for (int i = 0; i < frame->len; i++) {
		frame->buf[i] ^= frame->mask[i % 4];
	}
	memcpy(frame->head + frame->hlen, frame->mask, 4);
	frame->hlen += 4;
	frame->head[1] |= 0x80; // set masked bit
	frame->masked = true;
}

static void
ws_unmask_frame(ws_frame *frame)
{
	// frames sent by client need mask.
	if (!frame->masked) {
		return;
	}
	for (int i = 0; i < frame->len; i++) {
		frame->buf[i] ^= frame->mask[i % 4];
	}
	frame->hlen -= 4;
	frame->head[1] &= 0x7f; // clear masked bit
	frame->masked = false;
}

static int
ws_msg_init_control(
    ws_msg **wmp, nni_ws *ws, uint8_t op, const uint8_t *buf, size_t len)
{
	ws_msg *  wm;
	ws_frame *frame;

	if (len > 125) {
		return (NNG_EINVAL);
	}

	if ((wm = NNI_ALLOC_STRUCT(wm)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&wm->frames, ws_frame, node);
	memcpy(frame->sdata, buf, len);

	if ((frame = NNI_ALLOC_STRUCT(frame)) == NULL) {
		ws_msg_fini(wm);
		return (NNG_ENOMEM);
	}

	nni_list_append(&wm->frames, frame);
	frame->wmsg    = wm;
	frame->len     = len;
	frame->op      = op;
	frame->head[0] = op | 0x80; // final frame (control)
	frame->head[1] = len & 0x7F;
	frame->hlen    = 2;
	frame->buf     = frame->sdata;
	frame->bufsz   = 0;

	if (ws->mode == NNI_EP_MODE_DIAL) {
		ws_mask_frame(frame);
	} else {
		frame->masked = false;
	}

	wm->aio = NULL;
	wm->ws  = ws;
	return (0);
}

static int
ws_msg_init_tx(ws_msg **wmp, nni_ws *ws, nni_msg *msg, nni_aio *aio)
{
	ws_msg * wm;
	size_t   len;
	size_t   maxfrag = ws->fragsize; // make this tunable. (1MB default)
	uint8_t *buf;
	uint8_t  op;
	int      rv;

	// If the message has a header, move it to front of body.  Most of
	// the time this will not cause a reallocation (there should be
	// headroom).  Doing this simplifies our framing, and avoids sending
	// tiny frames for headers.
	if ((len = nni_msg_header_len(msg)) != 0) {
		buf = nni_msg_header(msg);
		if ((rv = nni_msg_insert(msg, buf, len)) != 0) {
			return (rv);
		}
		nni_msg_header_clear(msg);
	}

	if ((wm = NNI_ALLOC_STRUCT(wm)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&wm->frames, ws_frame, node);

	len = nni_msg_len(msg);
	buf = nni_msg_body(msg);
	op  = WS_BINARY; // to start -- no support for sending TEXT frames

	// do ... while because we want at least one frame (even for empty
	// messages.)   Headers get their own frame, if present.  Best bet
	// is to try not to have a header when coming here.
	do {
		ws_frame *frame;

		if ((frame = NNI_ALLOC_STRUCT(frame)) == NULL) {
			ws_msg_fini(wm);
			return (NNG_ENOMEM);
		}
		nni_list_append(&wm->frames, frame);
		frame->wmsg = wm;
		frame->len  = len > maxfrag ? maxfrag : len;
		frame->buf  = buf;
		frame->op   = op;

		buf += frame->len;
		len -= frame->len;
		op = WS_CONT;

		if (len == 0) {
			frame->final = true;
		}
		frame->head[0] = frame->op;
		frame->hlen    = 2;
		if (frame->final) {
			frame->head[0] |= 0x80; // final frame bit
		}
		if (frame->len < 126) {
			frame->head[1] = frame->len & 0x7f;
		} else if (frame->len < 65536) {
			frame->head[1] = 126;
			NNI_PUT16(frame->head + 2, (frame->len & 0xffff));
			frame->hlen += 2;
		} else {
			frame->head[1] = 127;
			NNI_PUT64(frame->head + 2, (uint64_t) frame->len);
			frame->hlen += 8;
		}

		if (ws->mode == NNI_EP_MODE_DIAL) {
			ws_mask_frame(frame);
		} else {
			frame->masked = false;
		}

	} while (len);

	wm->msg = msg;
	wm->aio = aio;
	wm->ws  = ws;
	*wmp    = wm;
	return (0);
}

static int
ws_msg_init_rx(ws_msg **wmp, nni_ws *ws, nni_aio *aio)
{
	ws_msg *wm;

	if ((wm = NNI_ALLOC_STRUCT(wm)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&wm->frames, ws_frame, node);
	wm->aio = aio;
	wm->ws  = ws;
	*wmp    = wm;
	return (0);
}

static void
ws_close_cb(void *arg)
{
	nni_ws *ws = arg;
	ws_msg *wm;

	// Either we sent a close frame, or we didn't.  Either way,
	// we are done, and its time to abort everything else.
	nni_mtx_lock(&ws->mtx);

	nni_http_close(ws->http);
	nni_aio_cancel(ws->txaio, NNG_ECLOSED);
	nni_aio_cancel(ws->rxaio, NNG_ECLOSED);

	// This list (receive) should be empty.
	while ((wm = nni_list_first(&ws->rxmsgs)) != NULL) {
		nni_list_remove(&ws->rxmsgs, wm);
		if (wm->aio) {
			nni_aio_finish_error(wm->aio, NNG_ECLOSED);
		}
		ws_msg_fini(wm);
	}

	while ((wm = nni_list_first(&ws->txmsgs)) != NULL) {
		nni_list_remove(&ws->txmsgs, wm);
		if (wm->aio) {
			nni_aio_finish_error(wm->aio, NNG_ECLOSED);
		}
		ws_msg_fini(wm);
	}

	if (ws->rxframe != NULL) {
		ws_frame_fini(ws->rxframe);
		ws->rxframe = NULL;
	}

	// Any txframe should have been killed with its wmsg.
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_close(nni_ws *ws, uint16_t code)
{
	ws_msg *wm;

	// Receive stuff gets aborted always.  No further receives
	// once we get a close.
	while ((wm = nni_list_first(&ws->rxmsgs)) != NULL) {
		nni_list_remove(&ws->rxmsgs, wm);
		if (wm->aio) {
			nni_aio_finish_error(wm->aio, NNG_ECLOSED);
		}
		ws_msg_fini(wm);
	}

	// If were closing "gracefully", then don't abort in-flight
	// stuff yet.  Note that reads should have stopped already.
	if (!ws->closed) {
		ws_send_close(ws, code);
		return;
	}

	while ((wm = nni_list_first(&ws->txmsgs)) != NULL) {
		nni_list_remove(&ws->txmsgs, wm);
		if (wm->aio) {
			nni_aio_finish_error(wm->aio, NNG_ECLOSED);
		}
		ws_msg_fini(wm);
	}
}

static void
ws_start_write(nni_ws *ws)
{
	ws_frame *frame;
	ws_msg *  wm;

	if ((ws->txframe != NULL) || (!ws->ready)) {
		return; // busy
	}

	if ((wm = nni_list_first(&ws->txmsgs)) == NULL) {
		// Nothing to send.
		return;
	}

	frame = nni_list_first(&wm->frames);
	NNI_ASSERT(frame != NULL);

	// Push it out.
	ws->txframe                 = frame;
	ws->txaio->a_niov           = frame->len > 0 ? 2 : 1;
	ws->txaio->a_iov[0].iov_len = frame->hlen;
	ws->txaio->a_iov[0].iov_buf = frame->head;
	ws->txaio->a_iov[1].iov_len = frame->len;
	ws->txaio->a_iov[1].iov_buf = frame->buf;
	nni_http_write_full(ws->http, ws->txaio);
}

static void
ws_write_cb(void *arg)
{
	nni_ws *  ws = arg;
	ws_frame *frame;
	ws_msg *  wm;
	nni_aio * aio;
	int       rv;

	nni_mtx_lock(&ws->mtx);

	if (ws->txframe->op == WS_CLOSE) {
		// If this was a close frame, we are done.
		// No other messages may succeed..
		while ((wm = nni_list_first(&ws->txmsgs)) != NULL) {
			nni_list_remove(&ws->txmsgs, wm);
			nni_aio_set_msg(wm->aio, NULL);
			nni_aio_finish_error(wm->aio, NNG_ECLOSED);
			ws_msg_fini(wm);
		}
		nni_mtx_unlock(&ws->mtx);
		return;
	}

	frame = ws->txframe;
	wm    = frame->wmsg;
	aio   = wm->aio;

	if ((rv = nni_aio_result(ws->txaio)) != 0) {

		ws_msg_fini(wm);
		if (aio != NULL) {
			nni_aio_finish_error(aio, rv);
		}

		ws->closed = true;
		nni_http_close(ws->http);
		nni_mtx_unlock(&ws->mtx);
		return;
	}

	// good frame, was it the last
	nni_list_remove(&wm->frames, frame);
	ws_frame_fini(frame);
	if (nni_list_empty(&wm->frames)) {
		nni_list_remove(&ws->txmsgs, wm);
		ws_msg_fini(wm);
		if (aio != NULL) {
			nni_aio_finish(aio, 0, 0);
		}
	}

	// Write the next frame.
	ws_start_write(ws);

	nni_mtx_unlock(&ws->mtx);
}

static void
ws_write_cancel(nni_aio *aio, int rv)
{
	nni_ws *  ws;
	ws_msg *  wm;
	ws_frame *frame;

	// Is this aio active?  We can tell by looking at the
	// active tx frame.

	wm = aio->a_prov_data;
	ws = wm->ws;
	if (((frame = ws->txframe) != NULL) && (frame->wmsg == wm)) {
		nni_aio_cancel(ws->txaio, rv);
		// We will wait for callback on the txaio to finish aio.
	} else if (nni_list_active(&ws->txmsgs, wm)) {
		// If scheduled, just need to remove node and complete it.
		nni_list_remove(&ws->txmsgs, wm);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_send_close(nni_ws *ws, uint16_t code)
{
	ws_msg * wm;
	uint8_t  buf[sizeof(uint16_t)];
	int      rv;
	nni_aio *aio;

	NNI_PUT16(buf, code);

	if (ws->closed) {
		return;
	}
	ws->closed = true;
	aio        = ws->closeaio;

	// We don't care about cancellation here.  If this times out,
	// we will still shut all the physical I/O down in the callback.
	if (nni_aio_start(aio, NULL, NULL) == 0) {
		return;
	}
	rv = ws_msg_init_control(&wm, ws, WS_CLOSE, buf, sizeof(buf));
	if (rv != 0) {
		nni_aio_finish_error(aio, rv);
		return;
	}
	// Close frames get priority!
	nni_list_prepend(&ws->txmsgs, wm);
	ws_start_write(ws);
}

static void
ws_send_control(nni_ws *ws, uint8_t op, uint8_t *buf, size_t len)
{
	ws_msg *wm;

	// Note that we do not care if this works or not.  So no AIO needed.

	nni_mtx_lock(&ws->mtx);
	if ((ws->closed) ||
	    (ws_msg_init_control(&wm, ws, op, buf, sizeof(buf)) != 0)) {
		nni_mtx_unlock(&ws->mtx);
		return;
	}

	// Control frames at head of list.  (Note that this may preempt
	// the close frame or other ping/pong requests.  Oh well.)
	nni_list_prepend(&ws->txmsgs, wm);
	ws_start_write(ws);
	nni_mtx_unlock(&ws->mtx);
}

void
nni_ws_send_msg(nni_ws *ws, nni_aio *aio)
{
	ws_msg * wm;
	nni_msg *msg;
	int      rv;

	msg = nni_aio_get_msg(aio);

	if ((rv = ws_msg_init_tx(&wm, ws, msg, aio)) != 0) {
		if (nni_aio_start(aio, NULL, NULL) == 0) {
			nni_aio_finish_error(aio, rv);
		}
		return;
	}

	nni_mtx_lock(&ws->mtx);
	nni_aio_set_msg(aio, NULL);

	if (ws->closed) {
		ws_msg_fini(wm);
		if (nni_aio_start(aio, NULL, NULL) == 0) {
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
		nni_mtx_unlock(&ws->mtx);
		return;
	}
	if (nni_aio_start(aio, ws_write_cancel, wm) == 0) {
		nni_mtx_unlock(&ws->mtx);
		ws_msg_fini(wm);
		return;
	}
	nni_list_append(&ws->txmsgs, wm);
	ws_start_write(ws);
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_start_read(nni_ws *ws)
{
	ws_frame *frame;
	ws_msg *  wm;
	nni_aio * aio;

	if ((ws->rxframe != NULL) || ws->closed) {
		return; // already reading or closed
	}

	if ((wm = nni_list_first(&ws->rxmsgs)) == NULL) {
		return; // no body expecting a message.
	}

	if ((frame = NNI_ALLOC_STRUCT(frame)) == NULL) {
		nni_list_remove(&ws->rxmsgs, wm);
		if (wm->aio != NULL) {
			nni_aio_finish_error(wm->aio, NNG_ENOMEM);
		}
		ws_msg_fini(wm);
		// XXX: NOW WHAT?
		return;
	}

	// Note that the frame is *not* associated with the message
	// as yet, because we don't know if that's right until we receive it.
	frame->hlen = 0;
	frame->len  = 0;
	ws->rxframe = frame;

	aio                   = ws->rxaio;
	aio->a_niov           = 1;
	aio->a_iov[0].iov_len = 2; // We want the first two bytes.
	aio->a_iov[0].iov_buf = frame->buf;
	nni_http_read_full(ws->http, aio);
}

static void
ws_read_frame_cb(nni_ws *ws, ws_frame *frame)
{
	ws_msg *wm = nni_list_first(&ws->rxmsgs);

	switch (frame->op) {
	case WS_CONT:
		if (wm == NULL) {
			ws_close(ws, WS_CLOSE_GOING_AWAY);
			return;
		}
		if (nni_list_empty(&wm->frames)) {
			ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
			return;
		}
		ws->rxframe = NULL;
		nni_list_append(&wm->frames, frame);
		break;
	case WS_BINARY:
		if (wm == NULL) {
			ws_close(ws, WS_CLOSE_GOING_AWAY);
			return;
		}
		if (!nni_list_empty(&wm->frames)) {
			ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
			return;
		}
		ws->rxframe = NULL;
		nni_list_append(&wm->frames, frame);
		break;
	case WS_TEXT:
		// No support for text mode at present.
		ws_close(ws, WS_CLOSE_UNSUPP_FORMAT);
		return;

	case WS_PING:
		if (frame->len > 125) {
			ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
			return;
		}
		ws_send_control(ws, WS_PONG, frame->buf, frame->len);
		ws->rxframe = NULL;
		ws_frame_fini(frame);
		break;
	case WS_PONG:
		if (frame->len > 125) {
			ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
			return;
		}
		ws->rxframe = NULL;
		ws_frame_fini(frame);
		break;
	case WS_CLOSE:
		ws->closed = true; // no need to send close reply
		ws_close(ws, 0);
		return;
	default:
		ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
		return;
	}

	// If this was the last (final) frame, then complete it.  But
	// we have to look at the msg, since we might have got a
	// control frame.
	if (((frame = nni_list_last(&wm->frames)) != NULL) && frame->final) {
		size_t   len = 0;
		nni_msg *msg;
		uint8_t *body;
		int      rv;

		nni_list_remove(&ws->rxmsgs, wm);
		NNI_LIST_FOREACH (&wm->frames, frame) {
			len += frame->len;
		}
		if ((rv = nni_msg_alloc(&msg, len)) != 0) {
			nni_aio_finish_error(wm->aio, rv);
			ws_msg_fini(wm);
			ws_close(ws, WS_CLOSE_INTERNAL);
			return;
		}
		body = nni_msg_body(msg);
		NNI_LIST_FOREACH (&wm->frames, frame) {
			memcpy(body, frame->buf, frame->len);
			body += frame->len;
		}
		nni_aio_finish_msg(wm->aio, msg);
		ws_msg_fini(wm);
	}
}

static void
ws_read_cb(void *arg)
{
	nni_ws *  ws  = arg;
	nni_aio * aio = ws->rxaio;
	ws_frame *frame;
	int       rv;

	nni_mtx_lock(&ws->mtx);
	if ((frame = ws->rxframe) == NULL) {
		nni_mtx_unlock(&ws->mtx); // canceled during close
		return;
	}

	if ((rv = nni_aio_result(aio)) != 0) {
		ws->closed = true; // do not send a close frame
		ws_close(ws, 0);
		nni_mtx_unlock(&ws->mtx);
		return;
	}

	if (frame->hlen == 0) {
		frame->hlen   = 2;
		frame->op     = frame->head[0] & 0x7f;
		frame->final  = frame->head[0] & 0x80 ? 1 : 0;
		frame->masked = frame->head[1] & 0x80 ? 1 : 0;
		if (frame->masked) {
			frame->hlen += 4;
		}
		if ((frame->head[1] & 0x7F) == 127) {
			frame->hlen += 8;
		} else if ((frame->head[1] & 0x7F) == 126) {
			frame->hlen += 2;
		}

		// If we didn't read the full header yet, then read
		// the rest of it.
		if (frame->hlen != 2) {
			aio->a_niov           = 1;
			aio->a_iov[0].iov_buf = frame->head + 2;
			aio->a_iov[0].iov_len = frame->hlen - 2;
			nni_http_read_full(ws->http, aio);
			nni_mtx_unlock(&ws->mtx);
			return;
		}
	}

	// If we are returning from a read of additional data, then
	// the buf will be set.  Otherwise we need to determine
	// how much data to read.  As our headers are complete, we take
	// this time to do some protocol checks -- no point in waiting
	// to read data.  (Frame size check needs to be done first
	// anyway to prevent DoS.)

	if (frame->buf == NULL) {

		// Determine expected frame size.
		switch ((frame->len = (frame->head[1] & 0x7F))) {
		case 127:
			NNI_GET64(frame->head + 2, frame->len);
			if (frame->len < 65536) {
				ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
				nni_mtx_unlock(&ws->mtx);
				return;
			}
			break;
		case 126:
			NNI_GET16(frame->head + 2, frame->len);
			if (frame->len < 126) {
				ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
				nni_mtx_unlock(&ws->mtx);
				return;
			}

			break;
		}

		if (frame->len > ws->maxframe) {
			ws_close(ws, WS_CLOSE_TOO_BIG);
			nni_mtx_unlock(&ws->mtx);
			return;
		}

		// Check for masking.  (We don't actually do the unmask
		// here, because we don't have data yet.)
		if (frame->masked) {
			memcpy(frame->mask, frame->head + frame->hlen - 4, 4);
			if (ws->mode == NNI_EP_MODE_DIAL) {
				ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
				nni_mtx_unlock(&ws->mtx);
				return;
			}
		} else if (ws->mode == NNI_EP_MODE_LISTEN) {
			ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
			nni_mtx_unlock(&ws->mtx);
			return;
		}

		// If we expected data, then ask for it.
		if (frame->len != 0) {

			// Short frames can avoid an alloc
			if (frame->len < 126) {
				frame->buf   = frame->sdata;
				frame->bufsz = 0;
			} else {
				frame->buf = nni_alloc(frame->len);
				if (frame->buf == NULL) {
					ws_close(ws, WS_CLOSE_INTERNAL);
					nni_mtx_unlock(&ws->mtx);
					return;
				}
				frame->bufsz = frame->len;
			}

			aio->a_niov           = 1;
			aio->a_iov[0].iov_buf = frame->buf;
			aio->a_iov[0].iov_len = frame->len;
			nni_http_read_full(ws->http, aio);
			nni_mtx_unlock(&ws->mtx);
			return;
		}
	}

	// At this point, we have a complete frame.
	ws_unmask_frame(frame); // idempotent

	ws_read_frame_cb(ws, frame);
	ws_start_read(ws);
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_read_cancel(nni_aio *aio, int rv)
{
	ws_msg *wm = aio->a_prov_data;
	nni_ws *ws = wm->ws;

	nni_mtx_lock(&ws->mtx);
	if (wm == nni_list_first(&ws->rxmsgs)) {
		// Cancellation will percolate back up.
		nni_aio_cancel(ws->rxaio, rv);
	} else if (nni_list_active(&ws->rxmsgs, wm)) {
		nni_list_remove(&ws->rxmsgs, wm);
		ws_msg_fini(wm);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&ws->mtx);
}

void
nni_ws_recv_msg(nni_ws *ws, nni_aio *aio)
{
	ws_msg *wm;
	int     rv;
	nni_mtx_lock(&ws->mtx);
	if ((rv = ws_msg_init_rx(&wm, ws, aio)) != 0) {
		if (nni_aio_start(aio, NULL, NULL)) {
			nni_aio_finish_error(aio, rv);
		}
		nni_mtx_unlock(&ws->mtx);
		return;
	}
	nni_list_append(&ws->rxmsgs, wm);
	ws_start_read(ws);
	nni_mtx_unlock(&ws->mtx);
}

void
nni_ws_close_error(nni_ws *ws, uint16_t code)
{
	nni_mtx_lock(&ws->mtx);
	ws_close(ws, code);
	nni_mtx_unlock(&ws->mtx);
}

void
nni_ws_close(nni_ws *ws)
{
	nni_ws_close_error(ws, WS_CLOSE_NORMAL_CLOSE);
}

nni_http_res *
nni_ws_response(nni_ws *ws)
{
	return (ws->res);
}

nni_http_req *
nni_ws_request(nni_ws *ws)
{
	return (ws->req);
}

void
nni_ws_fini(nni_ws *ws)
{
	ws_msg *wm;

	nni_ws_close(ws);

	// Give a chance for the close frame to drain.
	if (ws->closeaio) {
		nni_aio_wait(ws->closeaio);
	}

	nni_aio_stop(ws->rxaio);
	nni_aio_stop(ws->txaio);
	nni_aio_stop(ws->closeaio);
	nni_aio_stop(ws->httpaio);

	nni_mtx_lock(&ws->mtx);
	while ((wm = nni_list_first(&ws->rxmsgs)) != NULL) {
		nni_list_remove(&ws->rxmsgs, wm);
		if (wm->aio) {
			nni_aio_finish_error(wm->aio, NNG_ECLOSED);
		}
		ws_msg_fini(wm);
	}

	while ((wm = nni_list_first(&ws->txmsgs)) != NULL) {
		nni_list_remove(&ws->txmsgs, wm);
		if (wm->aio) {
			nni_aio_finish_error(wm->aio, NNG_ECLOSED);
		}
		ws_msg_fini(wm);
	}

	if (ws->rxframe) {
		ws_frame_fini(ws->rxframe);
	}
	nni_mtx_unlock(&ws->mtx);

	nni_http_fini(ws->http);
	nni_aio_fini(ws->rxaio);
	nni_aio_fini(ws->txaio);
	nni_aio_fini(ws->closeaio);
	nni_aio_fini(ws->httpaio);
	nni_mtx_fini(&ws->mtx);
	NNI_FREE_STRUCT(ws);
}

static void
ws_http_cb_listener(nni_ws *ws, nni_aio *aio)
{
	// This is only
	nni_ws_listener *l;
	l = nni_aio_get_data(aio, 0);

	nni_mtx_lock(&l->mtx);
	nni_list_remove(&l->reply, ws);
	if (nni_aio_result(aio) != 0) {
		nni_ws_fini(ws);
		nni_mtx_unlock(&l->mtx);
		return;
	}
	ws->ready = true;
	if ((aio = nni_list_first(&l->aios)) != NULL) {
		nni_aio_finish_pipe(aio, ws);
	} else {
		nni_list_append(&l->pend, ws);
	}
	nni_mtx_unlock(&l->mtx);
}

static void
ws_http_cb_dialer(nni_ws *ws, nni_aio *aio)
{
	nni_ws_dialer *d;
	nni_aio *      uaio;
	int            rv;
	uint16_t       status;
	char           wskey[29];
	const char *   ptr;

	d = nni_aio_get_data(aio, 0);

	nni_mtx_lock(&d->mtx);
	uaio = nni_list_first(&d->httpaios);
	NNI_ASSERT(uaio != NULL);
	// We have two steps.  In step 1, we just sent the request,
	// and need to retrieve the reply.  In step two we have
	// received the reply, and need to validate it.
	if ((rv = nni_aio_result(aio)) != 0) {
		goto err;
	}

	// If we have no response structure, then this was completion of the
	// send of the request.  Prepare an empty response, and read it.
	if (ws->res == NULL) {
		if ((rv = nni_http_res_init(&ws->res)) != 0) {
			goto err;
		}
		nni_http_read_res(ws->http, ws->res, ws->httpaio);
		nni_mtx_unlock(&d->mtx);
		return;
	}

	status = nni_http_res_get_status(ws->res);
	switch (status) {
	case NNI_HTTP_STATUS_SWITCHING:
		break;
	case NNI_HTTP_STATUS_FORBIDDEN:
	case NNI_HTTP_STATUS_UNAUTHORIZED:
		rv = NNG_EPERM;
		goto err;
	case NNI_HTTP_STATUS_NOT_FOUND:
	case NNI_HTTP_STATUS_METHOD_NOT_ALLOWED:
		rv = NNG_ECONNREFUSED; // Treat these as refusals.
		goto err;
	case NNI_HTTP_STATUS_BAD_REQUEST:
	default:
		// Perhaps we should use NNG_ETRANERR...
		rv = NNG_EPROTO;
		goto err;
	}

	// Check that the server gave us back the right key.
	rv = ws_make_accept(
	    nni_http_req_get_header(ws->req, "Sec-WebSocket-Key"), wskey);
	if (rv != 0) {
		goto err;
	}

#define GETH(h) nni_http_res_get_header(ws->res, h)

	if (((ptr = GETH("Sec-WebSocket-Accept")) == NULL) ||
	    (strcmp(ptr, wskey) != 0) ||
	    ((ptr = GETH("Connection")) != NULL) ||
	    (!ws_contains_word(ptr, "upgrade")) ||
	    ((ptr = GETH("Upgrade")) != NULL) ||
	    (strcmp(ptr, "websocket") != 0)) {
		nni_ws_close_error(ws, WS_CLOSE_PROTOCOL_ERR);
		rv = NNG_EPROTO;
		goto err;
	}
	if (d->proto != NULL) {
		if (((ptr = GETH("Sec-WebSocket-Protocol")) == NULL) ||
		    (!ws_contains_word(d->proto, ptr))) {
			nni_ws_close_error(ws, WS_CLOSE_PROTOCOL_ERR);
			rv = NNG_EPROTO;
			goto err;
		}
	}
#undef GETH

	// At this point, we are in business!
	nni_aio_list_remove(uaio);
	nni_aio_finish_pipe(uaio, ws);
	nni_mtx_unlock(&d->mtx);
	return;
err:
	nni_aio_list_remove(uaio);
	nni_aio_finish_error(uaio, rv);
	nni_ws_fini(ws);
	nni_mtx_unlock(&d->mtx);
}

static void
ws_http_cb(void *arg)
{
	// This is only done on the server/listener side.
	nni_ws * ws  = arg;
	nni_aio *aio = ws->httpaio;

	switch (ws->mode) {
	case NNI_EP_MODE_LISTEN:
		ws_http_cb_listener(ws, aio);
		break;
	case NNI_EP_MODE_DIAL:
		ws_http_cb_dialer(ws, aio);
		break;
	}
}

static int
ws_init(nni_ws **wsp, nni_http *http, nni_http_req *req, nni_http_res *res)
{
	nni_ws *ws;
	int     rv;

	if ((ws = NNI_ALLOC_STRUCT(ws)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&ws->mtx);
	NNI_LIST_INIT(&ws->rxmsgs, ws_msg, node);
	NNI_LIST_INIT(&ws->txmsgs, ws_msg, node);

	if (((rv = nni_aio_init(&ws->closeaio, ws_close_cb, ws)) != 0) ||
	    ((rv = nni_aio_init(&ws->txaio, ws_write_cb, ws)) != 0) ||
	    ((rv = nni_aio_init(&ws->rxaio, ws_read_cb, ws)) != 0) ||
	    ((rv = nni_aio_init(&ws->httpaio, ws_http_cb, ws)) != 0)) {
		nni_ws_fini(ws);
		return (rv);
	}

	ws->fragsize = 1 << 20; // we won't send a frame larger than this
	ws->maxframe = (1 << 20) * 10; // default limit on incoming frame size
	ws->http     = http;
	ws->req      = req;
	ws->res      = res;
	*wsp         = ws;
	return (0);
}

void
nni_ws_listener_fini(nni_ws_listener *l)
{
	nni_mtx_fini(&l->mtx);
	nni_strfree(l->url);
	nni_strfree(l->proto);
	nni_strfree(l->host);
	nni_strfree(l->serv);
	nni_strfree(l->path);
	NNI_FREE_STRUCT(l);
}

static void
ws_handler(nni_aio *aio)
{
	nni_ws_listener *l;
	nni_ws *         ws;
	nni_http *       http;
	nni_http_req *   req;
	nni_http_res *   res;
	const char *     ptr;
	const char *     proto;
	uint16_t         status;
	int              rv;
	char             key[29];

	http = nni_aio_get_input(aio, 0);
	req  = nni_aio_get_input(aio, 1);
	l    = nni_aio_get_input(aio, 2);

	// Now check the headers, etc.
	if (strcmp(nni_http_req_get_version(req), "HTTP/1.1") != 0) {
		status = NNI_HTTP_STATUS_HTTP_VERSION_NOT_SUPP;
		goto err;
	}

	if (strcmp(nni_http_req_get_method(req), "GET") != 0) {
		// HEAD request.  We can't really deal with it.
		status = NNI_HTTP_STATUS_BAD_REQUEST;
		goto err;
	}

#define GETH(h) nni_http_req_get_header(req, h)
#define SETH(h, v) nni_http_res_set_header(res, h, v)

	if ((((ptr = GETH("Content-Length")) != NULL) && (atoi(ptr) > 0)) ||
	    (((ptr = GETH("Transfer-Encoding")) != NULL) &&
	        (nni_strcasestr(ptr, "chunked") != NULL))) {
		status = NNI_HTTP_STATUS_PAYLOAD_TOO_LARGE;
		goto err;
	}

	// These headers have to be present.
	if (((ptr = GETH("Upgrade")) == NULL) ||
	    (!ws_contains_word(ptr, "websocket")) ||
	    ((ptr = GETH("Connection")) == NULL) ||
	    (!ws_contains_word(ptr, "upgrade")) ||
	    ((ptr = GETH("Sec-WebSocket-Version")) == NULL) ||
	    (strcmp(ptr, "13") != 0)) {
		status = NNI_HTTP_STATUS_BAD_REQUEST;
		goto err;
	}

	if (((ptr = GETH("Sec-WebSocket-Key")) == NULL) ||
	    (ws_make_accept(ptr, key) != 0)) {
		status = NNI_HTTP_STATUS_BAD_REQUEST;
		goto err;
	}

	// If the client has requested a specific subprotocol, then
	// we need to try to match it to what the handler says we support.
	// (If no suitable option is found in the handler, we fail the
	// request.)
	proto = GETH("Sec-WebSocket-Protocol");
	if (proto == NULL) {
		if (l->proto != NULL) {
			status = NNI_HTTP_STATUS_BAD_REQUEST;
			goto err;
		}
	} else if ((l->proto == NULL) ||
	    (!ws_contains_word(l->proto, proto))) {
		status = NNI_HTTP_STATUS_BAD_REQUEST;
		goto err;
	}

	if ((rv = nni_http_res_init(&res)) != 0) {
		// Give a chance to reply to client.
		status = NNI_HTTP_STATUS_INTERNAL_SERVER_ERROR;
		goto err;
	}

	if (nni_http_res_set_status(
	        res, NNI_HTTP_STATUS_SWITCHING, "Switching Protocols") != 0) {
		nni_http_res_fini(res);
		status = NNI_HTTP_STATUS_INTERNAL_SERVER_ERROR;
		goto err;
	}

	if ((SETH("Connection", "Upgrade") != 0) ||
	    (SETH("Upgrade", "websocket") != 0) ||
	    (SETH("Sec-WebSocket-Accept", key) != 0)) {
		status = NNI_HTTP_STATUS_INTERNAL_SERVER_ERROR;
		nni_http_res_fini(res);
		goto err;
	}
	if ((proto != NULL) && (SETH("Sec-WebSocket-Protocol", proto) != 0)) {
		status = NNI_HTTP_STATUS_INTERNAL_SERVER_ERROR;
		nni_http_res_fini(res);
		goto err;
	}

	if (l->hookfn != NULL) {
		rv = l->hookfn(l->hookarg, req, res);
		if (rv != 0) {
			nni_http_res_fini(res);
			nni_aio_finish_error(aio, rv);
			return;
		}

		if (nni_http_res_get_status(res) !=
		    NNI_HTTP_STATUS_SWITCHING) {
			// The hook has decided to give back a different
			// reply and we are not upgrading anymore.  For
			// example the Origin might not be permitted, or
			// another level of authentication may be required.
			// (Note that the hook can also give back various
			// other headers, but it would be bad for it to
			// alter the websocket mandated headers.)
			nni_http_req_fini(req);
			nni_aio_set_output(aio, 0, res);
			nni_aio_finish(aio, 0, 0);
			return;
		}
	}

#undef GETH
#undef SETH

	// We are good to go, provided we can get the websocket struct,
	// and send the reply.
	if ((rv = ws_init(&ws, http, req, res)) != 0) {
		nni_http_res_fini(res);
		status = NNI_HTTP_STATUS_INTERNAL_SERVER_ERROR;
		goto err;
	}
	ws->mode = NNI_EP_MODE_LISTEN;

	// XXX: Inherit fragmentation and message size limits!

	nni_list_append(&l->reply, ws);
	nni_aio_set_data(ws->httpaio, 0, l);
	nni_http_write_res(http, res, ws->httpaio);
	nni_aio_set_output(aio, 0, NULL);
	nni_aio_set_input(aio, 1, NULL);
	nni_aio_finish(aio, 0, 0);
	return;

err:
	nni_http_req_fini(req);
	if ((rv = nni_http_res_init_error(&res, status)) != 0) {
		nni_aio_finish_error(aio, rv);
	} else {
		nni_aio_set_output(aio, 0, res);
		nni_aio_finish(aio, 0, 0);
	}
}
static int
ws_parse_url(const char *url, char **schemep, char **hostp, char **servp,
    char **pathp, char **queryp)
{
	size_t scrlen;
	char * scr;
	char * pair;
	char * scheme = NULL;
	char * path   = NULL;
	char * query  = NULL;
	char * host   = NULL;
	char * serv   = NULL;
	int    rv;

	// We need a scratch copy of the url to parse.
	scrlen = strlen(url) + 1;
	if ((scr = nni_alloc(scrlen)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_strlcpy(scr, url, scrlen);
	scheme = scr;
	pair   = strchr(scr, ':');
	if ((pair == NULL) || (pair[1] != '/') || (pair[2] != '/')) {
		nni_free(scr, scrlen);
		return (NNG_EADDRINVAL);
	}

	*pair = '\0';
	pair += 3;

	path = strchr(pair, '/');
	if (path != NULL) {
		*path = '\0'; // We will restore it shortly.
	}
	if ((rv = nni_tran_parse_host_port(pair, hostp, servp)) != 0) {
		nni_free(scr, scrlen);
		return (rv);
	}

	// If service was missing, assume normal defaults.
	if (*servp == NULL) {
		if (strcmp(scheme, "wss")) {
			*servp = nni_strdup("443");
		} else {
			*servp = nni_strdup("80");
		}
	}

	if (path) {
		// Restore the path, and trim off the query parameter.
		*path = '/';
		if ((query = strchr(path, '?')) != NULL) {
			*query = '\0';
			query++;
		} else {
			query = "";
		}
	} else {
		path  = "/";
		query = "";
	}

	if (schemep) {
		*schemep = nni_strdup(scheme);
	}
	if (pathp) {
		*pathp = nni_strdup(path);
	}
	if (queryp) {
		*queryp = nni_strdup(query);
	}

	if ((schemep && (*schemep == NULL)) || (*pathp == NULL) ||
	    (*servp == NULL) || (*hostp == NULL) ||
	    (queryp && (*queryp == NULL))) {
		nni_strfree(*hostp);
		nni_strfree(*servp);
		if (schemep) {
			nni_strfree(*schemep);
		}
		if (pathp) {
			nni_strfree(*pathp);
		}
		if (queryp) {
			nni_strfree(*queryp);
		}
		nni_free(scr, scrlen);
		return (NNG_ENOMEM);
	}

	return (0);
}

int
nni_ws_listener_init(nni_ws_listener **wslp, const char *url)
{
	nni_ws_listener *l;
	int              rv;

	if ((l = NNI_ALLOC_STRUCT(l)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&l->mtx);
	nni_aio_list_init(&l->aios);

	NNI_LIST_INIT(&l->pend, nni_ws, node);
	NNI_LIST_INIT(&l->reply, nni_ws, node);

	rv = ws_parse_url(url, NULL, &l->host, &l->serv, &l->path, NULL);
	if (rv != 0) {
		nni_ws_listener_fini(l);
		return (rv);
	}
	l->handler.h_is_dir      = false;
	l->handler.h_is_upgrader = true;
	l->handler.h_method      = "GET";
	l->handler.h_path        = l->path;
	l->handler.h_host        = l->host;
	l->handler.h_cb          = ws_handler;

	*wslp = l;
	return (0);
}

int
nni_ws_listener_proto(nni_ws_listener *l, const char *proto)
{
	int   rv = 0;
	char *ns;
	nni_mtx_lock(&l->mtx);
	if (l->started) {
		rv = NNG_EBUSY;
	} else if ((ns = nni_strdup(proto)) == NULL) {
		rv = NNG_ENOMEM;
	} else {
		if (l->proto != NULL) {
			nni_strfree(l->proto);
		}
		l->proto = ns;
	}
	nni_mtx_unlock(&l->mtx);
	return (rv);
}

static void
ws_accept_cancel(nni_aio *aio, int rv)
{
	nni_ws_listener *l = aio->a_prov_data;

	nni_mtx_lock(&l->mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&l->mtx);
}

void
nni_ws_listener_accept(nni_ws_listener *l, nni_aio *aio)
{
	nni_ws *ws;

	nni_mtx_lock(&l->mtx);
	if (nni_aio_start(aio, ws_accept_cancel, l) != 0) {
		nni_mtx_unlock(&l->mtx);
		return;
	}
	if (l->closed) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
		nni_mtx_unlock(&l->mtx);
		return;
	}
	if (!l->started) {
		nni_aio_finish_error(aio, NNG_ESTATE);
		nni_mtx_unlock(&l->mtx);
		return;
	}
	if ((ws = nni_list_first(&l->pend)) != NULL) {
		nni_list_remove(&l->pend, ws);
		nni_aio_finish_pipe(aio, ws);
	} else {
		nni_list_append(&l->aios, aio);
	}
	nni_mtx_unlock(&l->mtx);
}

void
nni_ws_listener_close(nni_ws_listener *l)
{
	nni_aio *aio;
	nni_ws * ws;
	nni_mtx_lock(&l->mtx);
	if (l->closed) {
		nni_mtx_unlock(&l->mtx);
		return;
	}
	l->closed = true;
	if (l->server != NULL) {
		nni_http_server_del_handler(l->server, l->hp);
		nni_http_server_fini(l->server);
		l->server = NULL;
	}
	NNI_LIST_FOREACH (&l->pend, ws) {
		nni_ws_close_error(ws, WS_CLOSE_GOING_AWAY);
	}
	NNI_LIST_FOREACH (&l->reply, ws) {
		nni_ws_close_error(ws, WS_CLOSE_GOING_AWAY);
	}
	nni_mtx_unlock(&l->mtx);
}

int
nni_ws_listener_listen(nni_ws_listener *l)
{
	nng_sockaddr sa;
	nni_aio *    aio;
	int          rv;

	nni_mtx_lock(&l->mtx);
	if (l->closed) {
		nni_mtx_unlock(&l->mtx);
		return (NNG_ECLOSED);
	}
	if (l->started) {
		nni_mtx_unlock(&l->mtx);
		return (NNG_ESTATE);
	}

	if ((rv = nni_aio_init(&aio, NULL, NULL)) != 0) {
		nni_mtx_unlock(&l->mtx);
		return (rv);
	}
	aio->a_addr = &sa;
	nni_plat_tcp_resolv(l->host, l->serv, NNG_AF_UNSPEC, true, aio);
	nni_aio_wait(aio);
	rv = nni_aio_result(aio);
	nni_aio_fini(aio);
	if (rv != 0) {
		nni_mtx_unlock(&l->mtx);
		return (rv);
	}

	if ((rv = nni_http_server_init(&l->server, &sa)) != 0) {
		nni_mtx_unlock(&l->mtx);
		return (rv);
	}

	rv = nni_http_server_add_handler(&l->hp, l->server, &l->handler, l);
	if (rv != 0) {
		nni_http_server_fini(l->server);
		l->server = NULL;
		nni_mtx_unlock(&l->mtx);
		return (rv);
	}

	// XXX: DEAL WITH HTTPS here.

	if ((rv = nni_http_server_start(l->server)) != 0) {
		nni_http_server_del_handler(l->server, l->hp);
		nni_http_server_fini(l->server);
		l->server = NULL;
	}

	nni_mtx_unlock(&l->mtx);
	return (0);
}

void
nni_ws_listener_hook(
    nni_ws_listener *l, nni_ws_listen_hook hookfn, void *hookarg)
{
	nni_mtx_lock(&l->mtx);
	l->hookfn  = hookfn;
	l->hookarg = hookarg;
	nni_mtx_unlock(&l->mtx);
}

void
nni_ws_listener_tls(nni_ws_listener *l, nni_tls_config *tls)
{
	// We need to add this later.
}

void
ws_conn_cb(void *arg)
{
	nni_ws_dialer *d   = arg;
	nni_aio *      aio = d->conaio;
	nni_aio *      uaio;
	nni_http *     http;
	nni_http_req * req = NULL;
	int            rv;
	uint8_t        raw[16];
	char           wskey[25];
	nni_ws *       ws;

	nni_mtx_lock(&d->mtx);
	uaio = nni_list_first(&d->conaios);
	rv   = nni_aio_result(aio);
	http = rv == 0 ? nni_aio_get_output(aio, 0) : NULL;

	if (uaio == NULL) {
		if (http) {
			// Nobody listening anymore - hard abort.
			nni_http_fini(http);
		}
		nni_mtx_unlock(&d->mtx);
		return;
	}

	nni_aio_list_remove(uaio);
	nni_aio_set_output(aio, 0, NULL);

	// We are done with this aio, start another connection request while
	// we finish up, if we have more clients waiting.
	if (!nni_list_empty(&d->conaios)) {
		nni_http_client_connect(d->client, aio);
	}

	if (rv != 0) {
		goto err;
	}

	for (int i = 0; i < 16; i++) {
		raw[i] = nni_random();
	}
	nni_base64_encode(raw, 16, wskey, 24);
	wskey[24] = '\0';

	if (d->qinfo && d->qinfo[0] != '\0') {
		rv = nni_asprintf(&d->uri, "%s?%s", d->path, d->qinfo);
	} else if ((d->uri = nni_strdup(d->path)) == NULL) {
		rv = NNG_ENOMEM;
	}

#define SETH(h, v) nni_http_req_set_header(req, h, v)
	if ((rv != 0) || ((rv = nni_http_req_init(&req)) != 0) ||
	    ((rv = nni_http_req_set_uri(req, d->uri)) != 0) ||
	    ((rv = nni_http_req_set_version(req, "HTTP/1.1")) != 0) ||
	    ((rv = nni_http_req_set_method(req, "GET")) != 0) ||
	    ((rv = SETH("Host", d->host)) != 0) ||
	    ((rv = SETH("Upgrade", "websocket")) != 0) ||
	    ((rv = SETH("Connection", "Upgrade")) != 0) ||
	    ((rv = SETH("Sec-WebSocket-Key", wskey)) != 0) ||
	    ((rv = SETH("Sec-WebSocket-Version", "13")) != 0)) {
		goto err;
	}

	// If consumer asked for protocol, pass it on.
	if ((d->proto != NULL) &&
	    ((rv = SETH("Sec-WebSocket-Protocol", d->proto)) != 0)) {
		goto err;
	}
#undef SETH

	if ((rv = ws_init(&ws, http, req, NULL)) != 0) {
		goto err;
	}
	ws->mode = NNI_EP_MODE_DIAL;

	// Move this uaio to the http wait list.  Note that it is not
	// required that the uaio will be completed by this connection.
	// If another connection attempt completes first, then the first
	// aio queued will get the result.
	nni_list_append(&d->httpaios, uaio);
	nni_aio_set_data(ws->httpaio, 0, d);
	nni_http_write_req(http, req, ws->httpaio);
	nni_mtx_unlock(&d->mtx);
	return;

err:
	nni_aio_finish_error(uaio, rv);
	if (http != NULL) {
		nni_http_fini(http);
	}
	nni_mtx_unlock(&d->mtx);
}

void
nni_ws_dialer_fini(nni_ws_dialer *d)
{
	nni_strfree(d->proto);
	nni_strfree(d->addr);
	nni_strfree(d->uri);
	nni_strfree(d->host);
	nni_strfree(d->serv);
	nni_strfree(d->path);
	nni_strfree(d->qinfo);
	if (d->client) {
		nni_http_client_fini(d->client);
	}
	nni_mtx_fini(&d->mtx);
	NNI_FREE_STRUCT(d);
}

int
nni_ws_dialer_init(nni_ws_dialer **dp, const char *url)
{
	nni_ws_dialer *d;
	int            rv;
	nni_aio *      aio;

	if ((d = NNI_ALLOC_STRUCT(d)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&d->mtx);
	nni_aio_list_init(&d->conaios);
	nni_aio_list_init(&d->httpaios);

	if ((d->addr = nni_strdup(url)) == NULL) {
		nni_ws_dialer_fini(d);
		return (NNG_ENOMEM);
	}
	if ((rv = ws_parse_url(
	         url, NULL, &d->host, &d->serv, &d->path, &d->qinfo)) != 0) {
		nni_ws_dialer_fini(d);
		return (rv);
	}
	if ((rv = nni_aio_init(&d->conaio, ws_conn_cb, d)) != 0) {
		nni_ws_dialer_fini(d);
		return (rv);
	}

	if ((rv = nni_aio_init(&aio, NULL, NULL)) != 0) {
		nni_ws_dialer_fini(d);
		return (rv);
	}
	// XXX: this is synchronous.  We should fix this in the HTTP layer.
	aio->a_addr = &d->sa;
	nni_plat_tcp_resolv(d->host, d->serv, NNG_AF_UNSPEC, false, aio);
	nni_aio_wait(aio);
	rv = nni_aio_result(aio);
	nni_aio_fini(aio);
	if (rv != 0) {
		nni_ws_dialer_fini(d);
		return (rv);
	}

	if ((rv = nni_http_client_init(&d->client, &d->sa)) != 0) {
		nni_ws_dialer_fini(d);
		return (rv);
	}

	*dp = d;
	return (0);
}

void
nni_ws_dialer_close(nni_ws_dialer *d)
{
	// XXX: what to do here?
	nni_mtx_lock(&d->mtx);
	if (d->closed) {
		nni_mtx_unlock(&d->mtx);
		return;
	}
	d->closed = true;
	nni_mtx_unlock(&d->mtx);
	nni_aio_cancel(d->conaio, NNG_ECLOSED);
}

int
nni_ws_dialer_proto(nni_ws_dialer *d, const char *proto)
{
	int   rv = 0;
	char *ns;
	nni_mtx_lock(&d->mtx);
	if ((ns = nni_strdup(proto)) == NULL) {
		rv = NNG_ENOMEM;
	} else {
		if (d->proto != NULL) {
			nni_strfree(d->proto);
		}
		d->proto = ns;
	}
	nni_mtx_unlock(&d->mtx);
	return (rv);
}

static void
ws_dial_cancel(nni_aio *aio, int rv)
{
	nni_ws_dialer *d = aio->a_prov_data;
	nni_mtx_lock(&d->mtx);
	// If we are waiting, then we can cancel.  Otherwise we need
	// to abort.
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	// This does not cancel in-flight client negotiations with HTTP.
	nni_mtx_unlock(&d->mtx);
}

void
nni_ws_dialer_dial(nni_ws_dialer *d, nni_aio *aio)
{
	nni_mtx_lock(&d->mtx);
	// First look up the host.
	if (nni_aio_start(aio, ws_dial_cancel, d) != 0) {
		nni_mtx_unlock(&d->mtx);
		return;
	}
	if (d->closed) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
		nni_mtx_unlock(&d->mtx);
	}
	nni_list_append(&d->conaios, aio);

	if (!d->started) {
		d->started = true;
		nni_http_client_connect(d->client, d->conaio);
	}
	nni_mtx_unlock(&d->mtx);
}

extern int nni_ws_dialer_header(nni_ws_dialer *, const char *, const char *);

// Dialer does not get a hook chance, as it can examine the request
// and reply after dial is done; this is not a 3-way handshake, so
// the dialer does not confirm the server's response at the HTTP
// level. (It can still issue a websocket close).

// The implementation will send periodic PINGs, and respond with
// PONGs.
