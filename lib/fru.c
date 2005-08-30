/*
 * fru.c
 *
 * IPMI code for handling FRUs
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002,2003 MontaVista Software Inc.
 *
 * Note that this file was originally written by Thomas Kanngieser
 * <thomas.kanngieser@fci.com> of FORCE Computers, but I've pretty
 * much gutted it and rewritten it, nothing really remained the same.
 * Thomas' code was helpful, though and many thanks go to him.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
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
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <OpenIPMI/ipmiif.h>
#include <OpenIPMI/ipmi_fru.h>
#include <OpenIPMI/ipmi_err.h>
#include <OpenIPMI/ipmi_msgbits.h>

#include <OpenIPMI/internal/locked_list.h>
#include <OpenIPMI/internal/ipmi_domain.h>
#include <OpenIPMI/internal/ipmi_int.h>
#include <OpenIPMI/internal/ipmi_utils.h>
#include <OpenIPMI/internal/ipmi_oem.h>
#include <OpenIPMI/internal/ipmi_fru.h>

#define MAX_FRU_DATA_FETCH 32
#define FRU_DATA_FETCH_DECR 8
#define MIN_FRU_DATA_FETCH 16

#define MAX_FRU_DATA_WRITE 16
#define MAX_FRU_WRITE_RETRIES 30

#define IPMI_FRU_ATTR_NAME "ipmi_fru"

/*
 * A note of FRUs, fru attributes, and locking.
 *
 * Because we keep a list of FRUs, that makes locking a lot more
 * complicated.  While we are deleting a FRU another thread can come
 * along and iterate and find it.  The lock on the locked list is used
 * along with the FRU lock to prevent this from happening.  Since in
 * this situation, the locked list lock is held when the FRU is
 * referenced, when we destroy the FRU we make sure that it wasn't
 * resurrected after being deleted from this list.
 */

/* Record used for FRU writing. */
typedef struct fru_update_s fru_update_t;
struct fru_update_s
{
    unsigned short offset;
    unsigned short length;
    fru_update_t   *next;
};

struct ipmi_fru_s
{
    char name[IPMI_FRU_NAME_LEN+1];
    int deleted;

    unsigned int refcount;

    /* Is the FRU being read or written? */
    int in_use;

    ipmi_lock_t *lock;

    ipmi_domain_id_t     domain_id;
    unsigned char        is_logical;
    unsigned char        device_address;
    unsigned char        device_id;
    unsigned char        lun;
    unsigned char        private_bus;
    unsigned char        channel;

    unsigned int        fetch_mask;

    ipmi_fru_fetched_cb fetched_handler;
    ipmi_fru_cb         domain_fetched_handler;
    void                *fetched_cb_data;

    ipmi_fru_destroyed_cb destroy_handler;
    void                  *destroy_cb_data;

    int           access_by_words;
    unsigned char *data;
    unsigned int  data_len;
    unsigned int  curr_pos;

    int           fetch_size;

    /* Is this in the list of FRUs? */
    int in_frulist;

    /* The records for writing. */
    fru_update_t *update_recs;
    fru_update_t *update_recs_tail;

    /* The last send command for writing */
    unsigned char last_cmd[MAX_FRU_DATA_WRITE+4];
    unsigned int  last_cmd_len;
    unsigned int  retry_count;

    os_handler_t *os_hnd;

    /* If the FRU is a "normal" fru type, for backwards
       compatability. */
    int  normal_fru;

    char *fru_rec_type;
    void *rec_data;
    ipmi_fru_op_t *ops;


    char iname[IPMI_FRU_NAME_LEN+1];
};

#define FRU_DOMAIN_NAME(fru) (fru ? fru->iname : "")

static void final_fru_destroy(ipmi_fru_t *fru);

/***********************************************************************
 *
 * general utilities
 *
 **********************************************************************/
void
_ipmi_fru_lock(ipmi_fru_t *fru)
{
    ipmi_lock(fru->lock);
}

void
_ipmi_fru_unlock(ipmi_fru_t *fru)
{
    ipmi_unlock(fru->lock);
}

/*
 * Must already be holding the FRU lock to call this.
 */
static void
fru_get(ipmi_fru_t *fru)
{
    fru->refcount++;
}

static void
fru_put(ipmi_fru_t *fru)
{
    _ipmi_fru_lock(fru);
    fru->refcount--;
    if (fru->refcount == 0) {
	final_fru_destroy(fru);
	return;
    }
    _ipmi_fru_unlock(fru);
}

void
ipmi_fru_ref(ipmi_fru_t *fru)
{
    _ipmi_fru_lock(fru);
    fru_get(fru);
    _ipmi_fru_unlock(fru);
}

void
ipmi_fru_deref(ipmi_fru_t *fru)
{
    fru_put(fru);
}

/************************************************************************
 *
 * Decode registration handling
 *
 ************************************************************************/

static locked_list_t *fru_decode_handlers;

int
_ipmi_fru_register_decoder(ipmi_fru_reg_t *reg)
{
    if (!locked_list_add(fru_decode_handlers, reg, NULL))
	return ENOMEM;
    return 0;
}

