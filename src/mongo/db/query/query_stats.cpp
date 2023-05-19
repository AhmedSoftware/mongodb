/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats.h"

#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_stats_util.h"
#include "mongo/db/query/rate_limiting.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/system_clock_source.h"
#include "query_shape.h"
#include <optional>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace query_stats {

/**
 * Redacts all BSONObj field names as if they were paths, unless the field name is a special hint
 * operator.
 */
namespace {

boost::optional<std::string> getApplicationName(const OperationContext* opCtx) {
    if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
        return metadata->getApplicationName().toString();
    }
    return boost::none;
}
}  // namespace

CounterMetric queryStatsStoreSizeEstimateBytesMetric("queryStats.queryStatsStoreSizeEstimateBytes");

namespace {

CounterMetric queryStatsEvictedMetric("queryStats.numEvicted");
CounterMetric queryStatsRateLimitedRequestsMetric("queryStats.numRateLimitedRequests");
CounterMetric queryStatsStoreWriteErrorsMetric("queryStats.numQueryStatsStoreWriteErrors");

/**
 * Cap the queryStats store size.
 */
size_t capQueryStatsStoreSize(size_t requestedSize) {
    size_t cappedStoreSize = memory_util::capMemorySize(
        requestedSize /*requestedSizeBytes*/, 1 /*maximumSizeGB*/, 25 /*percentTotalSystemMemory*/);
    // If capped size is less than requested size, the queryStats store has been capped at its
    // upper limit.
    if (cappedStoreSize < requestedSize) {
        LOGV2_DEBUG(7106502,
                    1,
                    "The queryStats store size has been capped",
                    "cappedSize"_attr = cappedStoreSize);
    }
    return cappedStoreSize;
}

/**
 * Get the queryStats store size based on the query job's value.
 */
size_t getQueryStatsStoreSize() {
    auto status = memory_util::MemorySize::parse(queryQueryStatsStoreSize.get());
    uassertStatusOK(status);
    size_t requestedSize = memory_util::convertToSizeInBytes(status.getValue());
    return capQueryStatsStoreSize(requestedSize);
}

/**
 * A manager for the queryStats store allows a "pointer swap" on the queryStats store itself. The
 * usage patterns are as follows:
 *
 * - Updating the queryStats store uses the `getQueryStatsStore()` method. The queryStats store
 *   instance is obtained, entries are looked up and mutated, or created anew.
 * - The queryStats store is "reset". This involves atomically allocating a new instance, once
 * there are no more updaters (readers of the store "pointer"), and returning the existing
 * instance.
 */
class QueryStatsStoreManager {
public:
    template <typename... QueryStatsStoreArgs>
    QueryStatsStoreManager(size_t cacheSize, size_t numPartitions)
        : _queryStatsStore(std::make_unique<QueryStatsStore>(cacheSize, numPartitions)),
          _maxSize(cacheSize) {}

    /**
     * Acquire the instance of the queryStats store.
     */
    QueryStatsStore& getQueryStatsStore() {
        return *_queryStatsStore;
    }

    size_t getMaxSize() {
        return _maxSize;
    }

    /**
     * Resize the queryStats store and return the number of evicted
     * entries.
     */
    size_t resetSize(size_t cacheSize) {
        _maxSize = cacheSize;
        return _queryStatsStore->reset(cacheSize);
    }

private:
    std::unique_ptr<QueryStatsStore> _queryStatsStore;

    /**
     * Max size of the queryStats store. Tracked here to avoid having to recompute after it's
     * divided up into partitions.
     */
    size_t _maxSize;
};

const auto queryStatsStoreDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<QueryStatsStoreManager>>();

const auto queryStatsRateLimiter =
    ServiceContext::declareDecoration<std::unique_ptr<RateLimiting>>();

class TelemetryOnParamChangeUpdaterImpl final : public query_stats_util::OnParamChangeUpdater {
public:
    void updateCacheSize(ServiceContext* serviceCtx, memory_util::MemorySize memSize) final {
        auto requestedSize = memory_util::convertToSizeInBytes(memSize);
        auto cappedSize = capQueryStatsStoreSize(requestedSize);
        auto& queryStatsStoreManager = queryStatsStoreDecoration(serviceCtx);
        size_t numEvicted = queryStatsStoreManager->resetSize(cappedSize);
        queryStatsEvictedMetric.increment(numEvicted);
    }

