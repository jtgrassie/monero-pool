/*
  Copyright (c) 2014-2018, The Monero Project

  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification, are
  permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this list of
     conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice, this list
     of conditions and the following disclaimer in the documentation and/or other
     materials provided with the distribution.

  3. Neither the name of the copyright holder nor the names of its contributors may be
     used to endorse or promote products derived from this software without specific
     prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
  THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
  THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  Parts of the project are originally copyright (c) 2012-2013 The Cryptonote developers
*/

#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http.h>

#include <lmdb.h>

#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <math.h>
#include <uuid/uuid.h>
#include <getopt.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <json-c/json.h>
#include <openssl/bn.h>
#include <pthread.h>

#include "util.h"
#include "xmr.h"
#include "log.h"
#include "webui.h"

#define MAX_LINE 4096
#define POOL_CLIENTS_GROW 1024
#define RPC_BODY_MAX 4096
#define CLIENT_BODY_MAX 2048
#define CLIENT_JOBS_MAX 4
#define BLOCK_HEADERS_MAX 4
#define BLOCK_TEMPLATES_MAX 4
#define MAINNET_ADDRESS_PREFIX 18
#define TESTNET_ADDRESS_PREFIX 53
#define BLOCK_HEADERS_RANGE 10
#define DB_SIZE 0x140000000 /* 5G */
#define DB_COUNT_MAX 10
#define MAX_PATH 1024
#define RPC_PATH "/json_rpc"
#define ADDRESS_MAX 128
#define BLOCK_TIME 120
#define HR_BLOCK_COUNT 5

#define uint128_t unsigned __int128

/*
  A block is initially locked.
  After height + 60 blocks, we check to see if its on the chain.
  If not, it becomes orphaned, otherwise we unlock it for payouts.
*/

/*
  Tables:

  Shares
  ------
  height <-> share_t

  Blocks
  ------
  height <-> block_t

  Balance
  -------
  wallet addr <-> balance

  Payments
  --------
  wallet addr <-> payment_t
*/

enum block_status { BLOCK_LOCKED=0, BLOCK_UNLOCKED=1, BLOCK_ORPHANED=2 };

typedef struct config_t
{
    char rpc_host[256];
    uint32_t rpc_port;
    uint32_t rpc_timeout;
    char wallet_rpc_host[256];
    uint32_t wallet_rpc_port;
    char pool_wallet[ADDRESS_MAX];
    uint32_t pool_start_diff;
    float share_mul;
    float pool_fee;
    float payment_threshold;
    uint32_t pool_port;
    uint32_t log_level;
    uint32_t webui_port;
    char log_file[MAX_PATH];
} config_t;

typedef struct block_template_t
{
    char *blockhashing_blob;
    char *blocktemplate_blob;
    uint64_t difficulty;
    uint64_t height;
    char prev_hash[64];
    uint32_t reserved_offset;
} block_template_t;

typedef struct job_t
{
    uuid_t id;
    char *blob;
    block_template_t *block_template;
    uint32_t extra_nonce;
    uint64_t target;
    uint128_t *submissions;
    size_t submissions_count;
} job_t;

typedef struct client_t
{
    int fd;
    int json_id;
    struct bufferevent *bev;
    char address[ADDRESS_MAX];
    char worker_id[64];
    char client_id[32];
    char agent[256];
    job_t active_jobs[CLIENT_JOBS_MAX];
    uint64_t hashes;
    time_t connected_since;
    bool is_proxy;
} client_t;

typedef struct pool_clients_t
{
    client_t *clients;
    size_t count;
} pool_clients_t;

typedef struct share_t 
{
    uint64_t height;
    uint64_t difficulty;
    char address[ADDRESS_MAX];
    time_t timestamp;
} share_t;

typedef struct block_t 
{
    uint64_t height;
    char hash[64];
    char prev_hash[64];
    uint64_t difficulty;
    uint32_t status;
    uint64_t reward;
    time_t timestamp;
} block_t;

typedef struct payment_t
{
    char tx_hash[64];
    uint64_t amount;
    time_t timestamp;
} payment_t;

typedef struct rpc_callback_t
{
    void (*cb)(const char*, struct rpc_callback_t*);
    void *data;
} rpc_callback_t;

static int database_init();
static void database_close();
static int store_share(uint64_t height, share_t *share);
static int store_block(uint64_t height, block_t *block);
static int process_blocks(block_t *blocks, size_t count);
static int payout_block(block_t *block, MDB_txn *parent);
static int balance_add(const char *address, uint64_t amount, MDB_txn *parent);
static int send_payments();
static int startup_pauout(uint64_t height);
static void update_pool_hr();
static void block_template_free(block_template_t *block_template);
static void block_templates_free();
static void last_block_headers_free();
static void pool_clients_init();
static void pool_clients_free();
static void pool_clients_send_job();
static void target_to_hex(uint64_t target, char *target_hex);
static char * stratum_new_proxy_job_body(int json_id, const char *client_id, const char *job_id,
        const block_template_t *block_template, const char *template_blob,
        uint64_t target, bool response);
static char * stratum_new_job_body(int json_id, const char *client_id, const char *job_id,
        const char *blob, uint64_t target, uint64_t height, bool response);
static char * stratum_new_error_body(int json_id, const char *error);
static char * stratum_new_status_body(int json_id, const char *status);
static void client_add(int fd, struct bufferevent *bev);
static void client_find(struct bufferevent *bev, client_t **client);
static void client_clear(struct bufferevent *bev);
static void client_send_job(client_t *client, bool response);
static void client_clear_jobs(client_t *client);
static job_t * client_find_job(client_t *client, const char *job_id);
static void response_to_block_template(json_object *result, block_template_t *block_template);
static void response_to_block(json_object *result, block_t *block);
static char * rpc_new_request_body(const char* method, char* fmt, ...);
static void rpc_on_response(struct evhttp_request *req, void *arg);
static void rpc_request(struct event_base *base, const char *body, rpc_callback_t *callback);
static void rpc_wallet_request(struct event_base *base, const char *body, rpc_callback_t *callback);
static void rpc_on_block_template(const char* data, rpc_callback_t *callback);
static void rpc_on_block_headers_range(const char* data, rpc_callback_t *callback);
static void rpc_on_block_header_by_height(const char* data, rpc_callback_t *callback);
static void rpc_on_last_block_header(const char* data, rpc_callback_t *callback);
static void rpc_on_block_submitted(const char* data, rpc_callback_t *callback);
static void rpc_on_wallet_transferred(const char* data, rpc_callback_t *callback);
static void timer_on_120s(int fd, short kind, void *ctx);
static void timer_on_10m(int fd, short kind, void *ctx);
static void client_on_login(json_object *message, client_t *client);
static void client_on_submit(json_object *message, client_t *client);
static void client_on_read(struct bufferevent *bev, void *ctx);
static void client_on_error(struct bufferevent *bev, short error, void *ctx);
static void client_on_accept(evutil_socket_t listener, short event, void *arg);
static void send_validation_error(const client_t *client, const char *message);

static config_t config;
static pool_clients_t pool_clients;
static block_t *last_block_headers[BLOCK_HEADERS_MAX];
static block_template_t *block_templates[BLOCK_TEMPLATES_MAX];
static struct event_base *base;
static struct event *timer_120s;
static struct event *timer_10m;
static uint32_t extra_nonce;
static block_t block_headers_range[BLOCK_HEADERS_RANGE];
static MDB_env *env;
static MDB_dbi db_shares;
static MDB_dbi db_blocks;
static MDB_dbi db_balance;
static MDB_dbi db_payments;
static BN_CTX *bn_ctx;
static BIGNUM *base_diff;
static pool_stats_t pool_stats;
static pthread_mutex_t mutex_clients = PTHREAD_MUTEX_INITIALIZER;
static FILE *fd_log;

