/**
 * Ensure that the correct CBR mode is chosen given certain combinations of query knobs.
 */

import {
    canonicalizePlan,
    getExecutionStats,
    getRejectedPlans,
    getWinningPlanFromExplain,
    isCollscan
} from "jstests/libs/query/analyze_plan.js";

import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const collName1 = collName + "_ceModes";
const coll = db[collName];
const coll1 = db[collName1];
coll.drop();
coll1.drop();

// Insert such data that some queries will do well with an {a: 1} index while
// others with a {b: 1} index.
assert.commandWorked(
    coll.insertMany(Array.from({length: 5000}, (_, i) => ({a: 1, b: i, c: i % 7}))));
assert.commandWorked(
    coll.insertMany(Array.from({length: 5000}, (_, i) => ({a: i, b: 1, c: i % 3}))));

coll.createIndexes([{a: 1}, {b: 1}, {c: 1, b: 1, a: 1}, {a: 1, b: 1}, {c: 1, a: 1}]);

assert.commandWorked(coll.runCommand({analyze: collName, key: "a", numberBuckets: 10}));
assert.commandWorked(coll.runCommand({analyze: collName, key: "b", numberBuckets: 10}));
assert.commandWorked(coll.runCommand({analyze: collName, key: "c", numberBuckets: 10}));

// Queries designed in such a way that the winning plan is not the last enumerated plan.
// The current implementation of CBR chooses the last of all enumerated plans as winning.
// In this way we can verify that CBR was invoked by checking if the last rejected
// multi-planned plan is the winning plan.

const q1 = {
    a: {$gt: 10},
    b: {$eq: 99}
};
const q2 = {
    a: {$in: [5, 1]},
    b: {$in: [7, 99]}
};
const q3 = {
    a: {$gt: 90},
    b: {$eq: 99},
    c: {$lt: 5}
};
const q4 = {
    $or: [q1, q2]
};

/*
Query q5 has 4 plans:
1. Filter: a in (1,5) AND b in (7, 99)
   Or
   |  Ixscan: b: [99, inf]
   Ixscan: a: [10, 10], b: [MinKey, MaxKey]

2. Filter: a in (1,5) AND b in (7, 99)
   Or
   |  Ixscan: b: [99, inf]
   Ixscan: a: [10, 10]

3. Filter: a = 10 AND b > 99
   Or
   |  Ixscan: a: [1,1]U[5,5]
   Ixscan: b: [7,7]U[99,99]

4. Filter: a = 10 AND b > 99
   Or
   |  Ixscan: a: [1,1] U [5,5] b: [MinKey, MaxKey]
   Ixscan: b: [7,7] U [99,99]

In classic, plan 1 is the winner and in CBR, the winner is plan 2. In CBR we cost plans 1 and
2 to have equal costs. In reality, plan 2 is better because it uses an index with a shorter key
length, but multiplanning is unable to distinguish that because of it's early exit behavior. So
asserting that CBR choose the same plan as classic won't always work because CBR's plan may be
better. Therefore, instead of comparing plans the test compares the number of keys and documents
scanned by a plan. CBR plans should scan no more than Classic plans.
*/
const q5 = {
    $and: [
        {$or: [{a: 10}, {b: {$gt: 99}}]},
        {$or: [{a: {$in: [5, 1]}}, {b: {$in: [7, 99]}}]},
    ],
};

function assertCbrExplain(plan) {
    assert(plan.hasOwnProperty("cardinalityEstimate"));
    assert.gt(plan.cardinalityEstimate, 0);
    assert(plan.hasOwnProperty("costEstimate"));
    assert.gt(plan.costEstimate, 0);
    if (plan.hasOwnProperty("inputStage")) {
        assertCbrExplain(plan.inputStage);
    }
}

