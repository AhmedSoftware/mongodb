/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cstddef>
#include <vector>

#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

/**
 * This structure contains per-service-context state related to the oplog.
 */
class LocalOplogInfo {
public:
    static LocalOplogInfo* get(ServiceContext& service);
    static LocalOplogInfo* get(ServiceContext* service);
    static LocalOplogInfo* get(OperationContext* opCtx);

    LocalOplogInfo(const LocalOplogInfo&) = delete;
    LocalOplogInfo& operator=(const LocalOplogInfo&) = delete;
    LocalOplogInfo() = default;

    RecordStore* getRecordStore() const;
    void setRecordStore(RecordStore* rs);
    void resetRecordStore();
    Microseconds getTotalOplogSlotDurationMicros() const;

    /**
     * Sets the global Timestamp to be 'newTime'.
     */
    void setNewTimestamp(ServiceContext* opCtx, const Timestamp& newTime);

    /**
     * Allocates optimes for new entries in the oplog. Returns the new optimes in a vector along
     * with their terms.
     */
    std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx, std::size_t count);

private:
    // The "oplog" record store pointer is always valid (or null) because an operation must take
    // the global exclusive lock to set the pointer to null when the RecordStore instance is
    // destroyed. See "oplogCheckCloseDatabase".
    RecordStore* _rs = nullptr;

    // Stores the total time an operation spends with an uncommitted oplog slot held open. Indicator
    // that an operation is holding back replication by causing oplog holes to remain open for
    // unusual amounts of time.
    AtomicWord<int64_t> _totalOplogSlotDurationMicros;


    // Synchronizes the section where a new Timestamp is generated and when it is registered in the
    // storage engine.
    mutable stdx::mutex _newOpMutex;
};

}  // namespace mongo
