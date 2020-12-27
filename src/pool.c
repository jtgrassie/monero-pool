/*
Copyright (c) 2018, The Monero Project

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http.h>
#include <event2/thread.h>

#include <lmdb.h>

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

#include "bstack.h"
#include "util.h"
#include "xmr.h"
#include "log.h"
#include "webui.h"
#include "forkoff.h"
#include "growbag.h"
#include "uthash.h"

#define MAX_LINE 8192
#define CLIENTS_INIT 8192
#define RPC_BODY_MAX 8192
#define JOB_BODY_MAX 8192
#define ERROR_BODY_MAX 512
#define STATUS_BODY_MAX 512
#define CLIENT_JOBS_MAX 4
#define BLOCK_HEADERS_MAX 4
#define BLOCK_TEMPLATES_MAX 4
#define MAINNET_ADDRESS_PREFIX 18
#define TESTNET_ADDRESS_PREFIX 53
#define BLOCK_HEADERS_RANGE 10
#define DB_INIT_SIZE 0x140000000 /* 5G */
#define DB_GROW_SIZE 0xA0000000 /* 2.5G */
#define DB_COUNT_MAX 10
#define MAX_PATH 1024
#define RPC_PATH "/json_rpc"
#define ADDRESS_MAX 128
#define BLOCK_TIME 120
#define HR_BLOCK_COUNT 5
#define TEMLATE_HEIGHT_VARIANCE 5
#define MAX_BAD_SHARES 5
#define MAX_DOWNSTREAM 8
#define MAX_HOST 256

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

  Properties
  ----------
  name <-> value
*/

enum block_status { BLOCK_LOCKED, BLOCK_UNLOCKED, BLOCK_ORPHANED };
enum stratum_mode { MODE_NORMAL, MODE_SELF_SELECT };
enum msgbin_type  { BIN_PING, BIN_CONNECT, BIN_DISCONNECT, BIN_SHARE,
                    BIN_BLOCK, BIN_STATS, BIN_BALANCE };
const unsigned char msgbin[] = {0x4D,0x4E,0x52,0x4F,0x50,0x4F,0x4F,0x4C};

/* 2m, 10m, 30m, 1h, 1d, 1w */
const unsigned hr_intervals[] = {120,600,1800,3600,86400,604800};

typedef struct hr_stats_t
{
    time_t last_calc;
    uint64_t diff_since;
    /* 2m, 10m, 30m, 1h, 1d, 1w */
    double avg[6];
} hr_stats_t;

typedef struct config_t
{
    char rpc_host[MAX_HOST];
    uint16_t rpc_port;
    uint32_t rpc_timeout;
    uint32_t idle_timeout;
    char wallet_rpc_host[MAX_HOST];
    uint16_t wallet_rpc_port;
    char pool_wallet[ADDRESS_MAX];
    char pool_fee_wallet[ADDRESS_MAX];
    uint64_t pool_start_diff;
    uint64_t pool_fixed_diff;
    double share_mul;
    uint32_t retarget_time;
    double retarget_ratio;
    double pool_fee;
    double payment_threshold;
    char pool_listen[MAX_HOST];
    uint16_t pool_port;
    uint16_t pool_ssl_port;
    uint32_t log_level;
    uint16_t webui_port;
    char log_file[MAX_PATH];
    bool block_notified;
    bool disable_self_select;
    bool disable_hash_check;
    bool disable_payouts;
    char data_dir[MAX_PATH];
    char pid_file[MAX_PATH];
    bool forked;
    char trusted_listen[MAX_HOST];
    uint16_t trusted_port;
    char trusted_allowed[MAX_DOWNSTREAM][MAX_HOST];
    char upstream_host[MAX_HOST];
    uint16_t upstream_port;
    char pool_view_key[64];
    int processes;
    int32_t cull_shares;
} config_t;

typedef struct block_template_t
{
    char *blockhashing_blob;
    char *blocktemplate_blob;
    uint64_t difficulty;
    uint64_t height;
    char prev_hash[64];
    uint32_t reserved_offset;
    char seed_hash[64];
    char next_seed_hash[64];
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
    block_template_t *miner_template;
} job_t;

typedef struct client_t
{
    int fd;
    char host[MAX_HOST];
    uint16_t port;
    int json_id;
    struct bufferevent *bev;
    char address[ADDRESS_MAX];
    char worker_id[64];
    char client_id[32];
    char agent[256];
    bstack_t *active_jobs;
    uint64_t hashes;
    hr_stats_t hr_stats;
    time_t connected_since;
    bool is_xnp;
    uint32_t mode;
    uint8_t bad_shares;
    bool downstream;
    uint32_t downstream_accounts;
    uint64_t req_diff;
    UT_hash_handle hh;
} client_t;

typedef struct account_t
{
    char address[ADDRESS_MAX];
    size_t worker_count;
    time_t connected_since;
    uint64_t hashes;
    hr_stats_t hr_stats;
    UT_hash_handle hh;
} account_t;

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
    uint64_t amount;
    time_t timestamp;
    char address[ADDRESS_MAX];
} payment_t;

typedef struct rpc_callback_t rpc_callback_t;
typedef void (*rpc_callback_fun)(const char*, rpc_callback_t*);
typedef void (*rpc_datafree_fun)(void*);
struct rpc_callback_t
{
    rpc_callback_fun cf;
    void *data;
    rpc_datafree_fun df;
};

static config_t config;
static bstack_t *bst;
static bstack_t *bsh;
static struct event_base *pool_base;
static struct event *listener_event;
static struct event *timer_30s;
static struct event *timer_120s;
static struct event *timer_10m;
static struct event *signal_usr1;
static uint32_t extra_nonce;
static uint32_t instance_id;
static block_t block_headers_range[BLOCK_HEADERS_RANGE];
static MDB_env *env;
static MDB_dbi db_shares;
static MDB_dbi db_blocks;
static MDB_dbi db_balance;
static MDB_dbi db_payments;
static MDB_dbi db_properties;
static BN_CTX *bn_ctx;
static BIGNUM *base_diff;
static pool_stats_t pool_stats;
static unsigned clients_reading;
static pthread_cond_t cond_clients = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t mutex_clients = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_log = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t rwlock_tx = PTHREAD_RWLOCK_INITIALIZER;
static pthread_rwlock_t rwlock_acc = PTHREAD_RWLOCK_INITIALIZER;
static pthread_rwlock_t rwlock_cfd = PTHREAD_RWLOCK_INITIALIZER;
static FILE *fd_log;
static unsigned char sec_view[32];
static unsigned char pub_spend[32];
static uint8_t nettype;
static pthread_t trusted_th;
static struct event_base *trusted_base;
static struct event *trusted_event;
static struct bufferevent *upstream_event;
static struct event *timer_10s;
static time_t upstream_last_time;
static uint64_t upstream_last_height;
static uint32_t account_count;
static client_t *clients_by_fd = NULL;
static account_t *accounts = NULL;
static gbag_t *bag_accounts;
static gbag_t *bag_clients;
static bool abattoir;

#ifdef HAVE_RX
extern void rx_stop_mining();
extern void rx_slow_hash_free_state();
#else
void rx_stop_mining(){}
void rx_slow_hash_free_state(){}
#endif

