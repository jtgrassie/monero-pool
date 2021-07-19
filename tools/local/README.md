`create_test_env.sh` - creates the runtime environment
<img width="767" alt="create_test_env" src="https://user-images.githubusercontent.com/20960742/126140648-091783b7-3834-4a85-b4d9-40523c74df7f.png">

`launch.sh` - builds and starts the pool services for testing, $1=branchname $2=buildtype  if not specified they will default to master/debug
<img width="767" alt="launch" src="https://user-images.githubusercontent.com/20960742/126141250-643b9863-678b-4b90-807e-49724af22dd7.png">

`run_integ_tests.sh` - run all integration tests or specify a single test file to run at $1
<img width="769" alt="run_integ_tests sh" src="https://user-images.githubusercontent.com/20960742/126141271-a23340da-8a05-41e4-bc67-5be896343222.png">

`launch_and_test.sh` - launches, and runs integration tests, and then kills the environment takes the same parameters as launch.sh

`kill_all_test_env_processes.sh` - used to kill any stray processes 

A private stagenet with fixed diff of 300000 can pass all tests in two minutes with a hashrate of 2500h/s

If you want to setup your own stagenet see:
example stagenet https://github.com/moneroexamples/private-testnet/blob/master/make_private_stagenet.sh

If you start an empty blockchain stagenet or testnet you may want to edit the src/hardforks/hardforks.cpp before you compile your private [stage|test]net to quickly get to the latest version.   Mine in monerod after the [stage|test]net is setup, and then start mining with the pool software after hitting the latest version of the block.   See the below changes to stagenet_hard_forks. Only do this on a private [state|test]net.

```
const hardfork_t stagenet_hard_forks[] = {
  // version 1 from the start of the blockchain
  { 1, 1, 0, 1341378000 },

  // versions 2-7 in rapid succession from March 13th, 2018
  { 2, 2, 0, 1521000000 },
  { 3, 3, 0, 1521120000 },
  { 4, 4, 0, 1521240000 },
  { 5, 5, 0, 1521360000 },
  { 6, 6, 0, 1521480000 },
  { 7, 7, 0, 1521600000 },
  { 8, 8, 0, 1537821770 },
  { 9, 9, 0, 1537821771 },
  { 10, 10, 0, 1550153694 },
  { 11, 11, 0, 1550225678 },
  { 12, 12, 0, 1571419280 },
  { 13, 675405, 0, 1598180817 },
  { 14, 676125, 0, 1598180818 },
};
```