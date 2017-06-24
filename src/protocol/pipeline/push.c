//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"

// Push protocol.  The PUSH protocol is the "write" side of a pipeline.
// Push distributes fairly, or tries to, by giving messages in round-robin
// order.

typedef struct nni_push_pipe	nni_push_pipe;
typedef struct nni_push_sock	nni_push_sock;

static void nni_push_send_cb(void *);
static void nni_push_recv_cb(void *);
static void nni_push_getq_cb(void *);

// An nni_push_sock is our per-socket protocol private structure.
struct nni_push_sock {
	nni_msgq *	uwq;
	int		raw;
	nni_sock *	sock;
};

// An nni_push_pipe is our per-pipe protocol private structure.
struct nni_push_pipe {
	nni_pipe *	pipe;
	nni_push_sock * push;
	nni_list_node	node;

	nni_aio		aio_recv;
	nni_aio		aio_send;
	nni_aio		aio_getq;
	int		refcnt;
	nni_mtx		mtx;
};

static int
nni_push_sock_init(void **pushp, nni_sock *sock)
{
	nni_push_sock *push;
	int rv;

	if ((push = NNI_ALLOC_STRUCT(push)) == NULL) {
		return (NNG_ENOMEM);
	}
	push->raw = 0;
	push->sock = sock;
	push->uwq = nni_sock_sendq(sock);
	*pushp = push;
	nni_sock_recverr(sock, NNG_ENOTSUP);
	return (0);
}


static void
nni_push_sock_fini(void *arg)
{
	nni_push_sock *push = arg;

	if (push != NULL) {
		NNI_FREE_STRUCT(push);
	}
}


static void
nni_push_pipe_fini(void *arg)
{
	nni_push_pipe *pp = arg;

	nni_aio_fini(&pp->aio_recv);
	nni_aio_fini(&pp->aio_send);
	nni_aio_fini(&pp->aio_getq);
	nni_mtx_fini(&pp->mtx);
	NNI_FREE_STRUCT(pp);
}


static int
nni_push_pipe_init(void **ppp, nni_pipe *pipe, void *psock)
{
	nni_push_pipe *pp;
	int rv;

	if ((pp = NNI_ALLOC_STRUCT(pp)) == NULL) {
		return (NNG_ENOMEM);
	}
	if ((rv = nni_aio_init(&pp->aio_recv, nni_push_recv_cb, pp)) != 0) {
		goto fail;
	}
	if ((rv = nni_aio_init(&pp->aio_send, nni_push_send_cb, pp)) != 0) {
		goto fail;
		return (rv);
	}
	if ((rv = nni_aio_init(&pp->aio_getq, nni_push_getq_cb, pp)) != 0) {
		goto fail;
	}
	if ((rv = nni_mtx_init(&pp->mtx)) != 0) {
		goto fail;
	}

	NNI_LIST_NODE_INIT(&pp->node);
	pp->pipe = pipe;
	pp->push = psock;
	*ppp = pp;
	return (0);

fail:
	nni_push_pipe_fini(pp);
	return (rv);
}


static int
nni_push_pipe_start(void *arg)
{
	nni_push_pipe *pp = arg;
	nni_push_sock *push = pp->push;

	if (nni_pipe_peer(pp->pipe) != NNG_PROTO_PULL) {
		return (NNG_EPROTO);
	}

	nni_mtx_lock(&pp->mtx);
	pp->refcnt = 2;
	nni_mtx_unlock(&pp->mtx);

	// Schedule a receiver.  This is mostly so that we can detect
	// a closed transport pipe.
	nni_pipe_hold(pp->pipe);
	nni_pipe_aio_recv(pp->pipe, &pp->aio_recv);

	// Schedule a sender.
	nni_pipe_hold(pp->pipe);
	nni_msgq_aio_get(push->uwq, &pp->aio_getq);

	return (0);
}


static void
nni_push_pipe_stop(nni_push_pipe *pp)
{
	nni_push_sock *push = pp->push;
	int refcnt;

	nni_msgq_aio_cancel(push->uwq, &pp->aio_getq);

	nni_mtx_lock(&pp->mtx);
	NNI_ASSERT(pp->refcnt > 0);
	pp->refcnt--;
	refcnt = pp->refcnt;
	nni_mtx_unlock(&pp->mtx);

	if (refcnt == 0) {
		nni_pipe_remove(pp->pipe);
	}
}


static void
nni_push_recv_cb(void *arg)
{
	nni_push_pipe *pp = arg;

	// We normally expect to receive an error.  If a pipe actually
	// sends us data, we just discard it.
	if (nni_aio_result(&pp->aio_recv) != 0) {
		nni_push_pipe_stop(pp);
		return;
	}
	nni_msg_free(pp->aio_recv.a_msg);
	pp->aio_recv.a_msg = NULL;
	nni_pipe_aio_recv(pp->pipe, &pp->aio_recv);
}


static void
nni_push_send_cb(void *arg)
{
	nni_push_pipe *pp = arg;
	nni_push_sock *push = pp->push;

	if (nni_aio_result(&pp->aio_send) != 0) {
		nni_msg_free(pp->aio_send.a_msg);
		pp->aio_send.a_msg = NULL;
		nni_push_pipe_stop(pp);
		return;
	}

	nni_msgq_aio_get(push->uwq, &pp->aio_getq);
}


static void
nni_push_getq_cb(void *arg)
{
	nni_push_pipe *pp = arg;
	nni_aio *aio = &pp->aio_getq;

	if (nni_aio_result(aio) != 0) {
		// If the socket is closing, nothing else we can do.
		nni_push_pipe_stop(pp);
		return;
	}

	pp->aio_send.a_msg = aio->a_msg;
	aio->a_msg = NULL;

	nni_pipe_aio_send(pp->pipe, &pp->aio_send);
}


static int
nni_push_sock_setopt(void *arg, int opt, const void *buf, size_t sz)
{
	nni_push_sock *push = arg;
	int rv;

	switch (opt) {
	case NNG_OPT_RAW:
		rv = nni_setopt_int(&push->raw, buf, sz, 0, 1);
		break;
	default:
		rv = NNG_ENOTSUP;
	}
	return (rv);
}


static int
nni_push_sock_getopt(void *arg, int opt, void *buf, size_t *szp)
{
	nni_push_sock *push = arg;
	int rv;

	switch (opt) {
	case NNG_OPT_RAW:
		rv = nni_getopt_int(&push->raw, buf, szp);
		break;
	default:
		rv = NNG_ENOTSUP;
	}
	return (rv);
}


// This is the global protocol structure -- our linkage to the core.
// This should be the only global non-static symbol in this file.
static nni_proto_pipe_ops nni_push_pipe_ops = {
	.pipe_init	= nni_push_pipe_init,
	.pipe_fini	= nni_push_pipe_fini,
	.pipe_start	= nni_push_pipe_start,
};

static nni_proto_sock_ops nni_push_sock_ops = {
	.sock_init	= nni_push_sock_init,
	.sock_fini	= nni_push_sock_fini,
	.sock_setopt	= nni_push_sock_setopt,
	.sock_getopt	= nni_push_sock_getopt,
};

nni_proto nni_push_proto = {
	.proto_self	= NNG_PROTO_PUSH,
	.proto_peer	= NNG_PROTO_PULL,
	.proto_name	= "push",
	.proto_flags	= NNI_PROTO_FLAG_SND,
	.proto_pipe_ops = &nni_push_pipe_ops,
	.proto_sock_ops = &nni_push_sock_ops,
};
