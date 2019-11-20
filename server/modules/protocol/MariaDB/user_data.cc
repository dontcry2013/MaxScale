/*
 * Copyright (c) 2019 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "user_data.hh"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <maxbase/host.hh>
#include <maxsql/mariadb_connector.hh>
#include <maxsql/mariadb.hh>
#include <maxscale/server.hh>
#include <maxscale/service.hh>
#include <maxscale/protocol/mariadb/module_names.hh>
#include <maxscale/routingworker.hh>
#include <maxscale/config.hh>
#include <maxscale/cn_strings.hh>
#include <maxscale/secrets.hh>
#include "sqlite_strlike.hh"

using std::string;
using mxq::MariaDB;
using UserEntry = mariadb::UserEntry;
using SUserEntry = std::unique_ptr<UserEntry>;
using MutexLock = std::unique_lock<std::mutex>;
using Guard = std::lock_guard<std::mutex>;
using mariadb::UserSearchSettings;

namespace
{

constexpr auto acquire = std::memory_order_acquire;
constexpr auto release = std::memory_order_release;
constexpr auto npos = string::npos;
constexpr int ipv4min_len = 7;      // 1.1.1.1

namespace mariadb_queries
{
const string users_query = "SELECT * FROM mysql.user;";
const string db_grants_query =
    "SELECT DISTINCT * FROM ("
    // Select users/roles with general db-level privs ...
    "(SELECT a.user, a.host, a.db FROM mysql.db AS a) UNION "
    // and combine with table privs counting as db-level privs
    "(SELECT a.user, a.host, a.db FROM mysql.tables_priv AS a) UNION "
    // and finally combine with column-level privs as db-level privs
    "(SELECT a.user, a.host, a.db FROM mysql.columns_priv AS a) ) AS c;";
const string roles_query = "SELECT a.user, a.host, a.role FROM mysql.roles_mapping AS a;";
const string proxies_query = "SELECT DISTINCT a.user, a.host FROM mysql.proxies_priv AS a "
                             "WHERE a.proxied_host <> '' AND a.proxied_user <> '';";
}

namespace clustrix_queries
{
const string users_query = "SELECT *, IF(a.privileges & 1048576, 'Y', 'N') AS global_priv "
                           "FROM system.users AS u LEFT JOIN system.user_acl AS a ON (u.username = a.role);";
const string db_grants_query = "SELECT * FROM system.user_acl;";
}
}

void MariaDBUserManager::start()
{
    mxb_assert(!m_updater_thread.joinable());
    m_keep_running.store(true, release);
    update_user_accounts();
    m_updater_thread = std::thread([this] {
                                       updater_thread_function();
                                   });
}

void MariaDBUserManager::stop()
{
    mxb_assert(m_updater_thread.joinable());
    m_keep_running.store(false, release);
    m_notifier.notify_one();
    m_updater_thread.join();
}

void MariaDBUserManager::update_user_accounts()
{
    {
        Guard guard(m_notifier_lock);
        m_update_users_requested.store(true, release);
    }
    m_notifier.notify_one();
}

void MariaDBUserManager::set_credentials(const std::string& user, const std::string& pw)
{
    Guard guard(m_settings_lock);
    m_username = user;
    m_password = pw;
}

void MariaDBUserManager::set_backends(const std::vector<SERVER*>& backends)
{
    Guard guard(m_settings_lock);
    m_backends = backends;
}

std::string MariaDBUserManager::protocol_name() const
{
    return MXS_MARIADB_PROTOCOL_NAME;
}

void MariaDBUserManager::updater_thread_function()
{
    using mxb::TimePoint;
    using mxb::Duration;
    using mxb::Clock;

    // Minimum wait between update loops. User accounts should not be changing continuously.
    const std::chrono::seconds default_min_interval(1);
    // Default value for scheduled updates. Cannot set too far in the future, as the cv wait_until bugs and
    // doesn't wait.
    const std::chrono::hours default_max_interval(24);

    // In the beginning, don't update users right away as monitor may not have started yet.
    TimePoint last_update = Clock::now();
    int updates = 0;

    while (m_keep_running.load(acquire))
    {
        /**
         *  The user updating is controlled by several factors:
         *  1) In the beginning, a hardcoded interval is used to try to repeatedly update users as
         *  the monitor is performing its first loop.
         *  2) User refresh requests from the owning service. These can come at any time and rate.
         *  3) users_refresh_time, the minimum time which should pass between refreshes. This means that
         *  rapid update requests may be ignored.
         *  4) users_refresh_interval, the maximum time between refreshes. Users should be refreshed
         *  automatically if this time elapses.
         */
        MXS_CONFIG* glob_config = config_get_global_options();
        auto max_refresh_interval = glob_config->users_refresh_interval;
        auto min_refresh_interval = glob_config->users_refresh_time;

        // Calculate the time for the next scheduled update.
        TimePoint next_scheduled_update = last_update;
        if (updates == 0)
        {
            // If updating has not succeeded even once yet, keep trying again and again,
            // with just a minimal wait.
            next_scheduled_update += default_min_interval;
        }
        else if (max_refresh_interval > 0)
        {
            next_scheduled_update += Duration((double)max_refresh_interval);
        }
        else
        {
            next_scheduled_update += default_max_interval;
        }

        // Calculate the earliest allowed time for next update.
        TimePoint next_possible_update = last_update;
        if (min_refresh_interval > 0 && updates > 0)
        {
            next_possible_update += Duration((double)min_refresh_interval);
        }
        else
        {
            next_possible_update += default_min_interval;
        }

        MutexLock lock(m_notifier_lock);
        // Wait until "next_possible_update", or until the thread should stop.
        m_notifier.wait_until(lock, next_possible_update, [this]() {
                                  return !m_keep_running.load(acquire);
                              });

        // Wait until "next_scheduled_update", or until update requested or thread stop.
        m_notifier.wait_until(lock, next_scheduled_update, [this, &updates]() {
                                  return !m_keep_running.load(acquire)
                                  || m_update_users_requested.load(acquire) || updates == 0;
                              });
        lock.unlock();

        if (m_keep_running.load(acquire) && load_users())
        {
            updates++;
            m_warn_no_servers = true;
        }

        m_update_users_requested.store(false, release);
        last_update = Clock::now();
    }
}

