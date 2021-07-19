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
test_branch=$1
build_type=$2

if [ -z "$1" ]; then
    test_branch="master"
fi

if [ -z "$2" ]; then
    build_type="debug"
fi

./launch.sh $test_branch $build_type

if [ $? -ne 0 ]; then
    echo "Launch script failed see above!!"
    exit 1
fi

./run_integ_tests.sh

./kill_all_test_env_processes.sh