int
_ipmi_fru_deregister_decoder(ipmi_fru_reg_t *reg)
{
    if (!locked_list_remove(fru_decode_handlers, reg, NULL))
	return ENODEV;
    return 0;
}

typedef struct fru_decode_s
{
    ipmi_fru_t *fru;
    int        err;
} fru_decode_t;

static int
fru_call_decoder(void *cb_data, void *item1, void *item2)
{
    fru_decode_t   *info = cb_data;
    ipmi_fru_reg_t *reg = item1;
    int            err;

    err = reg->decode(info->fru);
    if (!err) {
	info->err = 0;
	return LOCKED_LIST_ITER_STOP;
    } else
	return LOCKED_LIST_ITER_CONTINUE;
}

static int
fru_call_decoders(ipmi_fru_t *fru)
{
    fru_decode_t info;

    info.err = ENOSYS;
    info.fru = fru;
    locked_list_iterate(fru_decode_handlers, fru_call_decoder, &info);
    return info.err;
}


/***********************************************************************
 *
 * FRU allocation and destruction
 *
 **********************************************************************/

static void
final_fru_destroy(ipmi_fru_t *fru)
{
    if (fru->in_frulist) {
	int                rv;
	ipmi_domain_attr_t *attr;
	locked_list_t      *frul;

	fru->in_frulist = 0;
	rv = ipmi_domain_id_find_attribute(fru->domain_id, IPMI_FRU_ATTR_NAME,
					   &attr);
	if (!rv) {
	    fru->refcount++;
	    _ipmi_fru_unlock(fru);
	    frul = ipmi_domain_attr_get_data(attr);
	    locked_list_remove(frul, fru, NULL);
	    ipmi_domain_attr_put(attr);
	    _ipmi_fru_lock(fru);
	    /* While we were unlocked, someone may have come in and
	       grabbed the FRU by iterating the list of FRUs.  That's
	       ok, we just let them handle the destruction since this
	       code will not be entered again. */
	    if (fru->refcount != 1) {
		fru->refcount--;
		_ipmi_fru_unlock(fru);
		return;
	    }
	}
    }
    _ipmi_fru_unlock(fru);

    /* No one else can be referencing this here, so it is safe to
       release the lock now. */

    if (fru->destroy_handler)
	fru->destroy_handler(fru, fru->destroy_cb_data);

    if (fru->ops)
	fru->ops->cleanup_recs(fru);

    while (fru->update_recs) {
	fru_update_t *to_free = fru->update_recs;
	fru->update_recs = to_free->next;
	ipmi_mem_free(to_free);
    }
    ipmi_destroy_lock(fru->lock);
    ipmi_mem_free(fru);
}

int
ipmi_fru_destroy_internal(ipmi_fru_t            *fru,
			  ipmi_fru_destroyed_cb handler,
			  void                  *cb_data)
{
    if (fru->in_frulist)
	return EPERM;

    _ipmi_fru_lock(fru);
    fru->destroy_handler = handler;
    fru->destroy_cb_data = cb_data;
    fru->deleted = 1;
    _ipmi_fru_unlock(fru);

    fru_put(fru);
    return 0;
}

int
ipmi_fru_destroy(ipmi_fru_t            *fru,
		 ipmi_fru_destroyed_cb handler,
		 void                  *cb_data)
{
    ipmi_domain_attr_t *attr;
    locked_list_t      *frul;
    int                rv;

    _ipmi_fru_lock(fru);
    if (fru->in_frulist) {
	rv = ipmi_domain_id_find_attribute(fru->domain_id, IPMI_FRU_ATTR_NAME,
					   &attr);
	if (rv) {
	    _ipmi_fru_unlock(fru);
	    return rv;
	}
	fru->in_frulist = 0;
	_ipmi_fru_unlock(fru);

	frul = ipmi_domain_attr_get_data(attr);
	if (! locked_list_remove(frul, fru, NULL)) {
	    /* Not in the list, it's already been removed. */
	    ipmi_domain_attr_put(attr);
	    _ipmi_fru_unlock(fru);
	    return EINVAL;
	}
	ipmi_domain_attr_put(attr);
	fru_put(fru); /* It's not in the list any more. */
    } else {
	/* User can't destroy FRUs he didn't allocate. */
	_ipmi_fru_unlock(fru);
	return EPERM;
    }

    return ipmi_fru_destroy_internal(fru, handler, cb_data);
}

static int start_logical_fru_fetch(ipmi_domain_t *domain, ipmi_fru_t *fru);
static int start_physical_fru_fetch(ipmi_domain_t *domain, ipmi_fru_t *fru);

static int
destroy_fru(void *cb_data, void *item1, void *item2)
{
    ipmi_fru_t *fru = item1;

    /* Users are responsible for handling their own FRUs, we don't
       delete here, just mark not in the list. */
    _ipmi_fru_lock(fru);
    fru->in_frulist = 0;
    _ipmi_fru_unlock(fru);
    return LOCKED_LIST_ITER_CONTINUE;
}

