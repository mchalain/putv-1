/*****************************************************************************
 * main.c
 * this file is part of https://github.com/ouistiti-project/putv
 *****************************************************************************
 * Copyright (C) 2016-2017
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *****************************************************************************/
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <libgen.h>
#include <sys/socket.h>
#include <sys/un.h>
# include <sys/ioctl.h>

#include <jansson.h>
#include <pthread.h>

#include "jsonrpc.h"
#include "client_json.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define client_dbg(...)

struct client_event_s
{
	const char *event;
	client_event_prototype_t proto;
	void *data;
	client_event_t *next;
};

static int method_subscribe(json_t *json_params, json_t **result, void *userdata)
{
	*result = json_pack("{ss}", "event", "onchange");
	return 0;
}

static int answer_proto(client_data_t * data, json_t *json_params)
{
	pthread_mutex_lock(&data->mutex);
	data->pid = 0;
	if (data->proto)
		data->proto(data->data, json_params);
	data->proto = NULL;
	data->data = NULL;
	pthread_mutex_unlock(&data->mutex);
	pthread_cond_broadcast(&data->cond);
	return 0;
}

static int answer_subscribe(json_t *json_params, json_t **result, void *userdata)
{
	int state = -1;
	json_unpack(json_params, "{sb}", "subscribed", &state);
	client_data_t *data = userdata;

	answer_proto(data, json_params);

	return !state;
}

static int method_nullparam(json_t *json_params, json_t **result, void *userdata)
{
	*result = json_null();
	return 0;
}

static int answer_stdparams(json_t *json_params, json_t **result, void *userdata)
{
	client_data_t *data = userdata;

	answer_proto(data, json_params);
	return 0;
}

static int method_volume(json_t *json_params, json_t **result, void *userdata)
{
	client_data_t *data = userdata;
	if (json_is_integer(data->params))
	{
		*result = json_object();
		json_object_set(*result, "step", data->params);
	}
	else if (json_is_object(data->params))
	{
		*result = data->params;
	}
	return 0;
}

static int method_stdparams(json_t *json_params, json_t **result, void *userdata)
{
	client_data_t *data = userdata;
	*result = data->params;
	return 0;
}

static int method_list(json_t *json_params, json_t **result, void *userdata)
{
	client_data_t *data = userdata;
	json_error_t error;
	*result = json_object();
	json_object_set(*result, "first", json_integer(data->list->first));
	return 0;
}

static int notification_onchange(json_t *json_params, json_t **result, void *userdata)
{
	client_data_t *data = userdata;
	client_event_t *event = data->events;
	while (event)
	{
		if (!strcmp(event->event, "onchange"))
			break;
		event = event->next;
	}
	if (event)
	{
		event->proto(event->data, json_params);
	}
	return 0;
}

struct jsonrpc_method_entry_t table[] =
{
	{'r',"subscribe", method_subscribe, "o", 0, NULL},
	{'a',"subscribe", answer_subscribe, "o", 0, NULL},
	{'r',"next", method_nullparam, "", 0, NULL},
	{'a',"next", answer_stdparams, "o", 0, NULL},
	{'r',"play", method_nullparam, "", 0, NULL},
	{'a',"play", answer_stdparams, "o", 0, NULL},
	{'r',"pause", method_nullparam, "", 0, NULL},
	{'a',"pause", answer_stdparams, "o", 0, NULL},
	{'r',"stop", method_nullparam, "", 0, NULL},
	{'a',"stop", answer_stdparams, "o", 0, NULL},
	{'r',"status", method_nullparam, "", 0, NULL},
	{'a',"status", answer_stdparams, "o", 0, NULL},
	{'r',"volume", method_volume, "o", 0, NULL},
	{'a',"volume", answer_stdparams, "o", 0, NULL},
	{'r',"change", method_stdparams, "o", 0, NULL},
	{'a',"change", answer_stdparams, "o", 0, NULL},
	{'r',"append", method_stdparams, "o", 0, NULL},
	{'a',"append", answer_stdparams, "o", 0, NULL},
	{'r',"remove", method_stdparams, "o", 0, NULL},
	{'a',"remove", answer_stdparams, "o", 0, NULL},
	{'r',"list", method_list, "o", 0, NULL},
	{'a',"list", answer_stdparams, "o", 0, NULL},
	{'r',"getposition", method_nullparam, "", 0, NULL},
	{'a',"getposition", answer_stdparams, "o", 0, NULL},
	{'r',"options", method_stdparams, "", 0, NULL},
	{'a',"options", answer_stdparams, "o", 0, NULL},
	{'n',"onchange", notification_onchange, "o", 0, NULL},
	{0, NULL},
};