bool MariaDBUserManager::load_users()
{
    MariaDB::ConnectionSettings sett;
    std::vector<SERVER*> backends;

    // Copy all settings under a lock.
    MutexLock lock(m_settings_lock);
    sett.user = m_username;
    sett.password = m_password;
    backends = m_backends;
    lock.unlock();

    sett.password = decrypt_password(sett.password);
    mxq::MariaDB con;

    MXS_CONFIG* glob_config = config_get_global_options();
    sett.timeout = glob_config->auth_conn_timeout;
    if (glob_config->local_address)
    {
        sett.local_address = glob_config->local_address;
    }

    bool found_valid_server = false;

    auto load_result = LoadResult::QUERY_FAILED;
    for (size_t i = 0; i < backends.size() && load_result == LoadResult::QUERY_FAILED; i++)
    {
        SERVER* srv = backends[i];
        if (srv->is_active && srv->is_usable())
        {
            found_valid_server = true;
            const mxb::SSLConfig* srv_ssl_config = srv->ssl().config();
            sett.ssl = (srv_ssl_config && !srv_ssl_config->empty()) ? *srv_ssl_config : mxb::SSLConfig();

            con.set_connection_settings(sett);
            if (con.open(srv->address, srv->port))
            {
                UserDatabase temp_userdata;
                auto srv_type = srv->type();
                switch (srv_type)
                {
                case SERVER::Type::MYSQL:
                case SERVER::Type::MARIADB:
                    load_result = load_users_mariadb(con, srv, &temp_userdata);
                    break;

                case SERVER::Type::CLUSTRIX:
                    load_result = load_users_clustrix(con, srv, &temp_userdata);
                    break;
                }

                switch (load_result)
                {
                    case LoadResult::SUCCESS:
                    // The comparison is not trivially cheap if there are many user entries,
                    // but it avoids unnecessary user cache updates which would involve copying all
                    // the data multiple times.
                    if (temp_userdata.equal_contents(m_userdb))
                    {
                        MXB_INFO("Read %lu user@host entries from '%s' for service '%s'. The data was "
                                 "identical to existing user data.",
                                 m_userdb.n_entries(), srv->name(), m_service->name());
                    }
                    else
                    {
                        // Data changed, update caches.
                        {
                            Guard guard(m_userdb_lock);
                            m_userdb = std::move(temp_userdata);
                        }
                        m_service->sync_user_account_caches();
                        MXB_NOTICE("Read %lu user@host entries from '%s' for service '%s'.",
                                   m_userdb.n_entries(), srv->name(), m_service->name());
                    }
                    break;

                    case LoadResult::QUERY_FAILED:
                    MXB_ERROR("Failed to query server '%s' for user account info. %s",
                              srv->name(), con.error());
                    break;

                    case LoadResult::INVALID_DATA:
                    MXB_ERROR("Received invalid data from '%s' when querying user accounts.",
                              srv->name());
                    break;
                }
            }
            else
            {
                MXB_ERROR("Could not connect to '%s'. %s", srv->name(), con.error());
            }
        }
    }

    if (!found_valid_server && m_warn_no_servers)
    {
        MXB_ERROR("No valid servers from which to query MariaDB user accounts found.");
    }
    return load_result == LoadResult::SUCCESS;
}

