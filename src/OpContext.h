/* Copyright (c) 2018, Stanford University
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef HOMA_CORE_OPCONTEXT_H
#define HOMA_CORE_OPCONTEXT_H

#include "ObjectPool.h"
#include "Receiver.h"
#include "Sender.h"
#include "SpinLock.h"
#include "Tub.h"

namespace Homa {
namespace Core {

/**
 * Holds all the relevant data and metadata for a RemoteOp or ServerOp.
 */
struct OpContext {
    /// Message to be sent out as part of this Op.  Processed by the Sender.
    Tub<Sender::OutboundMessage> outMessage;

    /// Message to be recieved as part of this Op.  Processed by the Receiver.
    Tub<Receiver::InboundMessage> inMessage;
};

/**
 * Provides a pool allocator for OpContext objects.
 *
 * This class is thread-safe.
 */
class OpContextPool {
  public:
    OpContextPool();
    ~OpContextPool() {}

    OpContext* construct();
    void destroy(OpContext* opContext);

  private:
    /// Monitor style lock for the pool.
    SpinLock mutex;

    /// Actual memory allocator for Message objects.
    ObjectPool<OpContext> pool;

    OpContextPool(const OpContextPool&) = delete;
    OpContextPool& operator=(const OpContextPool&) = delete;
};

}  // namespace Core
}  // namespace Homa

#endif  // HOMA_CORE_OPCONTEXT_H