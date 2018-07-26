#ifndef __MEDIA_H__
#define __MEDIA_H__

typedef struct media_ctx_s media_ctx_t;

typedef int (*play_fcn_t)(void *arg, const char *url, const char *info, const char *mime);

typedef struct media_ops_s media_ops_t;
struct media_ops_s
{
	media_ctx_t *(*init)();
	void (*destroy)(media_ctx_t *ctx);

	int (*count)(media_ctx_t *ctx);
	int (*insert)(media_ctx_t *ctx, const char *path, const char *info, const char *mime);
	int (*find)(media_ctx_t *ctx, int id, char *url, int *urllen, char *info, int *infolen);
	int (*current)(media_ctx_t *ctx, char *url, int *urllen, char *info, int *infolen);
	int (*play)(media_ctx_t *ctx, play_fcn_t play, void *data);
	int (*next)(media_ctx_t *ctx);
};

typedef struct media_s media_t;
struct media_s
{
	media_ops_t *ops;
	media_ctx_t *ctx;
};

extern media_ops_t *media_sqlite;
#endif