MariaDBUserManager::LoadResult
MariaDBUserManager::load_users_mariadb(mxq::MariaDB& con, SERVER* srv, UserDatabase* output)
{
    auto rval = LoadResult::QUERY_FAILED;
    bool got_data = false;
    // Roles were added in server 10.0.5, default roles in server 10.1.1. Strictly speaking, reading the
    // roles_mapping table for 10.0.5 is not required as they won't be used. Read anyway in case
    // diagnostics prints it.
    auto version = srv->version();
    bool role_support = (version.total >= 100005);

    QResult users_res, dbs_res, proxies_res, roles_res;
    // Perform the queries. All must succeed on the same backend.
    if (((users_res = con.query(mariadb_queries::users_query)) != nullptr)
        && ((dbs_res = con.query(mariadb_queries::db_grants_query)) != nullptr)
        && ((proxies_res = con.query(mariadb_queries::proxies_query)) != nullptr))
    {
        if (role_support)
        {
            if ((roles_res = con.query(mariadb_queries::roles_query)) != nullptr)
            {
                got_data = true;
            }
        }
        else
        {
            got_data = true;
        }
    }

    if (got_data)
    {
        rval = LoadResult::INVALID_DATA;
        if (read_users_mariadb(std::move(users_res), output))
        {
            read_dbs_and_roles(std::move(dbs_res), std::move(roles_res), output);
            read_proxy_grants(std::move(proxies_res), output);
            rval = LoadResult::SUCCESS;
        }
    }
    return rval;
}

MariaDBUserManager::LoadResult
MariaDBUserManager::load_users_clustrix(mxq::MariaDB& con, SERVER* srv, UserDatabase* output)
{
    auto rval = LoadResult::QUERY_FAILED;
    QResult users_res, acl_res;
    if (((users_res = con.query(clustrix_queries::users_query)) != nullptr)
        && ((acl_res = con.query(mariadb_queries::db_grants_query)) != nullptr))
    {
        rval = read_users_clustrix(std::move(users_res), std::move(acl_res), output);
    }
    return rval;
}

