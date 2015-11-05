/*!
 * \file mod_netconf.c
 * \brief NETCONF Apache modul for Netopeer
 * \author Tomas Cejka <cejkat@cesnet.cz>
 * \author Radek Krejci <rkrejci@cesnet.cz>
 * \date 2011
 * \date 2012
 * \date 2013
 */
/*
 * Copyright (C) 2011-2013 CESNET
 *
 * LICENSE TERMS
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */
#define _GNU_SOURCE

#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/fcntl.h>
#include <pwd.h>
#include <errno.h>
#include <grp.h>
#include <signal.h>
#include <pthread.h>
#include <ctype.h>

#include <libnetconf.h>
#include <libnetconf_ssh.h>

#include "../config.h"

#ifdef WITH_NOTIFICATIONS
#include "notification_module.h"
#endif

#include "message_type.h"
#include "mod_netconf.h"

#define MAX_PROCS 5
#define SOCKET_FILENAME "/var/run/mod_netconf.sock"
#define MAX_SOCKET_CL 10
#define BUFFER_SIZE 4096
#define ACTIVITY_CHECK_INTERVAL	10  /**< timeout in seconds, how often activity is checked */
#define ACTIVITY_TIMEOUT	(60*60)  /**< timeout in seconds, after this time, session is automaticaly closed. */

/* sleep in master process for non-blocking socket reading */
#define SLEEP_TIME 200

#ifndef offsetof
#define offsetof(type, member) ((size_t) ((type *) 0)->member)
#endif

/* timeout in msec */
struct timeval timeout = { 1, 0 };

#define NCWITHDEFAULTS	NCWD_MODE_NOTSET


#define MSG_OK 0
#define MSG_OPEN  1
#define MSG_DATA  2
#define MSG_CLOSE 3
#define MSG_ERROR 4
#define MSG_UNKNOWN 5

pthread_rwlock_t session_lock; /**< mutex protecting netconf_sessions_list from multiple access errors */
pthread_mutex_t ntf_history_lock; /**< mutex protecting notification history list */
pthread_mutex_t ntf_hist_clbc_mutex; /**< mutex protecting notification history list */
pthread_mutex_t json_lock; /**< mutex for protecting json-c calls */

struct session_with_mutex *netconf_sessions_list = NULL;

static const char *sockname;

static pthread_key_t notif_history_key;

pthread_key_t err_reply_key;

volatile int isterminated = 0;

static char* password;

static void signal_handler(int sign)
{
	switch (sign) {
	case SIGINT:
	case SIGTERM:
		isterminated = 1;
		break;
	}
}

int netconf_callback_ssh_hostkey_check(const char* UNUSED(hostname), ssh_session UNUSED(session))
{
	/* always approve */
	return (EXIT_SUCCESS);
}

char *netconf_callback_sshauth_passphrase(const char *UNUSED(username), const char *UNUSED(hostname), const char *UNUSED(priv_key_file))
{
	char *buf;
	buf = strdup(password);
	return (buf);
}

char *netconf_callback_sshauth_password(const char* UNUSED(username), const char* UNUSED(hostname))
{
	char *buf;
	buf = strdup(password);
	return (buf);
}

char *netconf_callback_sshauth_interactive(const char *UNUSED(name), const char *UNUSED(instruction),
                                           const char *UNUSED(prompt), int UNUSED(echo))
{
	char *buf;
	buf = strdup(password);
	return (buf);
}

void netconf_callback_error_process(const char *UNUSED(tag),
		const char *UNUSED(type),
		const char *UNUSED(severity),
		const char *UNUSED(apptag),
		const char *UNUSED(path),
		const char *message,
		const char *UNUSED(attribute),
		const char *UNUSED(element),
		const char *UNUSED(ns),
		const char *UNUSED(sid))
{
	json_object **err_reply_p = (json_object **) pthread_getspecific(err_reply_key);
	if (err_reply_p == NULL) {
		ERROR("Error message was not allocated. %s", __func__);
		return;
	}
	json_object *err_reply = *err_reply_p;

	json_object *array = NULL;
	if (err_reply == NULL) {
		ERROR("error calback: empty error list");
		pthread_mutex_lock(&json_lock);
		err_reply = json_object_new_object();
		array = json_object_new_array();
		json_object_object_add(err_reply, "type", json_object_new_int(REPLY_ERROR));
		json_object_object_add(err_reply, "errors", array);
		if (message != NULL) {
			json_object_array_add(array, json_object_new_string(message));
		}
		pthread_mutex_unlock(&json_lock);
		(*err_reply_p) = err_reply;
	} else {
		ERROR("error calback: nonempty error list");
		pthread_mutex_lock(&json_lock);
		if (json_object_object_get_ex(err_reply, "errors", &array) == TRUE) {
			if (message != NULL) {
				json_object_array_add(array, json_object_new_string(message));
			}
		}
		pthread_mutex_unlock(&json_lock);
	}
	pthread_setspecific(err_reply_key, err_reply_p);
	return;
}

/**
 * should be used in locked area
 */
void prepare_status_message(struct session_with_mutex *s, struct nc_session *session)
{
	json_object *json_obj = NULL;
	json_object *js_tmp = NULL;
	char *old_sid = NULL;
	const char *j_old_sid = NULL;
	const char *cpbltstr;
	struct nc_cpblts* cpblts = NULL;

	if (s == NULL) {
		ERROR("No session given.");
		return;
	}

	pthread_mutex_lock(&json_lock);
	if (s->hello_message != NULL) {
		ERROR("clean previous hello message");
		//json_object_put(s->hello_message);
		if (json_object_object_get_ex(s->hello_message, "sid", &js_tmp) == TRUE) {
			j_old_sid = json_object_get_string(js_tmp);
			if (j_old_sid != NULL) {
				old_sid = strdup(j_old_sid);
			}
			json_object_put(s->hello_message);
			json_object_put(js_tmp);
		}
		s->hello_message = NULL;
	}
	s->hello_message = json_object_get(json_object_new_object());
	if (session != NULL) {
		if (old_sid != NULL) {
			/* use previous sid */
			json_object_object_add(s->hello_message, "sid", json_object_new_string(old_sid));
			free(old_sid);
			old_sid = NULL;
		} else {
			/* we don't have old sid */
			json_object_object_add(s->hello_message, "sid", json_object_new_string(nc_session_get_id(session)));
		}
		json_object_object_add(s->hello_message, "version", json_object_new_string((nc_session_get_version(session) == 0)?"1.0":"1.1"));
		json_object_object_add(s->hello_message, "host", json_object_new_string(nc_session_get_host(session)));
		json_object_object_add(s->hello_message, "port", json_object_new_string(nc_session_get_port(session)));
		json_object_object_add(s->hello_message, "user", json_object_new_string(nc_session_get_user(session)));
		cpblts = nc_session_get_cpblts (session);
		if (cpblts != NULL) {
			json_obj = json_object_new_array();
			nc_cpblts_iter_start (cpblts);
			while ((cpbltstr = nc_cpblts_iter_next (cpblts)) != NULL) {
				json_object_array_add(json_obj, json_object_new_string(cpbltstr));
			}
			json_object_object_add(s->hello_message, "capabilities", json_obj);
		}
		DEBUG("%s", json_object_to_json_string(s->hello_message));
	} else {
		ERROR("Session was not given.");
		json_object_object_add(s->hello_message, "type", json_object_new_int(REPLY_ERROR));
		json_object_object_add(s->hello_message, "error-message", json_object_new_string("Invalid session identifier."));
	}
	DEBUG("Status info from hello message prepared");
	pthread_mutex_unlock(&json_lock);

}

void create_err_reply_p()
{
	json_object **err_reply = calloc(1, sizeof(json_object **));
	if (err_reply == NULL) {
		ERROR("Allocation of err_reply storage failed!");
		return;
	}
	if (pthread_setspecific(err_reply_key, err_reply) != 0) {
		ERROR("cannot set thread-specific value.");
	}
}

void clean_err_reply()
{
	json_object **err_reply = (json_object **) pthread_getspecific(err_reply_key);
	if (err_reply != NULL) {
		if (*err_reply != NULL) {
			pthread_mutex_lock(&json_lock);
			json_object_put(*err_reply);
			pthread_mutex_unlock(&json_lock);
		}
		if (pthread_setspecific(err_reply_key, err_reply) != 0) {
			ERROR("Cannot set thread-specific hash value.");
		}
	}
}

void free_err_reply()
{
	json_object **err_reply = (json_object **) pthread_getspecific(err_reply_key);
	if (err_reply != NULL) {
		if (*err_reply != NULL) {
			pthread_mutex_lock(&json_lock);
			json_object_put(*err_reply);
			pthread_mutex_unlock(&json_lock);
		}
		free(err_reply);
		err_reply = NULL;
		if (pthread_setspecific(err_reply_key, err_reply) != 0) {
			ERROR("Cannot set thread-specific hash value.");
		}
	}
}

/**
 * \defgroup netconf_operations NETCONF operations
 * The list of NETCONF operations that mod_netconf supports.
 * @{
 */

/**
 * \brief Send RPC and wait for reply with timeout.
 *
 * \param[in] session libnetconf session
 * \param[in] rpc     prepared RPC message
 * \param[in] timeout timeout in miliseconds, -1 for blocking, 0 for non-blocking
 * \param[out] reply  reply from the server
 * \return Value from nc_session_recv_reply() or NC_MSG_UNKNOWN when send_rpc() fails.
 * On success, it returns NC_MSG_REPLY.
 */
