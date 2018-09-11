/*****************************************************************************
 * media_dir.c
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
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>

#ifdef USE_ID3TAG
#include <id3tag.h>
#include "jsonrpc.h"
#endif

#include "player.h"
#include "media.h"

#define N_(string) string

typedef struct media_dirlist_s media_dirlist_t;
struct media_ctx_s
{
	const char *url;
	int mediaid;
	unsigned int options;
	media_dirlist_t *first;
	media_dirlist_t *current;
};

struct media_dirlist_s
{
	char *path;
	struct dirent **items;
	int nitems;
	int index;
	int mediaid;
	media_dirlist_t *next;
	media_dirlist_t *prev;
};

#define OPTION_LOOP 0x0001
#define OPTION_RANDOM 0x0002

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static int media_count(media_ctx_t *ctx);
static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime);
static int media_find(media_ctx_t *ctx, int id, media_parse_t cb, void *data);
static int media_current(media_ctx_t *ctx, media_parse_t cb, void *data);
static int media_play(media_ctx_t *ctx, media_parse_t play, void *data);
static int media_next(media_ctx_t *ctx);
static int media_end(media_ctx_t *ctx);

static const char *utils_getmime(const char *path)
{
	char *ext = strrchr(path, '.');
	if (ext && !strcmp(ext, ".mp3"))
		return mime_mp3;
	dbg("Unknonw mime for %s", path);
	return NULL;
}

/**
 * directory browsing functions
 **/
typedef int (*_findcb_t)(void *arg, media_ctx_t *ctx, int mediaid, const char *path, const char *mime);

typedef struct _find_mediaid_s _find_mediaid_t;
struct _find_mediaid_s
{
	int id;
	media_parse_t cb;
	void *arg;
};

static int _run_cb(_find_mediaid_t *mdata, const char *path, const char *mime)
{
	int ret = 0;
	if (mdata->cb != NULL)
	{
		char *info = NULL;
#ifdef USE_ID3TAG
		json_t *object = NULL;
		int i;
		static struct
		{
			char const *id;
			char const *label;
		} const labels[] = 
		{
		{ ID3_FRAME_TITLE,  N_("Title")     },
		{ ID3_FRAME_ARTIST, N_("Artist")    },
		{ ID3_FRAME_ALBUM,  N_("Album")     },
		{ ID3_FRAME_TRACK,  N_("Track")     },
		{ ID3_FRAME_YEAR,   N_("Year")      },
		{ ID3_FRAME_GENRE,  N_("Genre")     },
		};
		struct id3_file *fd = id3_file_open(path, ID3_FILE_MODE_READONLY);
		struct id3_tag *tag = id3_file_tag(fd);

		object = json_object();

		for (i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i)
		{
			struct id3_frame const *frame;
			frame = id3_tag_findframe(tag, labels[i].id, 0);
			if (frame)
			{
				union id3_field const *field;
				id3_ucs4_t const *ucs4;
				field    = id3_frame_field(frame, 1);
				ucs4 = id3_field_getstrings(field, 0);
				char *latin1 = id3_ucs4_latin1duplicate(ucs4);
				json_t *value = json_string(latin1);
				json_object_set(object, labels[i].label, value);
			}
		}
		info = json_dumps(object, JSON_INDENT(2));
		json_decref(object);
		id3_file_close(fd);
#endif
		ret = mdata->cb(mdata->arg, path, info, mime);
		if (info != NULL)
			free(info);
	}
	return ret;
}

static int _find_mediaid(void *arg, media_ctx_t *ctx, int mediaid, const char *path, const char *mime)
{
	int ret = 1;
	_find_mediaid_t *mdata = (_find_mediaid_t *)arg;
	if (mdata->id >= 0 && mdata->id == mediaid)
	{
		_run_cb(mdata, path, mime);
		ret = 0;
	}
	else if (mdata->id == -1 && mdata->cb != NULL)
		_run_cb(mdata, path, mime);

	
	return ret;
}

static int _find_display(void *arg, media_ctx_t *ctx, int mediaid, const char *path, const char mime)
{
	printf("Media: %d, %s\n", mediaid, path);
	return 1;
}

