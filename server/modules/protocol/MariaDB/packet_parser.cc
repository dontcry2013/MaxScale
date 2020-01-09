/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "packet_parser.hh"
#include <maxsql/mariadb.hh>
#include <maxscale/protocol/mariadb/mysql.hh>

using std::string;

namespace packet_parser
{
void pop_front(ByteVec& data, int len)
{
    auto begin = data.begin();
    data.erase(begin, begin + len);
}

ClientInfo parse_client_capabilities(ByteVec& data, const ClientInfo* old_info)
{
    auto rval = old_info ? *old_info : ClientInfo();

    // Can assume that client capabilities are in the first 32 bytes and the buffer is large enough.
    const uint8_t* ptr = data.data();
    /**
     * We OR the capability bits in order to retain the starting bits sent
     * when an SSL connection is opened. Oracle Connector/J 8.0 appears to drop
     * the SSL capability bit mid-authentication which causes MaxScale to think
     * that SSL is not used.
     */
    rval.m_client_capabilities |= gw_mysql_get_byte4(ptr);
    ptr += 4;

    // Next is max packet size, skip it.
    ptr += 4;

    rval.m_charset = *ptr;
    ptr += 1;

    // Next, 19 bytes of reserved filler. Skip.
    ptr += 19;

    /**
     * Next, 4 bytes of extra capabilities. Not always used.
     * MariaDB 10.2 compatible clients don't set the first bit to signal that
     * there are extra capabilities stored in the last 4 bytes of the filler.
     */
    if ((rval.m_client_capabilities & GW_MYSQL_CAPABILITIES_CLIENT_MYSQL) == 0)
    {
        rval.m_extra_capabilities |= gw_mysql_get_byte4(ptr);
    }
    ptr += 4;
    pop_front(data, ptr - data.data());
    return rval;
}

ClientResponseResult parse_client_response(ByteVec& data, uint32_t client_caps)
{
    ClientResponseResult rval;
    // A null-terminated username should be first. Cannot overrun since caller added 0 to end of buffer.
    rval.username = (const char*)data.data();
    pop_front(data, rval.username.size() + 1);

    auto read_stringz_if_cap = [client_caps, &data](uint32_t required_capability) {
            std::pair<bool, string> rval(true, ""); // success & result
            if (client_caps & required_capability)
            {
                if (!data.empty())
                {
                    rval.second = (const char*)data.data();
                    pop_front(data, rval.second.size() + 1);    // Should be null-terminated.
                }
                else
                {
                    rval.first = false;
                }
            }
            return rval;
        };

    // Next is authentication response. The length is encoded in different forms depending on
    // capabilities.
    rval.token_res = parse_auth_token(data, client_caps);
    if (rval.token_res.success)
    {
        // The following fields are optional.
        auto db_res = read_stringz_if_cap(GW_MYSQL_CAPABILITIES_CONNECT_WITH_DB);
        auto plugin_res = read_stringz_if_cap(GW_MYSQL_CAPABILITIES_PLUGIN_AUTH);
        if (db_res.first && plugin_res.first)
        {
            rval.db = std::move(db_res.second);
            rval.plugin = std::move(plugin_res.second);

            rval.attr_res = parse_attributes(data, client_caps);
            if (rval.attr_res.success)
            {
                rval.success = true;
            }
        }
    }
    return rval;
}

AuthParseResult parse_auth_token(ByteVec& data, uint32_t client_caps)
{
    AuthParseResult rval;
    if (data.empty())
    {
        return rval;
    }

    // The length is encoded in different forms depending on capabilities and packet type.
    const uint8_t* ptr = data.data();
    bool error = false;
    uint64_t len_remaining = data.size();
    uint64_t auth_token_len_bytes = 0;  // In how many bytes the auth token length is encoded in.
    uint64_t auth_token_len = 0;        // The actual auth token length.

    if (client_caps & GW_MYSQL_CAPABILITIES_AUTH_LENENC_DATA)
    {
        // Token is a length-encoded string. First is a length-encoded integer, then the token data.
        auth_token_len_bytes = mxq::leint_bytes(ptr);
        if (auth_token_len_bytes <= len_remaining)
        {
            auth_token_len = mxq::leint_value(ptr);
        }
        else
        {
            error = true;
        }
    }
    else if (client_caps & GW_MYSQL_CAPABILITIES_SECURE_CONNECTION)
    {
        // First token length 1 byte, then token data.
        auth_token_len_bytes = 1;
        auth_token_len = *ptr;
    }
    else
    {
        // unsupported client version
        rval.old_protocol = true;
        error = true;
    }

    if (!error)
    {
        auto total_len = auth_token_len_bytes + auth_token_len;
        if (total_len <= len_remaining)
        {
            rval.success = true;
            ptr += auth_token_len_bytes;
            if (auth_token_len > 0)
            {
                rval.auth_token.assign(ptr, ptr + auth_token_len);
            }
            pop_front(data, total_len);
        }
    }
    return rval;
}

AttrParseResult parse_attributes(ByteVec& data, uint32_t client_caps)
{
    // The data is not processed into key-value pairs as it is simply fed to backends as is.
    AttrParseResult rval;
    if (data.empty())
    {
        return rval;
    }

    auto len_remaining = data.size();

    if (client_caps & GW_MYSQL_CAPABILITIES_CONNECT_ATTRS)
    {
        if (len_remaining > 0)
        {
            const auto ptr = data.data();
            auto leint_len = mxq::leint_bytes(ptr);
            if (leint_len <= len_remaining)
            {
                auto attr_len = mxq::leint_value(ptr);
                auto total_attr_len = leint_len + attr_len;
                if (total_attr_len <= len_remaining)
                {
                    rval.success = true;
                    rval.attr_data.assign(ptr, ptr + total_attr_len);
                    pop_front(data, total_attr_len);
                }
            }
        }
    }
    else
    {
        rval.success = true;
    }
    return rval;
}
}