NC_MSG_TYPE netconf_send_recv_timed(struct nc_session *session, nc_rpc *rpc,
				   int timeout, nc_reply **reply)
{
	const nc_msgid msgid = NULL;
	NC_MSG_TYPE ret = NC_MSG_UNKNOWN;
	msgid = nc_session_send_rpc(session, rpc);
	if (msgid == NULL) {
		return ret;
	}
	do {
		ret = nc_session_recv_reply(session, timeout, reply);
		if (ret == NC_MSG_HELLO) {
			ERROR("<hello> received instead reply, it will be lost.");
			nc_reply_free(*reply);
		}
		if (ret == NC_MSG_WOULDBLOCK) {
			ERROR("Timeout for receiving RPC reply expired.");
			break;
		}
	} while (ret == NC_MSG_HELLO || ret == NC_MSG_NOTIFICATION);
	return ret;
}

/**
 * \brief Connect to NETCONF server
 *
 * \warning Session_key hash is not bound with caller identification. This could be potential security risk.
 */
static const char *netconf_connect(const char* host, const char* port, const char* user, const char* pass, struct nc_cpblts * cpblts)
{
	struct nc_session* session = NULL;
	struct session_with_mutex * locked_session, *last_session;

	/* connect to the requested NETCONF server */
	password = (char*)pass;
	DEBUG("prepare to connect %s@%s:%s", user, host, port);
	session = nc_session_connect(host, (unsigned short) atoi (port), user, cpblts);
	DEBUG("nc_session_connect done");

	/* if connected successful, add session to the list */
	if (session != NULL) {
		if ((locked_session = calloc(1, sizeof(struct session_with_mutex))) == NULL || pthread_mutex_init (&locked_session->lock, NULL) != 0) {
			nc_session_free(session);
			session = NULL;
			free(locked_session);
			locked_session = NULL;
			ERROR("Creating structure session_with_mutex failed %d (%s)", errno, strerror(errno));
			return 0;
		}
		locked_session->session = session;
		locked_session->last_activity = time(NULL);
		locked_session->hello_message = NULL;
		locked_session->closed = 0;
		pthread_mutex_init (&locked_session->lock, NULL);
		DEBUG("Before session_lock");
		/* get exclusive access to sessions_list (conns) */
		DEBUG("LOCK wrlock %s", __func__);
		if (pthread_rwlock_wrlock(&session_lock) != 0) {
			nc_session_free(session);
			free (locked_session);
			ERROR("Error while locking rwlock: %d (%s)", errno, strerror(errno));
			return 0;
		}
		locked_session->ntfc_subscribed = 0;
		DEBUG("Add connection to the list");
		if (!netconf_sessions_list) {
            netconf_sessions_list = locked_session;
        } else {
            for (last_session = netconf_sessions_list; last_session->next; last_session = last_session->next);
            last_session->next = locked_session;
            locked_session->prev = last_session;
        }
		DEBUG("Before session_unlock");

		/* lock session */
		DEBUG("LOCK mutex %s", __func__);
		pthread_mutex_lock(&locked_session->lock);

		/* unlock session list */
		DEBUG("UNLOCK wrlock %s", __func__);
		if (pthread_rwlock_unlock (&session_lock) != 0) {
			ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}

		/* store information about session from hello message for future usage */
		prepare_status_message(locked_session, session);

		DEBUG("UNLOCK mutex %s", __func__);
		pthread_mutex_unlock(&locked_session->lock);

		DEBUG("NETCONF session established");
		return nc_session_get_id(session);
	} else {
		ERROR("Connection could not be established");
		return 0;
	}

}

static int close_and_free_session(struct session_with_mutex *locked_session)
{
	DEBUG("lock private lock.");
	DEBUG("LOCK mutex %s", __func__);
	if (pthread_mutex_lock(&locked_session->lock) != 0) {
		ERROR("Error while locking rwlock");
	}
	locked_session->ntfc_subscribed = 0;
	locked_session->closed = 1;
	if (locked_session->session != NULL) {
		nc_session_free(locked_session->session);
		locked_session->session = NULL;
	}
	DEBUG("session closed.");
	DEBUG("unlock private lock.");
	DEBUG("UNLOCK mutex %s", __func__);
	if (pthread_mutex_unlock(&locked_session->lock) != 0) {
		ERROR("Error while locking rwlock");
	}

	DEBUG("unlock session lock.");
	DEBUG("closed session, disabled notif(?), wait 2s");
	usleep(500000); /* let notification thread stop */

	/* session shouldn't be used by now */
	/** \todo free all notifications from queue */
	free(locked_session->notifications);
	pthread_mutex_destroy(&locked_session->lock);
	if (locked_session->hello_message != NULL) {
		/** \todo free hello_message */
		//json_object_put(locked_session->hello_message);
		locked_session->hello_message = NULL;
	}
	locked_session->session = NULL;
	free(locked_session);
	locked_session = NULL;
	DEBUG("NETCONF session closed, everything cleared.");
	return (EXIT_SUCCESS);
}

static int netconf_close(const char *session_id, json_object **reply)
{
	struct session_with_mutex *locked_session;

	DEBUG("Session to close: %s", session_id);

	/* get exclusive (write) access to sessions_list (conns) */
	DEBUG("lock session lock.");
	DEBUG("LOCK wrlock %s", __func__);
	if (pthread_rwlock_wrlock (&session_lock) != 0) {
		ERROR("Error while locking rwlock");
		(*reply) = create_error("Internal: Error while locking.");
		return EXIT_FAILURE;
	}
	/* remove session from the active sessions list -> nobody new can now work with session */
	for (locked_session = netconf_sessions_list;
         locked_session && strcmp(nc_session_get_id(locked_session->session), session_id);
         locked_session = locked_session->next);

    if (!locked_session) {
        ERROR("Could not find the session \"%s\" to close.", session_id);
        (*reply) = create_error("Internal: Error while finding a session.");
        return EXIT_FAILURE;
    }

    if (!locked_session->prev) {
        netconf_sessions_list = netconf_sessions_list->next;
        netconf_sessions_list->prev = NULL;
    } else {
        locked_session->prev->next = locked_session->next;
        if (locked_session->next) {
            locked_session->next->prev = locked_session->prev;
        }
    }

	DEBUG("UNLOCK wrlock %s", __func__);
	if (pthread_rwlock_unlock (&session_lock) != 0) {
		ERROR("Error while unlocking rwlock");
		(*reply) = create_error("Internal: Error while unlocking.");
	}

	if ((locked_session != NULL) && (locked_session->session != NULL)) {
		return close_and_free_session(locked_session);
	} else {
		ERROR("Unknown session to close");
		(*reply) = create_error("Internal: Unkown session to close.");
		return (EXIT_FAILURE);
	}
	(*reply) = NULL;
}

/**
 * Test reply message type and return error message.
 *
 * \param[in] session	nc_session internal struct
 * \param[in] session_key session ID, 0 to disable disconnect on error
 * \param[in] msgt	RPC-REPLY message type
 * \param[out] data
 * \return NULL on success
 */
json_object *netconf_test_reply(struct nc_session *session, const char *session_id, NC_MSG_TYPE msgt, nc_reply *reply, char **data)
{
	NC_REPLY_TYPE replyt;
	json_object *err = NULL;

	/* process the result of the operation */
	switch (msgt) {
		case NC_MSG_UNKNOWN:
			if (nc_session_get_status(session) != NC_SESSION_STATUS_WORKING) {
				ERROR("mod_netconf: receiving rpc-reply failed");
				if (session_id) {
					netconf_close(session_id, &err);
				}
				if (err != NULL) {
					return err;
				}
				return create_error("Internal: Receiving RPC-REPLY failed.");
			}
		case NC_MSG_NONE:
			/* there is error handled by callback */
			if (data != NULL) {
				free(*data);
				(*data) = NULL;
			}
			return NULL;
		case NC_MSG_REPLY:
			switch (replyt = nc_reply_get_type(reply)) {
				case NC_REPLY_OK:
					if ((data != NULL) && (*data != NULL)) {
						free(*data);
						(*data) = NULL;
					}
					return create_ok();
				case NC_REPLY_DATA:
					if (((*data) = nc_reply_get_data(reply)) == NULL) {
						ERROR("mod_netconf: no data from reply");
						return create_error("Internal: No data from reply received.");
					} else {
						return NULL;
					}
					break;
				case NC_REPLY_ERROR:
					ERROR("mod_netconf: unexpected rpc-reply (%d)", replyt);
					if (data != NULL) {
						free(*data);
						(*data) = NULL;
					}
					return create_error(nc_reply_get_errormsg(reply));
				default:
					ERROR("mod_netconf: unexpected rpc-reply (%d)", replyt);
					if (data != NULL) {
						free(*data);
						(*data) = NULL;
					}
					return create_error("Unknown type of NETCONF reply.");
			}
			break;
		default:
			ERROR("mod_netconf: unexpected reply message received (%d)", msgt);
			if (data != NULL) {
				free(*data);
				(*data) = NULL;
			}
			return create_error("Internal: Unexpected RPC-REPLY message type.");
	}
}

json_object *netconf_unlocked_op(struct nc_session *session, nc_rpc* rpc)
{
	nc_reply* reply = NULL;
	NC_MSG_TYPE msgt;

	/* check requests */
	if (rpc == NULL) {
		ERROR("mod_netconf: rpc is not created");
		return create_error("Internal error: RPC is not created");
	}

	if (session != NULL) {
		/* send the request and get the reply */
		msgt = netconf_send_recv_timed(session, rpc, 5000, &reply);
		/* process the result of the operation */
		return netconf_test_reply(session, 0, msgt, reply, NULL);
	} else {
		ERROR("Unknown session to process.");
		return create_error("Internal error: Unknown session to process.");
	}
}