function checkWinningPlan(query) {
    const isRootedOr = (Object.keys(query).length == 1 && Object.keys(query)[0] === "$or");

    // Classic plan via multiplanning
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
    const e0 = coll.find(query).explain("executionStats");
    const w0 = getWinningPlanFromExplain(e0);
    const r0 = getRejectedPlans(e0);

    // Classic plan via CBR
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"}));
    const e1 = coll.find(query).explain("executionStats");
    const w1 = getWinningPlanFromExplain(e1);
    const r1 = getRejectedPlans(e1);

    if (!isRootedOr) {
        assertCbrExplain(w1);
        // Validate there are rejected plans
        assert.gte(r0.length, 1);
        assert.gte(r1.length, 1);
        // Both explains must have the same number of rejected plans
        assert.eq(r0.length, r1.length);
    }
    r1.map((e) => assertCbrExplain(e));

    // CBR's plan must be no worse than the Classic plan
    assert(e1.executionStats.totalKeysExamined <= e0.executionStats.totalKeysExamined);
    assert(e1.executionStats.totalDocsExamined <= e0.executionStats.totalDocsExamined);
    assert(e1.executionStats.executionStages.works <= e0.executionStats.executionStages.works);
}

function verifyCollectionCardinalityEstimate() {
    const card = 1234;
    coll.drop();
    assert.commandWorked(coll.insertMany(Array.from({length: card}, () => ({a: 1}))));
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"}));
    // This query should not have any predicates, as they are taken into account
    // by CE, and estimated cardinality will be less than the total.
    const e1 = coll.find({}).explain();
    const w1 = getWinningPlanFromExplain(e1);
    assert(isCollscan(db, w1));
    assert.eq(w1.cardinalityEstimate, card);
}

function verifyHeuristicEstimateSource() {
    coll.drop();
    assert.commandWorked(coll.insert({a: 1}));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "heuristicCE"}));
    const e1 = coll.find({a: 1}).explain();
    const w1 = getWinningPlanFromExplain(e1);
    assert.eq(w1.estimatesMetadata.ceSource, "Heuristics", w1);
}

try {
    checkWinningPlan(q1);
    checkWinningPlan(q2);
    checkWinningPlan(q3);
    checkWinningPlan(q4);
    checkWinningPlan(q5);
    verifyCollectionCardinalityEstimate();
    verifyHeuristicEstimateSource();

    /**
     * Test strict and automatic CE modes.
     */

    // With strict mode Histogram CE without an applicable histogram should produce
    // an error. Automatic mode should result in heuristicCE or mixedCE.

    // TODO SERVER-97867: Since in automaticCE mode we always fallback to heuristic CE,
    // it is not possible to ever fallback to multi-planning.

    assert.commandWorked(coll1.insertOne({a: 1}));

    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));

    // Request histogam CE while the collection has no histogram
    assert.throwsWithCode(() => coll1.find({a: 1}).explain(), ErrorCodes.HistogramCEFailure);

    // Create a histogram on field "b".
    // TODO SERVER-97814: Due to incompleteness of CBR 'analyze' must be run with multi-planning.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
    assert.commandWorked(coll1.runCommand({analyze: collName, key: "b"}));
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));

    // Request histogam CE on a field that doesn't have a histogram
    assert.throwsWithCode(() => coll1.find({a: 1}).explain(), ErrorCodes.HistogramCEFailure);
    assert.throwsWithCode(() => coll1.find({$and: [{b: 1}, {a: 3}]}).explain(),
                          ErrorCodes.HistogramCEFailure);

    // $or cannot fail because QueryPlanner::planSubqueries() falls back to choosePlanWholeQuery()
    // when one of the subqueries could not be planned. In this way the CE error is masked.
    const orExpl = coll1.find({$or: [{b: 1}, {a: 3}]}).explain();
    assert(isCollscan(db, getWinningPlanFromExplain(orExpl)));

    // Histogram CE invokes conversion of expression to an inexact interval, which fails
    assert.throwsWithCode(() => coll1.find({b: {$gt: []}}).explain(),
                          ErrorCodes.HistogramCEFailure);

    // Histogram CE fails because of inestimable interval
    assert.throwsWithCode(() => coll1.find({b: {$gte: {foo: 1}}}).explain(),
                          ErrorCodes.HistogramCEFailure);
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