static void
fru_attr_destroy(void *cb_data, void *data)
{
    locked_list_t *frul = data;

    locked_list_iterate(frul, destroy_fru, NULL);
    locked_list_destroy(frul);
}

static int
fru_attr_init(ipmi_domain_t *domain, void *cb_data, void **data)
{
    locked_list_t *frul;
    
    frul = locked_list_alloc(ipmi_domain_get_os_hnd(domain));
    if (!frul)
	return ENOMEM;

    *data = frul;
    return 0;
}

static int
ipmi_fru_alloc_internal(ipmi_domain_t       *domain,
			unsigned char       is_logical,
			unsigned char       device_address,
			unsigned char       device_id,
			unsigned char       lun,
			unsigned char       private_bus,
			unsigned char       channel,
			unsigned char       fetch_mask,
			ipmi_fru_fetched_cb fetched_handler,
			void                *fetched_cb_data,
			ipmi_fru_t          **new_fru)
{
    ipmi_fru_t    *fru;
    int           err;
    int           len, p;

    fru = ipmi_mem_alloc(sizeof(*fru));
    if (!fru)
	return ENOMEM;
    memset(fru, 0, sizeof(*fru));

    err = ipmi_create_lock(domain, &fru->lock);
    if (err) {
	ipmi_mem_free(fru);
	return err;
    }

    /* Refcount starts at 2 because we start a fetch immediately. */
    fru->refcount = 2;
    fru->in_use = 1;

    fru->domain_id = ipmi_domain_convert_to_id(domain);
    fru->is_logical = is_logical;
    fru->device_address = device_address;
    fru->device_id = device_id;
    fru->lun = lun;
    fru->private_bus = private_bus;
    fru->channel = channel;
    fru->fetch_mask = fetch_mask;
    fru->fetch_size = MAX_FRU_DATA_FETCH;
    fru->os_hnd = ipmi_domain_get_os_hnd(domain);

    len = sizeof(fru->name);
    p = ipmi_domain_get_name(domain, fru->name, len);
    len -= p;
    snprintf(fru->name+p, len, ".%d", ipmi_domain_get_unique_num(domain));

    snprintf(fru->iname, sizeof(fru->iname), "%s.%d.%x.%d.%d.%d.%d ",
	     DOMAIN_NAME(domain), is_logical, device_address, device_id, lun,
	     private_bus, channel);

    fru->fetched_handler = fetched_handler;
    fru->fetched_cb_data = fetched_cb_data;

    fru->deleted = 0;

    _ipmi_fru_lock(fru);
    if (fru->is_logical)
	err = start_logical_fru_fetch(domain, fru);
    else
	err = start_physical_fru_fetch(domain, fru);
    if (err) {
	_ipmi_fru_unlock(fru);
	ipmi_destroy_lock(fru->lock);
	ipmi_mem_free(fru);
	return err;
    }

    *new_fru = fru;
    return 0;
}

int
ipmi_domain_fru_alloc(ipmi_domain_t *domain,
		      unsigned char is_logical,
		      unsigned char device_address,
		      unsigned char device_id,
		      unsigned char lun,
		      unsigned char private_bus,
		      unsigned char channel,
		      ipmi_fru_cb   fetched_handler,
		      void          *fetched_cb_data,
		      ipmi_fru_t    **new_fru)
{
    ipmi_fru_t         *nfru;
    int                rv;
    ipmi_domain_attr_t *attr;
    locked_list_t      *frul;

    rv = ipmi_domain_register_attribute(domain, IPMI_FRU_ATTR_NAME,
					fru_attr_init,
					fru_attr_destroy,
					NULL,
					&attr);
    if (rv)
	return rv;
    frul = ipmi_domain_attr_get_data(attr);

    /* Be careful with locking, a FRU fetch is already going on when
       the alloc_internal function returns. */
    locked_list_lock(frul);
    rv = ipmi_fru_alloc_internal(domain, is_logical, device_address,
				 device_id, lun, private_bus, channel,
				 IPMI_FRU_ALL_AREA_MASK, NULL, NULL, &nfru);
    if (rv) {
	locked_list_unlock(frul);
	ipmi_domain_attr_put(attr);
	return rv;
    }

    nfru->in_frulist = 1;

    if (! locked_list_add_nolock(frul, nfru, NULL)) {
	locked_list_unlock(frul);
	nfru->fetched_handler = NULL;
	ipmi_fru_destroy(nfru, NULL, NULL);
	ipmi_domain_attr_put(attr);
	return ENOMEM;
    }
    nfru->domain_fetched_handler = fetched_handler;
    nfru->fetched_cb_data = fetched_cb_data;
    _ipmi_fru_unlock(nfru);
    locked_list_unlock(frul);
    ipmi_domain_attr_put(attr);

    if (new_fru)
	*new_fru = nfru;
    return 0;
}

