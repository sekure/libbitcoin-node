/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin-node.
 *
 * libbitcoin-node is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/node/p2p_node.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <bitcoin/blockchain.hpp>
#include <bitcoin/node/configuration.hpp>
#include <bitcoin/node/sessions/session_block_sync.hpp>
#include <bitcoin/node/sessions/session_header_sync.hpp>

namespace libbitcoin {
namespace node {

using namespace bc::blockchain;
using namespace bc::chain;
using namespace bc::config;
using namespace bc::network;
using std::placeholders::_1;
using std::placeholders::_2;

p2p_node::p2p_node(const configuration& configuration)
  : p2p(configuration.network),
    hashes_(configuration.chain.checkpoints),
    blockchain_(thread_pool(), configuration.chain, configuration.database),
    settings_(configuration.node)
{
}

p2p_node::~p2p_node()
{
    p2p_node::close();
}

// Start.
// ----------------------------------------------------------------------------

void p2p_node::start(result_handler handler)
{
    if (!stopped())
    {
        handler(error::operation_failed);
        return;
    }

    if (!blockchain_.start())
    {
        log::error(LOG_NODE)
            << "Blockchain failed to start.";
        handler(error::operation_failed);
        return;
    }

    // This is invoked on the same thread.
    // Stopped is true and no network threads until after this call.
    p2p::start(handler);
}

// Run sequence.
// ----------------------------------------------------------------------------

void p2p_node::run(result_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    const auto start_handler =
        std::bind(&p2p_node::handle_headers_synchronized,
            this, _1, handler);

    // This is invoked on a new thread.
    // The instance is retained by the stop handler (i.e. until shutdown).
    attach<session_header_sync>(hashes_, blockchain_)->
        start(start_handler);
}

void p2p_node::handle_headers_synchronized(const code& ec,
    result_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    if (ec)
    {
        log::error(LOG_NODE)
            << "Failure synchronizing headers: " << ec.message();
        handler(ec);
        return;
    }

    const auto start_handler =
        std::bind(&p2p_node::handle_running,
            this, _1, handler);

    // This is invoked on a new thread.
    // The instance is retained by the stop handler (i.e. until shutdown).
    attach<session_block_sync>(hashes_, blockchain_, settings_)->
        start(start_handler);
}

void p2p_node::handle_running(const code& ec, result_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    if (ec)
    {
        log::error(LOG_NODE)
            << "Failure synchronizing blocks: " << ec.message();
        handler(ec);
        return;
    }

    uint64_t height;

    if (!blockchain_.get_last_height(height))
    {
        log::error(LOG_NODE)
            << "The blockchain is corrupt.";
        handler(error::operation_failed);
        return;
    }

    BITCOIN_ASSERT(height <= max_size_t);
    set_height(static_cast<size_t>(height));

    // Generalize the final_height() function from protocol_header_sync.
    log::info(LOG_NODE)
        << "Node start height is (" << height << ").";

    // This is invoked on a new thread.
    // This is the end of the derived run sequence.
    p2p::run(handler);
}

// Shutdown
// ----------------------------------------------------------------------------

bool p2p_node::stop()
{
    // Suspend new work last so we can use work to clear subscribers.
    return blockchain_.stop() && p2p::stop();
}

// This must be called from the thread that constructed this class (see join).
bool p2p_node::close()
{
    // Invoke own stop to signal work suspension.
    if (!p2p_node::stop())
        return false;

    // Join threads first so that there is no activity on the chain at close.
    return p2p::close() && blockchain_.close();
}

// Properties.
// ----------------------------------------------------------------------------

const settings& p2p_node::node_settings() const
{
    return settings_;
}

block_chain& p2p_node::chain()
{
    return blockchain_;
}

transaction_pool& p2p_node::pool()
{
    return blockchain_.pool();
}

// Subscriptions.
// ----------------------------------------------------------------------------

void p2p_node::subscribe_blockchain(reorganize_handler handler)
{
    chain().subscribe_reorganize(handler);
}

void p2p_node::subscribe_transaction_pool(transaction_handler handler)
{
    pool().subscribe_transaction(handler);
}

} // namspace node
} //namespace libbitcoin