    void updateSamplingRate(ServiceContext* serviceCtx, int samplingRate) {
        queryStatsRateLimiter(serviceCtx).get()->setSamplingRate(samplingRate);
    }
};

ServiceContext::ConstructorActionRegisterer queryStatsStoreManagerRegisterer{
    "QueryStatsStoreManagerRegisterer", [](ServiceContext* serviceCtx) {
        // It is possible that this is called before FCV is properly set up. Setting up the store if
        // the flag is enabled but FCV is incorrect is safe, and guards against the FCV being
        // changed to a supported version later.
        if (!feature_flags::gFeatureFlagQueryStats.isEnabledAndIgnoreFCVUnsafeAtStartup()) {
            // featureFlags are not allowed to be changed at runtime. Therefore it's not an issue
            // to not create a queryStats store in ConstructorActionRegisterer at start up with the
            // flag off - because the flag can not be turned on at any point afterwards.
            query_stats_util::queryStatsStoreOnParamChangeUpdater(serviceCtx) =
                std::make_unique<query_stats_util::NoChangesAllowedTelemetryParamUpdater>();
            return;
        }

        query_stats_util::queryStatsStoreOnParamChangeUpdater(serviceCtx) =
            std::make_unique<TelemetryOnParamChangeUpdaterImpl>();
        size_t size = getQueryStatsStoreSize();
        auto&& globalQueryStatsStoreManager = queryStatsStoreDecoration(serviceCtx);
        // The plan cache and queryStats store should use the same number of partitions.
        // That is, the number of cpu cores.
        size_t numPartitions = ProcessInfo::getNumCores();
        size_t partitionBytes = size / numPartitions;
        size_t metricsSize = sizeof(QueryStatsEntry);
        if (partitionBytes < metricsSize * 10) {
            numPartitions = size / metricsSize;
            if (numPartitions < 1) {
                numPartitions = 1;
            }
        }
        globalQueryStatsStoreManager =
            std::make_unique<QueryStatsStoreManager>(size, numPartitions);
        auto configuredSamplingRate = queryQueryStatsSamplingRate.load();
        queryStatsRateLimiter(serviceCtx) = std::make_unique<RateLimiting>(
            configuredSamplingRate < 0 ? INT_MAX : configuredSamplingRate);
    }};

/**
 * Top-level checks for whether queryStats collection is enabled. If this returns false, we must go
 * no further.
 */
bool isQueryStatsEnabled(const ServiceContext* serviceCtx) {
    // During initialization FCV may not yet be setup but queries could be run. We can't
    // check whether queryStats should be enabled without FCV, so default to not recording
    // those queries.
    // TODO SERVER-75935 Remove FCV Check.
    return feature_flags::gFeatureFlagQueryStats.isEnabled(
               serverGlobalParams.featureCompatibility) &&
        queryStatsStoreDecoration(serviceCtx)->getMaxSize() > 0;
}

/**
 * Internal check for whether we should collect metrics. This checks the rate limiting
 * configuration for a global on/off decision and, if enabled, delegates to the rate limiter.
 */
bool shouldCollect(const ServiceContext* serviceCtx) {
    // Quick escape if queryStats is turned off.
    if (!isQueryStatsEnabled(serviceCtx)) {
        return false;
    }
    // Cannot collect queryStats if sampling rate is not greater than 0. Note that we do not
    // increment queryStatsRateLimitedRequestsMetric here since queryStats is entirely disabled.
    if (queryStatsRateLimiter(serviceCtx)->getSamplingRate() <= 0) {
        return false;
    }
    // Check if rate limiting allows us to collect queryStats for this request.
    if (queryStatsRateLimiter(serviceCtx)->getSamplingRate() < INT_MAX &&
        !queryStatsRateLimiter(serviceCtx)->handleRequestSlidingWindow()) {
        queryStatsRateLimitedRequestsMetric.increment();
        return false;
    }
    return true;
}

/**
 * Add a field to the find op's queryStats key. The `value` will have hmac applied.
 */
void addToFindKey(BSONObjBuilder& builder, const StringData& fieldName, const BSONObj& value) {
    serializeBSONWhenNotEmpty(value.redact(false), fieldName, &builder);
}

/**
 * Recognize FLE payloads in a query and throw an exception if found.
 */
void throwIfEncounteringFLEPayload(const BSONElement& e) {
    constexpr auto safeContentLabel = "__safeContent__"_sd;
    constexpr auto fieldpath = "$__safeContent__"_sd;
    if (e.type() == BSONType::Object) {
        auto fieldname = e.fieldNameStringData();
        uassert(ErrorCodes::EncounteredFLEPayloadWhileApplyingHmac,
                "Encountered __safeContent__, or an $_internalFle operator, which indicate a "
                "rewritten FLE2 query.",
                fieldname != safeContentLabel && !fieldname.startsWith("$_internalFle"_sd));
    } else if (e.type() == BSONType::String) {
        auto val = e.valueStringData();
        uassert(ErrorCodes::EncounteredFLEPayloadWhileApplyingHmac,
                "Encountered $__safeContent__ fieldpath, which indicates a rewritten FLE2 query.",
                val != fieldpath);
    } else if (e.type() == BSONType::BinData && e.isBinData(BinDataType::Encrypt)) {
        int len;
        auto data = e.binData(len);
        uassert(ErrorCodes::EncounteredFLEPayloadWhileApplyingHmac,
                "FLE1 Payload encountered in expression.",
                len > 1 && data[1] != char(EncryptedBinDataType::kDeterministic));
    }
}

/**
 * Upon reading telemetry data, we apply hmac to some keys. This is the list. See
 * QueryStatsEntry::makeQueryStatsKey().
 */
const stdx::unordered_set<std::string> kKeysToApplyHmac = {"pipeline", "find"};

std::string sha256HmacStringDataHasher(std::string key, const StringData& sd) {
    auto hashed = SHA256Block::computeHmac(
        (const uint8_t*)key.data(), key.size(), (const uint8_t*)sd.rawData(), sd.size());
    return hashed.toString();
}

std::string sha256HmacFieldNameHasher(std::string key, const BSONElement& e) {
    auto&& fieldName = e.fieldNameStringData();
    return sha256HmacStringDataHasher(key, fieldName);
}

std::string constantFieldNameHasher(const BSONElement& e) {
    return {"###"};
}

/**
 * Admittedly an abuse of the BSON redaction interface, we recognize FLE payloads here and avoid
 * collecting queryStats for the query.
 */
std::string fleSafeFieldNameRedactor(const BSONElement& e) {
    throwIfEncounteringFLEPayload(e);
    // Ideally we would change interface to avoid copying here.
    return e.fieldNameStringData().toString();
}

/**
 * Append the element to the builder and apply hmac to any literals within the element. The element
 * may be of any type.
 */
void appendWithAbstractedLiterals(BSONObjBuilder& builder, const BSONElement& el) {
    if (el.type() == Object) {
        builder.append(el.fieldNameStringData(), el.Obj().redact(false, fleSafeFieldNameRedactor));
    } else if (el.type() == Array) {
        BSONObjBuilder arrayBuilder = builder.subarrayStart(fleSafeFieldNameRedactor(el));
        for (auto&& arrayElem : el.Obj()) {
            appendWithAbstractedLiterals(arrayBuilder, arrayElem);
        }
        arrayBuilder.done();
    } else {
        auto fieldName = fleSafeFieldNameRedactor(el);
        builder.append(fieldName, "###"_sd);
    }
}

static const StringData replacementForLiteralArgs = "?"_sd;

std::size_t hash(const BSONObj& obj) {
    return absl::hash_internal::CityHash64(obj.objdata(), obj.objsize());
}

}  // namespace