int
ipmi_fru_alloc(ipmi_domain_t       *domain,
	       unsigned char       is_logical,
	       unsigned char       device_address,
	       unsigned char       device_id,
	       unsigned char       lun,
	       unsigned char       private_bus,
	       unsigned char       channel,
	       ipmi_fru_fetched_cb fetched_handler,
	       void                *fetched_cb_data,
	       ipmi_fru_t          **new_fru)
{
    ipmi_fru_t         *nfru;
    int                rv;
    ipmi_domain_attr_t *attr;
    locked_list_t      *frul;

    rv = ipmi_domain_register_attribute(domain, IPMI_FRU_ATTR_NAME,
					fru_attr_init,
					fru_attr_destroy,
					NULL,
					&attr);
    if (rv)
	return rv;
    frul = ipmi_domain_attr_get_data(attr);

    /* Be careful with locking, a FRU fetch is already going on when
       the alloc_internal function returns. */
    locked_list_lock(frul);
    rv = ipmi_fru_alloc_internal(domain, is_logical, device_address,
				 device_id, lun, private_bus, channel,
				 IPMI_FRU_ALL_AREA_MASK,
				 fetched_handler, fetched_cb_data, &nfru);
    if (rv) {
	ipmi_domain_attr_put(attr);
	locked_list_unlock(frul);
	return rv;
    }

    nfru->in_frulist = 1;

    if (! locked_list_add_nolock(frul, nfru, NULL)) {
	locked_list_unlock(frul);
	nfru->fetched_handler = NULL;
	ipmi_fru_destroy(nfru, NULL, NULL);
	ipmi_domain_attr_put(attr);
	return ENOMEM;
    }
    _ipmi_fru_unlock(nfru);
    locked_list_unlock(frul);
    ipmi_domain_attr_put(attr);

    if (new_fru)
	*new_fru = nfru;
    return 0;
}

int
ipmi_fru_alloc_notrack(ipmi_domain_t *domain,
		       unsigned char is_logical,
		       unsigned char device_address,
		       unsigned char device_id,
		       unsigned char lun,
		       unsigned char private_bus,
		       unsigned char channel,
		       unsigned char fetch_mask,
		       ipmi_ifru_cb  fetched_handler,
		       void          *fetched_cb_data,
		       ipmi_fru_t    **new_fru)
{
    ipmi_fru_t *nfru;
    int        rv;

    rv = ipmi_fru_alloc_internal(domain, is_logical, device_address,
				 device_id, lun, private_bus, channel,
				 fetch_mask, NULL, NULL, &nfru);
    if (rv)
	return rv;
    nfru->domain_fetched_handler = fetched_handler;
    nfru->fetched_cb_data = fetched_cb_data;
    _ipmi_fru_unlock(nfru);

    if (new_fru)
	*new_fru = nfru;
    return 0;
}

/***********************************************************************
 *
 * FRU Raw data reading
 *
 **********************************************************************/

void
fetch_complete(ipmi_domain_t *domain, ipmi_fru_t *fru, int err)
{
    if (!err)
	err = fru_call_decoders(fru);

    if (fru->data)
	ipmi_mem_free(fru->data);
    fru->data = NULL;

    fru->in_use = 0;
    _ipmi_fru_unlock(fru);

    if (fru->fetched_handler)
	fru->fetched_handler(fru, err, fru->fetched_cb_data);
    else if (fru->domain_fetched_handler)
	fru->domain_fetched_handler(domain, fru, err, fru->fetched_cb_data);

    fru_put(fru);
}

static int request_next_data(ipmi_domain_t *domain,
			     ipmi_fru_t    *fru,
			     ipmi_addr_t   *addr,
			     unsigned int  addr_len);

