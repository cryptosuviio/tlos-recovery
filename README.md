# tlos-recovery
EOSIO Smart contract to facilitate TLOS token recovery on Telos from unclaimed accounts

## Introduction
tlos-recovery is an EOSIO smart contract originally desiged to recover TLOS tokens from Telos accounts which have never been used.
This is supported by the original Telos Blockchain Network Operating Agreement, with community accepted amendment #0: https://chainspector.io/dashboard/ratify-proposals/0

## Usage
The contract is meant to be used as follows:

1. Contract operator will deploy the contract, and feed accounts into it using add()
2. Contract operator will resign (changing @active and @owner to "eosio") after verifying that the right accounts are present
3. BPs create and approve a msig setting the contract privileged after verifying the contract and accounts
4. Any account can then start calling unstake() and recover() (after 3 days)

## Current implementation
Currently the contract is deployed to "tlosrecovery" on both, Telos Testnet and Telos Mainnet:
 * https://telos-test.bloks.io/account/tlosrecovery
 * https://telos.bloks.io/account/tlosrecovery
 
 With codehash `8ce4f2037ce4de9531a67dd03d257a5485a845eec32e3de959b78717af1b63b0`