BSONObj QueryStatsEntry::computeQueryStatsKey(OperationContext* opCtx,
                                              bool applyHmacToIdentifiers,
                                              std::string hmacKey) const {
    // The telemetry key for find queries is generated by serializing all the command fields
    // and applying hmac if SerializationOptions indicate to do so. The resulting key is of the
    // form:
    // {
    //    queryShape: {
    //        cmdNs: {db: "...", coll: "..."},
    //        find: "...",
    //        filter: {"...": {"$eq": "?number"}},
    //    },
    //    applicationName: kHashedApplicationName
    // }
    // queryShape may include additional fields, eg hint, limit sort, etc, depending on the original
    // query.

    // TODO SERVER-73152 incorporate aggregation request into same path so that nullptr check is
    // unnecessary
    if (requestShapifier != nullptr) {
        auto serializationOpts = applyHmacToIdentifiers
            ? SerializationOptions(
                  [&](StringData sd) { return sha256HmacStringDataHasher(hmacKey, sd); },
                  LiteralSerializationPolicy::kToDebugTypeString)
            : SerializationOptions(LiteralSerializationPolicy::kToDebugTypeString);
        return requestShapifier->makeQueryStatsKey(serializationOpts, opCtx);
    }

    // TODO SERVER-73152 remove all special aggregation logic below
    // The telemetry key for agg queries is of the following form:
    // { "agg": {...}, "namespace": "...", "applicationName": "...", ... }
    //
    // The part of the key we need to apply hmac to is the object in the <CMD_TYPE> element. In the
    // case of an aggregate() command, it will look something like: > "pipeline" : [ { "$queryStats"
    // : {} },
    //					{ "$addFields" : { "x" : { "$someExpr" {} } } } ],
    // We should preserve the top-level stage names in the pipeline but apply hmac to all field
    // names of children.

    // TODO: SERVER-73152 literal and field name redaction for aggregate command.
    if (!applyHmacToIdentifiers) {
        return oldQueryStatsKey;
    }
    BSONObjBuilder hmacAppliedBuilder;
    for (BSONElement e : oldQueryStatsKey) {
        if ((e.type() == Object || e.type() == Array) &&
            kKeysToApplyHmac.count(e.fieldNameStringData().toString()) == 1) {
            auto hmacApplicator = [&](BSONObjBuilder subObj, const BSONObj& obj) {
                for (BSONElement e2 : obj) {
                    if (e2.type() == Object) {
                        subObj.append(e2.fieldNameStringData(),
                                      e2.Obj().redact(false, [&](const BSONElement& e) {
                                          return sha256HmacFieldNameHasher(hmacKey, e);
                                      }));
                    } else {
                        subObj.append(e2);
                    }
                }
                subObj.done();
            };

            // Now we're inside the <CMD_TYPE>:{} entry and want to preserve the top-level field
            // names. If it's a [pipeline] array, we redact each element in isolation.
            if (e.type() == Object) {
                hmacApplicator(hmacAppliedBuilder.subobjStart(e.fieldNameStringData()), e.Obj());
            } else {
                BSONObjBuilder subArr = hmacAppliedBuilder.subarrayStart(e.fieldNameStringData());
                for (BSONElement stage : e.Obj()) {
                    hmacApplicator(subArr.subobjStart(""), stage.Obj());
                }
            }
        } else {
            hmacAppliedBuilder.append(e);
        }
    }
    return hmacAppliedBuilder.obj();
}