int client_unix(const char *socketpath, client_data_t *data)
{
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock > 0)
	{
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(struct sockaddr_un));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, socketpath, sizeof(addr.sun_path));
		int ret = connect(sock, (const struct sockaddr *)&addr, sizeof(addr.sun_path));
		if (ret != 0)
			sock = 0;
		data->sock = sock;
		pthread_cond_init(&data->cond, NULL);
		pthread_mutex_init(&data->mutex, NULL);
	}

	return sock;
}

void client_async(client_data_t *data,int async)
{
	if (async)
		data->options |= OPTION_ASYNC;
	else
		data->options &= ~OPTION_ASYNC;
}

unsigned long int client_cmd(client_data_t *data, char * cmd)
{
	int ret;
	char *buffer = jsonrpc_request(cmd, strlen(cmd), table, (char*)data, &data->pid);
	if (buffer == NULL)
	{
		err("cmd %s unknown", cmd);
		return 0;
	}
	unsigned long int pid = data->pid;
	client_dbg("client: send %s", buffer);
	ret = send(data->sock, buffer, strlen(buffer) + 1, MSG_NOSIGNAL);
	if (ret < 0)
	{
		err("jsonrpc: send error %s", strerror(errno));
		return -1;
	}
	if (data->options & OPTION_ASYNC)
		return 0;
	return pid;
}

int client_wait(client_data_t *data, unsigned long int pid)
{
	if ((data->options & OPTION_ASYNC) == OPTION_ASYNC)
		return 0;
	while (pid > 0 && data->pid == pid)
	{
		pthread_cond_wait(&data->cond, &data->mutex);
	}
	return 0;
}