bool MariaDBUserManager::read_users_mariadb(QResult users, UserDatabase* output)
{
    auto get_bool_enum = [&users](int64_t col_ind) {
            string val = users->get_string(col_ind);
            return val == "Y" || val == "y";
        };

    // Get column indexes for the interesting fields. Depending on backend version, they may not all
    // exist. Some of the field name start with a capital and some don't. Should the index search be
    // ignorecase?
    auto ind_user = users->get_col_index("User");
    auto ind_host = users->get_col_index("Host");
    auto ind_sel_priv = users->get_col_index("Select_priv");
    auto ind_ins_priv = users->get_col_index("Insert_priv");
    auto ind_upd_priv = users->get_col_index("Update_priv");
    auto ind_del_priv = users->get_col_index("Delete_priv");
    auto ind_ssl = users->get_col_index("ssl_type");
    auto ind_plugin = users->get_col_index("plugin");
    auto ind_pw = users->get_col_index("Password");
    auto ind_auth_str = users->get_col_index("authentication_string");
    auto ind_is_role = users->get_col_index("is_role");
    auto ind_def_role = users->get_col_index("default_role");

    bool has_required_fields = (ind_user >= 0) && (ind_host >= 0)
        && (ind_sel_priv >= 0) && (ind_ins_priv >= 0) && (ind_upd_priv >= 0) && (ind_del_priv >= 0)
        && (ind_ssl >= 0) && (ind_plugin >= 0) && (ind_pw >= 0) && (ind_auth_str >= 0);

    bool error = false;
    if (has_required_fields)
    {
        while (users->next_row())
        {
            auto username = users->get_string(ind_user);

            UserEntry new_entry;
            new_entry.username = username;
            new_entry.host_pattern = users->get_string(ind_host);

            // Treat the user as having global privileges if any of the following global privileges
            // exists.
            new_entry.global_db_priv = get_bool_enum(ind_sel_priv) || get_bool_enum(ind_ins_priv)
                || get_bool_enum(ind_upd_priv) || get_bool_enum(ind_del_priv);

            // Require SSL if the entry is not empty.
            new_entry.ssl = !users->get_string(ind_ssl).empty();

            new_entry.plugin = users->get_string(ind_plugin);
            new_entry.password = users->get_string(ind_pw);
            new_entry.auth_string = users->get_string(ind_auth_str);

            if (ind_is_role >= 0)
            {
                new_entry.is_role = get_bool_enum(ind_is_role);
            }
            if (ind_def_role >= 0)
            {
                new_entry.default_role = users->get_string(ind_def_role);
            }

            output->add_entry(username, new_entry);
        }
    }
    else
    {
        error = true;
    }
    return !error;
}

void MariaDBUserManager::read_dbs_and_roles(QResult dbs, QResult roles, UserDatabase* output)
{
    using StringSetMap = UserDatabase::StringSetMap;

    auto map_builder = [](const string& grant_col_name, QResult source) {
            StringSetMap result;
            auto ind_user = source->get_col_index("user");
            auto ind_host = source->get_col_index("host");
            auto ind_grant = source->get_col_index(grant_col_name);
            bool valid_data = (ind_user >= 0 && ind_host >= 0 && ind_grant >= 0);
            if (valid_data)
            {
                while (source->next_row())
                {
                    string key = source->get_string(ind_user) + "@" + source->get_string(ind_host);
                    string grant = source->get_string(ind_grant);
                    result[key].insert(grant);
                }
            }
            return result;
        };

    // The maps are mutex-protected. Before locking, prepare the result maps entirely.
    StringSetMap new_db_grants = map_builder("db", std::move(dbs));
    StringSetMap new_roles_mapping;
    if (roles)
    {
        // Old backends may not have role data.
        new_roles_mapping = map_builder("role", std::move(roles));
    }

    output->set_dbs_and_roles(std::move(new_db_grants), std::move(new_roles_mapping));
}

void MariaDBUserManager::read_proxy_grants(MariaDBUserManager::QResult proxies, UserDatabase* output)
{
    if (proxies->get_row_count() > 0)
    {
        auto ind_user = proxies->get_col_index("user");
        auto ind_host = proxies->get_col_index("host");
        if (ind_user >= 0 && ind_host >= 0)
        {
            while (proxies->next_row())
            {
                output->add_proxy_grant(proxies->get_string(ind_user), proxies->get_string(ind_host));
            }
        }
    }
}

MariaDBUserManager::LoadResult
MariaDBUserManager::read_users_clustrix(QResult users, QResult acl, UserDatabase* output)
{
    auto ind_user = users->get_col_index("username");
    auto ind_host = users->get_col_index("host");
    auto ind_pw = users->get_col_index("password");
    auto ind_plugin = users->get_col_index("plugin");
    auto ind_priv = acl->get_col_index("global_priv");

    bool has_required_fields = (ind_user >= 0) && (ind_host >= 0) && (ind_pw >= 0) && (ind_plugin >= 0)
        && (ind_priv >= 0) && (ind_priv >= 0);

    auto rval = LoadResult::INVALID_DATA;
    if (has_required_fields)
    {
        while (users->next_row())
        {
            auto username = users->get_string(ind_user);

            UserEntry new_entry;
            new_entry.username = username;
            new_entry.host_pattern = users->get_string(ind_host);
            new_entry.password = users->get_string(ind_pw);
            new_entry.plugin = users->get_string(ind_plugin);
            new_entry.global_db_priv = (users->get_string(ind_priv) == "Y");
            output->add_entry(username, new_entry);
        }
        // TODO: read database privileges
        rval = LoadResult::SUCCESS;
    }
    return rval;
}

