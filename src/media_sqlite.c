/*****************************************************************************
 * putv.c
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

#include <sqlite3.h>

#include "putv.h"
#include "media.h"

struct media_ctx_s
{
	sqlite3 *db;
	int mediaid;
	mediaplayer_ctx_t *ctx;
};

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static const char *utils_getmime(const char *path)
{
	char *ext = strrchr(path, '.');
	if (!strcmp(ext, ".mp3"))
		return mime_mp3;
	return mime_octetstream;
}

static int _execute(sqlite3_stmt *statement)
{
	int id = -1;
	int ret;
	ret = sqlite3_step(statement);
	dbg("execute %d", ret);
	while (ret == SQLITE_ROW)
	{
		int i = 0, nbColumns = sqlite3_column_count(statement);
		if (i < nbColumns)
		{
			//const char *key = sqlite3_column_name(statement, i);
			if (sqlite3_column_type(statement, i) == SQLITE_INTEGER)
			{
				id = sqlite3_column_int(statement, i);
			}
		}
		ret = sqlite3_step(statement);
	}
	return id;
}

int media_count(media_ctx_t *ctx)
{
	sqlite3 *db = ctx->db;
	int count = 0;
	sqlite3_stmt *statement;
	int size = 256;
	char *sql = sqlite3_malloc(size);
	snprintf(sql, size, "select \"id\" from \"media\" ");
	sqlite3_prepare_v2(db, sql, size, &statement, NULL);

	int ret = sqlite3_step(statement);
	if (ret != SQLITE_ERROR)
	{
		do
		{
			count++;
			ret = sqlite3_step(statement);
		} while (ret == SQLITE_ROW);
	}
	sqlite3_finalize(statement);
	sqlite3_free(sql);

	return count;
}

static int findmedia(sqlite3 *db, const char *path)
{
	sqlite3_stmt *statement;
	int size = 256;
	char *sql = sqlite3_malloc(size);
	snprintf(sql, size, "select \"id\" from \"media\" where \"url\"=\"@PATH\"");
	sqlite3_prepare_v2(db, sql, size, &statement, NULL);
	/** set the default value of @FIELDS **/
	sqlite3_bind_text(statement, sqlite3_bind_parameter_index(statement, "@PATH"), path, -1, SQLITE_STATIC);

	int id = _execute(statement);

	sqlite3_finalize(statement);
	sqlite3_free(sql);
	return id;
}

int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime)
{
	sqlite3 *db = ctx->db;

	if (path == NULL)
		return -1;

	int id = findmedia(db, path);
	if (id != -1)
		return 0;

	int ret = 0;
	sqlite3_stmt *statement;
	int size = 1024;
	char *sql;
	sql = sqlite3_malloc(size);
	snprintf(sql, size, "insert into \"media\" (\"url\", \"mime\", \"info\") values(@PATH , @MIME , @INFO);");

	ret = sqlite3_prepare_v2(db, sql, size, &statement, NULL);
	if (ret != SQLITE_OK)
		return -1;

	int index;
	index = sqlite3_bind_parameter_index(statement, "@PATH");
	ret = sqlite3_bind_text(statement, index, path, -1, SQLITE_STATIC);
	index = sqlite3_bind_parameter_index(statement, "@INFO");
	if (info != NULL)
		ret = sqlite3_bind_text(statement, index, info, -1, SQLITE_STATIC);
	else
		ret = sqlite3_bind_null(statement, index);
	index = sqlite3_bind_parameter_index(statement, "@MIME");
	if (mime == NULL)
		mime = utils_getmime(path);
	if (mime)
		ret = sqlite3_bind_text(statement, index, mime, -1, SQLITE_STATIC);
	else
		ret = sqlite3_bind_null(statement, index);

	ret = sqlite3_step(statement);
	if (ret != SQLITE_DONE)
		ret = -1;
	else
	{
		dbg("putv: new media %s", path);
	}
	sqlite3_finalize(statement);
	sqlite3_free(sql);

	return ret;
}