#define JSON_GET_OR_ERROR(name, parent, type, client)                \
    json_object *name = NULL;                                        \
    if (!json_object_object_get_ex(parent, #name, &name)) {          \
        send_validation_error(client, #name " not found");           \
        return;                                                      \
    }                                                                \
    if (!json_object_is_type(name, type)) {                          \
        send_validation_error(client, #name " not a " #type);        \
        return;                                                      \
    }

#define JSON_GET_OR_WARN(name, parent, type)                         \
    json_object *name = NULL;                                        \
    if (!json_object_object_get_ex(parent, #name, &name)) {          \
        log_warn(#name " not found");                                \
    } else {                                                         \
        if (!json_object_is_type(name, type)) {                      \
            log_warn(#name " not a " #type);                         \
        }                                                            \
    }

static void
hr_update(hr_stats_t *stats)
{
    /*
       Update some time decayed EMA hashrates.
    */
    time_t now = time(NULL);
    double t = difftime(now, stats->last_calc);
    if (t <= 0)
        return;
    double h = stats->diff_since;
    double d, p, z;
    unsigned i = sizeof(hr_intervals)/sizeof(hr_intervals[0]);
    while (i--)
    {
        unsigned inter = hr_intervals[i];
        double *f = &stats->avg[i];
        d = t/inter;
        if (d > 32)
            d = 32;
        p = 1 - 1.0 / exp(d);
        z = 1 + p;
        *f += (h / t * p);
        *f /= z;
        if (*f < 2e-16)
            *f = 0;
    }
    stats->diff_since = 0;
    stats->last_calc = now;
}

static inline rpc_callback_t *
rpc_callback_new(rpc_callback_fun cf, void *data, rpc_datafree_fun df)
{
    rpc_callback_t *c = calloc(1, sizeof(rpc_callback_t));
    c->cf = cf;
    c->data = data;
    c->df = df;
    return c;
}

static inline void
rpc_callback_free(rpc_callback_t *callback)
{
    if (!callback)
        return;
    if (callback->data)
    {
        if (callback->df)
            callback->df(callback->data);
        else
            free(callback->data);
    }
    free(callback);
}

static inline void
rpc_bag_free(void* data)
{
    gbag_free((gbag_t*)data);
}

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
    return (va->timestamp < vb->timestamp) ? -1 : 1;
}

static int
compare_share(const MDB_val *a, const MDB_val *b)
{
    const share_t *va = (const share_t*) a->mv_data;
    const share_t *vb = (const share_t*) b->mv_data;
    return (va->timestamp < vb->timestamp) ? -1 : 1;
}

static int
compare_payment(const MDB_val *a, const MDB_val *b)
{
    const payment_t *va = (const payment_t*) a->mv_data;
    const payment_t *vb = (const payment_t*) b->mv_data;
    return (va->timestamp < vb->timestamp) ? -1 : 1;
}

static int
database_resize(void)
{
    const double threshold = 0.9;
    MDB_envinfo ei;
    MDB_stat st;
    int rc = 0;
    char *err = NULL;

    mdb_env_info(env, &ei);
    mdb_env_stat(env, &st);

    if(ei.me_mapsize < DB_INIT_SIZE)
    {
        if ((rc = pthread_rwlock_wrlock(&rwlock_tx)))
        {
            log_warn("Cannot cannot acquire lock");
            return rc;
        }
        pthread_mutex_lock(&mutex_clients);
        while (clients_reading)
            pthread_cond_wait(&cond_clients, &mutex_clients);
        if ((rc = mdb_env_set_mapsize(env, DB_INIT_SIZE)) != 0)
        {
            err = mdb_strerror(rc);
            log_error("%s", err);
            goto unlock;
        }
        log_debug("Database initial size: %"PRIu64, DB_INIT_SIZE);
        goto unlock;
    }

    uint64_t used = st.ms_psize * ei.me_last_pgno;
    uint64_t remaining = (uint64_t) ei.me_mapsize - used;
    log_debug("Database (used/free): %"PRIu64"/%"PRIu64, used, remaining);

    if ((double)used / ei.me_mapsize > threshold)
    {
        uint64_t ns = (uint64_t) ei.me_mapsize + DB_GROW_SIZE;
        if ((rc = pthread_rwlock_wrlock(&rwlock_tx)))
        {
            log_warn("Cannot cannot acquire lock");
            return rc;
        }
        pthread_mutex_lock(&mutex_clients);
        while (clients_reading)
            pthread_cond_wait(&cond_clients, &mutex_clients);
        if ((rc = mdb_env_set_mapsize(env, ns)) != 0)
        {
            err = mdb_strerror(rc);
            log_error("%s", err);
            goto unlock;
        }
        log_info("Database resized to: %"PRIu64, ns);
        goto unlock;
    }
    return 0;
unlock:
    pthread_mutex_unlock(&mutex_clients);
    pthread_rwlock_unlock(&rwlock_tx);
    return rc;
}

static int
database_init(const char* data_dir)
{
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;

    rc = mdb_env_create(&env);
    mdb_env_set_maxdbs(env, (MDB_dbi) DB_COUNT_MAX);
    if ((rc = mdb_env_open(env, data_dir, 0, 0664)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s (%s)", err, data_dir);
        exit(rc);
    }
    if ((rc = database_resize()))
    {
        log_fatal("Cannot resize DB");
        exit(rc);
    }
    if ((rc = mdb_txn_begin(env, NULL, 0, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s", err);
        exit(rc);
    }
    uint32_t flags = MDB_INTEGERKEY | MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED;
    if ((rc = mdb_dbi_open(txn, "shares", flags, &db_shares)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s", err);
        exit(rc);
    }
    if ((rc = mdb_dbi_open(txn, "blocks", flags, &db_blocks)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s", err);
        exit(rc);
    }
    flags = MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED;
    if ((rc = mdb_dbi_open(txn, "payments", flags, &db_payments)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s", err);
        exit(rc);
    }
    flags = MDB_CREATE;
    if ((rc = mdb_dbi_open(txn, "balance", flags, &db_balance)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s", err);
        exit(rc);
    }
    if ((rc = mdb_dbi_open(txn, "properties", flags, &db_properties)) != 0)
    {
        err = mdb_strerror(rc);
        log_fatal("%s", err);
        exit(rc);
    }
    MDB_val k, v;
    k.mv_data = "upstream_last_height";
    k.mv_size = strlen(k.mv_data);
    if (!mdb_get(txn, db_properties, &k, &v))
        memcpy(&upstream_last_height, v.mv_data, v.mv_size);
    k.mv_data = "upstream_last_time";
    k.mv_size = strlen(k.mv_data);
    if (!mdb_get(txn, db_properties, &k, &v))
        memcpy(&upstream_last_time, v.mv_data, v.mv_size);

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
database_close(void)
{
    log_info("Closing database");
    mdb_dbi_close(env, db_shares);
    mdb_dbi_close(env, db_blocks);
    mdb_dbi_close(env, db_balance);
    mdb_dbi_close(env, db_payments);
    mdb_dbi_close(env, db_properties);
    mdb_env_close(env);
}

static int
store_share(uint64_t height, share_t *share)
{
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_cursor *cursor = NULL;
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
    rc = mdb_cursor_put(cursor, &key, &val, MDB_APPENDDUP);
    if (rc != 0)
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
store_block(uint64_t height, block_t *block)
{
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_cursor *cursor = NULL;
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
    rc = mdb_cursor_put(cursor, &key, &val, MDB_APPENDDUP);
    if (rc != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }

    rc = mdb_txn_commit(txn);
    return rc;
}

void
account_hr(double *avg, const char *address)
{
    account_t *account = NULL;
    pthread_rwlock_rdlock(&rwlock_acc);
    HASH_FIND_STR(accounts, address, account);
    if (!account)
        goto bail;
    memcpy(avg, account->hr_stats.avg, sizeof(account->hr_stats.avg));
bail:
    pthread_rwlock_unlock(&rwlock_acc);
}

uint64_t
account_balance(const char *address)
{
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_cursor *cursor = NULL;
    uint64_t balance  = 0;

    if (strlen(address) > ADDRESS_MAX)
        return balance;

    pthread_rwlock_rdlock(&rwlock_tx);

    if ((rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)) != 0)
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

    MDB_val key = {ADDRESS_MAX, (void*)address};
    MDB_val val;

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
    pthread_rwlock_unlock(&rwlock_tx);
    if (cursor)
        mdb_cursor_close(cursor);
    if (txn)
        mdb_txn_abort(txn);
    return balance;
}

static int
balance_add(const char *address, uint64_t amount, MDB_txn *parent)
{
    log_trace("Adding %"PRIu64" to %s's balance", amount, address);
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_cursor *cursor = NULL;
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
        rc = mdb_cursor_put(cursor, &key, &new_val, 0);
        if (rc != 0)
        {
            err = mdb_strerror(rc);
            log_error("%s", err);
        }
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
payout_block(block_t *block, MDB_txn *parent)
{
    /*
      PPLNS
    */
    log_info("Payout on block at height: %"PRIu64, block->height);
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_cursor *cursor = NULL;
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
        uint64_t amount = floor((double)share->difficulty /
            ((double)block->difficulty * config.share_mul) * block->reward);
        if (total_paid + amount > block->reward)
            amount = block->reward - total_paid;
        total_paid += amount;
        uint64_t fee = amount * config.pool_fee;
        amount -= fee;
        if (fee > 0 && config.pool_fee_wallet[0])
        {
            rc = balance_add(config.pool_fee_wallet, fee, txn);
            if (rc != 0)
            {
                err = mdb_strerror(rc);
                log_error("Error adding pool fee balance: %s", err);
            }
        }
        if (amount == 0)
            continue;
        rc = balance_add(share->address, amount, txn);
        if (rc != 0)
        {
            err = mdb_strerror(rc);
            log_error("%s", err);
            mdb_cursor_close(cursor);
            mdb_txn_abort(txn);
            return rc;
        }
    }

    rc = mdb_txn_commit(txn);
    return rc;
}

static int
process_blocks(block_t *blocks, size_t count)
{
    if (!abattoir)
        return 0;

    log_debug("Processing blocks");
    /*
      For each block, lookup block in db.
      If found, make sure found is locked and not orphaned.
      If both not orphaned and unlocked, payout, set unlocked.
      If block heights differ / orphaned, set orphaned.
    */
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_cursor *cursor = NULL;
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

    for (size_t i=0; i<count; i++)
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
                log_trace("No stored block at height: %"PRIu64, ib->height);
                if (rc != MDB_NOTFOUND)
                {
                    err = mdb_strerror(rc);
                    log_debug("No stored block at height: %"PRIu64
                            ", with error: %d",
                            ib->height, err);
                }
                break;
            }
            block_t *sb = (block_t*)val.mv_data;
            if (sb->status != BLOCK_LOCKED)
            {
                continue;
            }
            block_t nb;
            memcpy(&nb, sb, sizeof(block_t));
            if (memcmp(ib->hash, sb->hash, 64) != 0)
            {
                log_trace("Orphaning because hashes differ: %.64s, %.64s",
                        ib->hash, sb->hash);
                log_debug("Orphaned block at height: %"PRIu64, ib->height);
                nb.status |= BLOCK_ORPHANED;
                MDB_val new_val = {sizeof(block_t), (void*)&nb};
                mdb_cursor_put(cursor, &key, &new_val, MDB_CURRENT);
                continue;
            }
            if (memcmp(ib->prev_hash, sb->prev_hash, 64) != 0)
            {
                log_warn("Block with matching height and hash "
                        "but differing parent! "
                        "Setting orphaned.\n");
                nb.status |= BLOCK_ORPHANED;
                MDB_val new_val = {sizeof(block_t), (void*)&nb};
                mdb_cursor_put(cursor, &key, &new_val, MDB_CURRENT);
                continue;
            }
            if (ib->status & BLOCK_ORPHANED)
            {
                log_debug("Orphaned block at height: %"PRIu64, ib->height);
                nb.status |= BLOCK_ORPHANED;
                MDB_val new_val = {sizeof(block_t), (void*)&nb};
                mdb_cursor_put(cursor, &key, &new_val, MDB_CURRENT);
                continue;
            }
            nb.status |= BLOCK_UNLOCKED;
            nb.reward = ib->reward;
            if (!*config.upstream_host)
                rc = payout_block(&nb, txn);
            if (*config.upstream_host || rc == 0)
            {
                log_debug("Paid out block: %"PRIu64, nb.height);
                MDB_val new_val = {sizeof(block_t), (void*)&nb};
                mdb_cursor_put(cursor, &key, &new_val, MDB_CURRENT);
            }
            else
                log_trace("%s", mdb_strerror(rc));
        }
    }

    rc = mdb_txn_commit(txn);
    return rc;
}

static void
update_pool_hr(void)
{
    uint64_t hr = 0;
    client_t *c = (client_t*)gbag_first(bag_clients);
    while ((c = gbag_next(bag_clients, 0)))
        hr += (uint64_t) c->hr_stats.avg[0];
    log_debug("Pool hashrate: %"PRIu64, hr);
    if (upstream_event)
        return;
    pool_stats.pool_hashrate = hr;
}

static void
job_recycle(void *item)
{
    job_t *job = (job_t*) item;
    log_trace("Recycle job with extra_nonce: %u", job->extra_nonce);
    if (job->blob)
    {
        free(job->blob);
        job->blob = NULL;
    }
    if (job->submissions)
    {
        free(job->submissions);
        job->submissions = NULL;
    }
    if (job->miner_template)
    {
        block_template_t *bt = job->miner_template;
        if (bt->blocktemplate_blob)
        {
            free(bt->blocktemplate_blob);
            bt->blocktemplate_blob = NULL;
        }
        free(job->miner_template);
        job->miner_template = NULL;
    }
    memset(job, 0, sizeof(job_t));
}

static void
template_recycle(void *item)
{
    block_template_t *bt = (block_template_t*) item;
    log_trace("Recycle block template at height: %"PRIu64, bt->height);
    if (bt->blockhashing_blob)
    {
        free(bt->blockhashing_blob);
        bt->blockhashing_blob = NULL;
    }
    if (bt->blocktemplate_blob)
    {
        free(bt->blocktemplate_blob);
        bt->blocktemplate_blob = NULL;
    }
}

static uint64_t
client_target(client_t *client, job_t *job)
{
    uint32_t rtt = client->is_xnp ? 5 : config.retarget_time;
    uint64_t bd = 0xFFFFFFFFFFFFFFFF;
    uint64_t sd = config.pool_start_diff;
    double cd;
    double duration;
    unsigned idx = 0;
    uint64_t rt;

    if (config.pool_fixed_diff)
        return config.pool_fixed_diff;
    if (client->req_diff)
        sd = fmax(client->req_diff, sd);
    if (job->block_template)
        bd = job->block_template->difficulty;
    duration = difftime(time(NULL), client->connected_since);
    if (duration > hr_intervals[2])
        idx = 1;
    cd = client->hr_stats.avg[idx] * rtt;
    rt = fmin(fmax(cd, sd), bd);
    return rt;
}

static bool
retarget_required(client_t *client, job_t *job)
{
    if (config.pool_fixed_diff)
        return false;
    return ((double)job->target / client_target(client, job)
            < config.retarget_ratio);
}

static void
retarget(client_t *client, job_t *job)
{
    uint64_t target = client_target(client, job);
    job->target = target;
    log_debug("Miner %.32s target now: %"PRIu64, client->client_id, target);
}

static void
target_to_hex(uint64_t target, char *target_hex)
{
    if (target & 0xFFFFFFFF00000000)
    {
        log_debug("High target requested: %"PRIu64, target);
        bin_to_hex((const unsigned char*)&target, 8, &target_hex[0], 16);
        return;
    }
    BIGNUM *diff = BN_new();
    BIGNUM *bnt = BN_new();
#ifdef SIXTY_FOUR_BIT_LONG
    BN_set_word(bnt, target);
#else
    char tmp[24] = {0};
    snprintf(tmp, 24, "%"PRIu64, target);
    BN_dec2bn(&bnt, tmp);
#endif
    BN_div(diff, NULL, base_diff, bnt, bn_ctx);
    BN_rshift(diff, diff, 224);
    uint32_t w = BN_get_word(diff);
    bin_to_hex((const unsigned char*)&w, 4, &target_hex[0], 8);
    BN_free(bnt);
    BN_free(diff);
}

static void
stratum_get_proxy_job_body(char *body, const client_t *client,
        const char *block_hex, bool response)
{
    int json_id = client->json_id;
    const char *client_id = client->client_id;
    const job_t *job = bstack_top(client->active_jobs);
    char job_id[33] = {0};
    bin_to_hex((const unsigned char*)job->id, sizeof(uuid_t), job_id, 32);
    uint64_t target = job->target;
    char target_hex[17] = {0};
    target_to_hex(target, &target_hex[0]);
    const block_template_t *bt = job->block_template;

    if (response)
    {
        snprintf(body, JOB_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\","
                "\"error\":null,\"result\""
                ":{\"id\":\"%.32s\",\"job\":{\"blocktemplate_blob\":\"%s\","
                "\"job_id\":\"%.32s\","
                "\"difficulty\":%"PRIu64",\"height\":%"PRIu64","
                "\"reserved_offset\":%u,"
                "\"client_nonce_offset\":%u,\"client_pool_offset\":%u,"
                "\"target_diff\":%"PRIu64",\"target_diff_hex\":\"%s\","
                "\"seed_hash\":\"%.64s\",\"next_seed_hash\":\"%.64s\"},"
                "\"status\":\"OK\"}}\n",
                json_id, client_id, block_hex, job_id,
                bt->difficulty, bt->height, bt->reserved_offset,
                bt->reserved_offset + 12,
                bt->reserved_offset + 8, target, target_hex,
                bt->seed_hash, bt->next_seed_hash);
    }
    else
    {
        snprintf(body, JOB_BODY_MAX, "{\"jsonrpc\":\"2.0\",\"method\":"
                "\"job\",\"params\""
                ":{\"id\":\"%.32s\",\"job\":{\"blocktemplate_blob\":\"%s\","
                "\"job_id\":\"%.32s\","
                "\"difficulty\":%"PRIu64",\"height\":%"PRIu64","
                "\"reserved_offset\":%u,"
                "\"client_nonce_offset\":%u,\"client_pool_offset\":%u,"
                "\"target_diff\":%"PRIu64",\"target_diff_hex\":\"%s\","
                "\"seed_hash\":\"%.64s\",\"next_seed_hash\":\"%.64s\"},"
                "\"status\":\"OK\"}}\n", client_id, block_hex, job_id,
                bt->difficulty, bt->height,
                bt->reserved_offset, bt->reserved_offset + 12,
                bt->reserved_offset + 8, target, target_hex,
                bt->seed_hash, bt->next_seed_hash);
    }
}

static void
stratum_get_job_body_ss(char *body, const client_t *client, bool response)
{
    /* job_id, target, pool_wallet, extra_nonce */
    int json_id = client->json_id;
    const char *client_id = client->client_id;
    const job_t *job = bstack_top(client->active_jobs);
    char job_id[33] = {0};
    bin_to_hex((const unsigned char*)job->id, sizeof(uuid_t), job_id, 32);
    uint64_t target = job->target;
    char target_hex[17] = {0};
    target_to_hex(target, &target_hex[0]);
    char empty[] = "";
    char *seed_hash = empty;
    char *next_seed_hash = empty;
    if (job->miner_template)
    {
        seed_hash = job->miner_template->seed_hash;
        next_seed_hash = job->miner_template->next_seed_hash;
    }
    unsigned char extra_bin[8] = {0};
    memcpy(extra_bin, &job->extra_nonce, 4);
    memcpy(extra_bin+4, &instance_id, 4);
    char extra_hex[17] = {0};
    bin_to_hex(extra_bin, 8, extra_hex, 16);

    if (response)
    {
        snprintf(body, JOB_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\","
                "\"error\":null,\"result\""
                ":{\"id\":\"%.32s\",\"job\":{"
                "\"job_id\":\"%.32s\",\"target\":\"%s\","
                "\"extra_nonce\":\"%s\", \"pool_wallet\":\"%s\","
                "\"seed_hash\":\"%.64s\",\"next_seed_hash\":\"%.64s\"},"
                "\"status\":\"OK\"}}\n",
                json_id, client_id, job_id, target_hex, extra_hex,
                config.pool_wallet, seed_hash, next_seed_hash);
    }
    else
    {
        snprintf(body, JOB_BODY_MAX, "{\"jsonrpc\":\"2.0\",\"method\":"
                "\"job\",\"params\""
                ":{\"id\":\"%.32s\",\"job_id\":\"%.32s\","
                "\"target\":\"%s\","
                "\"extra_nonce\":\"%s\", \"pool_wallet\":\"%s\","
                "\"seed_hash\":\"%.64s\",\"next_seed_hash\":\"%.64s\"}}\n",
                client_id, job_id, target_hex, extra_hex, config.pool_wallet,
                seed_hash, next_seed_hash);
    }
}

static void
stratum_get_job_body(char *body, const client_t *client, bool response)
{
    int json_id = client->json_id;
    const char *client_id = client->client_id;
    const job_t *job = bstack_top(client->active_jobs);
    char job_id[33] = {0};
    bin_to_hex((const unsigned char*)job->id, sizeof(uuid_t), job_id, 32);
    const char *blob = job->blob;
    uint64_t target = job->target;
    uint64_t height = job->block_template->height;
    char target_hex[17] = {0};
    target_to_hex(target, &target_hex[0]);
    char *seed_hash = job->block_template->seed_hash;
    char *next_seed_hash = job->block_template->next_seed_hash;

    if (response)
    {
        snprintf(body, JOB_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\","
                "\"error\":null,\"result\""
                ":{\"id\":\"%.32s\",\"job\":{"
                "\"blob\":\"%s\",\"job_id\":\"%.32s\",\"target\":\"%s\","
                "\"height\":%"PRIu64",\"seed_hash\":\"%.64s\","
                "\"next_seed_hash\":\"%.64s\"},"
                "\"status\":\"OK\"}}\n",
                json_id, client_id, blob, job_id, target_hex, height,
                seed_hash, next_seed_hash);
    }
    else
    {
        snprintf(body, JOB_BODY_MAX, "{\"jsonrpc\":\"2.0\",\"method\":"
                "\"job\",\"params\""
                ":{\"id\":\"%.32s\",\"blob\":\"%s\",\"job_id\":\"%.32s\","
                "\"target\":\"%s\","
                "\"height\":%"PRIu64",\"seed_hash\":\"%.64s\","
                "\"next_seed_hash\":\"%.64s\"}}\n",
                client_id, blob, job_id, target_hex, height,
                seed_hash, next_seed_hash);
    }
}

static inline void
stratum_get_error_body(char *body, int json_id, const char *error)
{
    snprintf(body, ERROR_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\","
            "\"error\":"
            "{\"code\":-1, \"message\":\"%s\"}}\n", json_id, error);
}

static inline void
stratum_get_status_body(char *body, int json_id, const char *status)
{
    snprintf(body, STATUS_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\","
            "\"error\":null,\"result\":{\"status\":\"%s\"}}\n",
            json_id, status);
}

static void
send_validation_error(const client_t *client, const char *message)
{
    struct evbuffer *output = bufferevent_get_output(client->bev);
    char body[ERROR_BODY_MAX] = {0};
    stratum_get_error_body(body, client->json_id, message);
    evbuffer_add(output, body, strlen(body));
    log_debug("[%s:%d] Validation error: %s",
            client->host, client->port, message);
}

static void
client_clear_jobs(client_t *client)
{
    if (!client->active_jobs)
        return;
    bstack_free(client->active_jobs);
    client->active_jobs = NULL;
}

static job_t *
client_find_job(client_t *client, const char *job_id)
{
    uuid_t jid;
    job_t *job = NULL;
    hex_to_bin(job_id, strlen(job_id), (unsigned char*)&jid, sizeof(uuid_t));
    bstack_reset(client->active_jobs);
    while ((job = bstack_next(client->active_jobs)))
    {
        if (memcmp(job->id, jid, sizeof(uuid_t)) == 0)
            break;
    }
    return job;
}

static void
miner_send_job(client_t *client, bool response)
{
    job_t *job = bstack_push(client->active_jobs, NULL);
    block_template_t *bt = bstack_top(bst);
    job->block_template = bt;

    if (client->mode == MODE_SELF_SELECT)
    {
        uuid_generate(job->id);
        retarget(client, job);
        ++extra_nonce;
        job->extra_nonce = extra_nonce;
        char body[JOB_BODY_MAX] = {0};
        stratum_get_job_body_ss(body, client, response);
        log_trace("Miner job: %s", body);
        struct evbuffer *output = bufferevent_get_output(client->bev);
        evbuffer_add(output, body, strlen(body));
        return;
    }

    /* Quick check we actually have a block template */
    if (!bt)
    {
        log_warn("Cannot send client a job: No block template");
        return;
    }

    /*
      1. Convert blocktemplate_blob to binary
      2. Update bytes in reserved space at reserved_offset
      3. Get block hashing blob for job
      4. Send
    */

    /* Convert template to blob */
    size_t hex_size = strlen(bt->blocktemplate_blob);
    size_t bin_size = hex_size >> 1;
    unsigned char *block = calloc(bin_size, sizeof(char));
    hex_to_bin(bt->blocktemplate_blob, hex_size, block, bin_size);

    /* Set the extra nonce in our reserved space */
    unsigned char *p = block;
    p += bt->reserved_offset;
    ++extra_nonce;
    memcpy(p, &extra_nonce, sizeof(extra_nonce));
    job->extra_nonce = extra_nonce;

    /* Add our instance ID */
    p += 4;
    memcpy(p, &instance_id, sizeof(instance_id));

    /* Get hashing blob */
    size_t hashing_blob_size = 0;
    unsigned char *hashing_blob = NULL;
    get_hashing_blob(block, bin_size, &hashing_blob, &hashing_blob_size);

    /* Make hex */
    job->blob = calloc((hashing_blob_size << 1) +1, sizeof(char));
    bin_to_hex(hashing_blob, hashing_blob_size, job->blob,
            hashing_blob_size << 1);
    log_trace("Miner hashing blob: %s", job->blob);

    /* Save a job id */
    uuid_generate(job->id);

    /* Send */
    char job_id[33] = {0};
    bin_to_hex((const unsigned char*)job->id, sizeof(uuid_t), &job_id[0], 32);

    /* Retarget */
    retarget(client, job);

    char body[JOB_BODY_MAX] = {0};
    if (!client->is_xnp)
    {
        stratum_get_job_body(body, client, response);
    }
    else
    {
        char *block_hex = calloc(hex_size+1, sizeof(char));
        bin_to_hex(block, bin_size, block_hex, hex_size);
        stratum_get_proxy_job_body(body, client, block_hex, response);
        free(block_hex);
    }
    log_trace("Miner job: %.*s", strlen(body)-1, body);
    struct evbuffer *output = bufferevent_get_output(client->bev);
    evbuffer_add(output, body, strlen(body));
    free(block);
    free(hashing_blob);
}

static void
accounts_moved(const void *items, size_t count)
{
    account_t *s, *e, *r;
    s = (account_t*) items;
    e = s + count;
    pthread_rwlock_wrlock(&rwlock_acc);
    while (s<e)
    {
        HASH_REPLACE_STR(accounts, address, s, r);
        s++;
    }
    pthread_rwlock_unlock(&rwlock_acc);
}

static void
clients_moved(const void *items, size_t count)
{
    client_t *s, *e, *r;
    s = (client_t*) items;
    e = s + count;
    pthread_rwlock_wrlock(&rwlock_cfd);
    while (s<e)
    {
        HASH_REPLACE_INT(clients_by_fd, fd, s, r);
        s++;
    }
    pthread_rwlock_unlock(&rwlock_cfd);
}

static void
clients_send_job(void)
{
    client_t *c = (client_t*) gbag_first(bag_clients);
    while ((c = gbag_next(bag_clients, 0)))
    {
        if (c->fd == 0 || c->address[0] == 0 || c->downstream)
            continue;
        miner_send_job(c, false);
    }
}

static void
clients_init(void)
{
    gbag_new(&bag_accounts, CLIENTS_INIT, sizeof(account_t), 0,
            accounts_moved);
    gbag_new(&bag_clients, CLIENTS_INIT, sizeof(client_t), 0,
            clients_moved);
}

static void
clients_free(void)
{
    if (!(bag_accounts && bag_clients))
        return;

    client_t *c = (client_t*) gbag_first(bag_clients);
    while ((c = gbag_next(bag_clients, 0)))
    {
        if (!c->active_jobs)
            continue;
        client_clear_jobs(c);
    }
    pthread_rwlock_wrlock(&rwlock_cfd);
    HASH_CLEAR(hh, clients_by_fd);
    gbag_free(bag_clients);
    pthread_rwlock_unlock(&rwlock_cfd);

    pthread_rwlock_wrlock(&rwlock_acc);
    HASH_CLEAR(hh, accounts);
    gbag_free(bag_accounts);
    pthread_rwlock_unlock(&rwlock_acc);
}

static void
response_to_block_template(json_object *result,
        block_template_t *block_template)
{
    JSON_GET_OR_WARN(blockhashing_blob, result, json_type_string);
    JSON_GET_OR_WARN(blocktemplate_blob, result, json_type_string);
    JSON_GET_OR_WARN(difficulty, result, json_type_int);
    JSON_GET_OR_WARN(height, result, json_type_int);
    JSON_GET_OR_WARN(prev_hash, result, json_type_string);
    JSON_GET_OR_WARN(reserved_offset, result, json_type_int);
    block_template->blockhashing_blob = strdup(
            json_object_get_string(blockhashing_blob));
    block_template->blocktemplate_blob = strdup(
            json_object_get_string(blocktemplate_blob));
    block_template->difficulty = json_object_get_int64(difficulty);
    block_template->height = json_object_get_int64(height);
    strncpy(block_template->prev_hash, json_object_get_string(prev_hash), 64);
    block_template->reserved_offset = json_object_get_int(reserved_offset);

    unsigned int major_version = 0;
    sscanf(block_template->blocktemplate_blob, "%2x", &major_version);
    uint8_t pow_variant = major_version >= 7 ? major_version - 6 : 0;
    log_trace("Variant: %u", pow_variant);

    if (pow_variant >= 6)
    {
        JSON_GET_OR_WARN(seed_hash, result, json_type_string);
        JSON_GET_OR_WARN(next_seed_hash, result, json_type_string);
        strncpy(block_template->seed_hash,
                json_object_get_string(seed_hash), 64);
        strncpy(block_template->next_seed_hash,
                json_object_get_string(next_seed_hash), 64);
    }
}

static void
response_to_block(json_object *block_header, block_t *block)
{
    memset(block, 0, sizeof(block_t));
    JSON_GET_OR_WARN(height, block_header, json_type_int);
    JSON_GET_OR_WARN(difficulty, block_header, json_type_int);
    JSON_GET_OR_WARN(hash, block_header, json_type_string);
    JSON_GET_OR_WARN(prev_hash, block_header, json_type_string);
    JSON_GET_OR_WARN(timestamp, block_header, json_type_int);
    JSON_GET_OR_WARN(reward, block_header, json_type_int);
    JSON_GET_OR_WARN(orphan_status, block_header, json_type_boolean);
    block->height = json_object_get_int64(height);
    block->difficulty = json_object_get_int64(difficulty);
    strncpy(block->hash, json_object_get_string(hash), 64);
    strncpy(block->prev_hash, json_object_get_string(prev_hash), 64);
    block->timestamp = json_object_get_int64(timestamp);
    block->reward = json_object_get_int64(reward);
    if (json_object_get_int(orphan_status))
        block->status |= BLOCK_ORPHANED;
}

static void
rpc_on_response(struct evhttp_request *req, void *arg)
{
    struct evbuffer *input;
    rpc_callback_t *callback = (rpc_callback_t*) arg;

    if (!req)
    {
        log_error("Request failure. Aborting.");
        rpc_callback_free(callback);
        return;
    }

    int rc = evhttp_request_get_response_code(req);
    if (rc < 200 || rc >= 300)
    {
        log_error("HTTP status code %d for %s. Aborting.",
                rc, evhttp_request_get_uri(req));
        rpc_callback_free(callback);
        return;
    }

    input = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(input);
    char body[len+1];
    evbuffer_remove(input, body, len);
    body[len] = '\0';
    callback->cf(body, callback);
    rpc_callback_free(callback);
}

static void
rpc_request(struct event_base *base, const char *body,
        rpc_callback_t *callback)
{
    struct evhttp_connection *con;
    struct evhttp_request *req;
    struct evkeyvalq *headers;
    struct evbuffer *output;

    con = evhttp_connection_base_new(base, NULL,
            config.rpc_host, config.rpc_port);
    evhttp_connection_free_on_completion(con);
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
rpc_wallet_request(struct event_base *base, const char *body,
        rpc_callback_t *callback)
{
    struct evhttp_connection *con;
    struct evhttp_request *req;
    struct evkeyvalq *headers;
    struct evbuffer *output;

    con = evhttp_connection_base_new(base, NULL,
            config.wallet_rpc_host, config.wallet_rpc_port);
    evhttp_connection_free_on_completion(con);
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
rpc_get_request_body(char *body, const char *method, char *fmt, ...)
{
    char *pb = body;
    char *end = body + RPC_BODY_MAX;

    pb = stecpy(pb, "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"", end);
    pb = stecpy(pb, method, end);
    pb = stecpy(pb, "\"", end);

    if (fmt && *fmt)
    {
        char *s;
        uint64_t d;
        pb = stecpy(pb, ",\"params\":{", end);
        va_list args;
        va_start(args, fmt);
        uint8_t count = 0;
        while (*fmt)
        {
            switch (*fmt++)
            {
                case 's':
                    s = va_arg(args, char *);
                    pb = stecpy(pb, "\"", end);
                    pb = stecpy(pb, s, end);
                    pb = stecpy(pb, "\"", end);
                    break;
                case 'd':
                    d = va_arg(args, uint64_t);
                    char tmp[24] = {0};
                    snprintf(tmp, 24, "%"PRIu64, d);
                    pb = stecpy(pb, tmp, end);
                    break;
            }
            *pb++ = count++ % 2 ? ',' : ':';
        }
        va_end(args);
        *--pb = '}';
        pb++;
    }
    *pb++ = '}';
    *pb = '\0';
    log_trace("Payload: %s", body);
}

static void
rpc_on_block_header_by_height(const char* data, rpc_callback_t *callback)
{
    log_trace("Got block header by height: \n%s", data);
    json_object *root = json_tokener_parse(data);
    JSON_GET_OR_WARN(result, root, json_type_object);
    JSON_GET_OR_WARN(status, result, json_type_string);
    const char *ss = json_object_get_string(status);
    json_object *error = NULL;
    json_object_object_get_ex(root, "error", &error);
    if (error)
    {
        JSON_GET_OR_WARN(code, error, json_type_object);
        JSON_GET_OR_WARN(message, error, json_type_string);
        int ec = json_object_get_int(code);
        const char *em = json_object_get_string(message);
        log_error("Error (%d) getting block header by height: %s", ec, em);
        json_object_put(root);
        return;
    }
    if (!status || strcmp(ss, "OK") != 0)
    {
        log_error("Error getting block header by height: %s", ss);
        json_object_put(root);
        return;
    }
    block_t rb;
    JSON_GET_OR_WARN(block_header, result, json_type_object);
    response_to_block(block_header, &rb);
    process_blocks(&rb, 1);
    json_object_put(root);
}

static void
rpc_on_block_headers_range(const char* data, rpc_callback_t *callback)
{
    json_object *root = json_tokener_parse(data);
    JSON_GET_OR_WARN(result, root, json_type_object);
    JSON_GET_OR_WARN(status, result, json_type_string);
    const char *ss = json_object_get_string(status);
    json_object *error = NULL;
    json_object_object_get_ex(root, "error", &error);
    if (error)
    {
        JSON_GET_OR_WARN(code, error, json_type_object);
        JSON_GET_OR_WARN(message, error, json_type_string);
        int ec = json_object_get_int(code);
        const char *em = json_object_get_string(message);
        log_warn("Error (%d) getting block headers by range: %s", ec, em);
        json_object_put(root);
        return;
    }
    if (!status || strcmp(ss, "OK") != 0)
    {
        log_warn("Error getting block headers by range: %s", ss);
        json_object_put(root);
        return;
    }

    JSON_GET_OR_WARN(headers, result, json_type_array);
    size_t headers_len = json_object_array_length(headers);
    for (size_t i=0; i<headers_len; i++)
    {
        json_object *header = json_object_array_get_idx(headers, i);
        block_t *bh = &block_headers_range[i];
        response_to_block(header, bh);
    }
    process_blocks(block_headers_range, BLOCK_HEADERS_RANGE);
    json_object_put(root);
}

static void
rpc_on_block_template(const char* data, rpc_callback_t *callback)
{
    log_trace("Got block template: \n%s", data);
    json_object *root = json_tokener_parse(data);
    JSON_GET_OR_WARN(result, root, json_type_object);
    JSON_GET_OR_WARN(status, result, json_type_string);
    const char *ss = json_object_get_string(status);
    json_object *error = NULL;
    json_object_object_get_ex(root, "error", &error);
    if (error)
    {
        JSON_GET_OR_WARN(code, error, json_type_object);
        JSON_GET_OR_WARN(message, error, json_type_string);
        int ec = json_object_get_int(code);
        const char *em = json_object_get_string(message);
        log_error("Error (%d) getting block template: %s", ec, em);
        json_object_put(root);
        return;
    }
    if (!status || strcmp(ss, "OK") != 0)
    {
        log_error("Error getting block template: %s", ss);
        json_object_put(root);
        return;
    }
    pool_stats.last_template_fetched = time(NULL);
    block_template_t *top = (block_template_t*) bstack_push(bst, NULL);
    response_to_block_template(result, top);
    clients_send_job();
    json_object_put(root);
}

static int
startup_scan_round_shares()
{
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_cursor *cursor = NULL;

    if (*config.upstream_host)
        return 0;

    if ((rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)) != 0)
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
    MDB_cursor_op op = MDB_LAST;
    while (1)
    {
        MDB_val key;
        MDB_val val;
        rc = mdb_cursor_get(cursor, &key, &val, op);
        if (rc != 0 && rc != MDB_NOTFOUND)
        {
            err = mdb_strerror(rc);
            log_error("%s", err);
            break;
        }
        if (rc == MDB_NOTFOUND)
            break;
        op = MDB_PREV;
        share_t *share = (share_t*)val.mv_data;
        if (share->timestamp > pool_stats.last_block_found)
            pool_stats.round_hashes += share->difficulty;
        else
            break;
    }
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    return 0;
}

static int
startup_payout(uint64_t height)
{
    /*
      Loop stored blocks < height - 60
      If block locked & not orphaned, payout
    */
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_cursor *cursor = NULL;
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

        if (!upstream_event)
            pool_stats.last_block_found = block->timestamp;

        if (block->height > height - 60)
            continue;
        if (block->status != BLOCK_LOCKED)
            continue;

        char body[RPC_BODY_MAX] = {0};
        rpc_get_request_body(body, "get_block_header_by_height", "sd",
                "height", block->height);
        rpc_callback_t *cb = rpc_callback_new(
                rpc_on_block_header_by_height, 0, 0);
        rpc_request(pool_base, body, cb);
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    return 0;
}

static void
rpc_on_view_key(const char* data, rpc_callback_t *callback)
{
    json_object *root = json_tokener_parse(data);
    JSON_GET_OR_WARN(result, root, json_type_object);
    JSON_GET_OR_WARN(key, result, json_type_string);
    json_object *error = NULL;
    json_object_object_get_ex(root, "error", &error);
    if (error)
    {
        JSON_GET_OR_WARN(code, error, json_type_object);
        JSON_GET_OR_WARN(message, error, json_type_string);
        int ec = json_object_get_int(code);
        const char *em = json_object_get_string(message);
        log_error("Error (%d) getting key: %s", ec, em);
        json_object_put(root);
        return;
    }
    const char *vk = json_object_get_string(key);
    hex_to_bin(vk, strlen(vk), &sec_view[0], 32);
    json_object_put(root);
}

static void
rpc_on_last_block_header(const char* data, rpc_callback_t *callback)
{
    log_trace("Got last block header: \n%s", data);
    json_object *root = json_tokener_parse(data);
    JSON_GET_OR_WARN(result, root, json_type_object);
    JSON_GET_OR_WARN(status, result, json_type_string);
    const char *ss = json_object_get_string(status);
    json_object *error = NULL;
    json_object_object_get_ex(root, "error", &error);
    if (error)
    {
        JSON_GET_OR_WARN(code, error, json_type_object);
        JSON_GET_OR_WARN(message, error, json_type_string);
        int ec = json_object_get_int(code);
        const char *em = json_object_get_string(message);
        log_error("Error (%d) getting last block header: %s", ec, em);
        json_object_put(root);
        return;
    }
    if (!status || strcmp(ss, "OK") != 0)
    {
        log_error("Error getting last block header: %s", ss);
        json_object_put(root);
        return;
    }

    JSON_GET_OR_WARN(block_header, result, json_type_object);
    JSON_GET_OR_WARN(height, block_header, json_type_int);
    uint64_t bh = json_object_get_int64(height);
    bool need_new_template = false;
    block_t *top = bstack_top(bsh);
    if (top && bh > top->height)
    {
        need_new_template = true;
        block_t *block = bstack_push(bsh, NULL);
        response_to_block(block_header, block);
    }
    else if (!top)
    {
        block_t *block = bstack_push(bsh, NULL);
        response_to_block(block_header, block);
        startup_payout(block->height);
        startup_scan_round_shares();
        need_new_template = true;
    }

    top = bstack_top(bsh);
    pool_stats.network_difficulty = top->difficulty;
    pool_stats.network_hashrate = top->difficulty / BLOCK_TIME;
    pool_stats.network_height = top->height;
    update_pool_hr();

    if (need_new_template)
    {
        log_info("Fetching new block template");
        char body[RPC_BODY_MAX] = {0};
        uint64_t reserve = 17;
        rpc_get_request_body(body, "get_block_template", "sssd",
                "wallet_address", config.pool_wallet, "reserve_size", reserve);
        rpc_callback_t *cb1 = rpc_callback_new(rpc_on_block_template, 0, 0);
        rpc_request(pool_base, body, cb1);

        uint64_t end = top->height - 60;
        uint64_t start = end - BLOCK_HEADERS_RANGE + 1;
        rpc_get_request_body(body, "get_block_headers_range", "sdsd",
                "start_height", start, "end_height", end);
        rpc_callback_t *cb2 = rpc_callback_new(
                rpc_on_block_headers_range, 0, 0);
        rpc_request(pool_base, body, cb2);
    }

    json_object_put(root);
}

static void
rpc_on_block_submitted(const char* data, rpc_callback_t *callback)
{
    json_object *root = json_tokener_parse(data);
    JSON_GET_OR_WARN(result, root, json_type_object);
    JSON_GET_OR_WARN(status, result, json_type_string);
    const char *ss = json_object_get_string(status);
    json_object *error = NULL;
    json_object_object_get_ex(root, "error", &error);
    /*
      The RPC reports submission as an error even when it's added as
      an alternative block. Thus, still store it. This doesn't matter
      as upon payout, blocks are checked whether they are orphaned or not.
    */
    if (error)
    {
        JSON_GET_OR_WARN(code, error, json_type_object);
        JSON_GET_OR_WARN(message, error, json_type_string);
        int ec = json_object_get_int(code);
        const char *em = json_object_get_string(message);
        log_warn("Error (%d) with block submission: %s", ec, em);
    }
    if (!status || strcmp(ss, "OK") != 0)
    {
        log_warn("Error submitting block: %s", ss);
    }
    pool_stats.pool_blocks_found++;
    block_t *b = (block_t*)callback->data;
    if (!upstream_event)
    {
        pool_stats.last_block_found = b->timestamp;
        pool_stats.round_hashes = 0;
    }
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
    json_object *root = json_tokener_parse(data);
    JSON_GET_OR_WARN(result, root, json_type_object);
    json_object *error = NULL;
    json_object_object_get_ex(root, "error", &error);
    if (error)
    {
        JSON_GET_OR_WARN(code, error, json_type_object);
        JSON_GET_OR_WARN(message, error, json_type_string);
        int ec = json_object_get_int(code);
        const char *em = json_object_get_string(message);
        log_error("Error (%d) with wallet transfer: %s", ec, em);
    }
    else
        log_info("Payout transfer successful");

    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_cursor *cursor = NULL;

    /* First, updated balance(s) */
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
    gbag_t *bag_pay = (gbag_t*) callback->data;
    payment_t *p = (payment_t*) gbag_first(bag_pay);
    while((p = gbag_next(bag_pay, 0)))
    {
        MDB_cursor_op op = MDB_SET;
        MDB_val key = {ADDRESS_MAX, (void*)p->address};
        MDB_val val;
        rc = mdb_cursor_get(cursor, &key, &val, op);
        if (rc == MDB_NOTFOUND)
        {
            log_error("Payment made to non-existent address");
            continue;
        }
        else if (rc != 0 && rc != MDB_NOTFOUND)
        {
            err = mdb_strerror(rc);
            log_error("%s", err);
            continue;
        }
        uint64_t current_amount = *(uint64_t*)val.mv_data;

        if (current_amount >= p->amount)
        {
            current_amount -= p->amount;
        }
        else
        {
            log_error("Payment was more than balance: %"PRIu64" > %"PRIu64,
                      p->amount, current_amount);
            current_amount = 0;
        }

        if (error)
        {
            log_warn("Error seen on transfer for %s with amount %"PRIu64,
                    p->address, p->amount);
        }
        MDB_val new_val = {sizeof(current_amount), (void*)&current_amount};
        rc = mdb_cursor_put(cursor, &key, &new_val, MDB_CURRENT);
        if (rc != 0)
        {
            err = mdb_strerror(rc);
            log_error("%s", err);
        }
    }
    if ((rc = mdb_txn_commit(txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("Error committing updated balance(s): %s", err);
        mdb_txn_abort(txn);
        goto cleanup;
    }

    /* Now store payment info */
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
    time_t now = time(NULL);
    p = (payment_t*) gbag_first(bag_pay);
    while((p = gbag_next(bag_pay, 0)))
    {
        p->timestamp = now;
        MDB_val key = {ADDRESS_MAX, (void*)p->address};
        MDB_val val = {sizeof(payment_t), p};
        if ((rc = mdb_cursor_put(cursor, &key, &val, MDB_APPENDDUP)) != 0)
        {
            err = mdb_strerror(rc);
            log_error("Error putting payment: %s", err);
            continue;
        }
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

static int
send_payments(void)
{
    if (*config.upstream_host || config.disable_payouts)
        return 0;
    uint64_t threshold = 1000000000000 * config.payment_threshold;
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_cursor *cursor = NULL;
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

    gbag_t *bag_pay = NULL;
    gbag_new(&bag_pay, 25, sizeof(payment_t), 0, 0);

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

        log_info("Sending payment: %"PRIu64", %.8s", amount, address);

        payment_t *p = (payment_t*) gbag_get(bag_pay);
        strncpy(p->address, address, ADDRESS_MAX-1);
        p->amount = amount;
    }
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);

    size_t proc = gbag_used(bag_pay);
    if (proc)
    {
        size_t body_size = 160 * proc + 128;
        char body[body_size];
        char *start = body;
        char *end = body + body_size;
        start = stecpy(start, "{\"id\":\"0\",\"jsonrpc\":\"2.0\",\"method\":"
                "\"transfer_split\",\"params\":{"
                "\"ring_size\":11,\"destinations\":[", end);
        payment_t *p = (payment_t*) gbag_first(bag_pay);
        while ((p = gbag_next(bag_pay, 0)))
        {
            start = stecpy(start, "{\"address\":\"", end);
            start = stecpy(start, p->address, end);
            start = stecpy(start, "\",\"amount\":", end);
            sprintf(start, "%"PRIu64"}", p->amount);
            start = body + strlen(body);
            if (--proc)
                start = stecpy(start, ",", end);
            else
                start = stecpy(start, "]}}", end);
        }
        log_trace(body);
        rpc_callback_t *cb = rpc_callback_new(
                rpc_on_wallet_transferred, bag_pay, rpc_bag_free);
        rpc_wallet_request(pool_base, body, cb);
    }
    else
        gbag_free(bag_pay);

    return 0;
}

static void
fetch_view_key(void)
{
    if (*config.pool_view_key)
    {
        hex_to_bin(config.pool_view_key, 64, sec_view, 32);
        log_info("Using pool view-key: %.4s<hidden>", config.pool_view_key);
        return;
    }
    if (*sec_view)
        return;
    char body[RPC_BODY_MAX] = {0};
    rpc_get_request_body(body, "query_key", "ss", "key_type", "view_key");
    rpc_callback_t *cb = rpc_callback_new(rpc_on_view_key, 0, 0);
    rpc_wallet_request(pool_base, body, cb);
}

static void
fetch_last_block_header(void)
{
    log_info("Fetching last block header");
    char body[RPC_BODY_MAX] = {0};
    rpc_get_request_body(body, "get_last_block_header", NULL);
    rpc_callback_t *cb = rpc_callback_new(rpc_on_last_block_header, 0, 0);
    rpc_request(pool_base, body, cb);
}

static int
store_last_height_time()
{
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_val k, v;
    if ((rc = mdb_txn_begin(env, NULL, 0, &txn)))
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return rc;
    }
    k.mv_data = "upstream_last_height";
    k.mv_size = strlen(k.mv_data);
    v.mv_data = &upstream_last_height;
    v.mv_size = sizeof(upstream_last_height);
    if ((rc = mdb_put(txn, db_properties, &k, &v, 0)))
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }
    k.mv_data = "upstream_last_time";
    k.mv_size = strlen(k.mv_data);
    v.mv_data = &upstream_last_time;
    v.mv_size = sizeof(upstream_last_time);
    if ((rc = mdb_put(txn, db_properties, &k, &v, 0)))
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }
    if ((rc = mdb_txn_commit(txn)))
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return rc;
    }
    return rc;
}

static void
trusted_send_stats(client_t *client)
{
    struct evbuffer *output = bufferevent_get_output(client->bev);
    size_t z = 9 + sizeof(pool_stats);
    char data[z];
    int t = BIN_STATS;
    memcpy(data, msgbin, 8);
    memcpy(data+8, &t, 1);
    memcpy(data+9, &pool_stats, z-9);
    evbuffer_add(output, data, z);
}

static void
trusted_send_balance(client_t *client, const char *address)
{
    struct evbuffer *output = bufferevent_get_output(client->bev);
    size_t z = 9 + sizeof(uint64_t) + ADDRESS_MAX;
    char data[z];
    int t = BIN_BALANCE;
    memcpy(data, msgbin, 8);
    memcpy(data+8, &t, 1);
    uint64_t balance = account_balance(address);
    memcpy(data+9, &balance, sizeof(uint64_t));
    memcpy(data+9+sizeof(uint64_t), address, ADDRESS_MAX);
    evbuffer_add(output, data, z);
}

static void
upstream_send_ping()
{
    struct evbuffer *output = bufferevent_get_output(upstream_event);
    char data[9];
    int t = BIN_PING;
    memcpy(data, msgbin, 8);
    memcpy(data+8, &t, 1);
    evbuffer_add(output, data, 9);
    log_trace("Sending message ping upstream");
}

static void
upstream_send_account_connect(uint32_t count)
{
    struct evbuffer *output = bufferevent_get_output(upstream_event);
    size_t z = 9 + sizeof(uint32_t);
    char data[z];
    int t = BIN_CONNECT;
    memcpy(data, msgbin, 8);
    memcpy(data+8, &t, 1);
    memcpy(data+9, &count, z-9);
    evbuffer_add(output, data, z);
    log_trace("Sending message account connect upstream");
}

static void
upstream_send_account_disconnect()
{
    struct evbuffer *output = bufferevent_get_output(upstream_event);
    char data[9];
    int t = BIN_DISCONNECT;
    memcpy(data, msgbin, 8);
    memcpy(data+8, &t, 1);
    evbuffer_add(output, data, 9);
    log_trace("Sending message disconnect upstream");
}

static void
upstream_send_client_share(share_t *share)
{
    struct evbuffer *output = bufferevent_get_output(upstream_event);
    size_t z = 9 + sizeof(share_t);
    char data[z];
    int t = BIN_SHARE;
    memcpy(data, msgbin, 8);
    memcpy(data+8, &t, 1);
    memcpy(data+9, share, z-9);
    evbuffer_add(output, data, z);
    bool update_req = false;
    if (share->height > upstream_last_height)
    {
        upstream_last_height = share->height;
        update_req = true;
    }
    if (share->timestamp > upstream_last_time)
    {
        upstream_last_time = share->timestamp;
        update_req = true;
    }
    if (update_req)
        store_last_height_time();
    log_trace("Sending share upstream: %"PRIu64", %"PRIu64", %"PRIu64,
            share->difficulty, share->height, share->timestamp);
}

static void
upstream_send_client_block(block_t *block)
{
    struct evbuffer *output = bufferevent_get_output(upstream_event);
    size_t z = 9 + sizeof(block_t);
    char data[z];
    int t = BIN_BLOCK;
    memcpy(data, msgbin, 8);
    memcpy(data+8, &t, 1);
    memcpy(data+9, block, z-9);
    evbuffer_add(output, data, z);
    bool update_req = false;
    if (block->height > upstream_last_height)
    {
        upstream_last_height = block->height;
        update_req = true;
    }
    if (block->timestamp > upstream_last_time)
    {
        upstream_last_time = block->timestamp;
        update_req = true;
    }
    if (update_req)
        store_last_height_time();
    log_info("Sending block upstream: %.8s, %d, %d",
            block->hash, block->height, block->timestamp);
}

static void
upstream_send_backlog()
{
    /*
      Send any unsent shares and blocks upstream.
    */
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    MDB_cursor *curshr = NULL, *curblk = NULL;
    if (!upstream_last_height || !upstream_last_time || !upstream_event)
        return;
    log_info("Sending upstream shares/blocks since: %"PRIu64", %"PRIu64,
            upstream_last_height, upstream_last_time);
    if ((rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)))
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return;
    }
    if ((rc = mdb_cursor_open(txn, db_shares, &curshr)))
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return;
    }
    if ((rc = mdb_cursor_open(txn, db_blocks, &curblk)))
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return;
    }
    uint64_t h = upstream_last_height;
    time_t t = upstream_last_time;
    MDB_cursor_op op = MDB_SET;
    while (1)
    {
        MDB_val k, v;
        if (op == MDB_SET)
        {
            k.mv_size = sizeof(h);
            k.mv_data = &h;
        }
        if (mdb_cursor_get(curshr, &k, &v, op))
            break;
        op = MDB_NEXT;
        share_t *s = (share_t*) v.mv_data;
        if (s->timestamp <= t)
            continue;
        upstream_send_client_share(s);
    }
    op = MDB_SET;
    while (1)
    {
        MDB_val k, v;
        if (op == MDB_SET)
        {
            k.mv_size = sizeof(h);
            k.mv_data = &h;
        }
        if (mdb_cursor_get(curblk, &k, &v, op))
            break;
        op = MDB_NEXT;
        block_t *b = (block_t*) v.mv_data;
        if (b->timestamp <= t)
            continue;
        upstream_send_client_block(b);
    }
    mdb_cursor_close(curshr);
    mdb_cursor_close(curblk);
    mdb_txn_abort(txn);
    upstream_send_account_connect(pool_stats.connected_accounts);
}

static void
trusted_on_account_connect(client_t *client)
{
    struct evbuffer *input = bufferevent_get_input(client->bev);
    uint32_t count;
    evbuffer_remove(input, &count, sizeof(uint32_t));
    pool_stats.connected_accounts += count;
    client->downstream_accounts += count;
    log_trace("Downstream account connected");
    trusted_send_stats(client);
    if (upstream_event)
        upstream_send_account_connect(count);
    log_trace("Pool accounts: %d, workers: %d, hashrate: %"PRIu64,
            pool_stats.connected_accounts,
            gbag_used(bag_clients),
            pool_stats.pool_hashrate);
}

static void
trusted_on_account_disconnect(client_t *client)
{
    pool_stats.connected_accounts--;
    if (client->downstream_accounts)
        client->downstream_accounts--;
    log_trace("Downstream account disconnected");
    trusted_send_stats(client);
    if (upstream_event)
        upstream_send_account_disconnect();
    log_trace("Pool accounts: %d, workers: %d, hashrate: %"PRIu64,
            pool_stats.connected_accounts,
            gbag_used(bag_clients),
            pool_stats.pool_hashrate);
}

static void
trusted_on_client_share(client_t *client)
{
    /*
      Downstream validated, so just store for payouts.
    */
    struct evbuffer *input = bufferevent_get_input(client->bev);
    share_t s;
    int rc = 0;
    evbuffer_remove(input, (void*)&s, sizeof(share_t));
    log_debug("Received share from downstream with difficulty: %"PRIu64,
            s.difficulty);
    client->hashes += s.difficulty;
    pool_stats.round_hashes += s.difficulty;
    client->hr_stats.diff_since += s.difficulty;
    hr_update(&client->hr_stats);
    rc = store_share(s.height, &s);
    if (rc != 0)
        log_warn("Failed to store share: %s", mdb_strerror(rc));
    trusted_send_stats(client);
    trusted_send_balance(client, s.address);
    if (upstream_event)
        upstream_send_client_share(&s);
}

static void
trusted_on_client_block(client_t *client)
{
    struct evbuffer *input = bufferevent_get_input(client->bev);
    block_t b;
    int rc = 0;
    evbuffer_remove(input, (void*)&b, sizeof(block_t));
    pool_stats.pool_blocks_found++;
    pool_stats.last_block_found = b.timestamp;
    pool_stats.round_hashes = 0;
    log_info("Block submitted by downstream: %.8s, %"PRIu64, b.hash, b.height);
    rc = store_block(b.height, &b);
    if (rc != 0)
        log_warn("Failed to store block: %s", mdb_strerror(rc));
    trusted_send_stats(client);
    if (upstream_event)
        upstream_send_client_block(&b);
}

static void
upstream_on_stats(struct bufferevent *bev)
{
    struct evbuffer *input = bufferevent_get_input(bev);
    evbuffer_remove(input, &pool_stats, sizeof(pool_stats_t));
    log_trace("Stats from upstream: "
            "%d, %"PRIu64", %"PRIu64", %d, %"PRIu64,
            pool_stats.connected_accounts,
            pool_stats.pool_hashrate,
            pool_stats.round_hashes,
            pool_stats.pool_blocks_found,
            pool_stats.last_block_found);
}

static int
upstream_on_balance(struct bufferevent *bev)
{
    int rc = 0;
    char *err = NULL;
    MDB_txn *txn = NULL;
    uint64_t balance = 0;
    char address[ADDRESS_MAX];
    struct evbuffer *input = bufferevent_get_input(bev);
    evbuffer_remove(input, &balance, sizeof(uint64_t));
    evbuffer_remove(input, address, ADDRESS_MAX);
    log_trace("Balance from upstream: %.8s, %"PRIu64, address, balance);
    if ((rc = mdb_txn_begin(env, NULL, 0, &txn)) != 0)
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        return rc;
    }
    MDB_val k = {ADDRESS_MAX, (void*)address};
    MDB_val v = {sizeof(uint64_t), (void*)&balance};
    if ((rc = mdb_put(txn, db_balance, &k, &v, 0)))
    {
        err = mdb_strerror(rc);
        log_error("%s", err);
        mdb_txn_abort(txn);
        return rc;
    }
    rc = mdb_txn_commit(txn);
    return rc;
}

static void
upstream_on_read(struct bufferevent *bev, void *ctx)
{
    struct evbuffer *input = bufferevent_get_input(bev);
    struct evbuffer_ptr tag;
    unsigned char tnt[9] = {0};
    size_t len = 0;

    input = bufferevent_get_input(bev);
    while ((len = evbuffer_get_length(input)) >= 9)
    {
        tag = evbuffer_search(input, (const char*) msgbin, 8, NULL);
        if (tag.pos < 0)
        {
            log_error("Bad message from upstream");
            evbuffer_drain(input, len);
            return;
        }

        evbuffer_copyout(input, tnt, 9);

        switch (tnt[8])
        {
            case BIN_STATS:
                if (len - 9 < sizeof(pool_stats_t))
                    return;
                evbuffer_drain(input, 9);
                upstream_on_stats(bev);
                break;
            case BIN_BALANCE:
                if (len - 9 < sizeof(uint64_t)+ADDRESS_MAX)
                    return;
                evbuffer_drain(input, 9);
                upstream_on_balance(bev);
                break;
            default:
                log_error("Unsupported message type: %d", tnt[8]);
                evbuffer_drain(input, len);
                return;
        }
    }
}

static void
upstream_on_event(struct bufferevent *bev, short error, void *ctx)
{
    if (error & BEV_EVENT_CONNECTED)
    {
        log_info("Connected to upstream: %s:%d",
                config.upstream_host, config.upstream_port);
        upstream_send_backlog();
        return;
    }
    if (error & BEV_EVENT_EOF)
    {
        log_debug("Upstream disconnected");
    }
    else if (error & BEV_EVENT_ERROR)
    {
        log_debug("Upstream connection error: %d", errno);
    }
    else if (error & BEV_EVENT_TIMEOUT)
    {
        log_debug("Upstream timeout");
    }
    /* Update stats due to upstream disconnect */
    if (pool_stats.connected_accounts != account_count)
    {
        pool_stats.connected_accounts = account_count;
        update_pool_hr();
    }
    /* Wait and try to reconnect */
    if (upstream_event)
    {
        bufferevent_free(upstream_event);
        upstream_event = NULL;
    }
    log_warn("No connection to upstream; retrying in 10s");
    struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
    evtimer_add(timer_10s, &timeout);
}

static void
upstream_connect()
{
    struct addrinfo *info = NULL;
    int rc = 0;
    char port[6] = {0};

    sprintf(port, "%d", config.upstream_port);
    if ((rc = getaddrinfo(config.upstream_host, port, 0, &info)))
    {
        log_fatal("Error parsing upstream host: %s", gai_strerror(rc));
        return;
    }

    upstream_event = bufferevent_socket_new(pool_base, -1,
            BEV_OPT_CLOSE_ON_FREE);

    if (bufferevent_socket_connect(upstream_event,
                info->ai_addr, info->ai_addrlen) < 0)
    {
        perror("connect");
        goto bail;
    }

    bufferevent_setcb(upstream_event,
            upstream_on_read, NULL, upstream_on_event, NULL);
    bufferevent_enable(upstream_event, EV_READ|EV_WRITE);
    evutil_make_socket_nonblocking(bufferevent_getfd(upstream_event));

bail:
    freeaddrinfo(info);
}

static void
timer_on_10s(int fd, short kind, void *ctx)
{
    log_info("Reconnecting to upstream: %s:%d",
            config.upstream_host, config.upstream_port);
    upstream_connect();
}

static void
timer_on_30s(int fd, short kind, void *ctx)
{
    if (upstream_event)
        upstream_send_ping();
    struct timeval timeout = { .tv_sec = 30, .tv_usec = 0 };
    evtimer_add(timer_30s, &timeout);
}

static void
timer_on_120s(int fd, short kind, void *ctx)
{
    log_trace("Fetching last block header from timer");
    fetch_last_block_header();
    struct timeval timeout = { .tv_sec = 120, .tv_usec = 0 };
    evtimer_add(timer_120s, &timeout);
}

static void
timer_on_10m(int fd, short kind, void *ctx)
{
    struct timeval timeout = { .tv_sec = 600, .tv_usec = 0 };
    time_t now = time(NULL);
    int rc = 0;
    uint64_t cc = 0;
    time_t cut = now - config.cull_shares * 86400;
    MDB_txn *txn = NULL;
    MDB_cursor *cursor = NULL;
    MDB_cursor_op op = MDB_FIRST;
    MDB_val k, v;

    if (database_resize())
        log_warn("DB resize needed, will retry later");

    send_payments();

    /* culling old shares */
    if (config.cull_shares < 1)
        goto done;
    log_debug("Culling shares older than: %d days", config.cull_shares);
    if ((rc = mdb_txn_begin(env, NULL, 0, &txn)) != 0)
    {
        log_error("%s", mdb_strerror(rc));
        goto done;
    }
    if ((rc = mdb_cursor_open(txn, db_shares, &cursor)) != 0)
    {
        log_error("%s", mdb_strerror(rc));
        goto abort;
    }
    while (1)
    {
        time_t st;
        if ((rc = mdb_cursor_get(cursor, &k, &v, op)))
        {
            if (rc != MDB_NOTFOUND)
            {
                log_error("%s", mdb_strerror(rc));
                goto abort;
            }
            break;
        }
        st = ((share_t*)v.mv_data)->timestamp;
        if (st < cut)
        {
            if ((rc = mdb_cursor_del(cursor, 0)))
            {
                log_error("%s", mdb_strerror(rc));
                goto abort;
            }
            cc++;
        }
        else
            break;
        op = MDB_NEXT;
    }

    mdb_cursor_close(cursor);
    if ((rc = mdb_txn_commit(txn)))
        log_error("%s", mdb_strerror(rc));
    else
        log_debug("Culled shares: %"PRIu64, cc);
    goto done;

abort:
    if (cursor)
        mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
done:
    evtimer_add(timer_10m, &timeout);
}

static const client_t *
client_add(int fd, struct sockaddr_storage *ss,
        struct bufferevent *bev, bool downstream)
{
    client_t *c = NULL;
    int rc = 0;
    bool resize = gbag_used(bag_clients) == gbag_max(bag_clients);
    if (resize)
    {
        pthread_mutex_lock(&mutex_clients);
        while (clients_reading)
            pthread_cond_wait(&cond_clients, &mutex_clients);
        c = gbag_get(bag_clients);
        pthread_mutex_unlock(&mutex_clients);
        log_debug("Client pool can now hold %zu clients",
                gbag_max(bag_clients));
    }
    else
        c = gbag_get(bag_clients);
    c->fd = fd;
    c->bev = bev;
    c->connected_since = time(NULL);
    c->downstream = downstream;
    if ((rc = getnameinfo((struct sockaddr*)ss, sizeof(*ss),
                    c->host, MAX_HOST, NULL, 0, NI_NUMERICHOST)))
    {
        log_error("Error getting client address: %s",
                gai_strerror(rc));
    }
    else
    {
        struct sockaddr_in *sin = (struct sockaddr_in*) ss;
        c->port = htons(sin->sin_port);
    }
    bstack_new(&c->active_jobs, CLIENT_JOBS_MAX, sizeof(job_t), job_recycle);
    pthread_rwlock_wrlock(&rwlock_cfd);
    HASH_ADD_INT(clients_by_fd, fd, c);
    pthread_rwlock_unlock(&rwlock_cfd);
    return c;
}

static void
client_find(struct bufferevent *bev, client_t **client)
{
    int fd = bufferevent_getfd(bev);
    if (fd < 0)
    {
        *client = NULL;
        return;
    }
    pthread_rwlock_rdlock(&rwlock_cfd);
    HASH_FIND_INT(clients_by_fd, &fd, *client);
    pthread_rwlock_unlock(&rwlock_cfd);
}

static void
client_clear(struct bufferevent *bev)
{
    client_t *client = NULL;
    account_t *account = NULL;
    client_find(bev, &client);
    if (!client)
        return;
    if (client->downstream)
    {
        pool_stats.connected_accounts -= client->downstream_accounts;
        goto clear;
    }
    pthread_rwlock_rdlock(&rwlock_acc);
    HASH_FIND_STR(accounts, client->address, account);
    pthread_rwlock_unlock(&rwlock_acc);
    if (!account)
        goto clear;
    if (account->worker_count == 1)
    {
        account_count--;
        pool_stats.connected_accounts--;
        if (upstream_event)
            upstream_send_account_disconnect();
        pthread_rwlock_wrlock(&rwlock_acc);
        HASH_DEL(accounts, account);
        pthread_rwlock_unlock(&rwlock_acc);
        gbag_put(bag_accounts, account);
    }
    else if (account->worker_count > 1)
        account->worker_count--;
clear:
    client_clear_jobs(client);
    pthread_rwlock_wrlock(&rwlock_cfd);
    HASH_DEL(clients_by_fd, client);
    pthread_rwlock_unlock(&rwlock_cfd);
    gbag_put(bag_clients, client);
    bufferevent_free(bev);
}

static void
miner_on_login(json_object *message, client_t *client)
{
    JSON_GET_OR_ERROR(params, message, json_type_object, client);
    JSON_GET_OR_ERROR(login, params, json_type_string, client);
    JSON_GET_OR_ERROR(pass, params, json_type_string, client);
    client->mode = MODE_NORMAL;
    json_object *mode = NULL;
    if (json_object_object_get_ex(params, "mode", &mode))
    {
        if (!json_object_is_type(mode, json_type_string))
            log_warn("mode not a json_type_string");
        else
        {
            const char *modestr = json_object_get_string(mode);
            if (strcmp(modestr, "self-select") == 0)
            {
                if (config.disable_self_select)
                {
                    send_validation_error(client,
                            "pool disabled self-select");
                    return;
                }
                client->mode = MODE_SELF_SELECT;
                log_trace("Miner login for mode: self-select");
            }
        }
    }

    const char *address = json_object_get_string(login);
    uint8_t nt = 0;
    if (parse_address(address, NULL, &nt, NULL))
    {
        send_validation_error(client,
                "Invalid address");
        return;
    }
    if (nt != nettype)
    {
        send_validation_error(client,
                "Invalid address network type");
        return;
    }

    const char *worker_id = json_object_get_string(pass);
    char *rd = strstr(worker_id, "d=");
    if (rd && rd[2])
    {
        client->req_diff = strtoull(rd+2, NULL, 0);
        log_trace("Miner requested diff: %"PRIu64, client->req_diff);
    }

    json_object *agent = NULL;
    if (json_object_object_get_ex(params, "agent", &agent))
    {
        const char *user_agent = json_object_get_string(agent);
        if (user_agent)
        {
            strncpy(client->agent, user_agent, 255);
            client->is_xnp = strstr(user_agent, "xmr-node-proxy") != NULL
                ? true : false;
        }
    }

    if (client->is_xnp && client->mode == MODE_SELF_SELECT)
    {
        send_validation_error(client,
                "mode self-select not supported by xmr-node-proxy");
        return;
    }

    strncpy(client->address, address, sizeof(client->address)-1);
    strncpy(client->worker_id, worker_id, sizeof(client->worker_id)-1);

    account_t *account = NULL;
    pthread_rwlock_rdlock(&rwlock_acc);
    HASH_FIND_STR(accounts, client->address, account);
    pthread_rwlock_unlock(&rwlock_acc);
    if (!account)
    {
        account_count++;
        if (!client->downstream)
            pool_stats.connected_accounts++;
        if (upstream_event)
            upstream_send_account_connect(1);
        account = gbag_get(bag_accounts);
        strncpy(account->address, address, sizeof(account->address)-1);
        account->worker_count = 1;
        account->connected_since = time(NULL);
        account->hashes = 0;
        pthread_rwlock_wrlock(&rwlock_acc);
        HASH_ADD_STR(accounts, address, account);
        pthread_rwlock_unlock(&rwlock_acc);
    }
    else
        account->worker_count++;

    uuid_t cid;
    uuid_generate(cid);
    bin_to_hex((const unsigned char*)cid, sizeof(uuid_t),
            client->client_id, 32);
    miner_send_job(client, true);
}

static void
miner_on_block_template(json_object *message, client_t *client)
{
    struct evbuffer *output = bufferevent_get_output(client->bev);

    JSON_GET_OR_ERROR(params, message, json_type_object, client);
    JSON_GET_OR_ERROR(id, params, json_type_string, client);
    JSON_GET_OR_ERROR(job_id, params, json_type_string, client);
    JSON_GET_OR_ERROR(blob, params, json_type_string, client);
    JSON_GET_OR_ERROR(difficulty, params, json_type_int, client);
    JSON_GET_OR_ERROR(height, params, json_type_int, client);
    JSON_GET_OR_ERROR(prev_hash, params, json_type_string, client);

    const char *jid = json_object_get_string(job_id);
    if (strlen(jid) != 32)
    {
        send_validation_error(client, "job_id invalid length");
        return;
    }

    int64_t h = json_object_get_int64(height);
    int64_t dh = llabs(h - (int64_t)pool_stats.network_height);
    if (dh > TEMLATE_HEIGHT_VARIANCE)
    {
        char m[64] = {0};
        snprintf(m, 64, "Bad height. "
                "Differs to pool by %"PRIu64" blocks.", dh);
        send_validation_error(client, m);
        return;
    }

    const char *btb = json_object_get_string(blob);
    int rc = validate_block_from_blob(btb, &sec_view[0], &pub_spend[0]);
    if (rc != 0)
    {
        log_warn("Bad template submitted: %d", rc);
        send_validation_error(client, "block template blob invalid");
        return;
    }

    job_t *job = client_find_job(client, jid);
    if (!job)
    {
        send_validation_error(client, "cannot find job with job_id");
        return;
    }

    if (job->miner_template)
    {
        send_validation_error(client, "job already has block template");
        return;
    }

    job->miner_template = calloc(1, sizeof(block_template_t));
    job->miner_template->blocktemplate_blob = strdup(btb);
    job->miner_template->difficulty = json_object_get_int64(difficulty);
    job->miner_template->height = json_object_get_int64(height);
    strncpy(job->miner_template->prev_hash,
            json_object_get_string(prev_hash), 64);

    unsigned int major_version = 0;
    sscanf(btb, "%2x", &major_version);
    uint8_t pow_variant = major_version >= 7 ? major_version - 6 : 0;
    log_trace("Variant: %u", pow_variant);

    if (pow_variant >= 6)
    {
        JSON_GET_OR_WARN(seed_hash, params, json_type_string);
        JSON_GET_OR_WARN(next_seed_hash, params, json_type_string);
        strncpy(job->miner_template->seed_hash,
                json_object_get_string(seed_hash), 64);
        strncpy(job->miner_template->next_seed_hash,
                json_object_get_string(next_seed_hash), 64);
    }

    log_trace("Miner set template: %s", btb);
    char body[STATUS_BODY_MAX] = {0};
    stratum_get_status_body(body, client->json_id, "OK");
    evbuffer_add(output, body, strlen(body));
}

static void
miner_on_submit(json_object *message, client_t *client)
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
    {
        send_validation_error(client, "nonce not an unsigned long int");
        return;
    }
    const uint32_t result_nonce = ntohl(uli);

    const char *result_hex = json_object_get_string(result);
    if (strlen(result_hex) != 64)
    {
        send_validation_error(client, "result invalid length");
        return;
    }
    if (is_hex_string(result_hex) != 0)
    {
        send_validation_error(client, "result not hex string");
        return;
    }

    const char *jid = json_object_get_string(job_id);
    if (strlen(jid) != 32)
    {
        send_validation_error(client, "job_id invalid length");
        return;
    }

    job_t *job = client_find_job(client, jid);
    if (!job)
    {
        send_validation_error(client, "cannot find job with job_id");
        return;
    }

    log_trace("Miner submitted nonce=%u, result=%s",
            result_nonce, result_hex);
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
         check result hash against template difficulty
         (submit to network if good) add share to db

      Note reserved space is: extra_nonce|instance_id|pool_nonce|worker_nonce
      4 bytes each.
    */

    /* Convert template to blob */
    if (client->mode == MODE_SELF_SELECT && !job->miner_template)
    {
        send_validation_error(client, "mode self-select and no template");
        return;
    }
    block_template_t *bt;
    if (job->miner_template)
        bt = job->miner_template;
    else
        bt = job->block_template;
    char *btb = bt->blocktemplate_blob;
    size_t bin_size = strlen(btb) >> 1;
    unsigned char *block = calloc(bin_size, sizeof(char));
    hex_to_bin(bt->blocktemplate_blob, bin_size << 1, block, bin_size);

    unsigned char *p = block;
    uint32_t pool_nonce = 0;
    uint32_t worker_nonce = 0;

    if (client->mode != MODE_SELF_SELECT)
    {
        /* Set the extra nonce and instance_id in our reserved space */
        p += bt->reserved_offset;
        memcpy(p, &job->extra_nonce, sizeof(extra_nonce));
        p += 4;
        memcpy(p, &instance_id, sizeof(instance_id));
        if (client->is_xnp)
        {
            /*
              A proxy supplies pool_nonce and worker_nonce
              so add them in the reserved space too.
            */
            JSON_GET_OR_WARN(poolNonce, params, json_type_int);
            JSON_GET_OR_WARN(workerNonce, params, json_type_int);
            pool_nonce = json_object_get_int(poolNonce);
            worker_nonce = json_object_get_int(workerNonce);
            p += 4;
            memcpy(p, &pool_nonce, sizeof(pool_nonce));
            p += 4;
            memcpy(p, &worker_nonce, sizeof(worker_nonce));
        }
    }

    uint128_t sub = 0;
    uint32_t *psub = (uint32_t*) &sub;
    *psub++ = result_nonce;
    *psub++ = job->extra_nonce;
    *psub++ = pool_nonce;
    *psub++ = worker_nonce;

    psub -= 4;
    log_trace("Submission reserved values: %u %u %u %u",
            *psub, *(psub+1), *(psub+2), *(psub+3));

    /* Check not already submitted */
    uint128_t *submissions = job->submissions;
    for (size_t i=0; i<job->submissions_count; i++)
    {
        if (submissions[i] == sub)
        {
            char body[ERROR_BODY_MAX] = {0};
            stratum_get_error_body(body, client->json_id, "Duplicate share");
            evbuffer_add(output, body, strlen(body));
            log_debug("[%s:%d] Duplicate share", client->host, client->port);
            free(block);
            return;
        }
    }
    if (!fmod(job->submissions_count, 10))
    {
        job->submissions = realloc((void*)submissions,
                10*sizeof(uint128_t)+job->submissions_count*sizeof(uint128_t));
    }
    job->submissions[job->submissions_count++] = sub;

    /* And the supplied nonce */
    p = block;
    p += 39;
    memcpy(p, &result_nonce, sizeof(result_nonce));

    /* Get hashing blob */
    size_t hashing_blob_size = 0;
    unsigned char *hashing_blob = NULL;
    if (get_hashing_blob(block, bin_size,
                &hashing_blob, &hashing_blob_size) != 0)
    {
        char body[ERROR_BODY_MAX] = {0};
        stratum_get_error_body(body, client->json_id, "Invalid block");
        evbuffer_add(output, body, strlen(body));
        log_debug("Invalid block");
        free(block);
        return;
    }

    /* Hash and compare */
    unsigned char result_hash[32] = {0};
    unsigned char submitted_hash[32] = {0};
    uint8_t major_version = (uint8_t)block[0];
    uint8_t pow_variant = major_version >= 7 ? major_version - 6 : 0;
    BIGNUM *hd = NULL;
    BIGNUM *jd = NULL;
    BIGNUM *bd = NULL;
    BIGNUM *rh = NULL;
    hex_to_bin(result_hex, 64, submitted_hash, 32);

    if (config.disable_hash_check)
    {
        memcpy(result_hash, submitted_hash, 32);
        goto post_hash;
    }

    if (pow_variant >= 6)
    {
        unsigned char seed_hash[32] = {0};
        hex_to_bin(bt->seed_hash, 64, seed_hash, 32);
        get_rx_hash(hashing_blob, hashing_blob_size,
                (unsigned char*)result_hash, seed_hash, bt->height);
    }
    else
    {
        get_hash(hashing_blob, hashing_blob_size,
                (unsigned char*)result_hash, pow_variant, bt->height);
    }

    if (memcmp(submitted_hash, result_hash, 32) != 0)
    {
        char body[ERROR_BODY_MAX] = {0};
        stratum_get_error_body(body, client->json_id, "Invalid share");
        evbuffer_add(output, body, strlen(body));
        log_debug("Invalid share");
        client->bad_shares++;
        free(block);
        free(hashing_blob);
        return;
    }

post_hash:
    hd = BN_new();
    jd = BN_new();
    bd = BN_new();
    BN_set_word(jd, job->target);
    BN_set_word(bd, bt->difficulty);
    reverse_bin(result_hash, 32);
    rh = BN_bin2bn((const unsigned char*)result_hash, 32, NULL);
    BN_div(hd, NULL, base_diff, rh, bn_ctx);
    BN_free(rh);

    /* Process share */
    account_t *account = NULL;
    pthread_rwlock_rdlock(&rwlock_acc);
    HASH_FIND_STR(accounts, client->address, account);
    client->hashes += job->target;
    client->hr_stats.diff_since += job->target;
    account->hashes += job->target;
    account->hr_stats.diff_since += job->target;
    hr_update(&client->hr_stats);
    /* TODO: account hr should be called less freq */
    hr_update(&account->hr_stats);
    pthread_rwlock_unlock(&rwlock_acc);
    time_t now = time(NULL);
    bool can_store = true;
    log_trace("Checking hash against block difficulty: "
            "%lu, job difficulty: %lu",
            BN_get_word(bd), BN_get_word(jd));

    if (BN_cmp(hd, bd) >= 0)
    {
        /* Yay! Mined a block so submit to network */
        log_info("+++ MINED A BLOCK +++");
        char *block_hex = calloc((bin_size << 1)+1, sizeof(char));
        bin_to_hex(block, bin_size, block_hex, bin_size << 1);
        char body[RPC_BODY_MAX] = {0};
        snprintf(body, RPC_BODY_MAX,
                "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":"
                "\"submit_block\", \"params\":[\"%s\"]}",
                block_hex);

        rpc_callback_t *cb = rpc_callback_new(rpc_on_block_submitted, 0, 0);
        cb->data = calloc(1, sizeof(block_t));
        block_t* b = (block_t*) cb->data;
        b->height = bt->height;
        unsigned char block_hash[32] = {0};
        if (get_block_hash(block, bin_size, block_hash) != 0)
            log_error("Error getting block hash!");
        bin_to_hex(block_hash, 32, b->hash, 64);
        strncpy(b->prev_hash, bt->prev_hash, 64);
        b->difficulty = bt->difficulty;
        b->status = BLOCK_LOCKED;
        b->timestamp = now;
        if (upstream_event)
            upstream_send_client_block(b);
        rpc_request(pool_base, body, cb);
        free(block_hex);
    }
    else if (BN_cmp(hd, jd) < 0)
    {
        can_store = false;
        char body[ERROR_BODY_MAX] = {0};
        stratum_get_error_body(body, client->json_id, "Low difficulty share");
        evbuffer_add(output, body, strlen(body));
        log_debug("Low difficulty (%lu) share", BN_get_word(jd));
        client->bad_shares++;
    }

    BN_free(hd);
    BN_free(jd);
    BN_free(bd);
    free(block);
    free(hashing_blob);

    if (can_store)
    {
        if (client->bad_shares)
            client->bad_shares--;
        share_t share = {0,0,{0},0};
        share.height = bt->height;
        share.difficulty = job->target;
        strncpy(share.address, client->address, sizeof(share.address)-1);
        share.timestamp = now;
        if (!upstream_event)
            pool_stats.round_hashes += share.difficulty;
        log_debug("Storing share with difficulty: %"PRIu64, share.difficulty);
        int rc = store_share(share.height, &share);
        if (rc != 0)
            log_warn("Failed to store share: %s", mdb_strerror(rc));
        char body[STATUS_BODY_MAX] = {0};
        stratum_get_status_body(body, client->json_id, "OK");
        evbuffer_add(output, body, strlen(body));
        if (upstream_event)
            upstream_send_client_share(&share);
    }
    if (retarget_required(client, job))
    {
        log_debug("Sending an early job as this was less than %u%% of"
                " potential", (unsigned)(100.*config.retarget_ratio));
        miner_send_job(client, false);
    }
}

static void
miner_on_read(struct bufferevent *bev, void *ctx)
{
    const char *unknown_method = "Removing client. Unknown method called.";
    const char *too_bad = "Removing client. Too many bad shares.";
    const char *too_long = "Removing client. Message too long.";
    const char *invalid_json = "Removing client. Invalid JSON.";
    struct evbuffer *input, *output;
    char *line = NULL;
    size_t n = 0;
    client_t *client = NULL;

    pthread_mutex_lock(&mutex_clients);
    clients_reading++;
    pthread_mutex_unlock(&mutex_clients);

    client_find(bev, &client);
    if (!client)
        goto unlock;

    input = bufferevent_get_input(bev);
    output = bufferevent_get_output(bev);

    size_t len = evbuffer_get_length(input);
    if (len > MAX_LINE)
    {
        char body[ERROR_BODY_MAX] = {0};
        stratum_get_error_body(body, client->json_id, too_long);
        evbuffer_add(output, body, strlen(body));
        log_warn("[%s:%d] %s", client->host, client->port, too_long);
        evbuffer_drain(input, len);
        client_clear(bev);
        goto unlock;
    }

    while ((line = evbuffer_readln(input, &n, EVBUFFER_EOL_LF)))
    {
        json_object *message = json_tokener_parse(line);
        if (!message)
        {
            free(line);
            char body[ERROR_BODY_MAX] = {0};
            stratum_get_error_body(body, client->json_id, invalid_json);
            evbuffer_add(output, body, strlen(body));
            log_warn("[%s:%d] %s", client->host, client->port, invalid_json);
            evbuffer_drain(input, len);
            client_clear(bev);
            goto unlock;
        }
        JSON_GET_OR_WARN(method, message, json_type_string);
        JSON_GET_OR_WARN(id, message, json_type_int);
        const char *method_name = json_object_get_string(method);
        client->json_id = json_object_get_int(id);

        bool unknown = false;

        if (!method || !method_name)
        {
            unknown = true;
        }
        else if (strcmp(method_name, "login") == 0)
        {
            miner_on_login(message, client);
        }
        else if (strcmp(method_name, "block_template") == 0)
        {
            miner_on_block_template(message, client);
        }
        else if (strcmp(method_name, "submit") == 0)
        {
            miner_on_submit(message, client);
        }
        else if (strcmp(method_name, "getjob") == 0)
        {
            miner_send_job(client, false);
        }
        else if (strcmp(method_name, "keepalived") == 0)
        {
            char body[STATUS_BODY_MAX] = {0};
            stratum_get_status_body(body, client->json_id, "KEEPALIVED");
            evbuffer_add(output, body, strlen(body));
        }
        else
        {
            unknown = true;
        }

        json_object_put(message);
        free(line);

        if (unknown)
        {
            char body[ERROR_BODY_MAX] = {0};
            stratum_get_error_body(body, client->json_id, unknown_method);
            evbuffer_add(output, body, strlen(body));
            log_warn("[%s:%d] %s", client->host, client->port, unknown_method);
            evbuffer_drain(input, len);
            client_clear(bev);
            goto unlock;
        }
        if (client->bad_shares > MAX_BAD_SHARES)
        {
            char body[ERROR_BODY_MAX] = {0};
            stratum_get_error_body(body, client->json_id, too_bad);
            evbuffer_add(output, body, strlen(body));
            log_warn("[%s:%d] %s", client->host, client->port, too_bad);
            evbuffer_drain(input, len);
            client_clear(bev);
            goto unlock;
        }
    }
unlock:
    pthread_mutex_lock(&mutex_clients);
    clients_reading--;
    pthread_cond_signal(&cond_clients);
    pthread_mutex_unlock(&mutex_clients);
}

static void
trusted_on_read(struct bufferevent *bev, void *ctx)
{
    struct evbuffer *input = NULL;
    client_t *client = NULL;
    struct evbuffer_ptr tag;
    unsigned char tnt[9] = {0};
    size_t len = 0;

    pthread_mutex_lock(&mutex_clients);
    clients_reading++;
    pthread_mutex_unlock(&mutex_clients);
    client_find(bev, &client);
    if (!client)
        goto unlock;
    if (!client->downstream)
    {
        /* should never happen; sanity check */
        log_trace("Only trusted downstreams allowed");
        client_clear(bev);
        goto unlock;
    }

    input = bufferevent_get_input(bev);

    while ((len = evbuffer_get_length(input)) >= 9)
    {
        tag = evbuffer_search(input, (const char*) msgbin, 8, NULL);
        if (tag.pos < 0)
        {
            log_warn("[%s:%d] Bad message from downstream",
                    client->host, client->port);
            evbuffer_drain(input, len);
            client_clear(bev);
            goto unlock;
        }

        evbuffer_copyout(input, tnt, 9);
        log_trace("Downstream message: %d", tnt[8]);
        switch (tnt[8])
        {
            case BIN_PING:
            case BIN_STATS:
                evbuffer_drain(input, 9);
                trusted_send_stats(client);
                break;
            case BIN_CONNECT:
                if (len - 9 < sizeof(uint32_t))
                    goto unlock;
                evbuffer_drain(input, 9);
                trusted_on_account_connect(client);
                break;
            case BIN_DISCONNECT:
                evbuffer_drain(input, 9);
                trusted_on_account_disconnect(client);
                break;
            case BIN_SHARE:
                if (len - 9 < sizeof(share_t))
                    goto unlock;
                evbuffer_drain(input, 9);
                trusted_on_client_share(client);
                break;
            case BIN_BLOCK:
                if (len - 9 < sizeof(block_t))
                    goto unlock;
                evbuffer_drain(input, 9);
                trusted_on_client_block(client);
                break;
            default:
                log_warn("[%s:%d] Unknown message: %d",
                        client->host, client->port, tnt[8]);
                evbuffer_drain(input, len);
                client_clear(bev);
                goto unlock;
        }
    }
unlock:
    pthread_mutex_lock(&mutex_clients);
    clients_reading--;
    pthread_cond_signal(&cond_clients);
    pthread_mutex_unlock(&mutex_clients);
}

static void
listener_on_error(struct bufferevent *bev, short error, void *ctx)
{
    struct event_base *base = (struct event_base*)ctx;
    client_t *client = NULL;
    client_find(bev, &client);
    char *type = base != trusted_base ? "Miner" : "Downstream";
    if (error & BEV_EVENT_EOF)
    {
        log_debug("[%s:%d] %s disconnected. Removing.",
                client->host, client->port, type);
    }
    else if (error & BEV_EVENT_ERROR)
    {
        log_debug("[%s:%d] %s error: %d. Removing.",
                client->host, client->port, type, errno);
    }
    else if (error & BEV_EVENT_TIMEOUT)
    {
        log_debug("[%s:%d] %s timeout. Removing.",
                client->host, client->port, type);
    }
    client_clear(bev);
}

static void
listener_on_accept(evutil_socket_t listener, short event, void *arg)
{
    struct event_base *base = (struct event_base*)arg;
    char *type = base != trusted_base ? "miner" : "downstream";
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    int fd = accept(listener, (struct sockaddr*)&ss, &slen);
    if (fd < 0)
    {
        perror("accept");
        return;
    }
    if (base == trusted_base && *config.trusted_allowed[0])
    {
        char *s = config.trusted_allowed[0];
        char *e = s + (MAX_DOWNSTREAM * MAX_HOST);
        char host[MAX_HOST] = {0};
        bool match = false;
        int rc = 0;
        if ((rc = getnameinfo((struct sockaddr*)&ss, slen,
                        host, MAX_HOST, NULL, 0, NI_NUMERICHOST)))
        {
            log_error("Error parsing trusted allowed address: %s",
                    gai_strerror(rc));
            return;
        }
        while (s < e)
        {
            if (strncmp(s, host, MAX_HOST) == 0)
            {
                match = true;
                break;
            }
            s += MAX_HOST;
        }
        if (!match)
        {
            close(fd);
            log_error("Host %s not allowed as trusted downstream", host);
            return;
        }
    }
    struct bufferevent *bev;
    evutil_make_socket_nonblocking(fd);
    bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    struct timeval tv = {config.idle_timeout, 0};
    if (base != trusted_base)
        bufferevent_set_timeouts(bev, &tv, &tv);
    bufferevent_setcb(bev, 
            base == trusted_base ? trusted_on_read : miner_on_read,
            NULL, listener_on_error, arg);
    bufferevent_setwatermark(bev, EV_READ, 0, MAX_LINE);
    const client_t *c = client_add(fd, &ss, bev, base == trusted_base);
    log_info("New %s [%s:%d] connected", type, c->host, c->port);
    log_info("Pool accounts: %d, workers: %d, hashrate: %"PRIu64,
            pool_stats.connected_accounts,
            gbag_used(bag_clients),
            pool_stats.pool_hashrate);
    bufferevent_enable(bev, EV_READ|EV_WRITE);
}

static void
log_lock(void *ud, int lock)
{
    if (lock)
        pthread_mutex_lock(ud);
    else
        pthread_mutex_unlock(ud);
}

static void
read_config(const char *config_file)
{
    /* Start with some defaults for any missing... */
    strcpy(config.rpc_host, "127.0.0.1");
    config.rpc_port = 18081;
    config.rpc_timeout = 15;
    config.idle_timeout = 150;
    config.pool_start_diff = 1000;
    config.share_mul = 2.0;
    config.retarget_time = 30;
    config.retarget_ratio = 0.55;
    config.pool_fee = 0.01;
    config.payment_threshold = 0.33;
    strcpy(config.pool_listen, "0.0.0.0");
    config.pool_port = 4242;
    config.pool_ssl_port = 0;
    config.log_level = 5;
    config.webui_port = 4243;
    config.block_notified = false;
    config.disable_self_select = false;
    config.disable_hash_check = false;
    config.disable_payouts = false;
    strcpy(config.data_dir, "./data");
    config.cull_shares = -1;

    char path[MAX_PATH] = {0};
    if (config_file)
    {
        strncpy(path, config_file, MAX_PATH-1);
    }
    else
    {
        if (!getcwd(path, MAX_PATH))
        {
            log_fatal("Cannot getcwd (%s). Aborting.", errno);
            exit(-1);
        }
        strcat(path, "/pool.conf");
        if (access(path, R_OK) != 0)
        {
            strncpy(path, getenv("HOME"), MAX_PATH-1);
            strcat(path, "/pool.conf");
            if (access(path, R_OK) != 0)
            {
                log_fatal("Cannot find a config file in ./ or ~/ "
                        "and no option supplied. Aborting.");
                exit(-1);
            }
        }
    }
    log_info("Reading config from: %s", path);

    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        log_fatal("Cannot open config file. Aborting.");
        exit(-1);
    }
    char line[1024] = {0};
    char *key;
    char *val;
    const char *tok = " =";
    while (fgets(line, sizeof(line), fp))
    {
        if (*line == '#')
            continue;
        key = strtok(line, tok);
        if (!key)
            continue;
        val = strtok(NULL, tok);
        if (!val)
            continue;
        val[strcspn(val, "\r\n")] = 0;
        if (strcmp(key, "pool-listen") == 0)
        {
            strncpy(config.pool_listen, val, sizeof(config.pool_listen)-1);
        }
        else if (strcmp(key, "pool-port") == 0)
        {
            config.pool_port = atoi(val);
        }
        else if (strcmp(key, "pool-ssl-port") == 0)
        {
            config.pool_ssl_port = atoi(val);
        }
        else if (strcmp(key, "webui-port") == 0)
        {
            config.webui_port = atoi(val);
        }
        else if (strcmp(key, "rpc-host") == 0)
        {
            strncpy(config.rpc_host, val, sizeof(config.rpc_host)-1);
        }
        else if (strcmp(key, "rpc-port") == 0)
        {
            config.rpc_port = atoi(val);
        }
        else if (strcmp(key, "wallet-rpc-host") == 0)
        {
            strncpy(config.wallet_rpc_host, val, sizeof(config.rpc_host)-1);
        }
        else if (strcmp(key, "wallet-rpc-port") == 0)
        {
            config.wallet_rpc_port = atoi(val);
        }
        else if (strcmp(key, "rpc-timeout") == 0)
        {
            config.rpc_timeout = atoi(val);
        }
        else if (strcmp(key, "idle-timeout") == 0)
        {
            config.idle_timeout = atoi(val);
        }
        else if (strcmp(key, "pool-wallet") == 0)
        {
            strncpy(config.pool_wallet, val, sizeof(config.pool_wallet)-1);
        }
        else if (strcmp(key, "pool-fee-wallet") == 0)
        {
            strncpy(config.pool_fee_wallet, val,
                    sizeof(config.pool_fee_wallet)-1);
        }
        else if (strcmp(key, "pool-start-diff") == 0)
        {
            config.pool_start_diff = strtoumax(val, NULL, 10);
        }
        else if (strcmp(key, "pool-fixed-diff") == 0)
        {
            config.pool_fixed_diff = strtoumax(val, NULL, 10);
        }
        else if (strcmp(key, "pool-fee") == 0)
        {
            config.pool_fee = atof(val);
        }
        else if (strcmp(key, "payment-threshold") == 0)
        {
            config.payment_threshold = atof(val);
        }
        else if (strcmp(key, "share-mul") == 0)
        {
            config.share_mul = atof(val);
        }
        else if (strcmp(key, "retarget-time") == 0)
        {
            config.retarget_time = atoi(val);
        }
        else if (strcmp(key, "retarget-ratio") == 0)
        {
            config.retarget_ratio = atof(val);
        }
        else if (strcmp(key, "log-level") == 0)
        {
            config.log_level = atoi(val);
        }
        else if (strcmp(key, "log-file") == 0)
        {
            strncpy(config.log_file, val, sizeof(config.log_file)-1);
        }
        else if (strcmp(key, "block-notified") == 0)
        {
            config.block_notified = atoi(val);
        }
        else if (strcmp(key, "disable-self-select") == 0)
        {
            config.disable_self_select = atoi(val);
        }
        else if (strcmp(key, "disable-hash-check") == 0)
        {
            config.disable_hash_check = atoi(val);
            if (config.disable_hash_check)
                log_warn("Share hash checking disabled");
        }
        else if (strcmp(key, "disable-payouts") == 0)
        {
            config.disable_payouts = atoi(val);
        }
        else if (strcmp(key, "data-dir") == 0)
        {
            strncpy(config.data_dir, val, sizeof(config.data_dir)-1);
        }
        else if (strcmp(key, "pid-file") == 0)
        {
            strncpy(config.pid_file, val, sizeof(config.pid_file)-1);
        }
        else if (strcmp(key, "forked") == 0)
        {
            config.forked = atoi(val);
        }
        else if (strcmp(key, "processes") == 0)
        {
            config.processes = atoi(val);
            if (config.processes < -1)
                config.processes = -1;
        }
        else if (strcmp(key, "cull-shares") == 0)
        {
            config.cull_shares = atoi(val);
        }
        else if (strcmp(key, "trusted-listen") == 0)
        {
            strncpy(config.trusted_listen, val,
                    sizeof(config.trusted_listen)-1);
        }
        else if (strcmp(key, "trusted-port") == 0)
        {
            config.trusted_port = atoi(val);
        }
        else if (strcmp(key, "trusted-allowed") == 0)
        {
            char *temp = strdup(val);
            char *search = temp;
            char *s = config.trusted_allowed[0];
            char *e = s + (MAX_DOWNSTREAM * MAX_HOST);
            char *ip;
            while ((ip = strsep(&search, " ,")) && s < e)
            {
                if (!strlen(ip))
                    continue;
                strncpy(s, ip, MAX_HOST-1);
                s += MAX_HOST;
            }
            free(temp);
        }
        else if (strcmp(key, "upstream-host") == 0)
        {
            strncpy(config.upstream_host, val, sizeof(config.upstream_host)-1);
        }
        else if (strcmp(key, "upstream-port") == 0)
        {
            config.upstream_port = atoi(val);
        }
        else if (strcmp(key, "pool-view-key") == 0 && strlen(val) == 64)
        {
            memcpy(config.pool_view_key, val, 64);
        }
    }
    fclose(fp);

    if (!config.pool_wallet[0])
    {
        log_fatal("No pool wallet supplied. Aborting.");
        exit(-1);
    }
    if (parse_address(config.pool_wallet, NULL, &nettype, &pub_spend[0]))
    {
        log_fatal("Invalid pool wallet");
        exit(-1);
    }
    if (config.pool_fee_wallet[0] &&
            parse_address(config.pool_fee_wallet, NULL, NULL, NULL))
    {
        log_error("Invalid fee wallet; ignoring");
        memset(config.pool_fee_wallet, 0, sizeof(config.pool_fee_wallet));
    }
    if (strncmp(config.pool_fee_wallet, config.pool_wallet,
            sizeof(config.pool_wallet)-1) == 0)
    {
        log_error("Fee wallet cannot match the pool wallet; ignoring");
        memset(config.pool_fee_wallet, 0, sizeof(config.pool_fee_wallet));
    }
    if (!config.wallet_rpc_host[0] || config.wallet_rpc_port == 0)
    {
        log_fatal("Both wallet-rpc-host and wallet-rpc-port need setting. "
                "Aborting.");
        exit(-1);
    }
    if (config.retarget_ratio < 0 || config.retarget_ratio > 1)
    {
        log_fatal("Set retarget-ratio to any rational value within range "
                "[0, 1]. Miners will receive new jobs earlier if their latest"
                " work is less than retarget-ratio percentage of potential.");
        exit(-1);
    }
    if (*config.upstream_host
            && strcmp(config.upstream_host, config.pool_listen) == 0
            && config.upstream_port == config.pool_port)
    {
        log_fatal("Cannot point upstream to the pool. Aborting.");
        exit(-1);
    }
    if (*config.upstream_host
            && strcmp(config.upstream_host, config.trusted_listen) == 0
            && config.upstream_port == config.trusted_port)
    {
        log_fatal("Cannot point upstream to this trusted listener. Aborting.");
        exit(-1);
    }
}

static void print_config()
{
    char display_allowed[MAX_HOST*MAX_DOWNSTREAM] = {0};
    if (*config.trusted_allowed[0])
    {
        char *s = display_allowed;
        char *e = display_allowed + sizeof(display_allowed);
        char *f = config.trusted_allowed[0];
        char *l = f + (MAX_DOWNSTREAM * MAX_HOST);
        s = stecpy(s, f, e);
        f += MAX_HOST;
        while(*f && f < l)
        {
            s = stecpy(s, ",", e);
            s = stecpy(s, f, e);
            f += MAX_HOST;
        }
    }
    log_info("\nCONFIG:\n"
        "  pool-listen = %s\n"
        "  pool-port = %u\n"
        "  pool-ssl-port = %u\n"
        "  webui-port= %u\n"
        "  rpc-host = %s\n"
        "  rpc-port = %u\n"
        "  wallet-rpc-host = %s\n"
        "  wallet-rpc-port = %u\n"
        "  rpc-timeout = %u\n"
        "  idle-timeout = %u\n"
        "  pool-wallet = %s\n"
        "  pool-fee-wallet = %s\n"
        "  pool-start-diff = %"PRIu64"\n"
        "  pool-fixed-diff = %"PRIu64"\n"
        "  pool-fee = %.3f\n"
        "  payment-threshold = %.2f\n"
        "  share-mul = %.2f\n"
        "  retarget-time = %u\n"
        "  retarget-ratio = %.2f\n"
        "  log-level = %u\n"
        "  log-file = %s\n"
        "  block-notified = %u\n"
        "  disable-self-select = %u\n"
        "  disable-hash-check = %u\n"
        "  disable-payouts = %u\n"
        "  data-dir = %s\n"
        "  pid-file = %s\n"
        "  forked = %u\n"
        "  processes = %d\n"
        "  cull-shares = %d\n"
        "  trusted-listen = %s\n"
        "  trusted-port = %u\n"
        "  trusted-allowed = %s\n"
        "  upstream-host = %s\n"
        "  upstream-port = %u\n",
        config.pool_listen,
        config.pool_port,
        config.pool_ssl_port,
        config.webui_port,
        config.rpc_host,
        config.rpc_port,
        config.wallet_rpc_host,
        config.wallet_rpc_port,
        config.rpc_timeout,
        config.idle_timeout,
        config.pool_wallet,
        config.pool_fee_wallet,
        config.pool_start_diff,
        config.pool_fixed_diff,
        config.pool_fee,
        config.payment_threshold,
        config.share_mul,
        config.retarget_time,
        config.retarget_ratio,
        config.log_level,
        config.log_file,
        config.block_notified,
        config.disable_self_select,
        config.disable_hash_check,
        config.disable_payouts,
        config.data_dir,
        config.pid_file,
        config.forked,
        config.processes,
        config.cull_shares,
        config.trusted_listen,
        config.trusted_port,
        display_allowed,
        config.upstream_host,
        config.upstream_port);
}

static void
sigusr1_handler(evutil_socket_t fd, short event, void *arg)
{
    log_trace("Fetching last block header from signal");
    fetch_last_block_header();
}

static void
sigint_handler(int sig)
{
    signal(sig, SIG_DFL);
    exit(0);
}

static void *
trusted_run(void *ctx)
{
    evutil_socket_t listener;
    struct addrinfo *info = NULL;
    int rc = 0;
    char port[6] = {0};

    trusted_base = event_base_new();
    if (!trusted_base)
    {
        log_fatal("Failed to create trusted event base");
        return 0;
    }

    sprintf(port, "%d", config.trusted_port);
    if ((rc = getaddrinfo(config.trusted_listen, port, 0, &info)))
    {
        log_fatal("Error parsing trusted listen address: %s",
                gai_strerror(rc));
        return 0;
    }

    listener = socket(info->ai_family, SOCK_STREAM, 0);
    evutil_make_socket_nonblocking(listener);

#ifndef WIN32
    {
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(listener, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    }
#endif

    if (bind(listener, info->ai_addr, info->ai_addrlen) < 0)
    {
        perror("bind");
        goto bail;
    }

    freeaddrinfo(info);
    info = NULL;

    if (listen(listener, 16)<0)
    {
        perror("listen");
        goto bail;
    }

    trusted_event = event_new(trusted_base, listener, EV_READ|EV_PERSIST,
            listener_on_accept, (void*)trusted_base);
    if (event_add(trusted_event, NULL) != 0)
    {
        log_fatal("Failed to add trusted socket listener event");
        goto bail;
    }

    event_base_dispatch(trusted_base);

bail:
    if (info)
        freeaddrinfo(info);
    event_base_free(trusted_base);
    return 0;
}

static void
run(void)
{
    evutil_socket_t listener;
    struct addrinfo *info = NULL;
    int rc = 0;
    char port[6] = {0};

    pool_base = event_base_new();
    if (!pool_base)
    {
        log_fatal("Failed to create event base");
        return;
    }

    sprintf(port, "%d", config.pool_port);
    if ((rc = getaddrinfo(config.pool_listen, port, 0, &info)))
    {
        log_fatal("Error parsing listen address: %s", gai_strerror(rc));
        return;
    }

    listener = socket(info->ai_family, SOCK_STREAM, 0);
    evutil_make_socket_nonblocking(listener);

#ifndef WIN32
    {
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(listener, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    }
#endif

    if (bind(listener, info->ai_addr, info->ai_addrlen) < 0)
    {
        perror("bind");
        goto bail;
    }

    freeaddrinfo(info);
    info = NULL;

    if (listen(listener, 16)<0)
    {
        perror("listen");
        goto bail;
    }

    listener_event = event_new(pool_base, listener, EV_READ|EV_PERSIST,
            listener_on_accept, (void*)pool_base);
    if (event_add(listener_event, NULL) != 0)
    {
        log_fatal("Failed to add socket listener event");
        goto bail;
    }

    signal_usr1 = evsignal_new(pool_base, SIGUSR1, sigusr1_handler, NULL);
    event_add(signal_usr1, NULL);

    if (*config.trusted_listen && config.trusted_port)
    {
        log_info("Starting trusted listener on: %s:%d",
                config.trusted_listen, config.trusted_port);
        if (pthread_create(&trusted_th, NULL, trusted_run, NULL))
        {
            log_fatal("Cannot create trusted thread");
            goto bail;
        }
        pthread_detach(trusted_th);
    }

    if (*config.upstream_host && config.upstream_port)
    {
        log_info("Starting upstream connection to: %s:%d",
                config.upstream_host, config.upstream_port);
        upstream_connect();
    }

    if (!config.block_notified)
    {
        timer_120s = evtimer_new(pool_base, timer_on_120s, NULL);
        timer_on_120s(-1, EV_TIMEOUT, NULL);
    }
    else
        fetch_last_block_header();
    fetch_view_key();

    if (abattoir)
    {
        timer_10m = evtimer_new(pool_base, timer_on_10m, NULL);
        timer_on_10m(-1, EV_TIMEOUT, NULL);
    }

    if (*config.upstream_host)
    {
        timer_10s = evtimer_new(pool_base, timer_on_10s, NULL);
        timer_30s = evtimer_new(pool_base, timer_on_30s, NULL);
        timer_on_30s(-1, EV_TIMEOUT, NULL);
    }

    event_base_dispatch(pool_base);

bail:
    if (info)
        freeaddrinfo(info);
}

static void
cleanup(void)
{
    log_info("Performing cleanup");
    if (timer_10s)
        event_free(timer_10s);
    if (timer_30s)
        event_free(timer_30s);
    if (timer_120s)
        event_free(timer_120s);
    if (timer_10m)
        event_free(timer_10m);
    if (listener_event)
        event_free(listener_event);
    if (trusted_event)
        event_free(trusted_event);
    if (upstream_event)
        bufferevent_free(upstream_event);
    if (config.webui_port)
        stop_web_ui();
    if (signal_usr1)
        event_free(signal_usr1);
    if (trusted_base)
        event_base_loopbreak(trusted_base);
    if (pool_base)
        event_base_free(pool_base);
    clients_free();
    if (bsh)
        bstack_free(bsh);
    if (bst)
        bstack_free(bst);
    database_close();
    BN_free(base_diff);
    BN_CTX_free(bn_ctx);
    rx_stop_mining();
    rx_slow_hash_free_state();
    pthread_mutex_destroy(&mutex_clients);
    pthread_mutex_destroy(&mutex_log);
    pthread_rwlock_destroy(&rwlock_tx);
    pthread_rwlock_destroy(&rwlock_acc);
    pthread_rwlock_destroy(&rwlock_cfd);
    pthread_cond_destroy(&cond_clients);
    log_info("Pool shutdown successfully");
    if (fd_log)
        fclose(fd_log);
}

static void
print_help(struct option *opts)
{
    for (; opts->name; ++opts)
    {
        printf("-%c, --%s %s\n", opts->val, opts->name,
            opts->has_arg==required_argument ?
            strstr(opts->name,"file") ? "<file>" :
            strstr(opts->name,"processes") ? "[-1|N]" : "<dir>" :
            opts->has_arg==optional_argument ? "[0|1]" : "" );
    }
}

int main(int argc, char **argv)
{
    static struct option options[] =
    {
        {"config-file", required_argument, 0, 'c'},
        {"log-file", required_argument, 0, 'l'},
        {"block-notified", optional_argument, 0, 'b'},
        {"data-dir", required_argument, 0, 'd'},
        {"pid-file", required_argument, 0, 'p'},
        {"forked", optional_argument, 0, 'f'},
        {"processes", required_argument, 0, 'm'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    char *config_file = NULL;
    char *log_file = NULL;
    int block_notified = -1;
    char *data_dir = NULL;
    char *pid_file = NULL;
    int forked = -1;
    int processes = 1;
    int c;
    while (1)
    {
        int option_index = 0;
        c = getopt_long (argc, argv, "c:l:b::d:p:f::m:h",
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
            case 'b':
                if (!optarg && argv[optind] && argv[optind][0] != '-')
                {
                    block_notified = atoi(argv[optind]);
                    ++optind;
                }
                else
                    block_notified = optarg ? atoi(optarg) : 1;
                break;
            case 'd':
                data_dir = strdup(optarg);
                break;
            case 'p':
                pid_file = strdup(optarg);
                break;
            case 'f':
                if (!optarg && argv[optind] && argv[optind][0] != '-')
                {
                    forked = atoi(argv[optind]);
                    ++optind;
                }
                else
                    forked = optarg ? atoi(optarg) : 1;
                break;
            case 'm':
                processes = atoi(optarg);
                if (processes < -1)
                    processes = -1;
                break;
            case 'h':
            default:
                print_help(options);
                exit(-1);
                break;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_set_level(LOG_INFO);

    read_config(config_file);
    if (config_file)
        free(config_file);

    /* Any supplied command line options take precedent... */
    if (log_file)
    {
        strncpy(config.log_file, log_file, sizeof(config.log_file)-1);
        free(log_file);
    }
    if (data_dir)
    {
        strncpy(config.data_dir, data_dir, sizeof(config.data_dir)-1);
        free(data_dir);
    }
    if (pid_file)
    {
        strncpy(config.pid_file, pid_file, sizeof(config.pid_file)-1);
        free(pid_file);
    }
    if (forked > -1)
        config.forked = forked;
    if (processes != 1)
        config.processes = processes;
    if (block_notified > -1)
        config.block_notified = block_notified;

    log_set_level(LOG_FATAL - config.log_level);
    if (config.log_file[0])
    {
        fd_log = fopen(config.log_file, "a");
        if (!fd_log)
            log_warn("Failed to open log file: %s", config.log_file);
        else
        {
            setvbuf(fd_log, NULL, _IOLBF, 0);
            log_set_fp(fd_log);
        }
    }

    print_config();
    log_info("Starting pool on: %s:%d", config.pool_listen, config.pool_port);

    if (config.forked)
    {
        log_info("Daemonizing");
        char *pf = NULL;
        if (config.pid_file[0])
            pf = config.pid_file;
        forkoff(pf);
    }

    if (config.processes < 0 || config.processes > 1)
    {
        int nproc = sysconf(_SC_NPROCESSORS_ONLN);
        if (config.processes > nproc)
        {
            log_warn("Requested more processes than available cores (%d)",
                    nproc);
            config.processes = -1;
        }
        nproc = config.processes < 0 ? nproc : config.processes;
        log_info("Launching processes: %d", nproc);
        int pid = 0;
        while(nproc--)
        {
            pid = fork();
            if (pid < 1)
                break;
            if (pid > 0)
                continue;
        }
        if (pid > 0)
        {
            while (waitpid(-1, 0, 0) > 0)
            {}
            _exit(0);
        }
        else if (pid == 0 && nproc == 0)
            abattoir = true;
    }
    else
        abattoir = true;

    log_set_udata(&mutex_log);
    log_set_lock(log_lock);
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);
    atexit(cleanup);

    int err = 0;
    if ((err = database_init(config.data_dir)) != 0)
    {
        log_fatal("Failed to initialize database. Return code: %d", err);
        goto cleanup;
    }

    bstack_new(&bst, BLOCK_TEMPLATES_MAX, sizeof(block_template_t),
            template_recycle);
    bstack_new(&bsh, BLOCK_HEADERS_MAX, sizeof(block_t), NULL);

    bn_ctx = BN_CTX_new();
    base_diff = NULL;
    BN_hex2bn(&base_diff,
        "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");

    uuid_t iid;
    uuid_generate(iid);
    memcpy(&instance_id, iid, 4);

    clients_init();

    wui_context_t uic;
    memset(&uic, 0, sizeof(wui_context_t));
    uic.port = config.webui_port;
    uic.pool_stats = &pool_stats;
    uic.pool_fee = config.pool_fee;
    strncpy(uic.pool_listen, config.pool_listen, sizeof(uic.pool_listen)-1);
    uic.pool_port = config.pool_port;
    uic.pool_ssl_port = config.pool_ssl_port;
    uic.allow_self_select = !config.disable_self_select;
    uic.payment_threshold = config.payment_threshold;
    if (config.webui_port)
        start_web_ui(&uic);

    run();

cleanup:
    return 0;
}