std::unique_ptr<mxs::UserAccountCache> MariaDBUserManager::create_user_account_cache()
{
    auto cache = new(std::nothrow) MariaDBUserCache(*this);
    return std::unique_ptr<mxs::UserAccountCache>(cache);
}

UserDatabase MariaDBUserManager::user_database() const
{
    UserDatabase rval;
    {
        Guard guard(m_userdb_lock);
        rval = m_userdb;
    }
    return rval;
}

void MariaDBUserManager::set_service(SERVICE* service)
{
    mxb_assert(!m_service);
    m_service = service;
}

void UserDatabase::add_entry(const std::string& username, const UserEntry& entry)
{
    auto& entrylist = m_users[username];
    // Find the correct spot to insert. Will insert duplicate hostname patterns, although these should
    // not exist in the source data.
    auto insert_iter = std::upper_bound(entrylist.begin(), entrylist.end(), entry,
                                        UserEntry::host_pattern_is_more_specific);
    entrylist.insert(insert_iter, entry);
}

void UserDatabase::clear()
{
    m_users.clear();
}

const UserEntry* UserDatabase::find_entry(const std::string& username, const std::string& host) const
{
    return find_entry(username, host, HostPatternMode::MATCH);
}

const mariadb::UserEntry* UserDatabase::find_entry(const std::string& username) const
{
    return find_entry(username, "", HostPatternMode::SKIP);
}

const UserEntry* UserDatabase::find_entry(const std::string& username, const std::string& host,
                                          HostPatternMode mode) const
{
    const UserEntry* rval = nullptr;
    auto iter = m_users.find(username);
    if (iter != m_users.end())
    {
        auto& entrylist = iter->second;
        // List is already ordered, take the first matching entry.
        for (auto& entry : entrylist)
        {
            // The entry must not be a role (they should have empty hostnames in any case) and the hostname
            // pattern should match the host.
            if (!entry.is_role
                && (mode == HostPatternMode::SKIP || address_matches_host_pattern(host, entry.host_pattern)))
            {
                rval = &entry;
                break;
            }
        }
    }
    return rval;
}

size_t UserDatabase::n_usernames() const
{
    return m_users.size();
}

size_t UserDatabase::n_entries() const
{
    size_t rval = 0;
    for (const auto& elem : m_users)
    {
        rval += elem.second.size();
    }
    return rval;
}

void UserDatabase::set_dbs_and_roles(StringSetMap&& db_grants, StringSetMap&& roles_mapping)
{
    m_database_grants = std::move(db_grants);
    m_roles_mapping = std::move(roles_mapping);
}

bool UserDatabase::check_database_access(const UserEntry& entry, const std::string& db,
                                         bool case_sensitive_db) const
{
    // Accept the user if the entry has a direct global privilege or if the user is not
    // connecting to a specific database,
    auto& user = entry.username;
    auto& host = entry.host_pattern;
    auto& def_role = entry.default_role;

    return entry.global_db_priv || db.empty()
            // or the user has a privilege to the database, or a table or column in the database,
           || (user_can_access_db(user, host, db, case_sensitive_db))
            // or the user can access db through its default role.
           || (!def_role.empty() && user_can_access_role(user, host, def_role)
               && role_can_access_db(def_role, db, case_sensitive_db));
}

bool UserDatabase::user_can_access_db(const string& user, const string& host_pattern, const string& db,
                                      bool case_sensitive_db) const
{
    string key = user + "@" + host_pattern;
    auto iter = m_database_grants.find(key);
    if (iter != m_database_grants.end())
    {
        // If comparing db-names case-insensitively, iterate through the set.
        if (!case_sensitive_db)
        {
            const StringSet& allowed_dbs = iter->second;
            for (const auto& allowed_db : allowed_dbs)
            {
                if (strcasecmp(allowed_db.c_str(), db.c_str()) == 0)
                {
                    return true;
                }
            }
        }
        else
        {
            return iter->second.count(db) > 0;
        }
    }
    return false;
}

bool UserDatabase::user_can_access_role(const std::string& user, const std::string& host_pattern,
                                        const std::string& target_role) const
{
    string key = user + "@" + host_pattern;
    auto iter = m_roles_mapping.find(key);
    if (iter != m_roles_mapping.end())
    {
        return iter->second.count(target_role) > 0;
    }
    return false;
}

