/*
 * lanserv_ipmi.c
 *
 * MontaVista IPMI IPMI LAN interface protocol engine
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2003,2004,2005 MontaVista Software Inc.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * Lesser General Public License (GPL) Version 2 or the modified BSD
 * license below.  The following disclamer applies to both licenses:
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * GNU Lesser General Public Licence
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Modified BSD Licence
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *   3. The name of the author may not be used to endorse or promote
 *      products derived from this software without specific prior
 *      written permission.
 */

#include <config.h>

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <OpenIPMI/ipmi_msgbits.h>
#include <OpenIPMI/serv.h>

int
ipmi_oem_send_msg(channel_t     *chan,
		  unsigned char netfn,
		  unsigned char cmd,
		  unsigned char *data,
		  unsigned int  len,
		  long          oem_data)
{
    msg_t *nmsg;
    int   rv;

    nmsg = chan->alloc(chan, sizeof(*nmsg)+len);
    if (!nmsg) {
	chan->log(OS_ERROR, NULL,
		  "SMI message: out of memory");
	return ENOMEM;
    }

    memset(nmsg, 0, sizeof(*nmsg));
    nmsg->oem_data = oem_data;
    nmsg->netfn = netfn;
    nmsg->cmd = cmd;
    nmsg->data = ((unsigned char *) nmsg) + sizeof(*nmsg);
    nmsg->len = len;
    if (len > 0)
	memcpy(nmsg->data, data, len);
    
    rv = chan->smi_send(chan, nmsg);
    if (rv) {
	chan->log(OS_ERROR, nmsg,
		  "SMI send: error %d", rv);
	chan->free(chan, nmsg);
    }

    return rv;
}

void
ipmi_handle_smi_rsp(channel_t *chan, msg_t *msg, uint8_t *rspd, int rsp_len)
{
    rsp_msg_t rsp;

    rsp.netfn = msg->netfn | 1;
    rsp.cmd = msg->cmd;
    rsp.data = rspd;
    rsp.data_len = rsp_len;

    chan->return_rsp(chan, msg, &rsp);
    chan->free(chan, msg);
}

static oem_handler_t *oem_handlers = NULL;

void
ipmi_register_oem(oem_handler_t *handler)
{
    handler->next = oem_handlers;
    oem_handlers = handler;
}

static void
check_oem_handlers(channel_t *chan)
{
    oem_handler_t *c;

    c = oem_handlers;
    while (c) {
	if ((c->manufacturer_id == chan->manufacturer_id)
	    && (c->product_id == chan->product_id))
	{
	    c->handler(chan, c->cb_data);
	    break;
	}
	c = c->next;
    }
}

static int
look_for_get_devid(channel_t *chan, msg_t *msg, rsp_msg_t *rsp)
{
    if ((rsp->netfn == (IPMI_APP_NETFN | 1))
	&& (rsp->cmd == IPMI_GET_DEVICE_ID_CMD)
	&& (rsp->data_len >= 12)
	&& (rsp->data[0] == 0))
    {
	chan->oem.oem_handle_rsp = NULL;
	chan->manufacturer_id = (rsp->data[7]
				 | (rsp->data[8] << 8)
				 | (rsp->data[9] << 16));
	chan->product_id = rsp->data[10] | (rsp->data[11] << 8);
	check_oem_handlers(chan);

	/* Will be set to 1 if we sent it. */
	return msg->oem_data;
    }
    return 0;
}

int
chan_init(channel_t *chan)
{
    /* If the calling code already hasn't set up an OEM handler, we
       set up our own to look for a get device id.  When we find a get
       device ID, we call the OEM code to install their own. */
    if (chan->oem.oem_handle_rsp == NULL) {
	int rv;

	chan->oem.oem_handle_rsp = look_for_get_devid;

	/* Send a get device id to the low-level code so we can
           discover who we are. */
	rv = ipmi_oem_send_msg(chan,
			       IPMI_APP_NETFN, IPMI_GET_DEVICE_ID_CMD,
			       NULL, 0, 1);
    }

}