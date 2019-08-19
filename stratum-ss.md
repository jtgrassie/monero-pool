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

To address these concerns, I've implemented a new, experimental and optional
*mode* to this pool, which enables miners to select their *own* block template
to mine on.

What follows are the instructions to test this new mode and the changes made to
the stratum messages. For a miner to test against the pool, there is a very
simple miner, [monero-powpy](https://github.com/jtgrassie/monero-powpy)
(`stratum-ss-miner.py`), and also a hastily cobbled together branch of
[XMRig](https://github.com/jtgrassie/xmrig/tree/stratum-ss).

## Building

The only variation to the standard build instructions in the
[README](./README.md#compiling-from-source), is that you'll first need to fetch
and compile the latest Monero ***master*** branch.

## Running

Start your newly compiled `monerod` and `monero-wallet-rpc`. For example, in one
shell:

    cd "$MONERO_ROOT"/build/Linux/master/release/bin
    ./monerod --testnet

And in another shell:

    cd "$MONERO_ROOT"/build/Linux/master/release/bin
    ./monero-wallet-rpc --testnet --rpc-bind-port 28084 --disable-rpc-login \
        --password "" --wallet-file ~/testnet-pool-wallet

Next, in a third shell, run `monero-pool`. Instructions per the
[README](./README.md#running).

Lastly you'll need to run a miner that supports this new stratum mode (see
above):

 - If using [monero-powpy](https://github.com/jtgrassie/monero-powpy), install
   the requirements per the projects
   [README](https://github.com/jtgrassie/monero-powpy/blob/master/README.md),
   then just run the `stratum-ss-miner.py` miner, optionally editing the
   parameters first.

 - If using [XMRig](https://github.com/jtgrassie/xmrig/tree/stratum-ss), edit
   your pool object in your `config.json` file setting the `algo` field to
   `"algo": "cryptonight/r"` and setting the `self-select` field to your daemon
   address (e.g. `"self-select": "localhost:28081"`).

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

(3) The miner can now call the daemons RPC method
[get_block_template](https://ww.getmonero.org/resources/developer-guides/daemon-rpc.html#get_block_template)
with parameters `extra_nonce: extra_nonce` (implemented in pull request
[#5728](https://github.com/monero-project/monero/pull/5728)), and
`wallet_address: pool_wallet`.

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