bool UserDatabase::role_can_access_db(const string& role, const string& db, bool case_sensitive_db) const
{
    auto role_has_global_priv = [this](const string& role) {
            bool rval = false;
            auto iter = m_users.find(role);
            if (iter != m_users.end())
            {
                auto& entrylist = iter->second;
                // Because roles have an empty host-pattern, they must be first in the list.
                if (!entrylist.empty())
                {
                    auto& entry = entrylist.front();
                    if (entry.is_role && entry.global_db_priv)
                    {
                        rval = true;
                    }
                }
            }
            return rval;
        };

    auto find_linked_roles = [this](const string& role) {
            std::vector<string> rval;
            string key = role + "@";
            auto iter = m_roles_mapping.find(key);
            if (iter != m_roles_mapping.end())
            {
                auto& roles_set = iter->second;
                for (auto& linked_role : roles_set)
                {
                    rval.push_back(linked_role);
                }
            }
            return rval;
        };

    // Roles are tricky since one role may have access to other roles and so on.
    StringSet open_set;     // roles which still need to be expanded.
    StringSet closed_set;   // roles which have been checked already.

    open_set.insert(role);
    bool privilege_found = false;
    while (!open_set.empty() && !privilege_found)
    {
        string current_role = *open_set.begin();
        // First, check if role has global privilege.
        if (role_has_global_priv(current_role))
        {
            privilege_found = true;
        }
        // No global privilege, check db-level privilege.
        else if (user_can_access_db(current_role, "", db, case_sensitive_db))
        {
            privilege_found = true;
        }
        else
        {
            // The current role does not have access to db. Add linked roles to the open set.
            auto linked_roles = find_linked_roles(current_role);
            for (const auto& linked_role : linked_roles)
            {
                if (open_set.count(linked_role) == 0 && closed_set.count(linked_role) == 0)
                {
                    open_set.insert(linked_role);
                }
            }
        }

        open_set.erase(current_role);
        closed_set.insert(current_role);
    }
    return privilege_found;
}

bool
UserDatabase::address_matches_host_pattern(const std::string& addr, const std::string& host_pattern) const
{
    // First, check the input address type. This affects how the comparison to the host pattern works.
    auto addrtype = parse_address_type(addr);
    // If host address form is unexpected, don't bother continuing.
    if (addrtype == AddrType::UNKNOWN)
    {
        MXB_ERROR("Address '%s' is not supported.", addr.c_str());      // TODO: print username as well.
        return false;
    }

    auto patterntype = parse_pattern_type(host_pattern);    // TODO: perform this step when loading users
    if (patterntype == PatternType::UNKNOWN)
    {
        MXB_ERROR("Host pattern '%s' is not supported.", addr.c_str());
        return false;
    }

    auto like = [](const string& pattern, const string& str) {
            return sql_strlike(pattern.c_str(), str.c_str(), '\\') == 0;
        };

    bool matched = false;
    if (patterntype == PatternType::ADDRESS)
    {
        if (like(host_pattern, addr))
        {
            matched = true;
        }
        else if (addrtype == AddrType::MAPPED)
        {
            // Try matching the ipv4-part of the address.
            auto ipv4_part = addr.find_last_of(':') + 1;
            if (like(host_pattern, addr.substr(ipv4_part)))
            {
                matched = true;
            }
        }
    }
    else if (patterntype == PatternType::MASK)
    {
        string effective_addr;
        if (addrtype == AddrType::IPV4)
        {
            effective_addr = addr;
        }
        else if (addrtype == AddrType::MAPPED)
        {
            effective_addr = addr.substr(addr.find_last_of(':') + 1);
        }

        if (!effective_addr.empty())
        {
            // The pattern is of type "base_ip/mask". The client ip should be accepted if
            // client_ip & mask == base_ip. To test this, all three parts need to be converted
            // to numbers.
            auto ip_to_integer = [](const string& ip) {
                    sockaddr_in sa;
                    inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr));
                    return (uint32_t)sa.sin_addr.s_addr;
                };

            auto div_loc = host_pattern.find('/');
            string base_ip_str = host_pattern.substr(0, div_loc);
            string netmask_str = host_pattern.substr(div_loc + 1);
            auto address = ip_to_integer(effective_addr);
            auto base_ip = ip_to_integer(base_ip_str);
            auto mask = ip_to_integer(netmask_str);
            if ((address & mask) == base_ip)
            {
                matched = true;
            }
        }
    }
    else if (patterntype == PatternType::HOSTNAME)
    {
        // Need a reverse lookup on the client address. This is slow. TODO: use a separate thread/cache
        string resolved_addr;
        if (mxb::reverse_name_lookup(addr, &resolved_addr))
        {
            if (like(host_pattern, resolved_addr))
            {
                matched = true;
            }
        }
    }

    return matched;
}