#define JSON_GET_OR_ERROR(name, parent, type, client)                \
    json_object *name = NULL;                                        \
    if (!json_object_object_get_ex(parent, #name, &name))            \
        return send_validation_error(client, #name " not found");    \
    if (!json_object_is_type(name, type))                            \
        return send_validation_error(client, #name " not a " #type);

static int
compare_uint64(const MDB_val *a, const MDB_val *b)
{
    const uint64_t va = *(const uint64_t *)a->mv_data;
    const uint64_t vb = *(const uint64_t *)b->mv_data;
    return (va < vb) ? -1 : va > vb;
}

static int
compare_string(const MDB_val *a, const MDB_val *b)
{
    const char *va = (const char*) a->mv_data;
    const char *vb = (const char*) b->mv_data;
    return strcmp(va, vb);
}

static int
compare_block(const MDB_val *a, const MDB_val *b)
{
    const block_t *va = (const block_t*) a->mv_data;
    const block_t *vb = (const block_t*) b->mv_data;
    int sc = memcmp(va->hash, vb->hash, 64);
    if (sc == 0)
        return (va->timestamp < vb->timestamp) ? -1 : va->timestamp > vb->timestamp;
    else
        return sc;
}

static int
compare_share(const MDB_val *a, const MDB_val *b)
{
    const share_t *va = (const share_t*) a->mv_data;
    const share_t *vb = (const share_t*) b->mv_data;
    int sc = strcmp(va->address, vb->address);
    if (sc == 0)
        return (va->timestamp < vb->timestamp) ? -1 : va->timestamp > vb->timestamp;
    else
        return sc;
}

static int
compare_payment(const MDB_val *a, const MDB_val *b)
{
    const payment_t *va = (const payment_t*) a->mv_data;
    const payment_t *vb = (const payment_t*) b->mv_data;
    return (va->timestamp < vb->timestamp) ? -1 : va->timestamp > vb->timestamp;
}

static int
database_init()
{
    int rc;
    char *err;
    MDB_txn *txn;

    rc = mdb_env_create(&env);
    mdb_env_set_maxdbs(env, (MDB_dbi) DB_COUNT_MAX);
    mdb_env_set_mapsize(env, DB_SIZE);
    if ((rc = mdb_env_open(env, "./data", 0, 0664)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s\n", err);
        exit(rc);
    }
    if ((rc = mdb_txn_begin(env, NULL, 0, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s\n", err);
        exit(rc);
    }
    if ((rc = mdb_dbi_open(txn, "shares", MDB_INTEGERKEY | MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED, &db_shares)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s\n", err);
        exit(rc);
    }
    if ((rc = mdb_dbi_open(txn, "blocks", MDB_INTEGERKEY | MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED, &db_blocks)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s\n", err);
        exit(rc);
    }
    if ((rc = mdb_dbi_open(txn, "payments", MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED, &db_payments)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s\n", err);
        exit(rc);
    }
    if ((rc = mdb_dbi_open(txn, "balance", MDB_CREATE, &db_balance)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s\n", err);
        exit(rc);
    }

    mdb_set_compare(txn, db_shares, compare_uint64);
    mdb_set_dupsort(txn, db_shares, compare_share);

    mdb_set_compare(txn, db_blocks, compare_uint64);
    mdb_set_dupsort(txn, db_blocks, compare_block);
    
    mdb_set_compare(txn, db_payments, compare_string);
    mdb_set_dupsort(txn, db_payments, compare_payment);

    mdb_set_compare(txn, db_balance, compare_string);

    rc = mdb_txn_commit(txn);
    return rc;
}

static void
database_close()
{
    log_info("Closing database");
    mdb_dbi_close(env, db_shares);
    mdb_dbi_close(env, db_blocks);
    mdb_dbi_close(env, db_balance);
    mdb_dbi_close(env, db_payments);
    return mdb_env_close(env);
}

static int
store_share(uint64_t height, share_t *share)
{
    int rc;
    char *err;
    MDB_txn *txn;
    MDB_cursor *cursor;
    if ((rc = mdb_txn_begin(env, NULL, 0, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return rc;
    }
    if ((rc = mdb_cursor_open(txn, db_shares, &cursor)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }

    MDB_val key = { sizeof(height), (void*)&height };
    MDB_val val = { sizeof(share_t), (void*)share };
    mdb_cursor_put(cursor, &key, &val, MDB_APPENDDUP);

    rc = mdb_txn_commit(txn);
    return rc;
}

static int
store_block(uint64_t height, block_t *block)
{
    int rc;
    char *err;
    MDB_txn *txn;
    MDB_cursor *cursor;
    if ((rc = mdb_txn_begin(env, NULL, 0, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return rc;
    }
    if ((rc = mdb_cursor_open(txn, db_blocks, &cursor)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }

    MDB_val key = { sizeof(height), (void*)&height };
    MDB_val val = { sizeof(block_t), (void*)block };
    mdb_cursor_put(cursor, &key, &val, MDB_APPENDDUP);

    rc = mdb_txn_commit(txn);
    return rc;
}

uint64_t
miner_hr(const char *address)
{
    pthread_mutex_lock(&mutex_clients);
    client_t *c = pool_clients.clients;
    uint64_t hr = 0;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        if (c->connected_since != 0 && strncmp(c->address, address, ADDRESS_MAX) == 0)
        {
            double d = difftime(time(NULL), c->connected_since);
            if (d == 0.0)
                continue;
            hr += c->hashes / d;
            continue;
        }
    }
    pthread_mutex_unlock(&mutex_clients);
    return hr;
}

uint64_t
miner_balance(const char *address)
{
    if (strlen(address) > ADDRESS_MAX)
        return 0;
    int rc;
    char *err;
    MDB_txn *txn;
    MDB_cursor *cursor;
    if ((rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return 0;
    }
    if ((rc = mdb_cursor_open(txn, db_balance, &cursor)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return 0;
    }

    MDB_val key = {ADDRESS_MAX, (void*)address};
    MDB_val val;
    uint64_t balance  = 0;

    rc = mdb_cursor_get(cursor, &key, &val, MDB_SET);
    if (rc != 0 && rc != MDB_NOTFOUND)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        goto cleanup;
    }
    if (rc != 0)
        goto cleanup;

    balance = *(uint64_t*)val.mv_data;

cleanup:
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    return balance;
}

static int
process_blocks(block_t *blocks, size_t count)
{
    log_debug("Processing blocks");
    /*
      For each block, lookup block in db.
      If found, make sure found is locked and not orphaned.
      If both not orphaned and unlocked, payout, set unlocked.
      If block heights differ / orphaned, set orphaned.
    */
    int rc;
    char *err;
    MDB_txn *txn;
    MDB_cursor *cursor;
    if ((rc = mdb_txn_begin(env, NULL, 0, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return rc;
    }
    if ((rc = mdb_cursor_open(txn, db_blocks, &cursor)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }

    for (int i=0; i<count; i++)
    {
        block_t *ib = &blocks[i];
        log_trace("Processing block at height %"PRIu64, ib->height);
        MDB_val key = { sizeof(ib->height), (void*)&ib->height };
        MDB_val val;
        MDB_cursor_op op = MDB_SET;
        while (1)
        {
            rc = mdb_cursor_get(cursor, &key, &val, op);
            op = MDB_NEXT_DUP;
            if (rc == MDB_NOTFOUND || rc != 0)
            {
                log_trace("No stored block at height %"PRIu64, ib->height);
                if (rc != MDB_NOTFOUND)
                {
                    err = mdb_strerror(rc);
                    log_debug("No stored block at height %"PRIu64" with error: %d", ib->height, err);
                }
                break;
            }
            block_t *sb = (block_t*)val.mv_data;
            if (sb->status != BLOCK_LOCKED)
            {
                continue;
            }
            if (ib->status & BLOCK_ORPHANED)
            {
                log_debug("Orphaned block at height %"PRIu64, ib->height);
                block_t bp;
                memcpy(&bp, sb, sizeof(block_t));
                bp.status |= BLOCK_ORPHANED;
                MDB_val new_val = {sizeof(block_t), (void*)&bp};
                mdb_cursor_put(cursor, &key, &new_val, MDB_CURRENT);
                continue;
            }
            if (memcmp(ib->hash, sb->hash, 64) == 0 && memcmp(ib->prev_hash, sb->prev_hash, 64) != 0)
            {
                log_warn("Have a block with matching heights but differing parents! Setting orphaned.\n");
                block_t bp;
                memcpy(&bp, sb, sizeof(block_t));
                bp.status |= BLOCK_ORPHANED;
                MDB_val new_val = {sizeof(block_t), (void*)&bp};
                mdb_cursor_put(cursor, &key, &new_val, MDB_CURRENT);
                continue;
            }
            block_t bp;
            memcpy(&bp, sb, sizeof(block_t));
            bp.status |= BLOCK_UNLOCKED;
            bp.reward = ib->reward;
            rc = payout_block(&bp, txn);
            if (rc == 0)
            {
                log_debug("Paided out block %"PRIu64, bp.height);
                MDB_val new_val = {sizeof(block_t), (void*)&bp};
                mdb_cursor_put(cursor, &key, &new_val, MDB_CURRENT);
            }
            else
                log_trace("%s", mdb_strerror(rc));
        }
    }

    rc = mdb_txn_commit(txn);
    return rc;
}

static int
payout_block(block_t *block, MDB_txn *parent)
{
    /*
      PPLNS
    */
    log_info("Payout on block at height %"PRIu64, block->height);
    int rc;
    char *err;
    MDB_txn *txn;
    MDB_cursor *cursor;
    uint64_t height = block->height;
    uint64_t total_paid = 0;
    if ((rc = mdb_txn_begin(env, parent, 0, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return rc;
    }
    if ((rc = mdb_cursor_open(txn, db_shares, &cursor)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }

    MDB_cursor_op op = MDB_SET;
    while (1)
    {
        uint64_t current_height = height;
        MDB_val key = { sizeof(current_height), (void*)&current_height };
        MDB_val val;
        rc = mdb_cursor_get(cursor, &key, &val, op);
        op = MDB_NEXT_DUP;
        if (rc == MDB_NOTFOUND && total_paid < block->reward)
        {
            if (height == 0)
                break;
            height--;
            op = MDB_SET;
            continue;
        }
        if (rc != 0 && rc != MDB_NOTFOUND)
        {
            log_error("Error getting balance: %s", mdb_strerror(rc));
            break;
        }
        if (total_paid == block->reward)
            break;

        share_t *share = (share_t*)val.mv_data;
        uint64_t amount = floor((double)share->difficulty / ((double)block->difficulty * config.share_mul) * block->reward);
        if (total_paid + amount > block->reward)
            amount = block->reward - total_paid;
        total_paid += amount;
        uint64_t fee = amount * config.pool_fee;
        amount -= fee;
        if (amount == 0)
            continue;
        rc = balance_add(share->address, amount, txn);
        if (rc != 0)
        {
            mdb_cursor_close(cursor);
            mdb_txn_abort(txn);
            return rc;
        }
    }

    rc = mdb_txn_commit(txn);
    return rc;
}

static int
balance_add(const char *address, uint64_t amount, MDB_txn *parent)
{
    log_trace("Adding %"PRIu64" to %s's balance", amount, address);
    int rc;
    char *err;
    MDB_txn *txn;
    MDB_cursor *cursor;
    if ((rc = mdb_txn_begin(env, parent, 0, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return rc;
    }
    if ((rc = mdb_cursor_open(txn, db_balance, &cursor)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }

    MDB_val key = {ADDRESS_MAX, (void*)address};
    MDB_val val;
    rc = mdb_cursor_get(cursor, &key, &val, MDB_SET);
    if (rc == MDB_NOTFOUND)
    {
        log_trace("Adding new balance entry");
        MDB_val new_val = { sizeof(amount), (void*)&amount };
        mdb_cursor_put(cursor, &key, &new_val, MDB_APPEND);
    }
    else if (rc == 0)
    {
        log_trace("Updating existing balance entry");
        uint64_t current_amount = *(uint64_t*)val.mv_data;
        current_amount += amount;
        MDB_val new_val = {sizeof(current_amount), (void*)&current_amount};
        rc = mdb_cursor_put(cursor, &key, &new_val, MDB_CURRENT);
        if (rc != 0)
        {
            err = mdb_strerror(rc);
            log_error("%s", err);
        }
    }
    else
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }

    rc = mdb_txn_commit(txn);
    return rc;
}

static int
send_payments()
{
    uint64_t threshold = 1000000000000 * config.payment_threshold;
    int rc;
    char *err;
    MDB_txn *txn;
    MDB_cursor *cursor;
    if ((rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return rc;
    }
    if ((rc = mdb_cursor_open(txn, db_balance, &cursor)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }

    MDB_cursor_op op = MDB_FIRST;
    while (1)
    {
        MDB_val key;
        MDB_val val;
        rc = mdb_cursor_get(cursor, &key, &val, op);
        op = MDB_NEXT;
        if (rc != 0)
            break;

        const char *address = (const char*)key.mv_data;
        uint64_t amount = *(uint64_t*)val.mv_data;

        if (amount < threshold)
            continue;

        log_info("Sending payment of %"PRIu64" to %s\n", amount, address);

        char body[RPC_BODY_MAX];
        snprintf(body, RPC_BODY_MAX, "{\"id\":\"0\",\"jsonrpc\":\"2.0\",\"method\":\"transfer\",\"params\":{"
                "\"destinations\":[{\"amount\":%"PRIu64",\"address\":\"%s\"}],\"mixin\":10}}",
                amount, address);
        log_trace(body);
        rpc_callback_t *callback = calloc(1, sizeof(rpc_callback_t));
        callback->data = calloc(ADDRESS_MAX, sizeof(char));
        memcpy(callback->data, address, ADDRESS_MAX);
        callback->cb = rpc_on_wallet_transferred;
        rpc_wallet_request(base, body, callback);
    }
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    return 0;
}

static int
startup_pauout(uint64_t height)
{
    /*
      Loop stored blocks < height - 60
      If block locked & not orphaned, payout
    */
    int rc;
    char *err;
    MDB_txn *txn;
    MDB_cursor *cursor;
    if ((rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return rc;
    }
    if ((rc = mdb_cursor_open(txn, db_blocks, &cursor)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }

    pool_stats.pool_blocks_found = 0;
    MDB_cursor_op op = MDB_FIRST;
    while (1)
    {
        MDB_val key;
        MDB_val val;
        rc = mdb_cursor_get(cursor, &key, &val, op);
        op = MDB_NEXT;
        if (rc != 0 && rc != MDB_NOTFOUND)
        {
            err = mdb_strerror(rc);
            log_error("%s", err);
            break;
        }
        if (rc == MDB_NOTFOUND)
            break;

        pool_stats.pool_blocks_found++;

        block_t *block = (block_t*)val.mv_data;
        pool_stats.last_block_found = block->timestamp;

        if (block->height > height - 60)
            break;
        if (block->status & BLOCK_UNLOCKED || block->status & BLOCK_ORPHANED)
            continue;

        char *body = rpc_new_request_body("get_block_header_by_height", "sd", "height", block->height);
        rpc_callback_t *c = calloc(1, sizeof(rpc_callback_t));
        c->cb = rpc_on_block_header_by_height;
        rpc_request(base, body, c);
        free(body);
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    return 0;
}

static void
update_pool_hr()
{
    uint64_t hr = 0;
    client_t *c = pool_clients.clients;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        if (c->connected_since != 0)
        {
            double d = difftime(time(NULL), c->connected_since);
            if (d == 0.0)
                continue;
            hr += c->hashes / d;
        }
    }
    pool_stats.pool_hashrate = hr;
}

static void
block_template_free(block_template_t *block_template)
{
    free(block_template->blockhashing_blob);
    free(block_template->blocktemplate_blob);
    free(block_template);
}

static void
block_templates_free()
{
    size_t length = sizeof(last_block_headers)/sizeof(*last_block_headers);
    for (size_t i=0; i<length; i++)
    {
        block_template_t *bt = block_templates[i];
        if (bt != NULL)
        {
            free(bt->blockhashing_blob);
            free(bt->blocktemplate_blob);
            free(bt);
        }
    }
}

static void
last_block_headers_free()
{
    size_t length = sizeof(last_block_headers)/sizeof(*last_block_headers);
    for (size_t i=0; i<length; i++)
    {
        block_t *block = last_block_headers[i];
        if (block != NULL)
            free(block);
    }
}

static void
pool_clients_init()
{
    assert(pool_clients.count == 0);
    pool_clients.count = POOL_CLIENTS_GROW;
    pool_clients.clients = (client_t*) calloc(pool_clients.count, sizeof(client_t));
}

static void
pool_clients_free()
{
    assert(pool_clients.count != 0);
    client_t *c = pool_clients.clients;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        client_clear_jobs(c);
    }
    free(pool_clients.clients);
}

static job_t *
client_find_job(client_t *client, const char *job_id)
{
    char jid[33];
    for (size_t i=0; i<CLIENT_JOBS_MAX; i++)
    {
        memset(jid, 0, 33);
        job_t *job = &client->active_jobs[i];
        bin_to_hex((const char*)job->id, sizeof(uuid_t), jid);
        jid[32] = '\0';
        if (strcmp(job_id, jid) == 0)
            return job;
    }
    return NULL;
}

static void
pool_clients_send_job()
{
    client_t *c = pool_clients.clients;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        if (c->fd == 0)
            continue;
        client_send_job(c, false);
    }
}

static void
target_to_hex(uint64_t target, char *target_hex)
{
    if (target & 0xFFFFFFFF00000000)
    {
        log_warn("Target too high: %"PRIu64, target);
    }
    BIGNUM *diff = BN_new();
    BIGNUM *bnt = BN_new();
#ifdef SIXTY_FOUR_BIT_LONG
    BN_set_word(bnt, target);
#else
    char st[24];
    snprintf(st, 24, "%"PRIu64, target);
    BN_dec2bn(&bnt, st);
#endif
    BN_div(diff, NULL, base_diff, bnt, bn_ctx);
    BN_rshift(diff, diff, 224);
    uint32_t w = BN_get_word(diff);
    bin_to_hex((const char*)&w, 4, &target_hex[0]);
    BN_free(bnt);
    BN_free(diff);
}

static char *
stratum_new_proxy_job_body(int json_id, const char *client_id, const char *job_id,
        const block_template_t *block_template, const char *template_blob,
        uint64_t target, bool response)
{
    char *body = calloc(CLIENT_BODY_MAX, sizeof(char));

    char target_hex[9];
    target_to_hex(target, &target_hex[0]);

    const block_template_t *bt = block_template;

    if (response)
    {
        snprintf(body, CLIENT_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\",\"error\":null,\"result\""
                ":{\"id\":\"%.32s\",\"job\":{\"blocktemplate_blob\":\"%s\",\"job_id\":\"%.32s\","
                "\"difficulty\":%"PRIu64",\"height\":%"PRIu64",\"reserved_offset\":%u,\"client_nonce_offset\":%u,"
                "\"client_pool_offset\":%u,\"target_diff\":%"PRIu64",\"target_diff_hex\":\"%.8s\"},"
                "\"status\":\"OK\"}}\n", json_id, client_id, template_blob, job_id,
                bt->difficulty, bt->height, bt->reserved_offset, bt->reserved_offset + 12,
                bt->reserved_offset + 8, target, target_hex);
    }
    else
    {
        snprintf(body, CLIENT_BODY_MAX, "{\"jsonrpc\":\"2.0\",\"method\":\"job\",\"params\""
                ":{\"id\":\"%.32s\",\"job\":{\"blocktemplate_blob\":\"%s\",\"job_id\":\"%.32s\","
                "\"difficulty\":%"PRIu64",\"height\":%"PRIu64",\"reserved_offset\":%u,\"client_nonce_offset\":%u,"
                "\"client_pool_offset\":%u,\"target_diff\":%"PRIu64",\"target_diff_hex\":\"%.8s\"},"
                "\"status\":\"OK\"}}\n", client_id, template_blob, job_id,
                bt->difficulty, bt->height, bt->reserved_offset, bt->reserved_offset + 12,
                bt->reserved_offset + 8, target, target_hex);
    }
    return body;
}

static char *
stratum_new_job_body(int json_id, const char *client_id, const char *job_id,
        const char *blob, uint64_t target, uint64_t height, bool response)
{
    char *body = calloc(CLIENT_BODY_MAX, sizeof(char));

    char target_hex[9];
    target_to_hex(target, &target_hex[0]);

    if (response)
    {
        snprintf(body, CLIENT_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\",\"error\":null,\"result\""
                ":{\"id\":\"%.32s\",\"job\":{"
                "\"blob\":\"%s\",\"job_id\":\"%.32s\",\"target\":\"%.8s\",\"height\":%"PRIu64"},"
                "\"status\":\"OK\"}}\n", json_id, client_id, blob, job_id, target_hex, height);
    }
    else
    {
        snprintf(body, CLIENT_BODY_MAX, "{\"jsonrpc\":\"2.0\",\"method\":\"job\",\"params\""
                ":{\"id\":\"%.32s\",\"blob\":\"%s\",\"job_id\":\"%.32s\",\"target\":\"%.8s\","
                "\"height\":%"PRIu64"}}\n",
                client_id, blob, job_id, target_hex, height);
    }
    return body;
}

static char *
stratum_new_error_body(int json_id, const char *error)
{
    char *body = calloc(512, sizeof(char));
    snprintf(body, 512, "{\"id\":%d,\"jsonrpc\":\"2.0\",\"error\":"
            "{\"code\":-1, \"message\":\"%s\"}}\n", json_id, error);
    return body;
}

static char *
stratum_new_status_body(int json_id, const char *status)
{
    char *body = calloc(256, sizeof(char));
    snprintf(body, 256, "{\"id\":%d,\"jsonrpc\":\"2.0\",\"error\":null,\"result\":{\"status\":\"%s\"}}\n",
            json_id, status);
    return body;
}

static void
response_to_block_template(json_object *result, block_template_t *block_template)
{
    block_template->blockhashing_blob = strdup(json_object_get_string(json_object_object_get(result, "blockhashing_blob")));
    block_template->blocktemplate_blob = strdup(json_object_get_string(json_object_object_get(result, "blocktemplate_blob")));
    block_template->difficulty = json_object_get_int64(json_object_object_get(result, "difficulty"));
    block_template->height = json_object_get_int64(json_object_object_get(result, "height"));
    memcpy(block_template->prev_hash, json_object_get_string(json_object_object_get(result, "prev_hash")), 64);
    block_template->reserved_offset = json_object_get_int(json_object_object_get(result, "reserved_offset"));
}

static void
response_to_block(json_object *result, block_t *block)
{
    memset(block, 0, sizeof(block_t));
    json_object *block_header = json_object_object_get(result, "block_header");
    block->height = json_object_get_int64(json_object_object_get(block_header, "height"));
    block->difficulty = json_object_get_int64(json_object_object_get(block_header, "difficulty"));
    memcpy(block->hash, json_object_get_string(json_object_object_get(block_header, "hash")), 64);
    memcpy(block->prev_hash, json_object_get_string(json_object_object_get(block_header, "prev_hash")), 64);
    block->timestamp = json_object_get_int64(json_object_object_get(block_header, "timestamp"));
    block->reward = json_object_get_int64(json_object_object_get(block_header, "reward"));
    int orphan_status = json_object_get_int64(json_object_object_get(block_header, "orphan_status"));
    if (orphan_status)
        block->status |= BLOCK_ORPHANED;
}

static char *
rpc_new_request_body(const char* method, char* fmt, ...)
{
    char *result = calloc(RPC_BODY_MAX, sizeof(char));
    char *pr = &result[0];

    snprintf(pr, RPC_BODY_MAX, "%s%s%s", "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"", method, "\"");
    pr += strlen(pr);

    if (fmt && *fmt)
    {
        char *s;
        uint64_t d;
        snprintf(pr, RPC_BODY_MAX - strlen(result), "%s", ",\"params\":{");
        pr += strlen(pr);
        va_list args;
        va_start(args, fmt);
        uint8_t count = 0;
        while (*fmt)
        {
            switch (*fmt++)
            {
                case 's':
                    s = va_arg(args, char *);
                    snprintf(pr, RPC_BODY_MAX - strlen(result), "\"%s\"", s);
                    pr += strlen(pr);
                    break;
                case 'd':
                    d = va_arg(args, uint64_t);
                    snprintf(pr, RPC_BODY_MAX - strlen(result), "%"PRIu64, d);
                    pr += strlen(pr);
                    break;
            }
            char append = ':';
            if (count++ % 2 != 0)
                append = ',';
            *pr++ = append;
        }
        va_end(args);
        *--pr = '}';
        pr++;
    }
    *pr = '}';
    log_trace("Payload: %s", result);
    return result;
}

static void
rpc_on_response(struct evhttp_request *req, void *arg)
{
    struct evbuffer *input;
    rpc_callback_t *callback = (rpc_callback_t*) arg;

    if (!req)
    {
        log_error("Request failure. Aborting.");
        return;
    }

    int rc = evhttp_request_get_response_code(req);
    if (rc < 200 || rc >= 300)
    {
        log_error("HTTP status code %d for %s. Aborting.", rc, evhttp_request_get_uri(req));
        return;
    }

    input = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(input);
    char data[len+1];
    evbuffer_remove(input, data, len);
    data[len] = '\0';
    callback->cb(&data[0], callback);
    if (callback->data)
        free(callback->data);
    free(callback);
}

static void
rpc_request(struct event_base *base, const char *body, rpc_callback_t *callback)
{
    struct evhttp_connection *con;
    struct evhttp_request *req;
    struct evkeyvalq *headers;
    struct evbuffer *output;

    con = evhttp_connection_base_new(base, NULL, config.rpc_host, config.rpc_port);
    evhttp_connection_set_timeout(con, config.rpc_timeout);
    req = evhttp_request_new(rpc_on_response, callback);
    output = evhttp_request_get_output_buffer(req);
    evbuffer_add(output, body, strlen(body));
    headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(headers, "Content-Type", "application/json");
    evhttp_add_header(headers, "Connection", "close");
    evhttp_make_request(con, req, EVHTTP_REQ_POST, RPC_PATH);
}

static void
rpc_wallet_request(struct event_base *base, const char *body, rpc_callback_t *callback)
{
    struct evhttp_connection *con;
    struct evhttp_request *req;
    struct evkeyvalq *headers;
    struct evbuffer *output;

    con = evhttp_connection_base_new(base, NULL, config.wallet_rpc_host, config.wallet_rpc_port);
    evhttp_connection_set_timeout(con, config.rpc_timeout);
    req = evhttp_request_new(rpc_on_response, callback);
    output = evhttp_request_get_output_buffer(req);
    evbuffer_add(output, body, strlen(body));
    headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(headers, "Content-Type", "application/json");
    evhttp_add_header(headers, "Connection", "close");
    evhttp_make_request(con, req, EVHTTP_REQ_POST, RPC_PATH);
}

static void
rpc_on_block_headers_range(const char* data, rpc_callback_t *callback)
{
    json_object *root = json_tokener_parse(data);
    json_object *result = json_object_object_get(root, "result");
    json_object *error = json_object_object_get(root, "error");
    const char *status = json_object_get_string(json_object_object_get(result, "status"));
    if (error != NULL)
    {
        int ec = json_object_get_int(json_object_object_get(error, "code"));
        const char *em = json_object_get_string(json_object_object_get(error, "message"));
        log_warn("Error (%d) getting block headers by range: %s", ec, em);
        json_object_put(root);
        return;
    }
    if (status == NULL || strcmp(status, "OK") != 0)
    {
        log_warn("Error getting block headers by range: %s", status);
        json_object_put(root);
        return;
    }

    json_object *headers = json_object_object_get(result, "headers");
    size_t headers_len = json_object_array_length(headers);
    for (int i=0; i<headers_len; i++)
    {
        json_object *header = json_object_array_get_idx(headers, i);
        block_t *bh = &block_headers_range[i];
        memcpy(bh->hash, json_object_get_string(json_object_object_get(header, "hash")), 64);
        memcpy(bh->prev_hash, json_object_get_string(json_object_object_get(header, "prev_hash")), 64);
        bh->height = json_object_get_int64(json_object_object_get(header, "height"));
        bh->difficulty = json_object_get_int64(json_object_object_get(header, "difficulty"));
        bh->reward = json_object_get_int64(json_object_object_get(header, "reward"));
        bh->timestamp = json_object_get_int64(json_object_object_get(header, "timestamp"));
        int orphan_status = json_object_get_int64(json_object_object_get(header, "orphan_status"));
        if (orphan_status)
            bh->status |= BLOCK_ORPHANED;
    }
    process_blocks(block_headers_range, BLOCK_HEADERS_RANGE);
    json_object_put(root);
}

static void
rpc_on_block_header_by_height(const char* data, rpc_callback_t *callback)
{
    log_trace("Got block header by height: \n%s", data);
    json_object *root = json_tokener_parse(data);
    json_object *result = json_object_object_get(root, "result");
    json_object *error = json_object_object_get(root, "error");
    const char *status = json_object_get_string(json_object_object_get(result, "status"));
    if (error != NULL)
    {
        int ec = json_object_get_int(json_object_object_get(error, "code"));
        const char *em = json_object_get_string(json_object_object_get(error, "message"));
        log_error("Error (%d) getting block header by height: %s", ec, em);
        json_object_put(root);
        return;
    }
    if (status == NULL || strcmp(status, "OK") != 0)
    {
        log_error("Error getting block header by height: %s", status);
        json_object_put(root);
        return;
    }
    block_t rb;
    response_to_block(result, &rb);
    process_blocks(&rb, 1);
    json_object_put(root);
}

static void
rpc_on_block_template(const char* data, rpc_callback_t *callback)
{
    log_trace("Got block template: \n%s", data);
    json_object *root = json_tokener_parse(data);
    json_object *result = json_object_object_get(root, "result");
    json_object *error = json_object_object_get(root, "error");
    const char *status = json_object_get_string(json_object_object_get(result, "status"));
    if (error != NULL)
    {
        int ec = json_object_get_int(json_object_object_get(error, "code"));
        const char *em = json_object_get_string(json_object_object_get(error, "message"));
        log_error("Error (%d) getting block template: %s", ec, em);
        json_object_put(root);
        return;
    }
    if (status == NULL || strcmp(status, "OK") != 0)
    {
        log_error("Error getting block template: %s", status);
        json_object_put(root);
        return;
    }

    block_template_t *bt = calloc(1, sizeof(block_template_t));
    response_to_block_template(result, bt);
    block_template_t *front = block_templates[0];
    if (front == NULL)
    {
        block_templates[0] = bt;
    }
    else
    {
        size_t i = BLOCK_TEMPLATES_MAX;
        while (--i)
        {
            if (i == BLOCK_TEMPLATES_MAX - 1 && block_templates[i] != NULL)
            {
                block_template_free(block_templates[i]);
            }
            block_templates[i] = block_templates[i-1];
        }
        block_templates[0] = bt;
    }
    pool_clients_send_job();
    json_object_put(root);
}

static void
rpc_on_last_block_header(const char* data, rpc_callback_t *callback)
{
    log_trace("Got last block header: \n%s", data);
    json_object *root = json_tokener_parse(data);
    json_object *result = json_object_object_get(root, "result");
    json_object *error = json_object_object_get(root, "error");
    const char *status = json_object_get_string(json_object_object_get(result, "status"));
    if (error != NULL)
    {
        int ec = json_object_get_int(json_object_object_get(error, "code"));
        const char *em = json_object_get_string(json_object_object_get(error, "message"));
        log_error("Error (%d) getting last block header: %s", ec, em);
        json_object_put(root);
        return;
    }
    if (status == NULL || strcmp(status, "OK") != 0)
    {
        log_error("Error getting last block header: %s", status);
        json_object_put(root);
        return;
    }
    block_t *front = last_block_headers[0];
    block_t *block = calloc(1, sizeof(block_t));
    response_to_block(result, block);
    if (front == NULL)
    {
        startup_pauout(block->height);
    }
    bool need_new_template = false;
    if (front != NULL && block->height > front->height)
    {
        size_t i = BLOCK_HEADERS_MAX;
        while (--i)
        {
            if (i == BLOCK_HEADERS_MAX - 1 && last_block_headers[i] != NULL)
            {
                free(last_block_headers[i]);
            }
            last_block_headers[i] = last_block_headers[i-1];
        }
        last_block_headers[0] = block;
        need_new_template = true;
    }
    else if (front == NULL)
    {
        last_block_headers[0] = block;
        need_new_template = true;
    }
    else
        free(block);

    pool_stats.network_difficulty = last_block_headers[0]->difficulty;
    pool_stats.network_hashrate = last_block_headers[0]->difficulty / BLOCK_TIME;
    update_pool_hr();

    if (need_new_template)
    {
        log_info("Fetching new block template");
        char *body = rpc_new_request_body("get_block_template", "sssd", "wallet_address", config.pool_wallet, "reserve_size", 17);
        rpc_callback_t *c1 = calloc(1, sizeof(rpc_callback_t));
        c1->cb = rpc_on_block_template;
        rpc_request(base, body, c1);
        free(body);

        uint32_t end = block->height - 60;
        uint32_t start = end - BLOCK_HEADERS_RANGE + 1;
        body = rpc_new_request_body("get_block_headers_range", "sdsd", "start_height", start, "end_height", end);
        rpc_callback_t *c2 = calloc(1, sizeof(rpc_callback_t));
        c2->cb = rpc_on_block_headers_range;
        rpc_request(base, body, c2);
        free(body);
    }

    json_object_put(root);
}

static void
rpc_on_block_submitted(const char* data, rpc_callback_t *callback)
{
    json_object *root = json_tokener_parse(data);
    json_object *result = json_object_object_get(root, "result");
    json_object *error = json_object_object_get(root, "error");
    const char *status = json_object_get_string(json_object_object_get(result, "status"));
    /*
      The RPC reports submission as an error even when it's added as
      an alternative block. Thus, still store it. This doesn't matter
      as upon payout, blocks are checked whether they are orphaned or not.
    */
    if (error != NULL)
    {
        int ec = json_object_get_int(json_object_object_get(error, "code"));
        const char *em = json_object_get_string(json_object_object_get(error, "message"));
        log_debug("Error (%d) with block submission: %s", ec, em);
    }
    if (status == NULL || strcmp(status, "OK") != 0)
    {
        log_debug("Error submitting block: %s", status);
    }
    pool_stats.pool_blocks_found++;
    block_t *b = (block_t*)callback->data;
    pool_stats.last_block_found = b->timestamp;
    log_info("Block submitted at height: %"PRIu64, b->height);
    int rc = store_block(b->height, b);
    if (rc != 0)
        log_warn("Failed to store block: %s", mdb_strerror(rc));
    json_object_put(root);
}

static void
rpc_on_wallet_transferred(const char* data, rpc_callback_t *callback)
{
    log_trace("Transfer response: \n%s", data);
    const char* address = callback->data;
    json_object *root = json_tokener_parse(data);
    json_object *result = json_object_object_get(root, "result");
    json_object *error = json_object_object_get(root, "error");
    if (error != NULL)
    {
        int ec = json_object_get_int(json_object_object_get(error, "code"));
        const char *em = json_object_get_string(json_object_object_get(error, "message"));
        log_error("Error (%d) with wallet transfer: %s", ec, em);
        goto cleanup;
    }
    log_info("Payout transfer successfull");

    int rc;
    char *err;
    MDB_txn *txn;
    MDB_cursor *cursor;
    if ((rc = mdb_txn_begin(env, NULL, 0, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        goto cleanup;
    }
    if ((rc = mdb_cursor_open(txn, db_balance, &cursor)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        goto cleanup;
    }

    MDB_cursor_op op = MDB_SET;
    MDB_val key = {ADDRESS_MAX, (void*)address};
    MDB_val val;
    rc = mdb_cursor_get(cursor, &key, &val, op);
    if (rc == MDB_NOTFOUND)
    {
        log_error("Payment made to non-existent address");
        mdb_txn_abort(txn);
        goto cleanup;
    }
    else if (rc != 0 && rc != MDB_NOTFOUND)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        goto cleanup;
    }
    mdb_cursor_del(cursor, 0);
    mdb_txn_commit(txn);

    /* Now store payment info */
    const char *tx_hash = json_object_get_string(json_object_object_get(result, "tx_hash"));
    uint64_t amount = json_object_get_int64(json_object_object_get(result, "amount"));
    time_t now = time(NULL);
    payment_t payment;
    memcpy(payment.tx_hash, tx_hash, sizeof(payment.tx_hash));
    payment.amount = amount;
    payment.timestamp = now;

    if ((rc = mdb_txn_begin(env, NULL, 0, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        goto cleanup;
    }
    if ((rc = mdb_cursor_open(txn, db_payments, &cursor)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        goto cleanup;
    }
    key.mv_data = (void*)address;
    key.mv_size = ADDRESS_MAX;
    val.mv_data = &payment;
    val.mv_size = sizeof(payment);
    if ((rc = mdb_cursor_put(cursor, &key, &val, MDB_APPENDDUP)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("Error putting payment: %s", err);
        mdb_txn_abort(txn);
        goto cleanup;
    }
    if ((rc = mdb_txn_commit(txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("Error committing payment: %s", err);
        mdb_txn_abort(txn);
        goto cleanup;
    }
cleanup:
    json_object_put(root);
}

static void
timer_on_120s(int fd, short kind, void *ctx)
{
    log_info("Fetching last block header");
    char *body = rpc_new_request_body("get_last_block_header", NULL);
    rpc_callback_t *callback = calloc(1, sizeof(rpc_callback_t));
    callback->cb = rpc_on_last_block_header;
    rpc_request(base, body, callback);
    free(body);
    struct timeval timeout = { .tv_sec = 120, .tv_usec = 0 };
    evtimer_add(timer_120s, &timeout);
}

static void
timer_on_10m(int fd, short kind, void *ctx)
{
    send_payments();
    struct timeval timeout = { .tv_sec = 600, .tv_usec = 0 };
    evtimer_add(timer_10m, &timeout);
}

static void
client_add(int fd, struct bufferevent *bev)
{
    log_info("New client connected");
    client_t *c = pool_clients.clients;
    bool resize = true;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        if (c->connected_since == 0)
        {
            resize = false;
            break;
        }
    }
    if (resize)
    {
        pthread_mutex_lock(&mutex_clients);
        pool_clients.count += POOL_CLIENTS_GROW;
        c = realloc(pool_clients.clients, sizeof(client_t) * pool_clients.count);
        pool_clients.clients = c;
        c += pool_clients.count - POOL_CLIENTS_GROW;
        pthread_mutex_unlock(&mutex_clients);
        log_debug("Client pool can now hold %zu clients", pool_clients.count);
    }
    memset(c, 0, sizeof(client_t));
    c->fd = fd;
    c->bev = bev;
    c->connected_since = time(NULL);
    pool_stats.connected_miners++;
}

static void
client_find(struct bufferevent *bev, client_t **client)
{
    int fd = bufferevent_getfd(bev);
    client_t *c = pool_clients.clients;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        if (c->fd == fd)
        {
            *client = c;
            return;
        }
    }
    *client = NULL;
}

static void
client_clear(struct bufferevent *bev)
{
    client_t *client;
    client_find(bev, &client);
    if (client == NULL)
        return;
    client_clear_jobs(client);
    memset(client, 0, sizeof(client_t));
    bufferevent_free(bev);
    pool_stats.connected_miners--;
}

static void
client_send_job(client_t *client, bool response)
{
    /* First cycle jobs */
    job_t *last = &client->active_jobs[CLIENT_JOBS_MAX-1];
    if (last->blob != NULL)
    {
        free(last->blob);
        last->blob = NULL;
    }
    if (last->submissions != NULL)
    {
        free(last->submissions);
        last->submissions = NULL;
        last->submissions_count = 0;
    }
    for (size_t i=CLIENT_JOBS_MAX-1; i>0; i--)
    {
        job_t *current = &client->active_jobs[i];
        job_t *prev = &client->active_jobs[i-1];
        memcpy(current, prev, sizeof(job_t));
    }
    job_t *job = &client->active_jobs[0];
    memset(job, 0, sizeof(job_t));

    /* Quick check we actually have a block template */
    block_template_t *bt = block_templates[0];
    if (!bt)
    {
        log_warn("Cannot send client a job as have not yet recieved a block template");
        return;
    }

    /*
      1. Convert blocktemplate_blob to binary
      2. Update bytes in reserved space at reserved_offset
      3. Get block hashing blob for job
      4. Send
    */

    /* Convert template to blob */
    size_t bin_size = strlen(bt->blocktemplate_blob) >> 1;
    char *block = calloc(bin_size, sizeof(char));
    hex_to_bin(bt->blocktemplate_blob, block, bin_size);

    /* Set the extra nonce in our reserved space */
    char *p = block;
    p += bt->reserved_offset;
    ++extra_nonce;
    memcpy(p, &extra_nonce, sizeof(extra_nonce));
    job->extra_nonce = extra_nonce;

    /* Get hashong blob */
    size_t hashing_blob_size;
    char *hashing_blob = NULL;
    get_hashing_blob(block, bin_size, &hashing_blob, &hashing_blob_size);

    /* Make hex */
    job->blob = calloc((hashing_blob_size << 1) +1, sizeof(char));
    bin_to_hex(hashing_blob, hashing_blob_size, job->blob);
    log_trace("Miner hashing blob: %s", job->blob);

    /* Save a job id */
    uuid_generate(job->id);

    /* Hold reference to block template */
    job->block_template = bt;

    /* Send */
    char job_id[33];
    bin_to_hex((const char*)job->id, sizeof(uuid_t), job_id);
    
    /* Retarget */
    double duration = difftime(time(NULL), client->connected_since);
    uint8_t retarget_time = client->is_proxy ? 5 : 120;
    uint64_t target = fmax((double)client->hashes / duration * retarget_time, config.pool_start_diff);
    job->target = target;
    log_debug("Client %.32s target now %"PRIu64, client->client_id, target);

    char *body;
    if (!client->is_proxy)
    {
        body = stratum_new_job_body(client->json_id, client->client_id, job_id,
            job->blob, target, bt->height, response);
    }
    else
    {
        char *template_hex = calloc(bin_size+1, sizeof(char));
        bin_to_hex(block, bin_size, template_hex);
        body = stratum_new_proxy_job_body(client->json_id, client->client_id, job_id,
            bt, template_hex, target, response);
    }
    log_trace("Client job: %s", body);
    struct evbuffer *output = bufferevent_get_output(client->bev);
    evbuffer_add(output, body, strlen(body));
    free(body);
    free(block);
    free(hashing_blob);
}

static void
client_clear_jobs(client_t *client)
{
    for (size_t i=0; i<CLIENT_JOBS_MAX; i++)
    {
        job_t *job = &client->active_jobs[i];
        if (job->blob != NULL)
        {
            free(job->blob);
            job->blob = NULL;
        }
        if (job->submissions != NULL)
        {
            free(job->submissions);
            job->submissions = NULL;
            job->submissions_count = 0;
        }
    }
}

static void
send_validation_error(const client_t *client, const char *message)
{
    struct evbuffer *output = bufferevent_get_output(client->bev);
    char *body = stratum_new_error_body(client->json_id, message);
    evbuffer_add(output, body, strlen(body));
    log_debug("Validation error: %s", message);
    free(body);
}

static void
client_on_login(json_object *message, client_t *client)
{
    JSON_GET_OR_ERROR(params, message, json_type_object, client);
    JSON_GET_OR_ERROR(login, params, json_type_string, client);
    JSON_GET_OR_ERROR(pass, params, json_type_string, client);

    const char *address = json_object_get_string(login);
    uint64_t prefix;
    parse_address(address, &prefix);
    if (prefix != MAINNET_ADDRESS_PREFIX && prefix != TESTNET_ADDRESS_PREFIX)
        return send_validation_error(client, "login only main wallet addresses are supported");

    const char *worker_id = json_object_get_string(pass);

    json_object *agent = NULL;
    if (json_object_object_get_ex(params, "agent", &agent))
    {
        const char *user_agent = json_object_get_string(agent);
        if (user_agent)
        {
            strncpy(client->agent, user_agent, 255);
            client->is_proxy = strstr(user_agent, "proxy") != NULL ? true : false;
        }
    }

    strncpy(client->address, address, sizeof(client->address));
    strncpy(client->worker_id, worker_id, sizeof(client->worker_id));
    uuid_t cid;
    uuid_generate(cid);
    bin_to_hex((const char*)cid, sizeof(uuid_t), client->client_id);
    char status[256];
    snprintf(status, 256, "Logged in: %s %s\n", worker_id, address);
    client_send_job(client, true);
}

static void
client_on_submit(json_object *message, client_t *client)
{
    struct evbuffer *output = bufferevent_get_output(client->bev);

    JSON_GET_OR_ERROR(params, message, json_type_object, client);
    JSON_GET_OR_ERROR(nonce, params, json_type_string, client);
    JSON_GET_OR_ERROR(result, params, json_type_string, client);
    JSON_GET_OR_ERROR(job_id, params, json_type_string, client);

    char *endptr = NULL;
    const char *nptr = json_object_get_string(nonce);
    errno = 0;
    unsigned long int uli = strtoul(nptr, &endptr, 16);
    if (errno != 0 || nptr == endptr)
        return send_validation_error(client, "nonce not an unsigned long int");
    const uint32_t result_nonce = ntohl(uli);

    const char *result_hex = json_object_get_string(result);
    if (strlen(result_hex) != 64)
        return send_validation_error(client, "result invalid length");
    if (is_hex_string(result_hex) != 0)
        return send_validation_error(client, "result not hex string");

    const char *jid = json_object_get_string(job_id);
    if (strlen(jid) != 32)
        return send_validation_error(client, "job_id invalid length");

    job_t *job = client_find_job(client, jid);
    if (!job)
        return send_validation_error(client, "cannot find job with job_id");

    log_trace("Client submitted nonce=%u, result=%s", result_nonce, result_hex);
    /*
      1. Validate submission
         active_job->blocktemplate_blob to bin
         add extra_nonce at reserved offset
         add nonce
         get hashing blob
         hash
         compare result
         check result hash against block difficulty (if ge then mined block)
         check result hash against target difficulty (if not ge, invalid share)
      2. Process share
         check result hash against template difficulty (submit to network if good)
         add share to db

      Note reserved space is: extra_nonce, instance_id, pool_nonce, worker_nonce
       4 bytes each. instance_id would be used for pool threads.
    */

    /* Convert template to blob */
    block_template_t *bt = job->block_template;
    char *btb = bt->blocktemplate_blob;
    size_t bin_size = strlen(btb) >> 1;
    char *block = calloc(bin_size, sizeof(char));
    hex_to_bin(bt->blocktemplate_blob, block, bin_size);

    /* Set the extra nonce in our reserved space */
    char *p = block;
    p += bt->reserved_offset;
    memcpy(p, &job->extra_nonce, sizeof(extra_nonce));

    uint32_t pool_nonce = 0;
    uint32_t worker_nonce = 0;
    if (client->is_proxy)
    {
        /*
          A proxy supplies pool_nonce and worker_nonce
          so add them in the resrved space too.
        */
        pool_nonce = json_object_get_int(
                        json_object_object_get(params, "poolNonce"));
        worker_nonce = json_object_get_int(
                        json_object_object_get(params, "workerNonce"));
        p += 8;
        memcpy(p, &pool_nonce, sizeof(pool_nonce));
        p += 4;
        memcpy(p, &worker_nonce, sizeof(worker_nonce));
    }
    uint128_t sub = 0;
    uint32_t *psub = (uint32_t*) &sub;
    *psub++ = result_nonce;
    *psub++ = job->extra_nonce;
    *psub++ = pool_nonce;
    *psub++ = worker_nonce;

    psub -= 4;
    log_trace("Submission reserved values: %u %u %u %u", *psub, *(psub+1), *(psub+2), *(psub+3));

    /* Check not already submitted */
    uint128_t *submissions = job->submissions;
    for (size_t i=0; i<job->submissions_count; i++)
    {
        if (submissions[i] == sub)
        {
            char *body = stratum_new_error_body(client->json_id, "Duplicate share");
            evbuffer_add(output, body, strlen(body));
            log_debug("Duplicate share");
            free(body);
            free(block);
            return;
        }
    }
    job->submissions = realloc((void*)submissions,
            sizeof(uint128_t) * ++job->submissions_count);
    job->submissions[job->submissions_count-1] = sub;

    /* And the supplied nonce */
    p = block;
    p += 39;
    memcpy(p, &result_nonce, sizeof(result_nonce));

    /* Get hashong blob */
    size_t hashing_blob_size;
    char *hashing_blob = NULL;
    if (get_hashing_blob(block, bin_size, &hashing_blob, &hashing_blob_size) != 0)
    {
        char *body = stratum_new_error_body(client->json_id, "Invalid block");
        evbuffer_add(output, body, strlen(body));
        log_debug("Invalid block");
        free(body);
        free(block);
        return;
    }

    /* Hash and compare */
    char result_hash[32];
    char submitted_hash[32];
    uint8_t major_version = (uint8_t)block[0];
    const int cn_variant = major_version >= 7 ? major_version - 6 : 0;
    get_hash(hashing_blob, hashing_blob_size, (char**)&result_hash, cn_variant, bt->height);
    hex_to_bin(result_hex, submitted_hash, 32);

    if (memcmp(submitted_hash, result_hash, 32) != 0)
    {
        char *body = stratum_new_error_body(client->json_id, "Invalid share");
        evbuffer_add(output, body, strlen(body));
        log_debug("Invalid share");
        /* TODO: record and ban if too many */
        free(body);
        free(block);
        free(hashing_blob);
        return;
    }

    BIGNUM *hd = BN_new();
    BIGNUM *jd = BN_new();
    BIGNUM *bd = BN_new();
    BIGNUM *rh = NULL;
    BN_set_word(jd, job->target);
    BN_set_word(bd, bt->difficulty);
    reverse_bin(result_hash, 32);
    rh = BN_bin2bn((const unsigned char*)result_hash, 32, NULL);
    BN_div(hd, NULL, base_diff, rh, bn_ctx);
    BN_free(rh);

    /* Process share */
    client->hashes += job->target;
    time_t now = time(NULL);
    log_trace("Checking hash against blobk difficulty: %lu, job difficulty: %lu",
            BN_get_word(bd), BN_get_word(jd));
    bool can_store = false;

    if (BN_cmp(hd, bd) >= 0)
    {
        can_store = true;
        /* Yay! Mined a block so submit to network */
        log_info("+++ MINED A BLOCK +++");
        char *block_hex = calloc((bin_size << 1)+1, sizeof(char));
        bin_to_hex(block, bin_size, block_hex);
        char *body = calloc(RPC_BODY_MAX, sizeof(char));
        snprintf(body, RPC_BODY_MAX,
                "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"submit_block\", \"params\":[\"%s\"]}", block_hex);

        rpc_callback_t *callback = calloc(1, sizeof(rpc_callback_t));
        callback->cb = rpc_on_block_submitted;
        callback->data = calloc(1, sizeof(block_t));

        block_t* b = (block_t*)callback->data;
        b->height = bt->height;
        bin_to_hex(submitted_hash, 32, b->hash);
        memcpy(b->prev_hash, bt->prev_hash, 64);
        b->difficulty = bt->difficulty;
        b->status = BLOCK_LOCKED;
        b->timestamp = now;

        rpc_request(base, body, callback);
        free(body);
        free(block_hex);
    }
    else if (BN_cmp(hd, jd) < 0)
    {
        can_store = false;
        char *body = stratum_new_error_body(client->json_id, "Low difficulty share");
        log_debug("Low difficulty (%lu) share", BN_get_word(jd));
        evbuffer_add(output, body, strlen(body));
        free(body);
    }
    else
        can_store = true;

    BN_free(hd);
    BN_free(jd);
    BN_free(bd);
    free(block);
    free(hashing_blob);

    if (can_store)
    {
        share_t share;
        share.height = bt->height;
        share.difficulty = job->target;
        strncpy(share.address, client->address, sizeof(share.address));
        share.timestamp = now;
        log_debug("Storing share with difficulty: %"PRIu64, share.difficulty);
        int rc = store_share(share.height, &share);
        if (rc != 0)
            log_warn("Failed to store share: %s", mdb_strerror(rc));
        char *body = stratum_new_status_body(client->json_id, "OK");
        evbuffer_add(output, body, strlen(body));
        free(body);
    }
}

static void
client_on_read(struct bufferevent *bev, void *ctx)
{
    struct evbuffer *input, *output;
    char *line;
    size_t n;
    client_t *client;

    client_find(bev, &client);
    if (client == NULL)
        return;

    input = bufferevent_get_input(bev);
    output = bufferevent_get_output(bev);

    if (evbuffer_get_length(input) >= MAX_LINE)
    {
        const char *too_long = "Message too long\n";
        evbuffer_add(output, too_long, strlen(too_long));
        return;
    }

    while ((line = evbuffer_readln(input, &n, EVBUFFER_EOL_LF)))
    {
        json_object *message = json_tokener_parse(line);
        const char *method = json_object_get_string(json_object_object_get(message, "method"));
        const char unknown[] = "Unknown method";
        client->json_id = json_object_get_int(json_object_object_get(message, "id"));

        if (method == NULL)
        {
            char *body = stratum_new_error_body(client->json_id, unknown);
            evbuffer_add(output, body, strlen(body));
            free(body);
        }
        else if (strcmp(method, "login") == 0)
        {
            client_on_login(message, client);
        }
        else if (strcmp(method, "submit") == 0)
        {
            client_on_submit(message, client);
        }
        else if (strcmp(method, "getjob") == 0)
        {
            client_send_job(client, false);
        }
        else if (strcmp(method, "keepalived") == 0)
        {
            char *body = stratum_new_status_body(client->json_id, "KEEPALIVED");
            evbuffer_add(output, body, strlen(body));
            free(body);
        }
        else
        {
            char *body = stratum_new_error_body(client->json_id, unknown);
            evbuffer_add(output, body, strlen(body));
            free(body);
        }

        json_object_put(message);
        free(line);
    }
}

static void
client_on_error(struct bufferevent *bev, short error, void *ctx)
{
    if (error & BEV_EVENT_EOF)
    {
        /* connection has been closed */
        log_debug("Client disconnected. Removing.");
    }
    else if (error & BEV_EVENT_ERROR)
    {
        /* check errno to see what error occurred */
        log_debug("Client error: %d. Removing.", errno);
    }
    else if (error & BEV_EVENT_TIMEOUT)
    {
        /* must be a timeout event handle, handle it */
        log_debug("Client timeout. Removing.");
    }
    client_clear(bev);
}

static void
client_on_accept(evutil_socket_t listener, short event, void *arg)
{
    struct event_base *base = (struct event_base*)arg;
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    int fd = accept(listener, (struct sockaddr*)&ss, &slen);
    if (fd < 0)
    {
        perror("accept");
    }
    else if (fd > FD_SETSIZE)
    {
        close(fd);
    }
    else
    {
        struct bufferevent *bev;
        evutil_make_socket_nonblocking(fd);
        bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
        bufferevent_setcb(bev, client_on_read, NULL, client_on_error, NULL);
        bufferevent_setwatermark(bev, EV_READ, 0, MAX_LINE);
        bufferevent_enable(bev, EV_READ|EV_WRITE);
        client_add(fd, bev);
    }
}

static void
read_config(const char *config_file, const char *log_file)
{
    /* Start with some defaults for any missing... */
    strncpy(config.rpc_host, "127.0.0.1", 10);
    config.rpc_port = 18081;
    config.rpc_timeout = 15;
    config.pool_start_diff = 100;
    config.share_mul = 2.0;
    config.pool_fee = 0.01;
    config.payment_threshold = 0.33;
    config.pool_port = 4242;
    config.log_level = 5;
    config.webui_port = 4243;

    char path[MAX_PATH];
    if (config_file)
    {
        strncpy(path, config_file, MAX_PATH);
    }
    else
    {
        getcwd(path, MAX_PATH);
        strcat(path, "/pool.conf");
        if (access(path, R_OK) != 0)
        {
            strncpy(path, getenv("HOME"), MAX_PATH);
            strcat(path, "/pool.conf");
            if (access(path, R_OK) != 0)
            {
                log_fatal("Cannot find a config file in ./ or ~/ and no option supplied. Aborting.");
                abort();
            }
        }
    }
    log_info("Reading config at: %s", path);

    FILE *fp = fopen(path, "r");
    if (fp == NULL)
    {
        log_fatal("Cannot open config file. Aborting.");
        abort();
    }
    char line[256];
    char *key;
    char *val;
    const char *tok = " =";
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        key = strtok(line, tok);
        if (key == NULL)
            continue;
        val = strtok(NULL, tok);
        if (val == NULL)
            continue;
        val[strcspn(val, "\r\n")] = 0;
        if (strcmp(key, "rpc-host") == 0)
        {
            strncpy(config.rpc_host, val, sizeof(config.rpc_host));
        }
        else if (strcmp(key, "rpc-port") == 0)
        {
            config.rpc_port = atoi(val);
        }
        else if (strcmp(key, "rpc-timeout") == 0)
        {
            config.rpc_timeout = atoi(val);
        }
        else if (strcmp(key, "wallet-rpc-host") == 0)
        {
            strncpy(config.wallet_rpc_host, val, sizeof(config.rpc_host));
        }
        else if (strcmp(key, "wallet-rpc-port") == 0)
        {
            config.wallet_rpc_port = atoi(val);
        }
        else if (strcmp(key, "pool-wallet") == 0)
        {
            strncpy(config.pool_wallet, val, sizeof(config.pool_wallet));
        }
        else if (strcmp(key, "pool-start-diff") == 0)
        {
            config.pool_start_diff = atoi(val);
        }
        else if (strcmp(key, "share-mul") == 0)
        {
            config.share_mul = atof(val);
        }
        else if (strcmp(key, "pool-fee") == 0)
        {
            config.pool_fee = atof(val);
        }
        else if (strcmp(key, "payment-threshold") == 0)
        {
            config.payment_threshold = atof(val);
        }
        else if (strcmp(key, "pool-port") == 0)
        {
            config.pool_port = atoi(val);
        }
        else if (strcmp(key, "log-level") == 0)
        {
            config.log_level = atoi(val);
        }
        else if (strcmp(key, "webui-port") == 0)
        {
            config.webui_port = atoi(val);
        }
        else if (strcmp(key, "log-file") == 0)
        {
            strncpy(config.log_file, val, sizeof(config.log_file));
        }
    }
    fclose(fp);

    if (log_file != NULL)
        strncpy(config.log_file, log_file, sizeof(config.log_file));

    if (!config.pool_wallet[0])
    {
        log_fatal("No pool wallet supplied. Aborting.");
        abort();
    }
    if (!config.wallet_rpc_host[0] || config.wallet_rpc_port == 0)
    {
        log_fatal("Both wallet-rpc-host and wallet-rpc-port need setting. Aborting.");
        abort();
    }
    log_info("\nCONFIG:\n  rpc_host = %s\n  rpc_port = %u\n  rpc_timeout = %u\n  pool_wallet = %s\n  "
            "pool_start_diff = %u\n  share_mul = %.2f\n  pool_fee = %.2f\n  payment_threshold = %.2f\n  "
            "wallet_rpc_host = %s\n  wallet_rpc_port = %u\n  pool_port = %u\n  "
            "log_level = %u\n  webui_port=%u\n  log-file = %s\n",
            config.rpc_host, config.rpc_port, config.rpc_timeout,
            config.pool_wallet, config.pool_start_diff, config.share_mul,
            config.pool_fee, config.payment_threshold,
            config.wallet_rpc_host, config.wallet_rpc_port, config.pool_port,
            config.log_level, config.webui_port, config.log_file);
}

static void
run(void)
{
    evutil_socket_t listener;
    struct sockaddr_in sin;
    struct event *listener_event;

    base = event_base_new();
    if (!base)
    {
        log_fatal("Failed to create event base");
        return;
    }

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = 0;
    sin.sin_port = htons(config.pool_port);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    evutil_make_socket_nonblocking(listener);

#ifndef WIN32
    {
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
#endif

    if (bind(listener, (struct sockaddr*)&sin, sizeof(sin)) < 0)
    {
        perror("bind");
        return;
    }

    if (listen(listener, 16)<0)
    {
        perror("listen");
        return;
    }

    listener_event = event_new(base, listener, EV_READ|EV_PERSIST, client_on_accept, (void*)base);
    if (event_add(listener_event, NULL) != 0)
    {
        log_fatal("Failed to add socket listener event");
        return;
    }

    timer_120s = evtimer_new(base, timer_on_120s, NULL);
    timer_on_120s(-1, EV_TIMEOUT, NULL);

    timer_10m = evtimer_new(base, timer_on_10m, NULL);
    timer_on_10m(-1, EV_TIMEOUT, NULL);

    event_base_dispatch(base);
}

static void
cleanup()
{
    printf("\n");
    log_info("Performing cleanup");
    stop_web_ui();
    evtimer_del(timer_120s);
    event_free(timer_120s);
    evtimer_del(timer_10m);
    event_free(timer_10m);
    event_base_free(base);
    pool_clients_free();
    last_block_headers_free();
    block_templates_free();
    database_close();
    BN_free(base_diff);
    BN_CTX_free(bn_ctx);
    pthread_mutex_destroy(&mutex_clients);
    log_info("Pool shutdown successfully");
    if (fd_log != NULL)
        fclose(fd_log);
}

static void
sigint_handler(int sig)
{
    signal(SIGINT, SIG_DFL);
    exit(0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, sigint_handler);
    atexit(cleanup);

    log_set_level(LOG_INFO);
    log_info("Starting pool");

    static struct option options[] =
    {
        {"config-file", required_argument, 0, 'c'},
        {"log-file", required_argument, 0, 'l'},
        {0, 0, 0, 0}
    };
    char *config_file = NULL;
    char *log_file = NULL;
    int c;
    while (1)
    {
        int option_index = 0;
        c = getopt_long (argc, argv, "c:l:",
                       options, &option_index);
        if (c == -1)
            break;
        switch (c)
        {
            case 'c':
                config_file = strdup(optarg);
                break;
            case 'l':
                log_file = strdup(optarg);
                break;
        }
    }
    read_config(config_file, log_file);

    log_set_level(LOG_FATAL - config.log_level);
    if (config.log_file[0] != '\0')
    {
        fd_log = fopen(config.log_file, "a");
        if (fd_log == NULL)
            log_info("Failed to open log file: %s", config.log_file);
        else
            log_set_fp(fd_log);
    }

    if (config_file != NULL)
        free(config_file);

    if (log_file != NULL)
        free(log_file);

    int err = 0;
    if ((err = database_init()) != 0)
    {
        log_fatal("Failed to initialize database. Return code: %d", err);
        goto cleanup;
    }

    bn_ctx = BN_CTX_new();
    base_diff = NULL;
    BN_hex2bn(&base_diff, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");

    pool_clients_init();

    wui_context_t uic;
    uic.port = config.webui_port;
    uic.pool_stats = &pool_stats;
    uic.pool_fee = config.pool_fee;
    uic.pool_port = config.pool_port;
    uic.payment_threshold = config.payment_threshold;
    start_web_ui(&uic);

    run();

cleanup:
    return 0;
}