static int
fru_data_handler(ipmi_domain_t *domain, ipmi_msgi_t *rspi)
{
    ipmi_addr_t   *addr = &rspi->addr;
    unsigned int  addr_len = rspi->addr_len;
    ipmi_msg_t    *msg = &rspi->msg;
    ipmi_fru_t    *fru = rspi->data1;
    unsigned char *data = msg->data;
    int           count;
    int           err;

    _ipmi_fru_lock(fru);

    if (fru->deleted) {
	fetch_complete(domain, fru, ECANCELED);
	goto out;
    }

    /* The timeout and unknown errors should not be necessary, but
       some broken systems just don't return anything if the response
       is too big. */
    if (((data[0] == IPMI_CANNOT_RETURN_REQ_LENGTH_CC)
	 || (data[0] == IPMI_REQUESTED_DATA_LENGTH_EXCEEDED_CC)
	 || (data[0] == IPMI_REQUEST_DATA_LENGTH_INVALID_CC)
	 || (data[0] == IPMI_TIMEOUT_CC)
	 || (data[0] == IPMI_UNKNOWN_ERR_CC))
	&& (fru->fetch_size > MIN_FRU_DATA_FETCH))
    {
	/* System couldn't support the given size, try decreasing and
	   starting again. */
	fru->fetch_size -= FRU_DATA_FETCH_DECR;
	err = request_next_data(domain, fru, addr, addr_len);
	if (err) {
	    ipmi_log(IPMI_LOG_ERR_INFO,
		     "%sfru.c(fru_data_handler): "
		     "Error requesting next FRU data (2)",
		     FRU_DOMAIN_NAME(fru));
	    fetch_complete(domain, fru, err);
	    goto out;
	}
	goto out_unlock;
    }

    if (data[0] != 0) {
	if (fru->curr_pos >= 8) {
	    /* Some screwy cards give more size in the info than they
	       really have, if we have enough, try to process it. */
	    ipmi_log(IPMI_LOG_WARNING,
		     "%sfru.c(fru_data_handler): "
		     "IPMI error getting FRU data: %x",
		     FRU_DOMAIN_NAME(fru), data[0]);
	    fru->data_len = fru->curr_pos;
	    fetch_complete(domain, fru, 0);
	} else {
	    ipmi_log(IPMI_LOG_ERR_INFO,
		     "%sfru.c(fru_data_handler): "
		     "IPMI error getting FRU data: %x",
		     FRU_DOMAIN_NAME(fru), data[0]);
	    fetch_complete(domain, fru, IPMI_IPMI_ERR_VAL(data[0]));
	}
	goto out;
    }

    if (msg->data_len < 2) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%sfru.c(fru_data_handler): "
		 "FRU data response too small",
		 FRU_DOMAIN_NAME(fru));
	fetch_complete(domain, fru, EINVAL);
	goto out;
    }

    count = data[1] << fru->access_by_words;

    if (count == 0) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%sfru.c(fru_data_handler): "
		 "FRU got zero-sized data, must make progress!",
		 FRU_DOMAIN_NAME(fru));
	fetch_complete(domain, fru, EINVAL);
	goto out;
    }

    if (count > msg->data_len-2) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%sfru.c(fru_data_handler): "
		 "FRU data count mismatch",
		 FRU_DOMAIN_NAME(fru));
	fetch_complete(domain, fru, EINVAL);
	goto out;
    }

    memcpy(fru->data+fru->curr_pos, data+2, count);
    fru->curr_pos += count;

    if (fru->curr_pos < fru->data_len) {
	/* More to fetch. */
	err = request_next_data(domain, fru, addr, addr_len);
	if (err) {
	    ipmi_log(IPMI_LOG_ERR_INFO,
		     "%sfru.c(fru_data_handler): "
		     "Error requesting next FRU data",
		     FRU_DOMAIN_NAME(fru));
	    fetch_complete(domain, fru, err);
	    goto out;
	}
    } else {
	fetch_complete(domain, fru, 0);
	goto out;
    }

 out_unlock:
    _ipmi_fru_unlock(fru);
 out:
    return IPMI_MSG_ITEM_NOT_USED;
}

static int
request_next_data(ipmi_domain_t *domain,
		  ipmi_fru_t    *fru,
		  ipmi_addr_t   *addr,
		  unsigned int  addr_len)
{
    unsigned char cmd_data[4];
    ipmi_msg_t    msg;
    int           to_read;

    /* We only request as much as we have to.  Don't always reqeust
       the maximum amount, some machines don't like this. */
    to_read = fru->data_len - fru->curr_pos;
    if (to_read > fru->fetch_size)
	to_read = fru->fetch_size;

    cmd_data[0] = fru->device_id;
    ipmi_set_uint16(cmd_data+1, fru->curr_pos >> fru->access_by_words);
    cmd_data[3] = to_read >> fru->access_by_words;
    msg.netfn = IPMI_STORAGE_NETFN;
    msg.cmd = IPMI_READ_FRU_DATA_CMD;
    msg.data = cmd_data;
    msg.data_len = 4;

    return ipmi_send_command_addr(domain,
				  addr, addr_len,
				  &msg,
				  fru_data_handler,
				  fru,
				  NULL);
}

static int
fru_inventory_area_handler(ipmi_domain_t *domain, ipmi_msgi_t *rspi)
{
    ipmi_addr_t   *addr = &rspi->addr;
    unsigned int  addr_len = rspi->addr_len;
    ipmi_msg_t    *msg = &rspi->msg;
    ipmi_fru_t    *fru = rspi->data1;
    unsigned char *data = msg->data;
    int           err;

    _ipmi_fru_lock(fru);

    if (fru->deleted) {
	fetch_complete(domain, fru, ECANCELED);
	goto out;
    }

    if (data[0] != 0) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%sfru.c(fru_inventory_area_handler): "
		 "IPMI error getting FRU inventory area: %x",
		 FRU_DOMAIN_NAME(fru), data[0]);
	fetch_complete(domain, fru, IPMI_IPMI_ERR_VAL(data[0]));
	goto out;
    }

    if (msg->data_len < 4) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%sfru.c(fru_inventory_area_handler): "
		 "FRU inventory area too small",
		 FRU_DOMAIN_NAME(fru));
	fetch_complete(domain, fru, EINVAL);
	goto out;
    }

    fru->data_len = ipmi_get_uint16(data+1);
    fru->access_by_words = data[3] & 1;

    if (fru->data_len < 8) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%sfru.c(fru_inventory_area_handler): "
		 "FRU space less than the header",
		 FRU_DOMAIN_NAME(fru));
	fetch_complete(domain, fru, EMSGSIZE);
	goto out;
    }

    fru->data = ipmi_mem_alloc(fru->data_len);
    if (!fru->data) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%sfru.c(fru_inventory_area_handler): "
		 "Error allocating FRU data",
		 FRU_DOMAIN_NAME(fru));
	fetch_complete(domain, fru, ENOMEM);
	goto out;
    }

    err = request_next_data(domain, fru, addr, addr_len);
    if (err) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%sfru.c(fru_inventory_area_handler): "
		 "Error requesting next FRU data",
		 FRU_DOMAIN_NAME(fru));
	fetch_complete(domain, fru, err);
	goto out;
    }

    _ipmi_fru_unlock(fru);
 out:
    return IPMI_MSG_ITEM_NOT_USED;
}

