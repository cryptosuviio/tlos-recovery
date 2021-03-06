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

      void add_internal(name account_name) {
         /* Here we check should we place the account to the unstaking list,
            or directly to the token recovery list */

         eosiosystem::del_bandwidth_table staked("eosio"_n, account_name.value);
         auto staked_iterator = staked.find(account_name.value);
         if(staked_iterator != staked.end()) {
            /* We put the account to the unstaking list */
            /* we use _this, _this scope for simplicity */
            unstake_accounts unstaking(get_self(), get_self().value);

            /* It would be a fun idea to get the account to pay (since we will be
               privileged), but that would have complicated testing.
               And would need total refactoring of the contract. */
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

      /* There is one known corner case with add():
         if account is added while staked, and then the account unstakes by
         themselves, and the contract operator adds the account again, then
         unstake() would not proceed until the account is removed (since it's
         already in the recover table, where it tries to re-add the account).

         However, privilege should not be given to the contract until adding is
         done. Otherwise it would be a huge security vulnerability. After this
         contract is privileged, the account lists should be modified only
         by BP multisig.
       */
      [[eosio::action]]
      void add(std::vector<name> account_names) {
         require_auth(get_self());

         for(auto& account_name : account_names) {
            add_internal(account_name);
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
      void remove(std::vector<name> account_names) {
         require_auth(get_self());

         for(auto& account_name : account_names) {
            remove_internal(account_name);
         }
      }

      [[eosio::action]]
      void removeme(name account_name) {
         require_auth(account_name);

         remove_internal(account_name);
      }

      /* unstake() and recover() work without account names to minimize attack surface */
      [[eosio::action]]
      void unstake(uint8_t n) {
         int i;

         DEBUG("Unstaking the next account from the list...");
         unstake_accounts unstaking(get_self(), get_self().value);

         auto unstaking_iterator = unstaking.begin();

         for(i = 0;  i < n && unstaking_iterator != unstaking.end(); i++) {
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

            unstaking_iterator = unstaking.erase(unstaking_iterator);
         }

         check(i > 0, "No accounts to unstake");
      }

      [[eosio::action]]
      void recover(uint8_t n) {
         int i;

         DEBUG("Recovering tokens from the next account from the list...");
         /* REMEMBER: Remember to check that unstaking is done */
         recover_accounts recovering(get_self(), get_self().value);

         auto recovering_iterator = recovering.begin();

         /* The list is implicitly ordered for us, since both of the tables
            have "name" as their primary key. That's why we don't need to care
            that unstaking delay would disturb us? */
         for(i = 0;  i < n && recovering_iterator != recovering.end(); i++) {
            DEBUG("Recover TLOS from: ", recovering_iterator->account_name);

            /* Unstaking must not be in progress */
            eosiosystem::refunds_table refunding("eosio"_n, recovering_iterator->account_name.value);
            auto refunding_iterator = refunding.find(recovering_iterator->account_name.value);
            if(refunding_iterator != refunding.end()) {
               /* We try to refund, if successful, we will skip the account for now. If not successful, we will bail out */
               eosiosystem::system_contract::refund_action refund("eosio"_n, {recovering_iterator->account_name, "active"_n});
               refund.send(recovering_iterator->account_name);
               DEBUG("eosio::refund() had to be called, skipping this account for now...");
               recovering_iterator++;
               continue;
            }

            asset balance = token::get_balance("eosio.token"_n, recovering_iterator->account_name, symbol_code("TLOS"));

            if(balance.amount > 0) {
               token::transfer_action transfer("eosio.token"_n, {recovering_iterator->account_name, "active"_n});
               transfer.send(recovering_iterator->account_name, get_self(), balance, "Recovering tokens per TBNOA: https://chainspector.io/dashboard/ratify-proposals/0");
            } else {
               DEBUG("Nothing to recover, skipping...");
            }

            recovering_iterator = recovering.erase(recovering_iterator);
         }

         check(i > 0, "No accounts to recover");
      }
};
