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
#include <maxscale/protocol/mariadb/module_names.hh>
#define MXS_MODULE_NAME MXS_MARIADB_PROTOCOL_NAME

#include "protocol_module.hh"
#include <maxscale/protocol/mariadb/client_connection.hh>
#include <maxscale/protocol/mariadb/backend_connection.hh>

#include <maxscale/modutil.hh>
#include <maxscale/config.hh>
#include "user_data.hh"

using std::string;

MySQLProtocolModule* MySQLProtocolModule::create(const std::string& auth_name, const std::string& auth_opts)
{
    std::unique_ptr<MySQLProtocolModule> protocol_module(new(std::nothrow) MySQLProtocolModule());
    if (protocol_module)
    {
        // TODO: Add support for multiple authenticators.

        const char* auth_namez {nullptr};
        const char* auth_optsz {nullptr};
        if (!auth_name.empty())
        {
            auth_namez = auth_name.c_str();
            auth_optsz = auth_opts.c_str();
        }
        else
        {
            auth_namez = MXS_MARIADBAUTH_AUTHENTICATOR_NAME;
        }

        auto new_auth_module = mxs::authenticator_init(auth_namez, auth_optsz);
        if (new_auth_module)
        {
            // Check that the authenticator supports the protocol. Use case-insensitive comparison.
            auto supported_protocol = new_auth_module->supported_protocol();
            if (strcasecmp(MXS_MODULE_NAME, supported_protocol.c_str()) == 0)
            {
                protocol_module->m_authenticators.emplace_back(
                    static_cast<mariadb::AuthenticatorModule*>(new_auth_module.release()));
            }
            else
            {
                // When printing protocol name, print the name user gave in configuration file,
                // not the effective name.
                MXB_ERROR("Authenticator module '%s' expects to be paired with protocol '%s', "
                          "not with '%s'.",
                          auth_namez, supported_protocol.c_str(), MXS_MODULE_NAME);
            }
        }
        else
        {
            MXB_ERROR("Failed to initialize authenticator module '%s'.", auth_namez);
            protocol_module.reset();
        }
    }

    if (protocol_module)
    {
        if (!protocol_module->parse_authenticator_opts(auth_opts))
        {
            protocol_module.reset();
        }
    }
    return protocol_module.release();
}

std::unique_ptr<mxs::ClientConnection>
MySQLProtocolModule::create_client_protocol(MXS_SESSION* session, mxs::Component* component)
{
    std::unique_ptr<mxs::ClientConnection> new_client_proto;
    std::unique_ptr<MYSQL_session> mdb_session(new(std::nothrow) MYSQL_session());
    if (mdb_session)
    {
        // The authenticator module used by this session is not known yet. The protocol code will figure
        // it out once authentication begins.
        mdb_session->allowed_authenticators = &m_authenticators;
        mdb_session->user_search_settings = &m_user_search_settings;
        mdb_session->remote = session->client_remote();
        session->set_protocol_data(std::move(mdb_session));

        new_client_proto = std::unique_ptr<mxs::ClientConnection>(
            new(std::nothrow) MariaDBClientConnection(session, component));
    }
    return new_client_proto;
}

std::string MySQLProtocolModule::auth_default() const
{
    return MXS_MARIADBAUTH_AUTHENTICATOR_NAME;
}

GWBUF* MySQLProtocolModule::reject(const std::string& host)
{
    std::string message = "Host '" + host
        + "' is temporarily blocked due to too many authentication failures.";
    return modutil_create_mysql_err_msg(0, 0, 1129, "HY000", message.c_str());
}

std::string MySQLProtocolModule::name() const
{
    return MXS_MODULE_NAME;
}

int MySQLProtocolModule::load_auth_users(SERVICE* service)
{
    for (auto& auth : m_authenticators)
    {
        int ret = auth->load_users(service);
        if (ret != MXS_AUTH_LOADUSERS_OK)
        {
            return ret;
        }
    }
    return MXS_AUTH_LOADUSERS_OK;
}

