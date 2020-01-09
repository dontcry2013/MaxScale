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

#pragma once

#include <maxscale/ccdefs.hh>
#include <maxscale/protocol/mariadb/protocol_classes.hh>

namespace packet_parser
{
using ByteVec = std::vector<uint8_t>;
using ClientInfo = MYSQL_session::ClientInfo;

struct AuthParseResult
{
    bool    success {false};        /**< Was parsing successful */
    ByteVec auth_token;             /**< authentication token */
    bool    old_protocol {false};   /**< Is client using too old protocol version? */

    AuthParseResult() = default;
};

struct AttrParseResult
{
    bool    success {false};
    ByteVec attr_data;

    AttrParseResult() = default;
};

struct ClientResponseResult
{
    bool success {false};

    std::string username;
    std::string db;
    std::string plugin;

    AuthParseResult token_res;
    AttrParseResult attr_res;

    ClientResponseResult() = default;
};

/**
 * Parse 32 bytes of client capabilities.
 *
 * @param data Data array. Should be at least 32 bytes.
 * @param old_info Old client capabilities info from SSLRequest packet. Can be null.
 * @return Parsed client capabilities
 */
ClientInfo parse_client_capabilities(ByteVec& data, const ClientInfo* old_info);

/**
 * Parse username, database etc from client handshake response. Client capabilities should have
 * already been parsed.
 *
 * @param data Data array
 * @return Result object
 */
ClientResponseResult parse_client_response(ByteVec& data, uint32_t client_caps);

/**
 * Parse authentication token from array.
 *
 * @param data Data array
 * @param client_caps Client capabilities
 * @return Result object
 */
AuthParseResult parse_auth_token(ByteVec& data, uint32_t client_caps);

/**
 * Parse connection attributes from array. The data is extracted as is, without breaking it to key-value
 * pairs.
 *
 * @param data Data array
 * @param client_caps Client capabilities
 * @return Result object
 */
AttrParseResult parse_attributes(ByteVec& data, uint32_t client_caps);
}