/**
 * Perform RPC method that returns data.
 *
 * \param[in] session_id	session identifier
 * \param[in] rpc	RPC message to perform
 * \param[out] received_data	received data string, can be NULL when no data expected, value can be set to NULL if no data received
 * \return NULL on success, json object with error otherwise
 */
static json_object *netconf_op(const char *session_id, nc_rpc* rpc, char **received_data)
{
	struct nc_session *session = NULL;
	struct session_with_mutex * locked_session;
	nc_reply* reply = NULL;
	json_object *res = NULL;
	char *data = NULL;
	NC_MSG_TYPE msgt;

	/* check requests */
	if (rpc == NULL) {
		ERROR("mod_netconf: rpc is not created");
		res = create_error("Internal: RPC could not be created.");
		data = NULL;
		goto finished;
	}

	/* get non-exclusive (read) access to sessions_list (conns) */
	DEBUG("LOCK wrlock %s", __func__);
	if (pthread_rwlock_rdlock(&session_lock) != 0) {
		ERROR("Error while locking rwlock: %d (%s)", errno, strerror(errno));
		res = create_error("Internal: Lock failed.");
		data = NULL;
		goto finished;
	}
	/* get session where send the RPC */
	for (locked_session = netconf_sessions_list;
         locked_session && strcmp(nc_session_get_id(locked_session->session), session_id);
         locked_session = locked_session->next);
	if (locked_session != NULL) {
		session = locked_session->session;
	}
	if (session != NULL) {
		/* get exclusive access to session */
		DEBUG("LOCK mutex %s", __func__);
		if (pthread_mutex_lock(&locked_session->lock) != 0) {
			/* unlock before returning error */
			DEBUG("UNLOCK wrlock %s", __func__);
			if (pthread_rwlock_unlock (&session_lock) != 0) {

				ERROR("Error while locking rwlock: %d (%s)", errno, strerror(errno));
				res = create_error("Internal: Could not unlock.");
				goto finished;
			}
			res = create_error("Internal: Could not unlock.");
		}
		DEBUG("UNLOCK wrlock %s", __func__);
		if (pthread_rwlock_unlock(&session_lock) != 0) {

			ERROR("Error while locking rwlock: %d (%s)", errno, strerror(errno));
			res = create_error("Internal: Could not unlock.");
		}

		locked_session->last_activity = time(NULL);

		/* send the request and get the reply */
		msgt = netconf_send_recv_timed(session, rpc, 5000, &reply);

		/* first release exclusive lock for this session */
		DEBUG("UNLOCK mutex %s", __func__);
		pthread_mutex_unlock(&locked_session->lock);
		/* end of critical section */

		res = netconf_test_reply(session, session_id, msgt, reply, &data);
	} else {
		/* release lock on failure */
		DEBUG("UNLOCK wrlock %s", __func__);
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		ERROR("Unknown session to process.");
		res = create_error("Unknown session to process.");
		data = NULL;
	}
finished:
	nc_reply_free(reply);
	if (received_data != NULL) {
		(*received_data) = data;
	} else {
		if (data != NULL) {
			free(data);
			data = NULL;
		}
	}
	return res;
}

static char* netconf_getconfig(const char* session_key, NC_DATASTORE source, const char* filter, json_object **err)
{
	nc_rpc* rpc;
	struct nc_filter *f = NULL;
	char* data = NULL;
	json_object *res = NULL;

	/* create filter if set */
	if (filter != NULL) {
		f = nc_filter_new(NC_FILTER_SUBTREE, filter);
	}

	/* create requests */
	rpc = nc_rpc_getconfig (source, f);
	nc_filter_free(f);
	if (rpc == NULL) {
		ERROR("mod_netconf: creating rpc request failed");
		return (NULL);
	}

	/* tell server to show all elements even if they have default values */
#ifdef HAVE_WITHDEFAULTS_TAGGED
	if (nc_rpc_capability_attr(rpc, NC_CAP_ATTR_WITHDEFAULTS_MODE, NCWD_MODE_ALL_TAGGED))
#else
	if (nc_rpc_capability_attr(rpc, NC_CAP_ATTR_WITHDEFAULTS_MODE, NCWD_MODE_NOTSET))
	//if (nc_rpc_capability_attr(rpc, NC_CAP_ATTR_WITHDEFAULTS_MODE, NCWD_MODE_ALL))
#endif
    {
		ERROR("mod_netconf: setting withdefaults failed");
	}

	res = netconf_op(session_key, rpc, &data);
	nc_rpc_free (rpc);
	if (res != NULL) {
		(*err) = res;
	} else {
		(*err) = NULL;
	}

	return (data);
}

static char* netconf_getschema(const char* session_key, const char* identifier, const char* version, const char* format, json_object **err)
{
	nc_rpc* rpc;
	char* data = NULL;
	json_object *res = NULL;

	/* create requests */
	rpc = nc_rpc_getschema(identifier, version, format);
	if (rpc == NULL) {
		ERROR("mod_netconf: creating rpc request failed");
		return (NULL);
	}

	res = netconf_op(session_key, rpc, &data);
	nc_rpc_free (rpc);
	if (res != NULL) {
		(*err) = res;
	} else {
		(*err) = NULL;
	}

	return (data);
}

static char* netconf_get(const char* session_key, const char* filter, json_object **err)
{
	nc_rpc* rpc;
	struct nc_filter *f = NULL;
	char* data = NULL;
	json_object *res = NULL;

	/* create filter if set */
	if (filter != NULL) {
		f = nc_filter_new(NC_FILTER_SUBTREE, filter);
	}

	/* create requests */
	rpc = nc_rpc_get (f);
	nc_filter_free(f);
	if (rpc == NULL) {
		ERROR("mod_netconf: creating rpc request failed");
		return (NULL);
	}

	/* tell server to show all elements even if they have default values */
	if (nc_rpc_capability_attr(rpc, NC_CAP_ATTR_WITHDEFAULTS_MODE, NCWD_MODE_NOTSET)) {
	//if (nc_rpc_capability_attr(rpc, NC_CAP_ATTR_WITHDEFAULTS_MODE, NCWD_MODE_ALL)) {
	//if (nc_rpc_capability_attr(rpc, NC_CAP_ATTR_WITHDEFAULTS_MODE, NCWD_MODE_ALL_TAGGED)) {
		ERROR("mod_netconf: setting withdefaults failed");
	}

	res = netconf_op(session_key, rpc, &data);
	nc_rpc_free (rpc);
	if (res == NULL) {
		(*err) = res;
	} else {
		(*err) = NULL;
	}

	return (data);
}

