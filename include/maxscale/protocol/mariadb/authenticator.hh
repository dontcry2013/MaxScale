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
#pragma once

#include <maxscale/authenticator.hh>
#include <unordered_set>

class DCB;
class SERVICE;
class GWBUF;
class MYSQL_session;

namespace maxscale
{
class Buffer;
}

namespace mariadb
{

class ClientAuthenticator;
class BackendAuthenticator;

using SClientAuth = std::unique_ptr<ClientAuthenticator>;
using SBackendAuth = std::unique_ptr<BackendAuthenticator>;

struct UserEntry
{
    std::string username;       /**< Username */
    std::string host_pattern;   /**< Hostname or IP, may have wildcards */
    std::string plugin;         /**< Auth plugin to use */
    std::string password;       /**< Auth data used by native auth plugin */
    std::string auth_string;    /**< Auth data used by other plugins */

    bool ssl {false};           /**< Should the user connect with ssl? */
    bool global_db_priv {false};/**< Does the user have access to all databases? */
    bool proxy_grant {false};   /**< Does the user have proxy grants? */

    bool        is_role {false};/**< Is the user a role? */
    std::string default_role;   /**< Default role if any */

    bool        operator==(const UserEntry& rhs) const;
    static bool host_pattern_is_more_specific(const UserEntry& lhs, const UserEntry& rhs);
};

// User account search result descriptor
enum class UserEntryType
{
    USER_NOT_FOUND,
    ROOT_ACCESS_DENIED,
    ANON_PROXY_ACCESS_DENIED,
    DB_ACCESS_DENIED,
    BAD_DB,
    PLUGIN_IS_NOT_LOADED,
    USER_ACCOUNT_OK,
};

struct UserEntryResult
{
    mariadb::UserEntry entry;
    UserEntryType      type {UserEntryType::USER_NOT_FOUND};
};

/**
 * The base class of all authenticators for MariaDB-protocol. Contains the global data for
 * an authenticator module instance.
 */
class AuthenticatorModule : public mxs::AuthenticatorModule
{
public:
    AuthenticatorModule(const AuthenticatorModule&) = delete;
    AuthenticatorModule& operator=(const AuthenticatorModule&) = delete;

    enum Capabilities
    {
        CAP_ANON_USER = (1 << 0),   /**< Does the module allow anonymous users? */
    };

    AuthenticatorModule() = default;
    virtual ~AuthenticatorModule() = default;

    /**
     * Create a client authenticator.
     *
     * @return Client authenticator
     */
    virtual SClientAuth create_client_authenticator() = 0;

    /**
     * Create a new backend authenticator. Should only be implemented by authenticator modules which
     * also support backend authentication.
     *
     * @return Backend authenticator
     */
    virtual SBackendAuth create_backend_authenticator() = 0;

    /**
     * @brief Return diagnostic information about the authenticator
     *
     * The authenticator module should return information about its internal
     * state when this function is called.
     *
     * @return JSON representation of the authenticator
     */
    virtual json_t* diagnostics() = 0;

    /**
     * List the server authentication plugins this authenticator module supports.
     *
     * @return Supported authenticator plugins
     */
    virtual const std::unordered_set<std::string>& supported_plugins() const = 0;

    /**
     * Get module runtime capabilities. Returns 0 by default.
     *
     * @return Capabilities as a bitfield
     */
    virtual uint64_t capabilities() const;
};

using SAuthModule = std::unique_ptr<AuthenticatorModule>;

/**
 * The base class of authenticator client sessions. Contains session-specific data for an authenticator.
 */
class ClientAuthenticator
{
public:
    using ByteVec = std::vector<uint8_t>;

    enum class ExchRes
    {
        FAIL,           /**< Packet processing failed */
        INCOMPLETE,     /**< Should be called again after client responds to output */
        READY           /**< Exchange with client complete, should continue to password check */
    };

    // Return values for authenticate()-function
    struct AuthRes
    {
        enum class Status
        {
            FAIL,           /**< Authentication failed */
            FAIL_WRONG_PW,  /**< Client provided wrong password */
            SUCCESS,        /**< Authentication was successful */
        };

        Status      status {Status::FAIL};
        std::string msg;
    };

    ClientAuthenticator(const ClientAuthenticator&) = delete;
    ClientAuthenticator& operator=(const ClientAuthenticator&) = delete;

    ClientAuthenticator() = default;
    virtual ~ClientAuthenticator() = default;

    /**
     * Exchange authentication packets. The module should read the input, optionally write to output,
     * and return status.
     *
     * @param input Packet from client
     * @param ses MySQL session
     * @param output Output for a packet that is sent to the client
     * @return Authentication status
     */
    virtual ExchRes exchange(GWBUF* input, MYSQL_session* ses, mxs::Buffer* output) = 0;

    /**
     * Check client token against the password.
     *
     * @param entry User account entry
     * @param session Protocol session data
     */
    virtual AuthRes authenticate(const UserEntry* entry, MYSQL_session* session) = 0;
};

// Helper template which stores the module reference.
template<class AuthModule>
class ClientAuthenticatorT : public ClientAuthenticator
{
public:
    /**
     * Constructor.
     *
     * @param module The global module data
     */
    ClientAuthenticatorT(AuthModule* module)
        : m_module(*module)
    {
    }

protected:
    AuthModule& m_module;
};

/**
 * The base class for all authenticator backend sessions. Created by the client session.
 */
class BackendAuthenticator
{
public:
    // Return values for authenticate-functions.
    enum class AuthRes
    {
        SUCCESS,    /**< Authentication was successful */
        FAIL,       /**< Authentication failed */
        INCOMPLETE, /**< Authentication is not yet complete */
    };

    BackendAuthenticator(const BackendAuthenticator&) = delete;
    BackendAuthenticator& operator=(const BackendAuthenticator&) = delete;

    BackendAuthenticator() = default;
    virtual ~BackendAuthenticator() = default;

    // Extract backend data from a buffer. Typically, this is called just before the authenticate-entrypoint.
    virtual bool extract(DCB* client, GWBUF* buffer) = 0;

    // Determine whether the connection can support SSL.
    virtual bool ssl_capable(DCB* client) = 0;

    // Carry out the authentication.
    virtual AuthRes authenticate(DCB* client) = 0;
};
}