int media_find(media_ctx_t *ctx, int id, char *url, int *urllen, char *info, int *infolen)
{
	sqlite3_stmt *statement;
	int size = 256;
	char *sql = sqlite3_malloc(size);
	snprintf(sql, size, "select \"url\" \"info\" from \"media\" where id = @ID");
	sqlite3_prepare_v2(ctx->db, sql, size, &statement, NULL);

	int index = sqlite3_bind_parameter_index(statement, "@ID");
	sqlite3_bind_int(statement, index, id);

	int ret = sqlite3_step(statement);
	if (ret == SQLITE_ROW)
	{
		int len = sqlite3_column_bytes(statement, 0);
		*urllen = (*urllen > len)? len: *urllen;
		len = sqlite3_column_bytes(statement, 1);
		*infolen = (*infolen > len)? len: *infolen;
		strncpy(url, (const char *)sqlite3_column_text(statement, 0), *urllen);
		strncpy(info, (const char *)sqlite3_column_text(statement, 1), *infolen);
		ret = 0;
	}
	else
		ret = -1;
	sqlite3_finalize(statement);
	sqlite3_free(sql);
	return ret;
}

int media_current(media_ctx_t *ctx, char *url, int *urllen, char *info, int *infolen)
{
	return media_find(ctx, ctx->mediaid, url, urllen, info, infolen);
}

int media_play(media_ctx_t *ctx, play_fcn_t play, void *data)
{
	int ret = -1;
	sqlite3_stmt *statement;
	int size = 256;
	char *sql = sqlite3_malloc(size);
	snprintf(sql, size, "select \"url\" \"mime\" from \"media\" where id = @ID");
	sqlite3_prepare_v2(ctx->db, sql, size, &statement, NULL);

	int index = sqlite3_bind_parameter_index(statement, "@ID");
	sqlite3_bind_int(statement, index, ctx->mediaid);

	const char *url = NULL;
	const char *info = NULL;
	const char *mime = NULL;
	int sqlret = sqlite3_step(statement);
	if (sqlret == SQLITE_ROW)
	{
		url = (const char *)sqlite3_column_text(statement, 0);
		mime = (const char *)sqlite3_column_text(statement, 1);
	}
	if (url != NULL)
	{
		ret = play(data, url, info, mime);
	}
	sqlite3_finalize(statement);
	sqlite3_free(sql);
	return ret;
}

int media_next(media_ctx_t *ctx)
{
	sqlite3_stmt *statement;
	int size = 256;
	char *sql = sqlite3_malloc(size);
	if (ctx->mediaid != 0)
	{
		snprintf(sql, size, "select \"id\" from \"media\" where id > @ID");
		sqlite3_prepare_v2(ctx->db, sql, size, &statement, NULL);

		int index = sqlite3_bind_parameter_index(statement, "@ID");
		sqlite3_bind_int(statement, index, ctx->mediaid);
	}
	else
	{
		snprintf(sql, size, "select \"id\" from \"media\"");
		sqlite3_prepare_v2(ctx->db, sql, size, &statement, NULL);
	}

	int ret = sqlite3_step(statement);
	if (ret == SQLITE_ROW)
	{
		ctx->mediaid = sqlite3_column_int(statement, 0);
	}
	else
		ctx->mediaid = -1;
	sqlite3_finalize(statement);
	sqlite3_free(sql);
	return ctx->mediaid;
}

media_ctx_t *media_init(const char *dbpath)
{
	media_ctx_t *ctx = NULL;
	if (dbpath)
	{
		int ret;
		sqlite3 *db;
		if (!access(dbpath, R_OK|W_OK))
		{
			ret = sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READWRITE, NULL);
		}
		else if (!access(dbpath, R_OK))
		{
			ret = sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL);
		}
		else
		{
			ret = sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, NULL);

			const char *query[] = {
				"create table media (\"id\" INTEGER PRIMARY KEY, \"url\" TEXT UNIQUE NOT NULL, \"mime\" TEXT, \"info\" BLOB);",
				NULL,
			};
			char *error = NULL;
			int i = 0;
			while (query[i] != NULL)
			{
				if (ret != SQLITE_OK)
				{
					break;
				}
				ret = sqlite3_exec(db, query[i], NULL, NULL, &error);
				i++;
			}
		}
		if (ret == SQLITE_OK)
		{
			dbg("open db %s", dbpath);
			ctx = calloc(1, sizeof(*ctx));
			ctx->db = db;
			ctx->mediaid = 0;
		}
	}
	return ctx;
}

void media_destroy(media_ctx_t *ctx)
{
	if (ctx->db)
		sqlite3_close_v2(ctx->db);
	free(ctx);
}