UserDatabase::AddrType UserDatabase::parse_address_type(const std::string& addr) const
{
    using mxb::Host;

    auto rval = AddrType::UNKNOWN;
    if (Host::is_valid_ipv4(addr))
    {
        rval = AddrType::IPV4;
    }
    else
    {
        // The address could be IPv4 mapped to IPv6.
        const string mapping_prefix = ":ffff:";
        auto prefix_loc = addr.find(mapping_prefix);
        if (prefix_loc != npos)
        {
            auto ipv4part_loc = prefix_loc + mapping_prefix.length();
            if (addr.length() >= (ipv4part_loc + ipv4min_len))
            {
                // The part after the prefix should be a normal ipv4-address.
                string ipv4part = addr.substr(ipv4part_loc);
                if (Host::is_valid_ipv4(ipv4part))
                {
                    rval = AddrType::MAPPED;
                }
            }
        }

        // Finally, the address could be ipv6.
        if (rval == AddrType::UNKNOWN && Host::is_valid_ipv6(addr))
        {
            rval = AddrType::IPV6;
        }
    }
    return rval;
}

UserDatabase::PatternType UserDatabase::parse_pattern_type(const std::string& host_pattern) const
{
    using mxb::Host;
    // The pattern is more tricky, as it may have wildcards. Assume that if the pattern looks like
    // an address, it is an address and not a hostname. This is not strictly true, but is
    // a reasonable assumption. This parsing is useful, as if we can be reasonably sure the pattern
    // is not a hostname, we can skip the expensive reverse name lookup.

    auto is_wc = [](char c) {
            return c == '%' || c == '_';
        };

    auto patterntype = PatternType::UNKNOWN;
    // First, check some common special cases.
    if (Host::is_valid_ipv4(host_pattern) || Host::is_valid_ipv6(host_pattern))
    {
        // No wildcards, just an address.
        patterntype = PatternType::ADDRESS;
    }
    else if (std::all_of(host_pattern.begin(), host_pattern.end(), is_wc))
    {
        // Pattern is composed entirely of wildcards.
        patterntype = PatternType::ADDRESS;
        // Could be hostname as well, but this would only make a difference with a pattern
        // like "________" or "__%___" where the resolved hostname is of correct length
        // while the address is not.
    }
    else
    {
        auto div_loc = host_pattern.find('/');
        if (div_loc != npos && (div_loc >= ipv4min_len) && host_pattern.length() > (div_loc + ipv4min_len))
        {
            // May be a base_ip/netmask-combination.
            string base_ip = host_pattern.substr(0, div_loc);
            string netmask = host_pattern.substr(div_loc + 1);
            if (Host::is_valid_ipv4(base_ip) && Host::is_valid_ipv4(netmask))
            {
                patterntype = PatternType::MASK;
            }
        }
    }

    if (patterntype == PatternType::UNKNOWN)
    {
        // Pattern is a hostname, or an address with wildcards. Go through it and take an educated guess.
        bool maybe_address = true;
        bool maybe_hostname = true;
        const char esc = '\\';      // '\' is an escape char to allow e.g. my_host.com to match properly.
        bool escaped = false;

        auto classify_char = [is_wc, &maybe_address, &maybe_hostname](char c) {
                auto is_ipchar = [](char c) {
                        return std::isxdigit(c) || c == ':' || c == '.';
                    };

                auto is_hostnamechar = [](char c) {
                        return std::isalnum(c) || c == '_' || c == '.' || c == '-';
                    };

                if (is_wc(c))
                {
                    // Can be address or hostname.
                }
                else
                {
                    if (!is_ipchar(c))
                    {
                        maybe_address = false;
                    }
                    if (!is_hostnamechar(c))
                    {
                        maybe_hostname = false;
                    }
                }
            };

        for (auto c : host_pattern)
        {
            if (escaped)
            {
                // % is not a valid escaped character.
                if (c == '%')
                {
                    maybe_address = false;
                    maybe_hostname = false;
                }
                else
                {
                    classify_char(c);
                }
                escaped = false;
            }
            else if (c == esc)
            {
                escaped = true;
            }
            else
            {
                classify_char(c);
            }

            if (!maybe_address && !maybe_hostname)
            {
                // Unrecognized pattern type.
                break;
            }
        }

        if (maybe_address)
        {
            // Address takes priority.
            patterntype = PatternType::ADDRESS;
        }
        else if (maybe_hostname)
        {
            patterntype = PatternType::HOSTNAME;
        }
    }
    return patterntype;
}

