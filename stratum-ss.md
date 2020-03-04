# Stratum mode self-select

One major concern of pool mining has been that of pool centralization. A
malicious pool operator, controlling a pool with a significant portion of the
total network hash-rate (e.g. 51% or more), has the ability to perform various
attacks. This is made possible due to the fact miners have no visibility into
what block template they are actually mining on. This leads to another concern -
censorship of transactions. Again, as miners have no visibility of the block
template they are mining, they also have no visibility of the transactions
included in the block template. This enables a malicious pool be selective as to
which transactions get included (or not) into a block.

To address these concerns, I've implemented an optional *mode* for this pool,
which enables miners to select their *own* block template to mine on.

Along with this pool, the popular miner [XMRig](https://github.com/xmrig/xmrig)
has this mode implemented. There is also a very simple [demonstration
miner](https://github.com/jtgrassie/monero-powpy/blob/master/stratum-ss-miner.py)
in the [monero-powpy](https://github.com/jtgrassie/monero-powpy) project which
can be used to augment this document for implementers.


## Specification

What follows are the stratum message and flow changes required to enable pool
miners to mine on miner created block templates.

(1) The miner logs into the pool with an additional *mode* parameter:

    {
        "method": "login",
        "params": {
            "login": "wallet address",
            "pass": "password",
            "agent": "user-agent/0.1",
            "mode": "self-select" /* new field */
        },
        "jsonrpc": "2.0",
        "id":1
    }

(2) The pool responds with a result job which includes the pool wallet address
and an extra nonce:

    {
        "result": {
            "job": {
                "pool_wallet": "pool wallet address", /* new field */
                "extra_nonce": "extra nonce hex", /* new field */
                "target": "target hex",
                "job_id": "job id"
            },
            "id": "client id",
            "status": "OK"
        },
        "jsonrpc": "2.0",
        "id":1
    }

(3) The miner can now call a local, or remote, Monero daemons RPC method
[get_block_template](https://getmonero.org/resources/developer-guides/daemon-rpc.html#get_block_template)
with parameters `extra_nonce: "<extra nonce hex>"` (implemented in pull request
[#5728](https://github.com/monero-project/monero/pull/5728)) and
`wallet_address: "<pool wallet address>"`, using the result values from step #2
above.

The miner now informs the pool of the resulting block template it will use for
the job:

    {
        "method":"block_template", /* new method */
        "params": {
            "id": "client id",
            "job_id": "job id",
            "blob": "block template hex",
            "height": N,
            "difficulty": N,
            "prev_hash": "prev hash hex"
        },
        "jsonrpc": "2.0",
        "id":1
    }

(4) The pool validates and caches the supplied block template and responds with
a status:

    {
        "result": {
            "status": "OK",
            "error", null
        },
        "jsonrpc": "2.0",
        "id":1
    }

The *degree* of validation required is at the discretion of the pool
implementer.  This pool simply ensures the supplied block template can be parsed
as a Monero block and that the destination coinbase reward pays out to the pool
wallet.

(5) The miner submits results. No changes here:

    {
        "method":"submit",
        "params": {
            "id": "client id",
            "job_id": "job id",
            "nonce": "deadbeef",
            "result": "hash hex"
        },
        "jsonrpc": "2.0",
        "id":1
    }

(6) The pool responds to job submissions. No changes here:

    {
        "result": {
            "status": "OK",
            "error", null
        },
        "jsonrpc": "2.0",
        "id":1
    }

(7) The pool asks the miner to start a new job:

    {
        "method": "job",
        "params": {
            "pool_wallet": "pool wallet address", /* new field */
            "extra_nonce": "extra nonce hex", /* new field */
            "target": "target hex",
            "job_id": "job id"
        },
        "jsonrpc": "2.0",
        "id":1
    }

The miner now repeats from step #3.

[//]: # ( vim: set tw=80: )