static int
start_logical_fru_fetch(ipmi_domain_t *domain, ipmi_fru_t *fru)
{
    unsigned char    cmd_data[1];
    ipmi_ipmb_addr_t ipmb;
    ipmi_msg_t       msg;

    ipmb.addr_type = IPMI_IPMB_ADDR_TYPE;
    ipmb.channel = fru->channel;
    ipmb.slave_addr = fru->device_address;
    ipmb.lun = fru->lun;

    cmd_data[0] = fru->device_id;
    msg.netfn = IPMI_STORAGE_NETFN;
    msg.cmd = IPMI_GET_FRU_INVENTORY_AREA_INFO_CMD;
    msg.data = cmd_data;
    msg.data_len = 1;

    return ipmi_send_command_addr(domain,
				  (ipmi_addr_t *) &ipmb,
				  sizeof(ipmb),
				  &msg,
				  fru_inventory_area_handler,
				  fru,
				  NULL);
}

static int
start_physical_fru_fetch(ipmi_domain_t *domain, ipmi_fru_t *fru)
{
    /* FIXME - this is going to suck, but needs to be implemented. */
    return ENOSYS;
}

/***********************************************************************
 *
 * FRU writing
 *
 **********************************************************************/

int
_ipmi_fru_new_update_record(ipmi_fru_t   *fru,
			    unsigned int offset,
			    unsigned int length)
{
    fru_update_t *urec;

    urec = ipmi_mem_alloc(sizeof(*urec));
    if (!urec)
	return ENOMEM;
    if (fru->access_by_words) {
	/* This handled the (really stupid) word access mode.  If the
	   address is odd, back it up one.  If the length is odd,
	   increment by one. */
	if (offset & 1) {
	    offset -= 1;
	    length += 1;
	}
	urec->offset = offset;
	if (length & 1) {
	    length += 1;
	}
	urec->length = length;
    } else {
	urec->offset = offset;
	urec->length = length;
    }
    urec->next = NULL;
    if (fru->update_recs)
	fru->update_recs_tail->next = urec;
    else
	fru->update_recs = urec;
    fru->update_recs_tail = urec;
    return 0;
}

static int next_fru_write(ipmi_domain_t *domain, ipmi_fru_t *fru,
			  ipmi_addr_t *addr, unsigned int addr_len);

void
write_complete(ipmi_domain_t *domain, ipmi_fru_t *fru, int err)
{
    if (!err)
	/* If we succeed, set everything unchanged. */
	fru->ops->write_complete(fru);
    if (fru->data)
	ipmi_mem_free(fru->data);
    fru->data = NULL;

    fru->in_use = 0;
    _ipmi_fru_unlock(fru);

    if (fru->domain_fetched_handler)
	fru->domain_fetched_handler(domain, fru, err, fru->fetched_cb_data);

    fru_put(fru);
}

