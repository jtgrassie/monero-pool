# monero-pool

A Monero mining pool server written in C.

Design decisions are focused on performance and efficiency, hence the use of
libevent and LMDB.  Currently it uses only two threads (one for the stratum
clients and one for the web UI clients). It gets away with this thanks to the
efficiency of libevent (for the stratum clients) and some sensible
proxying/caching being placed in front of the [web UI](#web-ui).

The single payout mechanism is PPLNS, which favors loyal pool miners.

I have no plans to add any other payout mechanisms or other coins. Work should
stay focussed on performance, efficiency and stability.

The pool also now supports a new, experimental and optional, method of mining
for the pool miners, whereby miners can select their *own* block template to
mine on. Further information can be found in [stratum-ss.md](./stratum-ss.md).

## Project status

I have tested this quite a bit on the Monero testnet (if you plan
to do the same, ensure to use `--testnet` flag when starting your wallet and
daemon) and mainnet, but there is always room for improvement. Please see the
[TODO](./TODO) file for the current list of things that could do with looking
at.

There is also a reference mainnet pool setup and running at
[http://monerop.com](http://monerop.com).

If you want to help with testing or help setting up your own pool, give me a
shout on IRC: jtgrassie on Freenode.

## Compiling from source

### Dependencies

The build system now requires the Monero source tree to be cloned and compiled.
Follow the
[instructions](https://github.com/monero-project/monero#compiling-monero-from-source)
for compiling Monero, then export the following variable:

```bash
export MONERO_ROOT=/path/to/cloned/monero
```

Replacing the path appropriately.

Beyond the Monero dependencies, the following extra libraries are also required
to build the pool:

- liblmdb
- libevent
- json-c
- libmicrohttpd
- uuid

As an example, on Ubuntu, these dependencies can be installed with the following
command:

```
sudo apt-get install liblmdb-dev libevent-dev libjson-c-dev libmicrohttpd-dev uuid-dev
```
### Compile

First install all the dependencies as described above.

Then to compile the pool as a release build, run:

```
make release
```

The application will be built in `build/release/`.

Optionally you can compile a debug build by simply running:

```
make
```

Debug builds are output in `build/debug/`.

## Configuration

Copy and edit the `pool.conf` file to either the same directory as the compiled
binary `monero-pool`, or place it in your home directory or launch `monero-pool`
with the flag `--config-file path/to/pool.conf` to use a custom location. The
configuration options should be self explanatory.

#### Block notification

There is one configuration option that deserves a special mention.

You can optionally start the pool with the flag `--block-notified` (or set in
the config file: `block-notified = 1`). This will prevent the pool from
*polling* for new blocks using a timer, and instead, fetch a new block template
when it receives a *signal* (specifically, *SIGUSR1*). Now whenever you start
`monerod`, you'll make use of its `--block-notify` option.

E.g.

<pre>
monerod ... <b>--block-notify '/usr/bin/pkill -USR1 monero-pool'</b>
</pre>

This instructs `monerod` to send the required signal, *SIGUSR1*, to your pool
whenever a new block is added to the chain.

Using this mechanism has a significant benefit - your pool *immediatley* knows
when to fetch a new block template to send to your miners. You're essentially
giving your miners a head-start over miners in pools which use polling (which
is what all the other pool implementations do).

## Running

Ensure you have your Monero daemon (`monerod`) and wallet RPC
(`monero-wallet-rpc`) up and running with the correct host and port settings as
defined in the pool config file.

It is highly recommended to run these on the same host as the pool server to
avoid network latency when their RPC methods are called.

Then simply `cd build/debug|release` and run `./monero-pool`.

## Web UI

There is a minimal web UI that gets served on the port specified in the config
file. It's advisable to use either Apache or Nginx as a proxy in front of this
with some appropriate caching.

## Supporting the project

This mining pool has **no built-in developer donation** (like *other* mining
pool software has), so if you use it and want to donate, XMR donations to:

```
451ytzQg1vUVkuAW73VsQ72G96FUjASi4WNQse3v8ALfjiR5vLzGQ2hMUdYhG38Fi15eJ5FJ1ZL4EV1SFVi228muGX4f3SV
```

![QR code](./qr-small.png)

would be very much appreciated.

## License

Please see the [LICENSE](./LICENSE) file.

[//]: # ( vim: set tw=80: )