static int _find(media_ctx_t *ctx, media_dirlist_t **pit, int *pmediaid, _findcb_t cb, void *arg)
{
	int ret = -1;
	media_dirlist_t *it = *pit;
	if (it == NULL)
	{
		it = calloc(1, sizeof(*it));
		char *path = strstr(ctx->url, "file://");
		if (path == NULL)
		{
			if (strstr(ctx->url, "://"))
			{
				free(it);
				return -1;
			}
			path = (char *)ctx->url;
		}
		else
			path += sizeof("file://") - 1;
		if (path[0] == '/')
			path++;
		it->path = malloc(1 + strlen(path) + 1);
		sprintf(it->path,"/%s", path);
		it->nitems = scandir(it->path, &it->items, NULL, alphasort);
		*pmediaid = 0;
	}
	while (it->index != it->nitems)
	{
		if (it->items[it->index]->d_name[0] == '.')
		{
			it->index++;
			continue;
		}
		switch (it->items[it->index]->d_type)
		{
			case DT_DIR:
			{
				media_dirlist_t *new = calloc(1, sizeof(*new));
				
				if (new)
				{
					new->path = malloc(strlen(it->path) + 1 + strlen(it->items[it->index]->d_name) + 1);
					if (new->path)
					{
						sprintf(new->path,"%s/%s", it->path, it->items[it->index]->d_name);
						new->nitems = scandir(new->path, &new->items, NULL, alphasort);
					}
					if (new->nitems > 0)
					{
						new->prev = it;
						ret = _find(ctx, &new, pmediaid, cb, arg);
						if (ret == 0)
							it = new;
					}
					else
					{
						if (new->path)
							free(new->path);
						if (new->items)
							free(new->items);
						free(new);
					}
				}
			}
			break;
			case DT_REG:
			{
				char *path = malloc(strlen(it->path) + 1 + strlen(it->items[it->index]->d_name) + 1);
				if (path)
				{
					sprintf(path,"%s/%s", it->path, it->items[it->index]->d_name);
					const char *mime = utils_getmime(path);
					if (mime != NULL)
						ret = cb(arg, ctx, *pmediaid, path, mime);
					free(path);
					if (ret > 0)
						(*pmediaid)++;
				}
			}
			break;
			default:
			break;
		}
		if (ret == 0)
		{
			break;
		}
		it->index++;
	}
	if (it->index == it->nitems)
	{
		if (it->path)
			free(it->path);
		it->path == NULL;
		free(it->items);
		media_dirlist_t *prev = it->prev;
		free(it);
		it = prev;
	}
	*pit = it;
	return ret;
}

/**
 * media API
 **/
static int media_count(media_ctx_t *ctx)
{
	return 1;
}

static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime)
{
	ctx->url = path;
	return 0;
}

static int media_remove(media_ctx_t *ctx, int id, const char *path)
{
	return -1;
}

static int media_find(media_ctx_t *ctx, int id, media_parse_t cb, void *arg)
{
	
	int ret;
	int mediaid = 0;
	media_dirlist_t *dir = NULL;
	_find_mediaid_t mdata = {id, cb, arg};
	ret = _find(ctx, &dir, &mediaid, _find_mediaid, &mdata);
	return ret;
}

static int media_current(media_ctx_t *ctx, media_parse_t cb, void *arg)
{
	return media_find(ctx, ctx->mediaid, cb, arg);
}

static int media_list(media_ctx_t *ctx, media_parse_t cb, void *arg)
{
	int ret;
	int mediaid = 0;
	media_dirlist_t *dir = NULL;
	_find_mediaid_t mdata = {-1, cb, arg};
	ret = _find(ctx, &dir, &mediaid, _find_mediaid, &mdata);
	return ret;
}

static int media_play(media_ctx_t *ctx, media_parse_t cb, void *data)
{
	int ret = -1;
	media_dirlist_t *it = ctx->current;
	if (it != NULL && it->items[it->index]->d_type != DT_DIR)
	{
		char *path = malloc(strlen(it->path) + 1 + strlen(it->items[it->index]->d_name) + 1);
		if (path)
		{
			sprintf(path,"%s/%s", it->path, it->items[it->index]->d_name);
			ret = cb(data, path, NULL, utils_getmime(path));
			free(path);
		}
	}
	return ctx->mediaid;
}

static int media_next(media_ctx_t *ctx)
{
	int ret;
	_find_mediaid_t data = {ctx->mediaid + 1, NULL, NULL};
	ret = _find(ctx, &ctx->current, &ctx->mediaid, _find_mediaid, &data);
	return ctx->mediaid;
}

static int media_end(media_ctx_t *ctx)
{
	media_dirlist_t *it = ctx->current;
	while (it != NULL)
	{
		ctx->current = it->prev;
		free(it);
		it = ctx->current;
	}
	ctx->current = NULL;
	ctx->mediaid = -1;
	return 0;
}

/**
 * the loop requires to restart the player.
 */
static void media_loop(media_ctx_t *ctx, int enable)
{
	if (enable)
		ctx->options |= OPTION_LOOP;
	else
		ctx->options &= ~OPTION_LOOP;
}

static void media_random(media_ctx_t *ctx, int enable)
{
}

static int media_options(media_ctx_t *ctx, media_options_t option, int enable)
{
	int ret = 0;
	if (option == MEDIA_LOOP)
	{
		media_loop(ctx, enable);
		ret = (ctx->options & OPTION_LOOP) == OPTION_LOOP;
	}
	else if (option == MEDIA_RANDOM)
	{
		ret = 0;
	}
	return ret;
}

static media_ctx_t *media_init(const char *url)
{
	media_ctx_t *ctx = NULL;
	if (url)
	{
		ctx = calloc(1, sizeof(*ctx));
		ctx->mediaid = 0;
		ctx->url = url;
		ctx->mediaid = -1;
	}
	return ctx;
}

static void media_destroy(media_ctx_t *ctx)
{
	free(ctx);
}

media_ops_t *media_dir = &(media_ops_t)
{
	.init = media_init,
	.destroy = media_destroy,
	.next = media_next,
	.play = media_play,
	.list = media_list,
	.current = media_current,
	.find = media_find,
	.remove = media_remove,
	.insert = media_insert,
	.count = media_count,
	.end = media_end,
	.options = media_options,
};