// The originating command/query does not persist through the end of query execution. In order to
// pair the queryStats metrics that are collected at the end of execution with the original query,
// it is necessary to register the original query during planning and persist it after execution.

// During planning, registerRequest is called to serialize the query shape and context (together,
// the queryStats context) and save it to OpDebug. Moreover, as query execution may span more than
// one request/operation and OpDebug does not persist through cursor iteration, it is necessary to
// communicate the queryStats context across operations. In this way, the queryStats context is
// registered to the cursor, so upon getMore() calls, the cursor manager passes the queryStats key
// from the pinned cursor to the new OpDebug.

// Once query execution is complete, the queryStats context is grabbed from OpDebug, a queryStats
// key is generated from this and metrics are paired to this key in the queryStats store.
void registerAggRequest(const AggregateCommandRequest& request, OperationContext* opCtx) {
    if (!isQueryStatsEnabled(opCtx->getServiceContext())) {
        return;
    }

    // Queries against metadata collections should never appear in queryStats data.
    if (request.getNamespace().isFLE2StateCollection()) {
        return;
    }

    if (!shouldCollect(opCtx->getServiceContext())) {
        return;
    }

    BSONObjBuilder queryStatsKey;
    BSONObjBuilder pipelineBuilder = queryStatsKey.subarrayStart("pipeline"_sd);
    try {
        for (auto&& stage : request.getPipeline()) {
            BSONObjBuilder stageBuilder = pipelineBuilder.subobjStart("stage"_sd);
            appendWithAbstractedLiterals(stageBuilder, stage.firstElement());
            stageBuilder.done();
        }
        pipelineBuilder.done();
        queryStatsKey.append("namespace", request.getNamespace().toString());
        if (request.getReadConcern()) {
            queryStatsKey.append("readConcern", *request.getReadConcern());
        }
        if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
            queryStatsKey.append("applicationName", metadata->getApplicationName());
        }
    } catch (ExceptionFor<ErrorCodes::EncounteredFLEPayloadWhileApplyingHmac>&) {
        return;
    }

    BSONObj key = queryStatsKey.obj();
    CurOp::get(opCtx)->debug().queryStatsStoreKeyHash = hash(key);
    CurOp::get(opCtx)->debug().queryStatsStoreKey = key.getOwned();
}

