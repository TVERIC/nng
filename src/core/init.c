//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

#include <stdio.h>
#include <stdlib.h>

static nni_mtx  nni_init_mtx;
static nni_list nni_init_list;

static int
nni_init_helper(void)
{
	int rv;

	nni_mtx_init(&nni_init_mtx);
	NNI_LIST_INIT(&nni_init_list, nni_initializer, i_node);

	if (((rv = nni_taskq_sys_init()) != 0) ||
	    ((rv = nni_reap_sys_init()) != 0) ||
	    ((rv = nni_timer_sys_init()) != 0) ||
	    ((rv = nni_aio_sys_init()) != 0) ||
	    ((rv = nni_random_sys_init()) != 0) ||
	    ((rv = nni_sock_sys_init()) != 0) ||
	    ((rv = nni_ep_sys_init()) != 0) ||
	    ((rv = nni_pipe_sys_init()) != 0) ||
	    ((rv = nni_proto_sys_init()) != 0) ||
	    ((rv = nni_tran_sys_init()) != 0)) {
		nni_fini();
	}

	return (rv);
}

int
nni_init(void)
{
	return (nni_plat_init(nni_init_helper));
}

void
nni_fini(void)
{
	nni_initializer *init;

	if (!nni_list_empty(&nni_init_list)) {
		nni_mtx_lock(&nni_init_mtx);
		while ((init = nni_list_first(&nni_init_list)) != NULL) {
			if (init->i_fini != NULL) {
				init->i_fini();
			}
			init->i_once = 0;
			nni_list_remove(&nni_init_list, init);
		}
		nni_mtx_unlock(&nni_init_mtx);
	}
	nni_tran_sys_fini();
	nni_proto_sys_fini();
	nni_pipe_sys_fini();
	nni_ep_sys_fini();
	nni_sock_sys_fini();
	nni_random_sys_fini();
	nni_aio_sys_fini();
	nni_timer_sys_fini();
	nni_taskq_sys_fini();
	nni_reap_sys_fini();

	nni_mtx_fini(&nni_init_mtx);
	nni_plat_fini();
}

int
nni_initialize(nni_initializer *init)
{
	int rv;
	if (init->i_once) {
		return (0);
	}
	nni_mtx_lock(&nni_init_mtx);
	if (init->i_once) {
		nni_mtx_unlock(&nni_init_mtx);
		return (0);
	}
	if ((rv = init->i_init()) == 0) {
		init->i_once = 1;
		nni_list_append(&nni_init_list, init);
	}
	nni_mtx_unlock(&nni_init_mtx);
	return (rv);
}