void UserDatabase::add_proxy_grant(const std::string& user, const std::string& host)
{
    auto user_iter = m_users.find(user);
    if (user_iter != m_users.end())
    {
        EntryList& entries = user_iter->second;
        UserEntry needle;
        needle.host_pattern = host;
        auto entry_iter = std::lower_bound(entries.begin(), entries.end(), needle,
                                           UserEntry::host_pattern_is_more_specific);
        if (entry_iter != entries.end() && entry_iter->host_pattern == host)
        {
            entry_iter->proxy_grant = true;
        }
    }
}

bool UserDatabase::equal_contents(const UserDatabase& rhs) const
{
    return (m_users == rhs.m_users) && (m_database_grants == rhs.m_database_grants)
           && (m_roles_mapping == rhs.m_roles_mapping);
}

MariaDBUserCache::MariaDBUserCache(const MariaDBUserManager& master)
    : m_master(master)
{
}

SUserEntry
MariaDBUserCache::find_user(const string& user, const string& host, const string& requested_db,
                            const UserSearchSettings& sett) const
{
    auto userz = user.c_str();
    auto hostz = host.c_str();

    // If "root" user is not allowed, block such user immediately.
    if (!sett.allow_root_user && user == "root")
    {
        MXB_INFO("Client '%s'@'%s' blocked because '%s' is false.",
                 userz, hostz, CN_ENABLE_ROOT_USER);
        return nullptr;
    }

    SUserEntry rval;
    // TODO: the user may be empty, is it ok to match normally in that case?
    const UserEntry* found = nullptr;
    // First try to find a normal user entry. If host pattern matching is disabled, match only username.
    found = sett.match_host_pattern ? m_userdb.find_entry(user, host) :  m_userdb.find_entry(user);
    if (found)
    {
        // TODO: when checking db access, also check if database exists.
        if (m_userdb.check_database_access(*found, requested_db, sett.case_sensitive_db))
        {
            MXB_INFO("Found matching user '%s'@'%s' for client '%s'@'%s' with sufficient privileges.",
                     found->username.c_str(), found->host_pattern.c_str(), userz, hostz);
            rval.reset(new (std::nothrow) UserEntry(*found));
        }
        else
        {
            MXB_INFO("Found matching user '%s'@'%s' for client '%s'@'%s' but user does not have "
                     "sufficient privileges.",
                     found->username.c_str(), found->host_pattern.c_str(), userz, hostz);
        }
    }
    else if (sett.allow_anon_user)
    {
        // Try find an anonymous entry. Such an entry has empty username and matches any client username.
        // If host pattern matching is disabled, any user from any host can log in if an anonymous
        // entry exists.
        found = sett.match_host_pattern ? m_userdb.find_entry("", host) : m_userdb.find_entry("");
        if (found)
        {
            // For anonymous users, do not check database access as the final effective user is unknown.
            // Instead, check that the entry has a proxy grant.
            if (found->proxy_grant)
            {
                MXB_INFO("Found matching anonymous user ''@'%s' for client '%s'@'%s' with proxy grant.",
                         found->host_pattern.c_str(), userz, hostz);
                rval.reset(new (std::nothrow) UserEntry(*found));
            }
            else
            {
                MXB_INFO("Found matching anonymous user ''@'%s' for client '%s'@'%s' but user does not have "
                         "proxy privileges.",
                         found->host_pattern.c_str(), userz, hostz);
            }
        }
    }

    if (!found)
    {
        MXB_INFO("Found no matching user for client '%s'@'%s'.", userz, hostz);
    }
    return rval;
}

void MariaDBUserCache::update_from_master()
{
    m_userdb = m_master.user_database();
}