int client_next(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	if (data->pid > 0)
	{
		return -2;
	}
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	long int pid = client_cmd(data, "next");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

int client_play(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	if (data->pid > 0)
		return -2;
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	long int pid = client_cmd(data, "play");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

int client_pause(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	if (data->pid > 0)
		return -2;
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	long int pid = client_cmd(data, "pause");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

int client_stop(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	if (data->pid > 0)
		return -2;
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	long int pid = client_cmd(data, "stop");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

int client_status(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	if (data->pid > 0)
		return -2;
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	long int pid = client_cmd(data, "status");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

int client_getposition(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	if (data->pid > 0)
		return -2;
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	long int pid = client_cmd(data, "getposition");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

int client_volume(client_data_t *data, client_event_prototype_t proto, void *protodata, json_t *step)
{
	if (data->pid > 0)
		return -2;
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	data->params = step;
	long int pid = client_cmd(data, "volume");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

int client_eventlistener(client_data_t *data, const char *name, client_event_prototype_t proto, void *protodata)
{
	client_event_t *event = calloc(1, sizeof(*event));
	event->event = strdup(name);
	event->proto = proto;
	event->data = protodata;
	event->next = data->events;
	data->events = event;
	return 0;
}

int media_change(client_data_t *data, client_event_prototype_t proto, void *protodata, json_t *media)
{
	if (data->pid > 0)
		return -2;
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	data->params = media;
	long int pid = client_cmd(data, "change");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	dbg("client: new media %s", json_string_value(json_object_get(media, "name")));
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

int media_insert(client_data_t *data, client_event_prototype_t proto, void *protodata, json_t *media)
{
	if (data->pid > 0)
		return -2;
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	data->params = media;
	long int pid = client_cmd(data, "append");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

int media_remove(client_data_t *data, client_event_prototype_t proto, void *protodata, json_t *media)
{
	if (data->pid > 0)
		return -2;
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	data->params = media;
	long int pid = client_cmd(data, "remove");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

int media_list(client_data_t *data, client_event_prototype_t proto, void *protodata, list_t *list)
{
	if (data->pid > 0)
		return -2;
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	data->list = list;
	long int pid = client_cmd(data, "list");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

int media_options(client_data_t *data, client_event_prototype_t proto, void *protodata, int random, int loop)
{
	if (data->pid > 0)
		return -2;
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	data->params = json_object();
	json_object_set_new(data->params, "random", json_boolean(random));
	json_object_set_new(data->params, "loop", json_boolean(loop));
	long int pid = client_cmd(data, "options");
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return 0;
}

#ifdef JSONRPC_LARGEPACKET
static size_t recv_cb(void *buffer, size_t len, void *arg)
{
	int ret;
	client_data_t *data = (client_data_t *)arg;
	if (data->message == NULL)
	{
		ret = recv(data->sock, buffer, len, MSG_NOSIGNAL);
		if (ret == 0)
			data->run = 0;
	}
	else
	{
		ret = data->messagelen;
		memcpy(buffer, data->message, data->messagelen);
		free(data->message);
		data->message = NULL;
	}
	int length = strlen(buffer);
	if ((length + 1) < ret)
	{
		err("client: two messages in ONE");
		/**
		 * disable the last command, the response may be lost
		 * TODO
		 * store the second message and call again the json parser.
		 */
		data->pid = 0;
		client_dbg("client: recv %ld/%d" , strlen(buffer), ret);
		data->message = malloc(ret - length - 1);
		memcpy(data->message, buffer + length + 1, ret - length - 1);
		data->messagelen = ret - length - 1;
	}
	client_dbg("client: recv %d %s", ret, (char *)buffer);
	if (ret >= 0)
		((char*)buffer)[ret] = 0;
	else
		length = -1;
	return length;
}
#endif

json_t *client_error_response(json_t *json_id, json_t *json_error, void *arg)
{
	client_data_t *data = (client_data_t *)arg;
	data->pid = 0;
	return jsonrpc_request_error_response(json_id, json_error, arg);
}

int client_loop(client_data_t *data)
{
	client_event_t *event = data->events;
	while (event)
	{
//		jsonrpc_request("subscribe");
		event = event->next;
	}
	data->run = 1;
	jsonrpc_set_errorhandler(client_error_response);
	while (data->sock > 0  && data->run)
	{
		fd_set rfds;
		int maxfd = data->sock;
		FD_ZERO(&rfds);
		FD_SET(data->sock, &rfds);
		int ret;
		ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
		if (ret > 0 && FD_ISSET(data->sock, &rfds))
		{
#ifdef JSONRPC_LARGEPACKET
			do
			{
				json_error_t error;
				json_t *response = json_load_callback(recv_cb, (void *)data, JSON_DISABLE_EOF_CHECK, &error);
				if (response != NULL)
				{
					jsonrpc_jresponse(response, table, data);
				}
				else
				{
					err("client: receive mal formated json %s", error.text);
					data->pid = 0;
				}
			} while (data->message != NULL);
#else
			int len;
			ret = ioctl(data->sock, FIONREAD, &len);
			char *buffer = malloc(len + 1);
			ret = recv(data->sock, buffer, len, MSG_NOSIGNAL);
			buffer[ret] = 0;
			if (ret > 0)
				jsonrpc_handler(buffer, strlen(buffer), table, data);
			if (ret == 0)
				data->run = 0;
#endif
		}
	}
	pthread_cond_destroy(&data->cond);
	pthread_mutex_destroy(&data->mutex);
	return 0;
}
