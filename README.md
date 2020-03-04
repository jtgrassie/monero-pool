# monero-pool

A Monero mining pool server written in C.

Design decisions are focused on performance and efficiency, hence the use of
libevent and LMDB.  Currently it uses only *two* threads under normal operation
(one for the stratum clients and one for the web UI clients). It gets away with
this thanks to the efficiency of both LMDB and libevent (for the stratum
clients) and some sensible proxying/caching being placed in front of the [web
UI](#web-ui).

This pool was the *first* pool to support RandomX and is currently the *only*
pool which supports the RandomX fast/full-memory mode.

The single payout mechanism is PPLNS, which favors loyal pool miners, and there
are no plans to add any other payout mechanisms or other coins. Work should stay
focussed on performance, efficiency and stability.

The pool also supports an optional method of mining whereby miners select their
*own* block template to mine on. Further information can be found in the
document: [Stratum mode self-select](./stratum-ss.md).

For testing, a reference mainnet pool can be found at
[monerop.com](http://monerop.com).

## Compiling from source

### Dependencies

The build system requires the Monero source tree to be cloned and compiled.
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
- uuid

As an example, on Ubuntu, these dependencies can be installed with the following
command:

```
sudo apt-get install liblmdb-dev libevent-dev libjson-c-dev uuid-dev
```
### Compile

After installing all the dependencies as described above, to compile the pool as
a release build, run:

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

During compilation, a copy of [pool.conf](./pool.conf) is placed in the output
build directory. Edit this file as you see fit. When running the pool, if a
custom location is not set via the command-line parameter `--config-file
<file>`, the pool will first look for this file in the same directory as the
pool binary, then in the current users home directory. The configuration options
should all be self explanatory.

There are also some [command-line parameters](#command-line-parameters) which
can be used to override some of these settings.

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

Using this mechanism has a *significant* benefit - your pool *immediatley* knows
when to fetch a new block template to send to your miners. You're essentially
giving your miners a head-start over miners in pools which use polling (which is
what all the other pool implementations do).

## Running

Ensure you have your Monero daemon (`monerod`) and wallet RPC
(`monero-wallet-rpc`) up and running with the correct host and port settings as
defined in your pool config file.

It is highly recommended to run these on the same host as the pool server to
avoid any network latency when their RPC methods are called.

Then simply `cd build/[debug|release]` and run `./monero-pool`.

### Command-line parameters

A few of the configuration options can be overridden via the following
command-line parameters:

    -c, --config-file <file>
    -l, --log-file <file>
    -b, --block-notified [0|1]
    -d, --data-dir <dir>
    -p, --pid-file <file>
    -f, --forked [0|1]

## Web UI

There is a minimal web UI that gets served on the port specified in the config
file. If you plan on running a *public* pool, it's advisable to use either
Apache or Nginx as a proxy in front of this with some appropriate caching
configured. The goal is to offload browser based traffic to something built for
the task and allow the pool to focus on its primary function - serving miners.

If you intend to make changes to the web UI, note that the HTML gets compiled
into the pool binary. The single web page that gets served simply makes use of a
JSON endpoint to populate the stats.

## SSL

The pool has been tested behind both [HAProxy](http://www.haproxy.org/) and
[stunnel](https://www.stunnel.org/), so if you wish to provide SSL access to the
pool, these are both good options and simple to setup. The [reference
pool](https://monerop.com) makes use of HAProxy and port 4343 for SSL traffic.

The web UI, as mentioned above, should ideally be placed behind a *caching
proxy*. Therefore SSL termination should be be configured there (i.e. in
Apache/Nginx).

## Help / Contact

If you need help setting up your own pool, you can find
me (jtgrassie) on IRC in [#monero-pool](irc://chat.freenode.net/#monero-pool)
and many of the other Monero channels.

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