json_t* MySQLProtocolModule::print_auth_users_json()
{
    // TODO: print all to json array or combine elements? In any case this will be removed later on
    return m_authenticators.front()->diagnostics();
}

std::unique_ptr<mxs::UserAccountManager> MySQLProtocolModule::create_user_data_manager()
{
    return std::unique_ptr<mxs::UserAccountManager>(new MariaDBUserManager());
}

std::unique_ptr<mxs::BackendConnection>
MySQLProtocolModule::create_backend_protocol(MXS_SESSION* session, SERVER* server, mxs::Component* component)
{
    // Allocate DCB specific backend-authentication data from the client session.
    mariadb::SBackendAuth new_backend_auth;
    auto mariases = static_cast<MYSQL_session*>(session->protocol_data());
    auto auth_module = mariases->m_current_authenticator;
    if (auth_module->capabilities() & mariadb::AuthenticatorModule::CAP_BACKEND_AUTH)
    {
        new_backend_auth = auth_module->create_backend_authenticator();
        if (!new_backend_auth)
        {
            MXS_ERROR("Failed to create backend authenticator session.");
        }
    }
    else
    {
        MXS_ERROR("Authenticator '%s' does not support backend authentication. "
                  "Cannot create backend connection.", auth_module->name().c_str());
    }

    std::unique_ptr<mxs::BackendConnection> rval;
    if (new_backend_auth)
    {
        rval = MariaDBBackendConnection::create(session, component, std::move(new_backend_auth));
    }
    return rval;
}

uint64_t MySQLProtocolModule::capabilities() const
{
    return mxs::ProtocolModule::CAP_BACKEND | mxs::ProtocolModule::CAP_AUTHDATA;
}

bool MySQLProtocolModule::parse_authenticator_opts(const std::string& opts)
{
    bool error = false;
    // Check if any of the authenticators support anonymous users.
    for (const auto & auth_module : m_authenticators)
    {
        if (auth_module->capabilities() & mariadb::AuthenticatorModule::CAP_ANON_USER)
        {
            m_user_search_settings.allow_anon_user = true;
            break;
        }
    }

    auto opt_list = mxb::strtok(opts, ",");
    for (auto opt : opt_list)
    {
        auto equals_pos = opt.find('=');
        if (equals_pos != string::npos && equals_pos > 0 && opt.length() > equals_pos + 1)
        {
            string opt_name = opt.substr(0, equals_pos);
            mxb::trim(opt_name);
            string opt_value = opt.substr(equals_pos + 1);
            mxb::trim(opt_value);
            // TODO: add better parsing, check for invalid values
            int val = config_truth_value(opt_value.c_str());
            if (opt_name == "cache_dir")
            {
                // ignored
            }
            else if (opt_name == "inject_service_user")
            {
                m_user_search_settings.allow_service_user = val;
            }
            else if (opt_name == "skip_authentication")
            {
                m_user_search_settings.match_host_pattern = !val;
            }
            else if (opt_name == "lower_case_table_names")
            {
                m_user_search_settings.case_sensitive_db = !val;
            }
            else
            {
                MXB_ERROR("Unknown authenticator option: %s", opt_name.c_str());
                error = true;
            }
        }
        else
        {
            MXB_ERROR("Invalid authenticator option setting: %s", opt.c_str());
            error = true;
        }
    }
    return !error;
}

/**
 * mariadbclient module entry point.
 *
 * @return The module object
 */
extern "C" MXS_MODULE* MXS_CREATE_MODULE()
{
    static MXS_MODULE info =
    {
        MXS_MODULE_API_PROTOCOL,
        MXS_MODULE_GA,
        MXS_PROTOCOL_VERSION,
        "The client to MaxScale MySQL protocol implementation",
        "V1.1.0",
        MXS_NO_MODULE_CAPABILITIES,
        &mxs::ProtocolApiGenerator<MySQLProtocolModule>::s_api,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        {
            {MXS_END_MODULE_PARAMS}
        }
    };

    return &info;
}