/*********************************************************************
 * config.(c/h)
 * - a configuration parser for epproxy (using libyaml)
 *********************************************************************/
#include <stdlib.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <yaml.h>
/*---------------------------------------------------------------------------*/
#include "config.h"
#include "l4proxy.h"
/*---------------------------------------------------------------------------*/
#define PRINT_CONF_DEBUG
/*---------------------------------------------------------------------------*/
#define MAX_DEPTH 4
/*---------------------------------------------------------------------------*/
enum
{
	NONE,
	KEY,
	VALUE
};
/*---------------------------------------------------------------------------*/
enum
{
	UNKNOWN,
	PROXY,
	LISTEN,
	BALANCE,
	BACKEND,
	CONN_POOL,
	PERSIST,
	NAME,
	ADDR,
	WEIGHT
};
const char *const config_opts[] = {"", "proxy", "listen", "balance",
								   "backend", "conn_pool", "persist",
								   "name", "addr", "weight"};
/*---------------------------------------------------------------------------*/
const char *const persist_opts[] = {"",
									"set-cookie",
									"append-cookie",
									"learn-cookie"};
/*---------------------------------------------------------------------------*/
inline int
ParseKey(const char *key)
{
	int i;
	for (i = 1; i < (sizeof(config_opts) / sizeof(const char *)); i++)
		if (!strcmp(key, config_opts[i]))
			return i;

	return UNKNOWN;
}
/*---------------------------------------------------------------------------*/
inline int
ParseProxyValue(int *parse_target, int depth,
				struct proxy_context *cur_pctx, const char *value)
{
	char ip[MAX_IPADDR_STRLEN];
	uint16_t port;
	struct sockaddr_in saddr;

	switch (parse_target[depth])
	{

	case LISTEN:
		if (cur_pctx->listen_done)
			return -1;
		if (sscanf(value, "%[0-9.]:%hu", ip, &port) != 2)
			return -1;

		memset(&(cur_pctx->listen_addr), 0, sizeof(saddr));
		cur_pctx->listen_addr.sin_family = AF_INET;
		cur_pctx->listen_addr.sin_addr.s_addr = inet_addr(ip);
		cur_pctx->listen_addr.sin_port = htons(port);
		cur_pctx->listen_done = 1;
		break;

	case CONN_POOL:
		cur_pctx->conn_per_backend = atoi(value);
		cur_pctx->conn_pool_done = 1;
		break;
	}

	return 0;
}
/*---------------------------------------------------------------------------*/
void ValidateProxyConfig(struct proxy_context *pconf)
{

	int ret = 0;

	if (!pconf->listen_done)
	{
		printf("There are missing option(s) in config file: listen\n");
		ret = -1;
	}
	if (pconf->backend_num == 0)
	{
		printf("There are missing option(s) in config file: backend\n");
		ret = -1;
	}
	if (!pconf->conn_pool_done)
	{
		/* if the conn_pool parameter is missing, use 0 as a default value */
		pconf->conn_per_backend = 0;
	}

	if (ret == -1)
		exit(-1);
}
/*---------------------------------------------------------------------------*/
inline int
ParseBackendValue(int *parse_target, int depth,
				  struct backend_info *bconf, const char *value)
{

	char ip[MAX_IPADDR_STRLEN];
	uint16_t port;

	switch (parse_target[depth])
	{

	case NAME:
		if (bconf->name_done)
			return -1;
		if (strlen(value) > MAX_SERVER_NAME_LEN)
			return -1;
		if ((bconf->name = strdup(value)) == NULL)
			return -1;
		bconf->name_done = 1;
		break;

	case ADDR:
		if (bconf->addr_done)
			return -1;
		if (sscanf(value, "%[0-9.]:%hu", ip, &port) != 2)
			return -1;
		bconf->addr.sin_family = AF_INET;
		bconf->addr.sin_addr.s_addr = (inet_addr(ip)) ? inet_addr(ip) : INADDR_ANY;
		bconf->addr.sin_port = htons(port);
		bconf->addr_done = 1;
		break;
	}