static json_object *netconf_copyconfig(const char* session_key, NC_DATASTORE source, NC_DATASTORE target, const char* config, const char *uri_src, const char *uri_trg)
{
	nc_rpc* rpc;
	json_object *res = NULL;

	/* create requests */
	if (source == NC_DATASTORE_CONFIG) {
		if (target == NC_DATASTORE_URL) {
			/* config, url */
			rpc = nc_rpc_copyconfig(source, target, config, uri_trg);
		} else {
			/* config, datastore */
			rpc = nc_rpc_copyconfig(source, target, config);
		}
	} else if (source == NC_DATASTORE_URL) {
		if (target == NC_DATASTORE_URL) {
			/* url, url */
			rpc = nc_rpc_copyconfig(source, target, uri_src, uri_trg);
		} else {
			/* url, datastore */
			rpc = nc_rpc_copyconfig(source, target, uri_src);
		}
	} else {
		if (target == NC_DATASTORE_URL) {
			/* datastore, url */
			rpc = nc_rpc_copyconfig(source, target, uri_trg);
		} else {
			/* datastore, datastore */
			rpc = nc_rpc_copyconfig(source, target);
		}
	}
	if (rpc == NULL) {
		ERROR("mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	res = netconf_op(session_key, rpc, NULL);
	nc_rpc_free (rpc);

	return res;
}

static json_object *netconf_editconfig(const char* session_key, NC_DATASTORE source, NC_DATASTORE target, NC_EDIT_DEFOP_TYPE defop, NC_EDIT_ERROPT_TYPE erropt, NC_EDIT_TESTOPT_TYPE testopt, const char* config_or_url)
{
	nc_rpc* rpc;
	json_object *res = NULL;

	/* create requests */
	rpc = nc_rpc_editconfig(target, source, defop, erropt, testopt, config_or_url);
	if (rpc == NULL) {
		ERROR("mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	res = netconf_op(session_key, rpc, NULL);
	nc_rpc_free (rpc);

	return res;
}

static json_object *netconf_killsession(const char* session_key, const char* sid)
{
	nc_rpc* rpc;
	json_object *res = NULL;

	/* create requests */
	rpc = nc_rpc_killsession(sid);
	if (rpc == NULL) {
		ERROR("mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	res = netconf_op(session_key, rpc, NULL);
	nc_rpc_free (rpc);
	return res;
}

static json_object *netconf_onlytargetop(const char* session_key, NC_DATASTORE target, nc_rpc* (*op_func)(NC_DATASTORE))
{
	nc_rpc* rpc;
	json_object *res = NULL;

	/* create requests */
	rpc = op_func(target);
	if (rpc == NULL) {
		ERROR("mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	res = netconf_op(session_key, rpc, NULL);
	nc_rpc_free (rpc);
	return res;
}

static json_object *netconf_deleteconfig(const char* session_key, NC_DATASTORE target, const char *url)
{
	nc_rpc *rpc = NULL;
	json_object *res = NULL;
	if (target != NC_DATASTORE_URL) {
		rpc = nc_rpc_deleteconfig(target);
	} else {
		rpc = nc_rpc_deleteconfig(target, url);
	}
	if (rpc == NULL) {
		ERROR("mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	res = netconf_op(session_key, rpc, NULL);
	nc_rpc_free (rpc);
	return res;
}

static json_object *netconf_lock(const char* session_key, NC_DATASTORE target)
{
	return (netconf_onlytargetop(session_key, target, nc_rpc_lock));
}

static json_object *netconf_unlock(const char* session_key, NC_DATASTORE target)
{
	return (netconf_onlytargetop(session_key, target, nc_rpc_unlock));
}

static json_object *netconf_generic(const char* session_key, const char* content, char** data)
{
	nc_rpc* rpc = NULL;
	json_object *res = NULL;

	/* create requests */
	rpc = nc_rpc_generic(content);
	if (rpc == NULL) {
		ERROR("mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	if (data != NULL) {
		// TODO ?free(*data);
		(*data) = NULL;
	}

	/* get session where send the RPC */
	res = netconf_op(session_key, rpc, data);
	nc_rpc_free (rpc);
	return res;
}

/**
 * @}
 *//* netconf_operations */

void clb_print(NC_VERB_LEVEL level, const char* msg)
{
#define FOREACH(I) \
        I(NC_VERB_ERROR) I(NC_VERB_WARNING)

#define CASE(VAL) case VAL: ERROR("%s: %s", #VAL, msg); \
	break;

	switch (level) {
	FOREACH(CASE);
	case NC_VERB_VERBOSE:
	case NC_VERB_DEBUG:
		DEBUG("DEBUG: %s", msg);
		break;
	}
	if (level == NC_VERB_ERROR) {
		/* return global error */
		netconf_callback_error_process(NULL /* tag */, NULL /* type */,
				NULL /* severity */, NULL /* apptag */,
				NULL /* path */, msg, NULL /* attribute */,
				NULL /* element */, NULL /* ns */, NULL /* sid */);
	}
}

/**
 * Receive message from client over UNIX socket and return pointer to it.
 * Caller should free message memory.
 * \param[in] client	socket descriptor of client
 * \return pointer to message
 */
char *get_framed_message(int client)
{
	/* read json in chunked framing */
	unsigned int buffer_size = 0;
	ssize_t buffer_len = 0;
	char *buffer = NULL;
	char c;
	ssize_t ret;
	int i, chunk_len;
	char chunk_len_str[12];

	while (1) {
		/* read chunk length */
		if ((ret = recv (client, &c, 1, 0)) != 1 || c != '\n') {
			if (buffer != NULL) {
				free (buffer);
				buffer = NULL;
			}
			break;
		}
		if ((ret = recv (client, &c, 1, 0)) != 1 || c != '#') {
			if (buffer != NULL) {
				free (buffer);
				buffer = NULL;
			}
			break;
		}
		i=0;
		memset (chunk_len_str, 0, 12);
		while ((ret = recv (client, &c, 1, 0) == 1 && (isdigit(c) || c == '#'))) {
			if (i==0 && c == '#') {
				if (recv (client, &c, 1, 0) != 1 || c != '\n') {
					/* end but invalid */
					if (buffer != NULL) {
						free (buffer);
						buffer = NULL;
					}
				}
				/* end of message, double-loop break */
				goto msg_complete;
			}
			chunk_len_str[i++] = c;
			if (i==11) {
				ERROR("Message is too long, buffer for length is not big enought!!!!");
				break;
			}
		}
		if (c != '\n') {
			if (buffer != NULL) {
				free (buffer);
				buffer = NULL;
			}
			break;
		}
		chunk_len_str[i] = 0;
		if ((chunk_len = atoi (chunk_len_str)) == 0) {
			if (buffer != NULL) {
				free (buffer);
				buffer = NULL;
			}
			break;
		}
		buffer_size += chunk_len+1;
		buffer = realloc (buffer, sizeof(char)*buffer_size);
		memset(buffer + (buffer_size-chunk_len-1), 0, chunk_len+1);
		if ((ret = recv (client, buffer+buffer_len, chunk_len, 0)) == -1 || ret != chunk_len) {
			if (buffer != NULL) {
				free (buffer);
				buffer = NULL;
			}
			break;
		}
		buffer_len += ret;
	}
msg_complete:
	return buffer;
}

NC_DATASTORE parse_datastore(const char *ds)
{
	if (strcmp(ds, "running") == 0) {
		return NC_DATASTORE_RUNNING;
	} else if (strcmp(ds, "startup") == 0) {
		return NC_DATASTORE_STARTUP;
	} else if (strcmp(ds, "candidate") == 0) {
		return NC_DATASTORE_CANDIDATE;
	} else if (strcmp(ds, "url") == 0) {
		return NC_DATASTORE_URL;
	} else if (strcmp(ds, "config") == 0) {
		return NC_DATASTORE_CONFIG;
	}
	return -1;
}

NC_EDIT_TESTOPT_TYPE parse_testopt(const char *t)
{
	if (strcmp(t, "notset") == 0) {
		return NC_EDIT_TESTOPT_NOTSET;
	} else if (strcmp(t, "testset") == 0) {
		return NC_EDIT_TESTOPT_TESTSET;
	} else if (strcmp(t, "set") == 0) {
		return NC_EDIT_TESTOPT_SET;
	} else if (strcmp(t, "test") == 0) {
		return NC_EDIT_TESTOPT_TEST;
	}
	return NC_EDIT_TESTOPT_ERROR;
}

json_object *create_error(const char *errmess)
{
	pthread_mutex_lock(&json_lock);
	json_object *reply = json_object_new_object();
	json_object *array = json_object_new_array();
	json_object_object_add(reply, "type", json_object_new_int(REPLY_ERROR));
	json_object_array_add(array, json_object_new_string(errmess));
	json_object_object_add(reply, "errors", array);
	pthread_mutex_unlock(&json_lock);
	return reply;

}

json_object *create_data(const char *data)
{
	pthread_mutex_lock(&json_lock);
	json_object *reply = json_object_new_object();
	json_object_object_add(reply, "type", json_object_new_int(REPLY_DATA));
	json_object_object_add(reply, "data", json_object_new_string(data));
	pthread_mutex_unlock(&json_lock);
	return reply;
}

json_object *create_ok()
{
	pthread_mutex_lock(&json_lock);
	json_object *reply = json_object_new_object();
	reply = json_object_new_object();
	json_object_object_add(reply, "type", json_object_new_int(REPLY_OK));
	pthread_mutex_unlock(&json_lock);
	return reply;
}

char *get_param_string(json_object *data, const char *name)
{
	json_object *js_tmp = NULL;
	char *res = NULL;
	if (json_object_object_get_ex(data, name, &js_tmp) == TRUE) {
		res = strdup(json_object_get_string(js_tmp));
		json_object_put(js_tmp);
	}
	return res;
}

json_object *handle_op_connect(json_object *request)
{
	char *host = NULL;
	char *port = NULL;
	char *user = NULL;
	char *pass = NULL;
	json_object *capabilities = NULL;
	json_object *reply = NULL;
	const char *session_id = NULL;
	struct nc_cpblts* cpblts = NULL;
	unsigned int len, i;

	DEBUG("Request: Connect");
	pthread_mutex_lock(&json_lock);

	host = get_param_string(request, "host");
	port = get_param_string(request, "port");
	user = get_param_string(request, "user");
	pass = get_param_string(request, "pass");

	if (json_object_object_get_ex(request, "capabilities", &capabilities) == TRUE) {
		if ((capabilities != NULL) && ((len = json_object_array_length(capabilities)) > 0)) {
			cpblts = nc_cpblts_new(NULL);
			for (i=0; i<len; i++) {
				nc_cpblts_add(cpblts, json_object_get_string(json_object_array_get_idx(capabilities, i)));
			}
		} else {
			ERROR("no capabilities specified");
		}
		json_object_put(capabilities);
	}

	pthread_mutex_unlock(&json_lock);

	DEBUG("host: %s, port: %s, user: %s", host, port, user);
	if ((host == NULL) || (user == NULL)) {
		ERROR("Cannot connect - insufficient input.");
		session_id = NULL;
	} else {
		session_id = netconf_connect(host, port, user, pass, cpblts);
		DEBUG("SID: %s", session_id);
	}
	if (cpblts != NULL) {
		nc_cpblts_free(cpblts);
	}

	GETSPEC_ERR_REPLY

	pthread_mutex_lock(&json_lock);
	if (session_id == NULL) {
		/* negative reply */
		if (err_reply == NULL) {
			reply = json_object_new_object();
			json_object_object_add(reply, "type", json_object_new_int(REPLY_ERROR));
			json_object_object_add(reply, "error-message", json_object_new_string("Connecting NETCONF server failed."));
			ERROR("Connection failed.");
		} else {
			/* use filled err_reply from libnetconf's callback */
			reply = err_reply;
			ERROR("Connect - error from libnetconf's callback.");
		}
	} else {
		/* positive reply */
		reply = json_object_new_object();
		json_object_object_add(reply, "type", json_object_new_int(REPLY_OK));
		json_object_object_add(reply, "session", json_object_new_string(session_id));
	}
	memset(pass, 0, strlen(pass));
	pthread_mutex_unlock(&json_lock);
	CHECK_AND_FREE(host);
	CHECK_AND_FREE(user);
	CHECK_AND_FREE(port);
	CHECK_AND_FREE(pass);
	return reply;
}

json_object *handle_op_get(json_object *request, const char *session_id)
{
	char *filter = NULL;
	char *data = NULL;
	json_object *reply = NULL;

	DEBUG("Request: get (session %s)", session_id);

	pthread_mutex_lock(&json_lock);
	filter = get_param_string(request, "filter");
	pthread_mutex_unlock(&json_lock);

	if ((data = netconf_get(session_id, filter, &reply)) == NULL) {
		CHECK_ERR_SET_REPLY_ERR("Get information failed.")
	} else {
		reply = create_data(data);
		free(data);
	}
	return reply;
}

json_object *handle_op_getconfig(json_object *request, const char *session_id)
{
	NC_DATASTORE ds_type_s = -1;
	char *filter = NULL;
	char *data = NULL;
	char *source = NULL;
	json_object *reply = NULL;

	DEBUG("Request: get-config (session %s)", session_id);

	pthread_mutex_lock(&json_lock);
	filter = get_param_string(request, "filter");

	source = get_param_string(request, "source");
	if (source != NULL) {
		ds_type_s = parse_datastore(source);
	}
	pthread_mutex_unlock(&json_lock);

	if ((int)ds_type_s == -1) {
		reply = create_error("Invalid source repository type requested.");
		goto finalize;
	}

	if ((data = netconf_getconfig(session_id, ds_type_s, filter, &reply)) == NULL) {
		CHECK_ERR_SET_REPLY_ERR("Get configuration operation failed.")
	} else {
		reply = create_data(data);
		free(data);
	}
finalize:
	CHECK_AND_FREE(filter);
	CHECK_AND_FREE(source);
	return reply;
}

json_object *handle_op_getschema(json_object *request, const char *session_id)
{
	char *data = NULL;
	char *identifier = NULL;
	char *version = NULL;
	char *format = NULL;
	json_object *reply = NULL;

	DEBUG("Request: get-schema (session %s)", session_id);
	pthread_mutex_lock(&json_lock);
	identifier = get_param_string(request, "identifier");
	version = get_param_string(request, "version");
	format = get_param_string(request, "format");
	pthread_mutex_unlock(&json_lock);

	if (identifier == NULL) {
		reply = create_error("No identifier for get-schema supplied.");
		goto finalize;
	}

	DEBUG("get-schema(version: %s, format: %s)", version, format);
	if ((data = netconf_getschema(session_id, identifier, version, format, &reply)) == NULL) {
		CHECK_ERR_SET_REPLY_ERR("Get models operation failed.")
	} else {
		reply = create_data(data);
		free(data);
	}
finalize:
	CHECK_AND_FREE(identifier);
	CHECK_AND_FREE(version);
	CHECK_AND_FREE(format);
	return reply;
}

json_object *handle_op_editconfig(json_object *request, const char *session_id)
{
	NC_DATASTORE ds_type_s = -1;
	NC_DATASTORE ds_type_t = -1;
	NC_EDIT_DEFOP_TYPE defop_type = NC_EDIT_DEFOP_NOTSET;
	NC_EDIT_ERROPT_TYPE erropt_type = 0;
	NC_EDIT_TESTOPT_TYPE testopt_type = NC_EDIT_TESTOPT_TESTSET;
	char *defop = NULL;
	char *erropt = NULL;
	char *config = NULL;
	char *source = NULL;
	char *target = NULL;
	char *testopt = NULL;
	json_object *reply = NULL;

	DEBUG("Request: edit-config (session %s)", session_id);

	pthread_mutex_lock(&json_lock);
	/* get parameters */
	defop = get_param_string(request, "default-operation");
	erropt = get_param_string(request, "error-option");
	target = get_param_string(request, "target");
	source = get_param_string(request, "source");
	config = get_param_string(request, "config");
	testopt = get_param_string(request, "test-option");
	pthread_mutex_unlock(&json_lock);

	if (target != NULL) {
		ds_type_t = parse_datastore(target);
	}
	if (source != NULL) {
		ds_type_s = parse_datastore(source);
	} else {
		/* source is optional, default value is config */
		ds_type_s = NC_DATASTORE_CONFIG;
	}

	if (defop != NULL) {
		if (strcmp(defop, "merge") == 0) {
			defop_type = NC_EDIT_DEFOP_MERGE;
		} else if (strcmp(defop, "replace") == 0) {
			defop_type = NC_EDIT_DEFOP_REPLACE;
		} else if (strcmp(defop, "none") == 0) {
			defop_type = NC_EDIT_DEFOP_NONE;
		} else {
			reply = create_error("Invalid default-operation parameter.");
			goto finalize;
		}
	} else {
		defop_type = NC_EDIT_DEFOP_NOTSET;
	}

	if (erropt != NULL) {
		if (strcmp(erropt, "continue-on-error") == 0) {
			erropt_type = NC_EDIT_ERROPT_CONT;
		} else if (strcmp(erropt, "stop-on-error") == 0) {
			erropt_type = NC_EDIT_ERROPT_STOP;
		} else if (strcmp(erropt, "rollback-on-error") == 0) {
			erropt_type = NC_EDIT_ERROPT_ROLLBACK;
		} else {
			reply = create_error("Invalid error-option parameter.");
			goto finalize;
		}
	} else {
		erropt_type = 0;
	}

	if ((int)ds_type_t == -1) {
		reply = create_error("Invalid target repository type requested.");
		goto finalize;
	}
	if (ds_type_s == NC_DATASTORE_CONFIG) {
		if (config == NULL) {
			reply = create_error("Invalid config data parameter.");
			goto finalize;
		}
	} else if (ds_type_s == NC_DATASTORE_URL){
		if (config == NULL) {
			config = "";
		}
	}

	if (testopt != NULL) {
		testopt_type = parse_testopt(testopt);
	} else {
		testopt_type = NC_EDIT_TESTOPT_TESTSET;
	}

	reply = netconf_editconfig(session_id, ds_type_s, ds_type_t, defop_type, erropt_type, testopt_type, config);

	CHECK_ERR_SET_REPLY
finalize:
	CHECK_AND_FREE(defop);
	CHECK_AND_FREE(erropt);
	CHECK_AND_FREE(config);
	CHECK_AND_FREE(source);
	CHECK_AND_FREE(target);
	CHECK_AND_FREE(testopt);
	return reply;
}

json_object *handle_op_copyconfig(json_object *request, const char *session_id)
{
	NC_DATASTORE ds_type_s = -1;
	NC_DATASTORE ds_type_t = -1;
	char *config = NULL;
	char *target = NULL;
	char *source = NULL;
	char *uri_src = NULL;
	char *uri_trg = NULL;

	json_object *reply = NULL;

	DEBUG("Request: copy-config (session %s)", session_id);

	/* get parameters */
	pthread_mutex_lock(&json_lock);
	target = get_param_string(request, "target");
	source = get_param_string(request, "source");
	config = get_param_string(request, "config");
	uri_src = get_param_string(request, "uri-source");
	uri_trg = get_param_string(request, "uri-target");
	pthread_mutex_unlock(&json_lock);

	if (target != NULL) {
		ds_type_t = parse_datastore(target);
	}
	if (source != NULL) {
		ds_type_s = parse_datastore(source);
	} else {
		/* source == NULL *//* no explicit source specified -> use config data */
		ds_type_s = NC_DATASTORE_CONFIG;
	}
	if ((int)ds_type_s == -1) {
		/* source datastore specified, but it is invalid */
		reply = create_error("Invalid source repository type requested.");
		goto finalize;
	}

	if ((int)ds_type_t == -1) {
		/* invalid target datastore specified */
		reply = create_error("Invalid target repository type requested.");
		goto finalize;
	}

	/* source can be missing when config is given */
	if (source == NULL && config == NULL) {
		reply = create_error("invalid input parameters - source and config is required.");
		goto finalize;
	}

	if (ds_type_s == NC_DATASTORE_URL) {
		if (uri_src == NULL) {
			uri_src = "";
		}
	}
	if (ds_type_t == NC_DATASTORE_URL) {
		if (uri_trg == NULL) {
			uri_trg = "";
		}
	}
	reply = netconf_copyconfig(session_id, ds_type_s, ds_type_t, config, uri_src, uri_trg);

	CHECK_ERR_SET_REPLY

finalize:
	CHECK_AND_FREE(config);
	CHECK_AND_FREE(target);
	CHECK_AND_FREE(source);
	CHECK_AND_FREE(uri_src);
	CHECK_AND_FREE(uri_trg);

	return reply;
}

json_object *handle_op_generic(json_object *request, const char *session_id)
{
	json_object *reply = NULL;
	char *config = NULL;
	char *data = NULL;

	DEBUG("Request: generic request for session %s", session_id);

	pthread_mutex_lock(&json_lock);
	config = get_param_string(request, "content");
	pthread_mutex_unlock(&json_lock);

	reply = netconf_generic(session_id, config, &data);
	if (reply == NULL) {
		GETSPEC_ERR_REPLY
		if (err_reply != NULL) {
			/* use filled err_reply from libnetconf's callback */
			reply = err_reply;
		}
	} else {
		if (data == NULL) {
			pthread_mutex_lock(&json_lock);
			reply = json_object_new_object();
			json_object_object_add(reply, "type", json_object_new_int(REPLY_OK));
			pthread_mutex_unlock(&json_lock);
		} else {
			reply = create_data(data);
			free(data);
		}
	}
	CHECK_AND_FREE(config);
	return reply;
}

json_object *handle_op_disconnect(json_object *UNUSED(request), const char *session_id)
{
	json_object *reply = NULL;
	DEBUG("Request: Disconnect session %s", session_id);

	if (netconf_close(session_id, &reply) != EXIT_SUCCESS) {
		CHECK_ERR_SET_REPLY_ERR("Get configuration information from device failed.")
	} else {
		reply = create_ok();
	}
	return reply;
}

json_object *handle_op_kill(json_object *request, const char *session_id)
{
	json_object *reply = NULL;
	char *sid = NULL;

	DEBUG("Request: kill-session, session %s", session_id);

	pthread_mutex_lock(&json_lock);
	sid = get_param_string(request, "session-id");
	pthread_mutex_unlock(&json_lock);

	if (sid == NULL) {
		reply = create_error("Missing session-id parameter.");
		goto finalize;
	}

	reply = netconf_killsession(session_id, sid);

	CHECK_ERR_SET_REPLY

finalize:
	CHECK_AND_FREE(sid);
	return reply;
}

json_object *handle_op_reloadhello(json_object *UNUSED(request), const char *session_id)
{
	struct nc_session *temp_session = NULL;
	struct session_with_mutex * locked_session = NULL;
	json_object *reply = NULL;
	DEBUG("Request: get info about session %s", session_id);

	DEBUG("LOCK wrlock %s", __func__);
	if (pthread_rwlock_wrlock(&session_lock) != 0) {
		ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		return NULL;
	}

	for (locked_session = netconf_sessions_list;
         locked_session && strcmp(nc_session_get_id(locked_session->session), session_id);
         locked_session = locked_session->next);
	if ((locked_session != NULL) && (locked_session->hello_message != NULL)) {
		DEBUG("LOCK mutex %s", __func__);
		pthread_mutex_lock(&locked_session->lock);
		DEBUG("creating temporal NC session.");
		temp_session = nc_session_connect_channel(locked_session->session, NULL);
		if (temp_session != NULL) {
			prepare_status_message(locked_session, temp_session);
			DEBUG("closing temporal NC session.");
			nc_session_free(temp_session);
			temp_session = NULL;
		} else {
			DEBUG("Reload hello failed due to channel establishment");
			reply = create_error("Reload was unsuccessful, connection failed.");
		}
		DEBUG("UNLOCK mutex %s", __func__);
		pthread_mutex_unlock(&locked_session->lock);
		DEBUG("UNLOCK wrlock %s", __func__);
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
	} else {
		DEBUG("UNLOCK wrlock %s", __func__);
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		reply = create_error("Invalid session identifier.");
	}

	if ((reply == NULL) && (locked_session->hello_message != NULL)) {
		reply = locked_session->hello_message;
	}
	return reply;
}

json_object *handle_op_info(json_object *UNUSED(request), const char *session_id)
{
	json_object *reply = NULL;
	struct session_with_mutex * locked_session = NULL;
	DEBUG("Request: get info about session %s", session_id);

	DEBUG("LOCK wrlock %s", __func__);
	if (pthread_rwlock_rdlock(&session_lock) != 0) {
		ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
	}

	for (locked_session = netconf_sessions_list;
         locked_session && strcmp(nc_session_get_id(locked_session->session), session_id);
         locked_session = locked_session->next);
	if (locked_session != NULL) {
		DEBUG("LOCK mutex %s", __func__);
		pthread_mutex_lock(&locked_session->lock);
		DEBUG("UNLOCK wrlock %s", __func__);
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		if (locked_session->hello_message != NULL) {
			reply = locked_session->hello_message;
		} else {
			reply = create_error("Invalid session identifier.");
		}
		DEBUG("UNLOCK mutex %s", __func__);
		pthread_mutex_unlock(&locked_session->lock);
	} else {
		DEBUG("UNLOCK wrlock %s", __func__);
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		reply = create_error("Invalid session identifier.");
	}


	return reply;
}

void notification_history(time_t eventtime, const char *content)
{
	json_object *notif_history_array = (json_object *) pthread_getspecific(notif_history_key);
	if (notif_history_array == NULL) {
		ERROR("No list of notification history found.");
		return;
	}
	DEBUG("Got notification from history %lu.", (long unsigned) eventtime);
	pthread_mutex_lock(&json_lock);
	json_object *notif = json_object_new_object();
	if (notif == NULL) {
		ERROR("Could not allocate memory for notification (json).");
		goto failed;
	}
	json_object_object_add(notif, "eventtime", json_object_new_int64(eventtime));
	json_object_object_add(notif, "content", json_object_new_string(content));
	json_object_array_add(notif_history_array, notif);
failed:
	pthread_mutex_unlock(&json_lock);
}

json_object *handle_op_ntfgethistory(json_object *request, const char *session_id)
{
	json_object *reply = NULL;
	json_object *js_tmp = NULL;
	char *sid = NULL;
	struct session_with_mutex *locked_session = NULL;
	struct nc_session *temp_session = NULL;
	nc_rpc *rpc = NULL;
	time_t start = 0;
	time_t stop = 0;
	int64_t from = 0, to = 0;

	DEBUG("Request: get notification history, session %s", session_id);

	pthread_mutex_lock(&json_lock);
	sid = get_param_string(request, "session");

	if (json_object_object_get_ex(request, "from", &js_tmp) == TRUE) {
		from = json_object_get_int64(js_tmp);
		json_object_put(js_tmp);
	}
	if (json_object_object_get_ex(request, "to", &js_tmp) == TRUE) {
		to = json_object_get_int64(js_tmp);
		json_object_put(js_tmp);
	}
	pthread_mutex_unlock(&json_lock);

	start = time(NULL) + from;
	stop = time(NULL) + to;

	DEBUG("notification history interval %li %li", (long int) from, (long int) to);

	if (sid == NULL) {
		reply = create_error("Missing session parameter.");
		goto finalize;
	}

	DEBUG("LOCK wrlock %s", __func__);
	if (pthread_rwlock_rdlock(&session_lock) != 0) {
		ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		reply = create_error("Internal lock failed.");
		goto finalize;
	}

	for (locked_session = netconf_sessions_list;
         locked_session && strcmp(nc_session_get_id(locked_session->session), session_id);
         locked_session = locked_session->next);
	if (locked_session != NULL) {
		DEBUG("LOCK mutex %s", __func__);
		pthread_mutex_lock(&locked_session->lock);
		DEBUG("UNLOCK wrlock %s", __func__);
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		DEBUG("creating temporal NC session.");
		temp_session = nc_session_connect_channel(locked_session->session, NULL);
		if (temp_session != NULL) {
			rpc = nc_rpc_subscribe(NULL /* stream */, NULL /* filter */, &start, &stop);
			if (rpc == NULL) {
				DEBUG("UNLOCK mutex %s", __func__);
				pthread_mutex_unlock(&locked_session->lock);
				DEBUG("notifications: creating an rpc request failed.");
				reply = create_error("notifications: creating an rpc request failed.");
				goto finalize;
			}

			DEBUG("Send NC subscribe.");
			/** \todo replace with sth like netconf_op(http_server, session_hash, rpc) */
			json_object *res = netconf_unlocked_op(temp_session, rpc);
			if (res != NULL) {
				DEBUG("UNLOCK mutex %s", __func__);
				pthread_mutex_unlock(&locked_session->lock);
				DEBUG("Subscription RPC failed.");
				reply = res;
				goto finalize;
			}
			rpc = NULL; /* just note that rpc is already freed by send_recv_process() */

			DEBUG("UNLOCK mutex %s", __func__);
			pthread_mutex_unlock(&locked_session->lock);
			DEBUG("LOCK mutex %s", __func__);
			pthread_mutex_lock(&ntf_history_lock);
			pthread_mutex_lock(&json_lock);
			json_object *notif_history_array = json_object_new_array();
			pthread_mutex_unlock(&json_lock);
			if (pthread_setspecific(notif_history_key, notif_history_array) != 0) {
				ERROR("notif_history: cannot set thread-specific hash value.");
			}

			ncntf_dispatch_receive(temp_session, notification_history);

			pthread_mutex_lock(&json_lock);
			reply = json_object_new_object();
			json_object_object_add(reply, "notifications", notif_history_array);
			//json_object_put(notif_history_array);
			pthread_mutex_unlock(&json_lock);

			DEBUG("UNLOCK mutex %s", __func__);
			pthread_mutex_unlock(&ntf_history_lock);
			DEBUG("closing temporal NC session.");
			nc_session_free(temp_session);
			temp_session = NULL;
		} else {
			DEBUG("UNLOCK mutex %s", __func__);
			pthread_mutex_unlock(&locked_session->lock);
			DEBUG("Get history of notification failed due to channel establishment");
			reply = create_error("Get history of notification was unsuccessful, connection failed.");
		}
	} else {
		DEBUG("UNLOCK wrlock %s", __func__);
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		reply = create_error("Invalid session identifier.");
	}

finalize:
	CHECK_AND_FREE(sid);
	return reply;
}

json_object *handle_op_validate(json_object *request, const char *session_id)
{
	json_object *reply = NULL;
	char *sid = NULL;
	char *target = NULL;
	char *url = NULL;
	nc_rpc *rpc = NULL;
	NC_DATASTORE target_ds;

	DEBUG("Request: validate datastore, session %s", session_id);

	pthread_mutex_lock(&json_lock);
	sid = get_param_string(request, "session");
	target = get_param_string(request, "target");
	url = get_param_string(request, "url");
	pthread_mutex_unlock(&json_lock);


	if ((sid == NULL) || (target == NULL)) {
		reply = create_error("Missing session parameter.");
		goto finalize;
	}

	/* validation */
	target_ds = parse_datastore(target);
	if (target_ds == NC_DATASTORE_URL) {
		if (url != NULL) {
			rpc = nc_rpc_validate(target_ds, url);
		}
	} else if ((target_ds == NC_DATASTORE_RUNNING) || (target_ds == NC_DATASTORE_STARTUP)
			|| (target_ds == NC_DATASTORE_CANDIDATE)) {
		rpc = nc_rpc_validate(target_ds);
	}
	if (rpc == NULL) {
		DEBUG("mod_netconf: creating rpc request failed");
		reply = create_error("Creation of RPC request failed.");
		goto finalize;
	}

	DEBUG("Request: validate datastore");
	if ((reply = netconf_op(session_id, rpc, NULL)) == NULL) {

		CHECK_ERR_SET_REPLY

		if (reply == NULL) {
			DEBUG("Request: validation ok.");
			reply = create_ok();
		}
	}
	nc_rpc_free (rpc);
finalize:
	CHECK_AND_FREE(sid);
	CHECK_AND_FREE(target);
	CHECK_AND_FREE(url);
	return reply;
}

void * thread_routine (void * arg)
{
	void * retval = NULL;
	struct pollfd fds;
	json_object *request = NULL;
	json_object *reply = NULL;
	json_object *js_tmp = NULL;
	int operation = (-1);
	NC_DATASTORE ds_type_t = -1;
	int status = 0;
	const char *msgtext;
	char *session_id = NULL;
	char *target = NULL;
	char *url = NULL;
	char *chunked_out_msg = NULL;
	//server_rec * server = ((struct pass_to_thread*)arg)->server;
	int client = ((struct pass_to_thread*)arg)->client;

	char *buffer = NULL;

	/* init thread specific err_reply memory */
	create_err_reply_p();

	while (!isterminated) {
		fds.fd = client;
		fds.events = POLLIN;
		fds.revents = 0;

		status = poll(&fds, 1, 1000);

		if (status == 0 || (status == -1 && (errno == EAGAIN || (errno == EINTR && isterminated == 0)))) {
			/* poll was interrupted - check if the isterminated is set and if not, try poll again */
			continue;
		} else if (status < 0) {
			/* 0:  poll time outed
			 *     close socket and ignore this request from the client, it can try it again
			 * -1: poll failed
			 *     something wrong happend, close this socket and wait for another request
			 */
			close(client);
			break;
		}
		/* status > 0 */

		/* check the status of the socket */

		/* if nothing to read and POLLHUP (EOF) or POLLERR set */
		if ((fds.revents & POLLHUP) || (fds.revents & POLLERR)) {
			/* close client's socket (it's probably already closed by client */
			close(client);
			break;
		}

		DEBUG("Get framed message...");
		buffer = get_framed_message(client);

		DEBUG("Check read buffer.");
		if (buffer != NULL) {
			enum json_tokener_error jerr;
			pthread_mutex_lock(&json_lock);
			request = json_tokener_parse_verbose(buffer, &jerr);
			if (jerr != json_tokener_success) {
				ERROR("JSON parsing error");
				pthread_mutex_unlock(&json_lock);
				continue;
			}

			session_id = get_param_string(request, "session");
			if (json_object_object_get_ex(request, "type", &js_tmp) == TRUE) {
				operation = json_object_get_int(js_tmp);
				json_object_put(js_tmp);
				js_tmp = NULL;
			}
			pthread_mutex_unlock(&json_lock);
			if (operation == -1) {
				reply = create_error("Missing operation type form frontend.");
				goto send_reply;
			}

			DEBUG("operation %d session_id %s.", operation, session_id);
			/* DO NOT FREE session_key HERE, IT IS PART OF REQUEST */
			if (operation != MSG_CONNECT && session_id == NULL) {
				reply = create_error("Missing session specification.");
				pthread_mutex_lock(&json_lock);
				msgtext = json_object_to_json_string(reply);
				pthread_mutex_unlock(&json_lock);

				send(client, msgtext, strlen(msgtext) + 1, 0);

				pthread_mutex_lock(&json_lock);
				json_object_put(reply);
				pthread_mutex_unlock(&json_lock);
				/* there is some stupid client, so close the connection to give a chance to some other client */
				close(client);
				break;
			}

			/* null global JSON error-reply */
			clean_err_reply();

			/* prepare reply envelope */
			if (reply != NULL) {
				pthread_mutex_lock(&json_lock);
				json_object_put(reply);
				pthread_mutex_unlock(&json_lock);
			}
			reply = NULL;

			/* process required operation */
			switch (operation) {
			case MSG_CONNECT:
				reply = handle_op_connect(request);
				break;
			case MSG_GET:
				reply = handle_op_get(request, session_id);
				break;
			case MSG_GETCONFIG:
				reply = handle_op_getconfig(request, session_id);
				break;
			case MSG_GETSCHEMA:
				reply = handle_op_getschema(request, session_id);
				break;
			case MSG_EDITCONFIG:
				reply = handle_op_editconfig(request, session_id);
				break;
			case MSG_COPYCONFIG:
				reply = handle_op_copyconfig(request, session_id);
				break;

			case MSG_DELETECONFIG:
			case MSG_LOCK:
			case MSG_UNLOCK:
				/* get parameters */
				pthread_mutex_lock(&json_lock);
				target = get_param_string(request, "target");
				pthread_mutex_unlock(&json_lock);
				if (target != NULL) {
					ds_type_t = parse_datastore(target);
				}

				if ((int)ds_type_t == -1) {
					reply = create_error("Invalid target repository type requested.");
					break;
				}
				switch(operation) {
				case MSG_DELETECONFIG:
					DEBUG("Request: delete-config (session %s)", session_id);
					pthread_mutex_lock(&json_lock);
					url = get_param_string(request, "url");
					pthread_mutex_unlock(&json_lock);
					reply = netconf_deleteconfig(session_id, ds_type_t, url);
					break;
				case MSG_LOCK:
					DEBUG("Request: lock (session %s)", session_id);
					reply = netconf_lock(session_id, ds_type_t);
					break;
				case MSG_UNLOCK:
					DEBUG("Request: unlock (session %s)", session_id);
					reply = netconf_unlock(session_id, ds_type_t);
					break;
				default:
					reply = create_error("Internal: Unknown request type.");
					break;
				}

				CHECK_ERR_SET_REPLY
				if (reply == NULL) {
					reply = create_ok();
				}
				break;
			case MSG_KILL:
				reply = handle_op_kill(request, session_id);
				break;
			case MSG_DISCONNECT:
				reply = handle_op_disconnect(request, session_id);
				break;
			case MSG_RELOADHELLO:
				reply = handle_op_reloadhello(request, session_id);
				break;
			case MSG_INFO:
				reply = handle_op_info(request, session_id);
				break;
			case MSG_GENERIC:
				reply = handle_op_generic(request, session_id);
				break;
			case MSG_NTF_GETHISTORY:
				reply = handle_op_ntfgethistory(request, session_id);
				break;
			case MSG_VALIDATE:
				reply = handle_op_validate(request, session_id);
				break;
			default:
				DEBUG("Unknown mod_netconf operation requested (%d)", operation);
				reply = create_error("Operation not supported.");
				break;
			}
			/* free parameters */
			CHECK_AND_FREE(url);
			CHECK_AND_FREE(target);
			request = NULL;
			operation = (-1);
			ds_type_t = (-1);

			DEBUG("Clean request json object.");
			if (request != NULL) {
				pthread_mutex_lock(&json_lock);
				json_object_put(request);
				pthread_mutex_unlock(&json_lock);
			}
			DEBUG("Send reply json object.");


send_reply:
			/* send reply to caller */
			if (reply != NULL) {
				pthread_mutex_lock(&json_lock);
				msgtext = json_object_to_json_string(reply);
				if (asprintf(&chunked_out_msg, "\n#%d\n%s\n##\n", (int) strlen(msgtext), msgtext) == -1) {
					if (buffer != NULL) {
						free(buffer);
						buffer = NULL;
					}
					pthread_mutex_unlock(&json_lock);
					break;
				}
				pthread_mutex_unlock(&json_lock);

				DEBUG("Send framed reply json object.");
				send(client, chunked_out_msg, strlen(chunked_out_msg) + 1, 0);
				DEBUG("Clean reply json object.");
				pthread_mutex_lock(&json_lock);
				json_object_put(reply);
				reply = NULL;
				DEBUG("Clean message buffer.");
				CHECK_AND_FREE(chunked_out_msg);
				chunked_out_msg = NULL;
				if (buffer != NULL) {
					free(buffer);
					buffer = NULL;
				}
				pthread_mutex_unlock(&json_lock);
				clean_err_reply();
			} else {
				ERROR("Reply is NULL, shouldn't be...");
				continue;
			}
		}
	}
	free (arg);

	free_err_reply();

	return retval;
}

/**
 * \brief Close all open NETCONF sessions.
 *
 * During termination of mod_netconf, it is useful to close all remaining
 * sessions. This function iterates over the list of sessions and close them
 * all.
 */
static void close_all_nc_sessions(void)
{
	struct session_with_mutex *swm = NULL;
	int ret;

	/* get exclusive access to sessions_list (conns) */
	DEBUG("LOCK wrlock %s", __func__);
	if ((ret = pthread_rwlock_wrlock (&session_lock)) != 0) {
		ERROR("Error while locking rwlock: %d (%s)", ret, strerror(ret));
		return;
	}
	for (swm = netconf_sessions_list; swm; swm = swm->next) {
		DEBUG("Closing NETCONF session (%s).", nc_session_get_id(swm->session));

        /* close_and_free_session handles locking on its own */
        close_and_free_session(swm);
	}
	/* get exclusive access to sessions_list (conns) */
	DEBUG("UNLOCK wrlock %s", __func__);
	if (pthread_rwlock_unlock (&session_lock) != 0) {
		ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
	}
}

static void check_timeout_and_close(void)
{
	struct nc_session *ns = NULL;
	struct session_with_mutex *swm = NULL;
	time_t current_time = time(NULL);
	int ret;

	/* get exclusive access to sessions_list (conns) */
	if ((ret = pthread_rwlock_wrlock(&session_lock)) != 0) {
		DEBUG("Error while locking rwlock: %d (%s)", ret, strerror(ret));
		return;
	}
	for (swm = netconf_sessions_list; swm; swm = swm->next) {
		ns = swm->session;
		if (ns == NULL) {
			continue;
		}
		pthread_mutex_lock(&swm->lock);
		if ((current_time - swm->last_activity) > ACTIVITY_TIMEOUT) {
			DEBUG("Closing NETCONF session (%s).", nc_session_get_id(swm->session));

			/* close_and_free_session handles locking on its own */
			close_and_free_session(swm);
		} else {
			pthread_mutex_unlock(&swm->lock);
		}
	}
	/* get exclusive access to sessions_list (conns) */
	if (pthread_rwlock_unlock(&session_lock) != 0) {
		ERROR("Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
	}
}


/**
 * This is actually implementation of NETCONF client
 * - requests are received from UNIX socket in the predefined format
 * - results are replied through the same way
 * - the daemon run as a separate process
 *
 */
static void forked_proc(void)
{
	struct timeval tv;
	struct sockaddr_un local, remote;
	int lsock, client, ret, i, pthread_count = 0;
	unsigned int olds = 0, timediff = 0;
	socklen_t len;
	struct pass_to_thread * arg;
	pthread_t * ptids = calloc(1, sizeof(pthread_t));
	struct timespec maxtime;
	pthread_rwlockattr_t lock_attrs;
	#ifdef WITH_NOTIFICATIONS
	char use_notifications = 0;
	#endif

	/* wait at most 5 seconds for every thread to terminate */
	maxtime.tv_sec = 5;
	maxtime.tv_nsec = 0;

#ifdef HAVE_UNIXD_SETUP_CHILD
	/* change uid and gid of process for security reasons */
	unixd_setup_child();
#else
# ifdef SU_GROUP
	if (strlen(SU_GROUP) > 0) {
		struct group *g = getgrnam(SU_GROUP);
		if (g == NULL) {
			ERROR("GID (%s) was not found.", SU_GROUP);
			return;
		}
		if (setgid(g->gr_gid) != 0) {

			ERROR("Switching to %s GID failed. (%s)", SU_GROUP, strerror(errno));
			return;
		}
	}
# else
	DEBUG("no SU_GROUP");
# endif
# ifdef SU_USER
	if (strlen(SU_USER) > 0) {
		struct passwd *p = getpwnam(SU_USER);
		if (p == NULL) {
			ERROR("UID (%s) was not found.", SU_USER);
			return;
		}
		if (setuid(p->pw_uid) != 0) {
			ERROR("Switching to UID %s failed. (%s)", SU_USER, strerror(errno));
			return;
		}
	}
# else
	DEBUG("no SU_USER");
# endif
#endif

	/* try to remove if exists */
	unlink(sockname);

	/* create listening UNIX socket to accept incoming connections */
	if ((lsock = socket(PF_UNIX, SOCK_STREAM, 0)) == -1) {
		ERROR("Creating socket failed (%s)", strerror(errno));
		goto error_exit;
	}

	local.sun_family = AF_UNIX;
	strncpy(local.sun_path, sockname, sizeof(local.sun_path));
	len = offsetof(struct sockaddr_un, sun_path) + strlen(local.sun_path);

	if (bind(lsock, (struct sockaddr *) &local, len) == -1) {
		if (errno == EADDRINUSE) {
			ERROR("mod_netconf socket address already in use");
			goto error_exit;
		}
		ERROR("Binding socket failed (%s)", strerror(errno));
		goto error_exit;
	}

	if (listen(lsock, MAX_SOCKET_CL) == -1) {
		ERROR("Setting up listen socket failed (%s)", strerror(errno));
		goto error_exit;
	}
	chmod(sockname, S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR | S_IRGRP | S_IROTH);

	uid_t user = -1;
	if (strlen(CHOWN_USER) > 0) {
		struct passwd *p = getpwnam(CHOWN_USER);
		if (p != NULL) {
			user = p->pw_uid;
		}
	}
	gid_t group = -1;
	if (strlen(CHOWN_GROUP) > 0) {
		struct group *g = getgrnam(CHOWN_GROUP);
		if (g != NULL) {
			group = g->gr_gid;
		}
	}
	if (chown(sockname, user, group) == -1) {
		ERROR("Chown on socket file failed (%s).", strerror(errno));
	}

	/* prepare internal lists */

	#ifdef WITH_NOTIFICATIONS
	if (notification_init() == -1) {
		ERROR("libwebsockets initialization failed");
		use_notifications = 0;
	} else {
		use_notifications = 1;
	}
	#endif

	/* setup libnetconf's callbacks */
	nc_verbosity(NC_VERB_DEBUG);
	nc_callback_print(clb_print);
	nc_callback_ssh_host_authenticity_check(netconf_callback_ssh_hostkey_check);
	nc_callback_sshauth_interactive(netconf_callback_sshauth_interactive);
	nc_callback_sshauth_password(netconf_callback_sshauth_password);
	nc_callback_sshauth_passphrase(netconf_callback_sshauth_passphrase);
	nc_callback_error_reply(netconf_callback_error_process);

	/* disable publickey authentication */
	nc_ssh_pref(NC_SSH_AUTH_PUBLIC_KEYS, -1);

	/* create mutex protecting session list */
	pthread_rwlockattr_init(&lock_attrs);
	/* rwlock is shared only with threads in this process */
	pthread_rwlockattr_setpshared(&lock_attrs, PTHREAD_PROCESS_PRIVATE);
	/* create rw lock */
	if (pthread_rwlock_init(&session_lock, &lock_attrs) != 0) {
		ERROR("Initialization of mutex failed: %d (%s)", errno, strerror(errno));
		goto error_exit;
	}
	pthread_mutex_init(&ntf_history_lock, NULL);
	pthread_mutex_init(&json_lock, NULL);
	DEBUG("Initialization of notification history.");
	if (pthread_key_create(&notif_history_key, NULL) != 0) {
		ERROR("Initialization of notification history failed.");
	}
	if (pthread_key_create(&err_reply_key, NULL) != 0) {
		ERROR("Initialization of reply key failed.");
	}

	fcntl(lsock, F_SETFL, fcntl(lsock, F_GETFL, 0) | O_NONBLOCK);
	while (isterminated == 0) {
		gettimeofday(&tv, NULL);
		timediff = (unsigned int)tv.tv_sec - olds;
		#ifdef WITH_NOTIFICATIONS
		if (use_notifications == 1) {
			notification_handle();
		}
		#endif
		if (timediff > ACTIVITY_CHECK_INTERVAL) {
			check_timeout_and_close();
		}

		/* open incoming connection if any */
		len = sizeof(remote);
		client = accept(lsock, (struct sockaddr *) &remote, &len);
		if (client == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			sleep(SLEEP_TIME);
			continue;
		} else if (client == -1 && (errno == EINTR)) {
			continue;
		} else if (client == -1) {
			ERROR("Accepting mod_netconf client connection failed (%s)", strerror(errno));
			continue;
		}

		/* set client's socket as non-blocking */
		//fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) | O_NONBLOCK);

		arg = malloc(sizeof(struct pass_to_thread));
		arg->client = client;
		arg->netconf_sessions_list = netconf_sessions_list;

		/* start new thread. It will serve this particular request and then terminate */
		if ((ret = pthread_create (&ptids[pthread_count], NULL, thread_routine, (void*)arg)) != 0) {
			ERROR("Creating POSIX thread failed: %d\n", ret);
		} else {
			DEBUG("Thread %lu created", ptids[pthread_count]);
			pthread_count++;
			ptids = realloc (ptids, sizeof(pthread_t)*(pthread_count+1));
			ptids[pthread_count] = 0;
		}

		/* check if some thread already terminated, free some resources by joining it */
		for (i=0; i<pthread_count; i++) {
			if (pthread_tryjoin_np (ptids[i], (void**)&arg) == 0) {
				DEBUG("Thread %lu joined with retval %p", ptids[i], arg);
				pthread_count--;
				if (pthread_count > 0) {
					/* place last Thread ID on the place of joined one */
					ptids[i] = ptids[pthread_count];
				}
			}
		}
		DEBUG("Running %d threads", pthread_count);
	}

	DEBUG("mod_netconf terminating...");
	/* join all threads */
	for (i=0; i<pthread_count; i++) {
		pthread_timedjoin_np (ptids[i], (void**)&arg, &maxtime);
	}

	#ifdef WITH_NOTIFICATIONS
	notification_close();
	#endif

	/* close all NETCONF sessions */
	close_all_nc_sessions();

	/* destroy rwlock */
	pthread_rwlock_destroy(&session_lock);
	pthread_rwlockattr_destroy(&lock_attrs);

	DEBUG("Exiting from the mod_netconf daemon");

	free(ptids);
	close(lsock);
	exit(0);
	return;
error_exit:
	close(lsock);
	free(ptids);
	return;
}

int main(int argc, char **argv)
{
    struct sigaction action;
    sigset_t block_mask;

    if (argc > 1) {
        sockname = argv[1];
    } else {
        sockname = SOCKET_FILENAME;
    }

	sigfillset (&block_mask);
    action.sa_handler = signal_handler;
    action.sa_mask = block_mask;
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

	forked_proc();
	DEBUG("Terminated");
	return 0;
}
