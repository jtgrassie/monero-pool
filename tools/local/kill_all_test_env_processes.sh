#!/bin/bash


if [ ! -f ./.env ]; then
    echo "please run ./create_test_env.sh"
    exit 1
fi

source ./.env

activate_script="./testenv/bin/activate"
if [ ! -f $activate_script ]; then
    echo "please run ./create_test_env.sh"
    exit 1
fi

for i in `ls $test_build_dir/*.pid`; do
    pid=`cat $i`
    echo "killing $i at $pid"
    kill $pid
done