	return 0;
}
/*---------------------------------------------------------------------------*/
void ValidateBackendConfig(struct backend_info *bconf)
{

	int ret = 0;

	if (!bconf->addr_done)
	{
		printf("There are missing option(s) in a backend entry: addr\n");
		ret = -1;
	}
	if (!bconf->name_done)
	{
		printf("There are missing option(s) in a backend entry: name\n");
		ret = -1;
	}

	if (ret == -1)
		exit(-1);
}
/*---------------------------------------------------------------------------*/
struct proxy_context *
LoadConfigData(const char *fname)
{
	FILE *file;
	yaml_parser_t parser;
	yaml_token_t token;
	int done = 0, depth = 0, type = 0;
	int parse_target[MAX_DEPTH] = {
		UNKNOWN,
	};

	struct proxy_context *prx_ctx = NULL;

	fflush(stdout);

	printf("scanning %s..\n", fname);
	if ((file = fopen(fname, "rb")) < 0)
	{
		printf("fopen() error\n");
		exit(-1);
	}

	if (!yaml_parser_initialize(&parser))
	{
		printf("yaml_parser_initialize() error\n");
		exit(-1);
	}
	yaml_parser_set_input_file(&parser, file);

	while (!done)
	{
		if (!yaml_parser_scan(&parser, &token))
		{
			printf("yaml_parser_set_input_file() error\n");
			exit(-1);
		}
		if (token.type == YAML_STREAM_END_TOKEN)
			done = 1;
		else if (token.type == YAML_KEY_TOKEN)
			type = KEY;
		else if (token.type == YAML_VALUE_TOKEN)
			type = VALUE;
		else if (token.type == YAML_SCALAR_TOKEN && type == KEY)
		{
			if (depth < 0 || depth >= MAX_DEPTH)
			{
				printf("wrong formatting in %s\n", fname);
				exit(-1);
			}
			parse_target[depth] = ParseKey((const char *)token.data.scalar.value);
			type = NONE;
		}
		else if (token.type == YAML_SCALAR_TOKEN && type == VALUE)
		{
			if (depth == 1 && parse_target[depth - 1] == PROXY)
			{
				if (!prx_ctx)
				{
					printf("wrong formatting in %s\n", fname);
					exit(-1);
				}
				if (ParseProxyValue(parse_target, depth, prx_ctx,
									(const char *)token.data.scalar.value) < 0)
				{
					printf("duplicate/wrong value in the config file "
						   "(key: %s, value: %s)\n",
						   config_opts[parse_target[depth]],
						   token.data.scalar.value);
					exit(-1);
				}
			}
			else if (depth == 2 && parse_target[depth - 1] == BACKEND)
			{
				if (!prx_ctx)
				{
					printf("wrong formatting in %s\n", fname);
					exit(-1);
				}
				if (ParseBackendValue(parse_target, depth, &prx_ctx->backend,
									  (const char *)token.data.scalar.value) < 0)
				{
					printf("duplicate/wrong value in the config file "
						   "(key: %s, value: %s)\n",
						   config_opts[parse_target[depth]],
						   token.data.scalar.value);
					exit(-1);
				}
			}
			else
			{
				/* value outside the block -> wrong config file */
				printf("wrong formatting in %s\n", fname);
				printf("depth = %d, parse_target = %s\n",
					   depth, config_opts[parse_target[depth]]);
				exit(-1);
			}

			type = NONE;
		}
		else if (token.type == YAML_FLOW_MAPPING_START_TOKEN ||
				 token.type == YAML_FLOW_SEQUENCE_START_TOKEN)
		{
			if (depth == 0 && parse_target[depth] == PROXY)
			{
				if (prx_ctx != NULL)
				{
					printf("we do not support more than 1 proxies for now\n");
					exit(-1);
				}
				/* initialize proxy context */
				prx_ctx = calloc(1, sizeof(struct proxy_context));
				if (!prx_ctx)
				{
					printf("calloc() error\n");
					exit(-1);
				}
			}
			else if (depth == 1 && parse_target[depth] == BACKEND)
			{
				/* grouped value outside the proxy block -> wrong config file */
				if (!prx_ctx)
				{
					printf("wrong formatting in %s\n", fname);
					exit(-1);
				}
				/* initialize backend entry */
				memset(&prx_ctx->backend, 0, sizeof(prx_ctx->backend));
			}
			else
			{
				printf("wrong formatting in %s\n", fname);
				exit(-1);
			}
			depth++;
			type = NONE;
		}
		else if (token.type == YAML_FLOW_MAPPING_END_TOKEN ||
				 token.type == YAML_FLOW_SEQUENCE_END_TOKEN)
		{
			depth--;
			if (depth == 0 && parse_target[depth] == PROXY)
			{
				/* verify proxy context */
				ValidateProxyConfig(prx_ctx);
			}
			else if (depth == 1 && parse_target[depth] == BACKEND)
			{
				/* validate backend entry */
				ValidateBackendConfig(&prx_ctx->backend);
				prx_ctx->backend_num++;
			}
			else
			{
				printf("wrong formatting in %s\n", fname);
				exit(-1);
			}
			type = NONE;
		}
		yaml_token_delete(&token);
	}

	yaml_parser_delete(&parser);

	if (fclose(file) < 0)
	{
		printf("fclose() error\n");
		exit(-1);
	}

	printf("finished parsing %s!\n", fname);

	return prx_ctx;
}
/*----------------------------------------------------------------------------*/
