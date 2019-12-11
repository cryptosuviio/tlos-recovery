/*
 * Copyright 2019 Ville Sundell/CRYPTOSUVI OSK
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/name.hpp>
#include <eosio.system/eosio.system.hpp>
#include <eosio.token/eosio.token.hpp>

/* We can leave debug messages on, since that could help solving problems on Mainnet */
#define DEBUG(...) print("tlosrecovery: ", __VA_ARGS__, "\n");
#define LOOP(x, n) for(int i = 0;  i < n; i++){x;}

using namespace eosio;

class [[eosio::contract]] tlosrecovery : public contract {
   public:
      using contract::contract;

      struct [[eosio::table]] account {
         name account_name;
         auto primary_key() const { return account_name.value; }
      };

      /* By having a two separate tables, we are building an implicit and safe state machine */
      typedef multi_index<"unstake"_n, account> unstake_accounts;
      typedef multi_index<"recover"_n, account> recover_accounts;

      /* Originally I had plans to make this contract completely autonomous:
            * Let anyone add accounts
            * Check added account @owner and @active for inactivity (get_permission_last_used())
            * Check that the account is old enough (get_account_creation_time())
            * Let anyone remove accounts by re-evaluating the conditions above

         But that would have added complexity, and unnecessary attack vectors, since this is
         a single use contract, autonomous function is not needed. Hence, require_auth().
      */

      [[eosio::action]]
      void add(name account_name) {
         require_auth(get_self());

         /* Here we check should we place the account to the unstaking list,
            or directly to the token recovery list */
         eosiosystem::del_bandwidth_table staked("eosio"_n, account_name.value);
         auto staked_iterator = staked.find(account_name.value);
         if (staked_iterator != staked.end()) {
            /* We put the account to the unstaking list */
            /* we use _this, _this scope for simplicity */
            unstake_accounts unstaking(get_self(), get_self().value);

            /* It would be a fun idea to get the account to pay (since we will be
               privileged), but that would have complicated testing. */
            unstaking.emplace(get_self(), [&](auto& a) {
               a.account_name = account_name;
            });

            DEBUG("Adding account to the unstaking list: ", account_name);
         } else {
            /* Nothing to unstake, let's just recover the funds */

            recover_accounts recovering(get_self(), get_self().value);

            recovering.emplace(get_self(), [&](auto& a) {
               a.account_name = account_name;
            });

            DEBUG("Adding account to the recovery list: ", account_name);
         }
      }

      void remove_internal(name account_name) {
         /* Removing recovering could be inside an IF, but we want also handle cases
            that we think are impossible at the moment (since it does not cost us anything),
            welcome to smart contracts :D */

         unstake_accounts unstaking(get_self(), get_self().value);

         auto unstaking_iterator = unstaking.find(account_name.value);
         if(unstaking_iterator != unstaking.end()) {
            DEBUG("Removing account from the unstake list: ", account_name);
            unstaking.erase(unstaking_iterator);
         }

         recover_accounts recovering(get_self(), get_self().value);

         auto recovering_iterator = recovering.find(account_name.value);
         if(recovering_iterator != recovering.end()) {
            DEBUG("Removing account from the recovery list: ", account_name);
            recovering.erase(recovering_iterator);
         }
      }

      [[eosio::action]]
      void remove(name account_name) {
         require_auth(get_self());

         remove_internal(account_name);
      }

      [[eosio::action]]
      void removeme(name account_name) {
         require_auth(account_name);

         remove_internal(account_name);
      }

      /* unstake() and claim() work without arguments to minimize attack surface */
      [[eosio::action]]
      void unstake() {
         DEBUG("Unstaking the next account from the list...");

         unstake_accounts unstaking(get_self(), get_self().value);

         auto unstaking_iterator = unstaking.begin();
         check(unstaking_iterator != unstaking.end(), "No accounts to unstake");

         DEBUG("Unstaking: ", unstaking_iterator->account_name);
         eosiosystem::del_bandwidth_table staked("eosio"_n, unstaking_iterator->account_name.value);
         auto staked_iterator = staked.find(unstaking_iterator->account_name.value);
         if(staked_iterator != staked.end()) {
            /* We are using inline actions, since deferred actions will be depracated */
            eosiosystem::system_contract::undelegatebw_action unstaker("eosio"_n, {unstaking_iterator->account_name, "active"_n});
            unstaker.send(unstaking_iterator->account_name, unstaking_iterator->account_name, staked_iterator->net_weight, staked_iterator->cpu_weight);
            DEBUG("Sent inline transaction eosio::undelegate()...");
         } else {
            DEBUG("Nothing to unstake? Skipping...");
         }

         recover_accounts recovering(get_self(), get_self().value);

         recovering.emplace(get_self(), [&](auto& a) {
            a.account_name = unstaking_iterator->account_name;
         });

         unstaking.erase(unstaking_iterator);
      }

      [[eosio::action]]
      void recover() {
         DEBUG("Recovering tokens from the next account from the list...");
         /* REMEMBER: Remember to check that unstaking is done */
         recover_accounts recovering(get_self(), get_self().value);

         auto recovering_iterator = recovering.begin();

         /* The list is implicitly ordered for us, since both of the tables
            have "name" as their primary key. That's why we don't need to care
            that unstaking delay would disturb us? */
         check(recovering_iterator != recovering.end(), "No accounts to recover");
         DEBUG("Recover TLOS from: ", recovering_iterator->account_name);

         /* Unstaking must not be in progress */
         eosiosystem::refunds_table refunding("eosio"_n, recovering_iterator->account_name.value);
         auto refunding_iterator = refunding.find(recovering_iterator->account_name.value);
         check(refunding_iterator == refunding.end(), "Unstaking still in progress");

         asset balance = token::get_balance("eosio.token"_n, recovering_iterator->account_name, symbol_code("TLOS"));

         if (balance.amount > 0) {
            token::transfer_action transfer("eosio.token"_n, {recovering_iterator->account_name, "active"_n});
            transfer.send(recovering_iterator->account_name, get_self(), balance, "Recovering tokens per TBNOA: https://chainspector.io/dashboard/ratify-proposals/0");
         } else {
            DEBUG("Nothing to recover, skipping...");
         }

         recovering.erase(recovering_iterator);
      }

      [[eosio::action]]
      void unstakemany(uint8_t n) {
         LOOP(unstake(), n);
      }

      [[eosio::action]]
      void recovermany(uint8_t n) {
         LOOP(recover(), n);
      }
};
