# monero-pool

A Monero mining pool server written in C.

Design decisions are focused on performance, hence the use of libevent and LMDB.
Currently this is single threaded (which is fine because of libevent) but work
is planned to offload some of the tasks to a thread pool to allow even faster
operation.

The single payout mechanism is PPLNS, which favors loyal pool miners.

I have no plans to add any other payout mechanisms or other coins. Work should
ideally stay focussed on performance and stability.

## Project status

Definitely "alpha". I have tested quite a bit on the Monero testnet (if you plan
to do the same, ensure to use `--testnet` flag when starting your wallet and
daemon) and there is certainly room for improvement. Please see the
[TODO](./TODO) file for the current list of things that could do with looking
at.

There is a reference pool setup at [http://monerop.com](http://monerop.com) so
it can be load tested further.

If you want to help with testing or help setting up your own pool, give me a
shout on IRC: jtgrassie on Freenode.

## Compiling from source

### Dependencies

- liblmdb
- libevent
- json-c
- boost
- openssl
- libmicrohttpd
- uuid

As an example, on Ubuntu, the dependencies can be installed with the following
command:

```
sudo apt-get install libjson-c-dev uuid-dev libevent-dev libmicrohttpd-dev \
  liblmdb-dev libboost-all-dev openssl
```

Another obvious dependency is to have a running Monero daemon (`monerod`) and a
running wallet RPC (`monero-wallet-rpc`) for the pool wallet. It is highly
recommended to run these on the same host as the pool server to avoid network
latency.

### Compile

First install all the dependencies.

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
options are self explanatory.

## Running

Ensure you have your daemon and wallet RPC up and running with the correct host
and port settings in the pool config file.

Then simply `cd build/debug|release` and run `./monero-pool`.

## Supporting the project

This mining pool has **no built-in developer donation** (like *other* mining
pool software has), so if you use it and want to donate, XMR donations to:

```
451ytzQg1vUVkuAW73VsQ72G96FUjASi4WNQse3v8ALfjiR5vLzGQ2hMUdYhG38Fi15eJ5FJ1ZL4EV1SFVi228muGX4f3SV
```

would be very much appreciated.

## License

Please see the [LICENSE](./LICENSE) file.

[//]: # ( vim: set tw=80: )