static int
fru_write_handler(ipmi_domain_t *domain, ipmi_msgi_t *rspi)
{
    ipmi_addr_t   *addr = &rspi->addr;
    unsigned int  addr_len = rspi->addr_len;
    ipmi_msg_t    *msg = &rspi->msg;
    ipmi_fru_t    *fru = rspi->data1;
    unsigned char *data = msg->data;
    int           rv;

    _ipmi_fru_lock(fru);

    /* Note that for safety, we do not stop a fru write on deletion. */

    if (data[0] == 0x81) {
	ipmi_msg_t msg;
	/* Got a busy response.  Try again if we haven't run out of
	   retries. */
	if (fru->retry_count >= MAX_FRU_WRITE_RETRIES) {
	    write_complete(domain, fru, IPMI_IPMI_ERR_VAL(data[0]));
	    goto out;
	}
	fru->retry_count++;
	msg.netfn = IPMI_STORAGE_NETFN;
	msg.cmd = IPMI_WRITE_FRU_DATA_CMD;
	msg.data = fru->last_cmd;
	msg.data_len = fru->last_cmd_len;
	rv = ipmi_send_command_addr(domain,
				    addr, addr_len,
				    &msg,
				    fru_write_handler,
				    fru,
				    NULL);
	if (rv) {
	    write_complete(domain, fru, rv);
	    goto out;
	}
	goto out_cmd;
    } else if (data[0] != 0) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%sfru.c(fru_write_handler): "
		 "IPMI error writing FRU data: %x",
		 FRU_DOMAIN_NAME(fru), data[0]);
	write_complete(domain, fru, IPMI_IPMI_ERR_VAL(data[0]));
	goto out;
    }

    if (msg->data_len < 2) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%sfru.c(fru_write_handler): "
		 "FRU write response too small",
		 FRU_DOMAIN_NAME(fru));
	write_complete(domain, fru, EINVAL);
	goto out;
    }

    if ((data[1] << fru->access_by_words) != (fru->last_cmd_len - 3)) {
	/* Write was incomplete for some reason.  Just go on but issue
	   a warning. */
	ipmi_log(IPMI_LOG_WARNING,
		 "%sfru.c(fru_write_handler): "
		 "Incomplete writing FRU data, write %d, expected %d",
		 FRU_DOMAIN_NAME(fru),
		 data[1] << fru->access_by_words, fru->last_cmd_len-3);
    }

    if (fru->update_recs) {
	/* More to do. */
	rv = next_fru_write(domain, fru, addr, addr_len);
	if (rv) {
	    write_complete(domain, fru, rv);
	    goto out;
	}
    } else {
	write_complete(domain, fru, 0);
	goto out;
    }

 out_cmd:
    _ipmi_fru_unlock(fru);
 out:
    return IPMI_MSG_ITEM_NOT_USED;
}

static int
next_fru_write(ipmi_domain_t *domain,
	       ipmi_fru_t    *fru,
	       ipmi_addr_t   *addr,
	       unsigned int  addr_len)
{
    unsigned char *data = fru->last_cmd;
    int           offset, length = 0, left, noff, tlen;
    ipmi_msg_t    msg;

    noff = fru->update_recs->offset;
    offset = noff;
    left = MAX_FRU_DATA_WRITE;
    while (fru->update_recs
	   && (left > 0)
	   && (noff == fru->update_recs->offset))
    {
	if (left < fru->update_recs->length)
	    tlen = left;
	else
	    tlen = fru->update_recs->length;

	noff += tlen;
	length += tlen;
	left -= tlen;
	fru->update_recs->length -= tlen;
	if (fru->update_recs->length > 0) {
	    fru->update_recs->offset += tlen;
	} else {
	    fru_update_t *to_free = fru->update_recs;
	    fru->update_recs = to_free->next;
	    ipmi_mem_free(to_free);
	}
    }

    fru->retry_count = 0;
    data[0] = fru->device_id;
    ipmi_set_uint16(data+1, offset >> fru->access_by_words);
    memcpy(data+3, fru->data+offset, length);
    msg.netfn = IPMI_STORAGE_NETFN;
    msg.cmd = IPMI_WRITE_FRU_DATA_CMD;
    msg.data = data;
    msg.data_len = length + 3;
    fru->last_cmd_len = msg.data_len;

    return ipmi_send_command_addr(domain,
				  addr, addr_len,
				  &msg,
				  fru_write_handler,
				  fru,
				  NULL);
}

typedef struct start_domain_fru_write_s
{
    ipmi_fru_t *fru;
    int        rv;
} start_domain_fru_write_t;

static void
start_domain_fru_write(ipmi_domain_t *domain, void *cb_data)
{
    start_domain_fru_write_t *info = cb_data;
    ipmi_ipmb_addr_t         ipmb;
    int                      rv;
    ipmi_fru_t               *fru = info->fru;

    /* We allocate and format the entire FRU data.  We do this because
       of the stupid word access capability, which means we cannot
       necessarily do byte-aligned writes.  Because of that, we might
       have to have the byte before or after the actual one being
       written, and it may come from a different data field. */
    fru->data = ipmi_mem_alloc(fru->data_len);
    if (!fru->data) {
	rv = ENOMEM;
	goto out_err;
    }
    memset(fru->data, 0, fru->data_len);

    rv = fru->ops->write(fru);
    if (rv)
	goto out_err;

    if (!fru->update_recs) {
	/* No data changed, no write is needed. */
	ipmi_mem_free(fru->data);
	fru->data = NULL;
	fru->in_use = 0;
	_ipmi_fru_unlock(fru);

	if (fru->domain_fetched_handler)
	    fru->domain_fetched_handler(domain, fru, 0, fru->fetched_cb_data);
	return;
    }

    ipmb.addr_type = IPMI_IPMB_ADDR_TYPE;
    ipmb.channel = info->fru->channel;
    ipmb.slave_addr = info->fru->device_address;
    ipmb.lun = info->fru->lun;

    /* Data is fully encoded and the update records are in place.
       Start the write process. */
    rv = next_fru_write(domain, fru,
			(ipmi_addr_t *) &ipmb, sizeof(ipmb));
    if (rv)
	goto out_err;

    fru_get(fru);
    _ipmi_fru_unlock(fru);
    return;

 out_err:
    while (fru->update_recs) {
	fru_update_t *to_free = fru->update_recs;
	fru->update_recs = to_free->next;
	ipmi_mem_free(to_free);
    }
    if (fru->data)
	ipmi_mem_free(fru->data);
    fru->data = NULL;
    fru->in_use = 0;
    _ipmi_fru_unlock(fru);
    info->rv = rv;
}