void registerRequest(std::unique_ptr<RequestShapifier> requestShapifier,
                     const NamespaceString& collection,
                     OperationContext* opCtx,
                     const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (!isQueryStatsEnabled(opCtx->getServiceContext())) {
        return;
    }

    // Queries against metadata collections should never appear in queryStats data.
    if (collection.isFLE2StateCollection()) {
        return;
    }

    if (!shouldCollect(opCtx->getServiceContext())) {
        return;
    }
    SerializationOptions options;
    options.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    options.replacementForLiteralArgs = replacementForLiteralArgs;
    CurOp::get(opCtx)->debug().queryStatsStoreKeyHash =
        hash(requestShapifier->makeQueryStatsKey(options, expCtx));
    CurOp::get(opCtx)->debug().queryStatsRequestShapifier = std::move(requestShapifier);
}

QueryStatsStore& getQueryStatsStore(OperationContext* opCtx) {
    uassert(6579000,
            "Telemetry is not enabled without the feature flag on and a cache size greater than 0 "
            "bytes",
            isQueryStatsEnabled(opCtx->getServiceContext()));
    return queryStatsStoreDecoration(opCtx->getServiceContext())->getQueryStatsStore();
}

void writeQueryStats(OperationContext* opCtx,
                     boost::optional<size_t> queryStatsKeyHash,
                     boost::optional<BSONObj> queryStatsKey,
                     std::unique_ptr<RequestShapifier> requestShapifier,
                     const uint64_t queryExecMicros,
                     const uint64_t docsReturned) {
    if (!queryStatsKeyHash) {
        return;
    }
    auto&& queryStatsStore = getQueryStatsStore(opCtx);
    auto&& [statusWithMetrics, partitionLock] =
        queryStatsStore.getWithPartitionLock(*queryStatsKeyHash);
    std::shared_ptr<QueryStatsEntry> metrics;
    if (statusWithMetrics.isOK()) {
        metrics = *statusWithMetrics.getValue();
    } else {
        BSONObj key = queryStatsKey.value_or(BSONObj{});
        size_t numEvicted =
            queryStatsStore.put(*queryStatsKeyHash,
                                std::make_shared<QueryStatsEntry>(
                                    std::move(requestShapifier), CurOp::get(opCtx)->getNSS(), key),
                                partitionLock);
        queryStatsEvictedMetric.increment(numEvicted);
        auto newMetrics = partitionLock->get(*queryStatsKeyHash);
        if (!newMetrics.isOK()) {
            // This can happen if the budget is immediately exceeded. Specifically if the there is
            // not enough room for a single new entry if the number of partitions is too high
            // relative to the size.
            queryStatsStoreWriteErrorsMetric.increment();
            LOGV2_DEBUG(7560900,
                        1,
                        "Failed to store queryStats entry.",
                        "status"_attr = newMetrics.getStatus(),
                        "queryStatsKeyHash"_attr = queryStatsKeyHash);
            return;
        }
        metrics = newMetrics.getValue()->second;
    }

    metrics->lastExecutionMicros = queryExecMicros;
    metrics->execCount++;
    metrics->queryExecMicros.aggregate(queryExecMicros);
    metrics->docsReturned.aggregate(docsReturned);
}
}  // namespace query_stats
}  // namespace mongo