int
ipmi_fru_write(ipmi_fru_t *fru, ipmi_fru_cb done, void *cb_data)
{
    int                      rv;
    start_domain_fru_write_t info = {fru, 0};

    _ipmi_fru_lock(fru);
    if (fru->in_use) {
	/* Something else is happening with the FRU, error this
	   operation. */
	_ipmi_fru_unlock(fru);
	return EAGAIN;
    }
    fru->in_use = 1;

    fru->domain_fetched_handler = done;
    fru->fetched_cb_data = cb_data;

    /* Data is fully encoded and the update records are in place.
       Start the write process. */
    rv = ipmi_domain_pointer_cb(fru->domain_id, start_domain_fru_write, &info);
    if (!rv)
	rv = info.rv;
    else
	_ipmi_fru_unlock(fru);

    return rv;
}

/***********************************************************************
 *
 * Misc stuff.
 *
 **********************************************************************/
ipmi_domain_id_t
ipmi_fru_get_domain_id(ipmi_fru_t *fru)
{
    return fru->domain_id;
}

void
ipmi_fru_data_free(char *data)
{
    ipmi_mem_free(data);
}

unsigned int
ipmi_fru_get_data_length(ipmi_fru_t *fru)
{
    return fru->data_len;
}

int
ipmi_fru_get_name(ipmi_fru_t *fru, char *name, int length)
{
    int  slen;

    if (length <= 0)
	return 0;

    /* Never changes, no lock needed. */
    slen = strlen(fru->name);
    if (slen == 0) {
	if (name)
	    *name = '\0';
	goto out;
    }

    if (name) {
	memcpy(name, fru->name, slen);
	name[slen] = '\0';
    }
 out:
    return slen;
}

typedef struct iterate_frus_info_s
{
    ipmi_fru_ptr_cb handler;
    void            *cb_data;
} iterate_frus_info_t;

static int
frus_handler(void *cb_data, void *item1, void *item2)
{
    iterate_frus_info_t *info = cb_data;
    info->handler(item1, info->cb_data);
    fru_put(item1);
    return LOCKED_LIST_ITER_CONTINUE;
}

static int
frus_prefunc(void *cb_data, void *item1, void *item2)
{
    ipmi_fru_t *fru = item1;
    ipmi_lock(fru->lock);
    fru_get(fru);
    ipmi_unlock(fru->lock);
    return LOCKED_LIST_ITER_CONTINUE;
}

void
ipmi_fru_iterate_frus(ipmi_domain_t   *domain,
		      ipmi_fru_ptr_cb handler,
		      void            *cb_data)
{
    iterate_frus_info_t info;
    ipmi_domain_attr_t  *attr;
    locked_list_t       *frus;
    int                 rv;

    rv = ipmi_domain_find_attribute(domain, IPMI_FRU_ATTR_NAME,
				    &attr);
    if (rv)
	return;
    frus = ipmi_domain_attr_get_data(attr);

    info.handler = handler;
    info.cb_data = cb_data;
    locked_list_iterate_prefunc(frus, frus_prefunc, frus_handler, &info);
    ipmi_domain_attr_put(attr);
}

/************************************************************************
 *
 * Misc external interfaces
 *
 ************************************************************************/

void *
_ipmi_fru_get_rec_data(ipmi_fru_t *fru)
{
    return fru->rec_data;
}

void
_ipmi_fru_set_rec_data(ipmi_fru_t *fru, void *rec_data)
{
    fru->rec_data = rec_data;
}

char *
_ipmi_fru_get_iname(ipmi_fru_t *fru)
{
    return FRU_DOMAIN_NAME(fru);
}

unsigned int
_ipmi_fru_get_fetch_mask(ipmi_fru_t *fru)
{
    return fru->fetch_mask;
}

void *
_ipmi_fru_get_data_ptr(ipmi_fru_t *fru)
{
    return fru->data;
}
unsigned int
_ipmi_fru_get_data_len(ipmi_fru_t *fru)
{
    return fru->data_len;
}

int
_ipmi_fru_is_normal_fru(ipmi_fru_t *fru)
{
    return fru->normal_fru;
}

void
_ipmi_fru_set_is_normal_fru(ipmi_fru_t *fru, int val)
{
    fru->normal_fru = val;
}

void
_ipmi_fru_set_ops(ipmi_fru_t *fru, ipmi_fru_op_t *ops)
{
    fru->ops = ops;
}

/************************************************************************
 *
 * Init/shutdown
 *
 ************************************************************************/

int
_ipmi_fru_init(void)
{
    fru_decode_handlers = locked_list_alloc(ipmi_get_global_os_handler());
    if (!fru_decode_handlers)
	return ENOMEM;
    return 0;
}

void
_ipmi_fru_shutdown(void)
{
    locked_list_destroy(fru_decode_handlers);
}
