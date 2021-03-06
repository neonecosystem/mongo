/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/client/fetcher.h"
#include "mongo/db/client.h"
#include "mongo/db/json.h"
#include "mongo/db/repl/base_cloner_test_fixture.h"
#include "mongo/db/repl/data_replicator.h"
#include "mongo/db/repl/data_replicator_external_state_mock.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/replication_executor_test_fixture.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

namespace {
using namespace mongo;
using namespace mongo::repl;
using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using LockGuard = stdx::lock_guard<stdx::mutex>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;
using mutex = stdx::mutex;

class SyncSourceSelectorMock : public SyncSourceSelector {
    MONGO_DISALLOW_COPYING(SyncSourceSelectorMock);

public:
    SyncSourceSelectorMock(const HostAndPort& syncSource) : _syncSource(syncSource) {}
    void clearSyncSourceBlacklist() override {}
    HostAndPort chooseNewSyncSource(const Timestamp& ts) override {
        HostAndPort result = _syncSource;
        _syncSource = HostAndPort();
        return result;
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        _blacklistedSource = host;
    }
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& metadata) override {
        return false;
    }
    SyncSourceResolverResponse selectSyncSource(OperationContext* txn,
                                                const OpTime& lastOpTimeFetched) override {
        return SyncSourceResolverResponse();
    }

    HostAndPort _syncSource;
    HostAndPort _blacklistedSource;
};

class DataReplicatorTest : public ReplicationExecutorTest, public SyncSourceSelector {
public:
    DataReplicatorTest() {}

    void postExecutorThreadLaunch() override{};

    /**
     * clear/reset state
     */
    void reset() {
        _rollbackFn = [](OperationContext*, const OpTime&, const HostAndPort&) -> Status {
            return Status::OK();
        };
        _setMyLastOptime = [this](const OpTime& opTime) { _myLastOpTime = opTime; };
        _myLastOpTime = OpTime();
        _memberState = MemberState::RS_UNKNOWN;
        _syncSourceSelector.reset(new SyncSourceSelectorMock(HostAndPort("localhost", -1)));
    }

    // SyncSourceSelector
    void clearSyncSourceBlacklist() override {
        _syncSourceSelector->clearSyncSourceBlacklist();
    }
    HostAndPort chooseNewSyncSource(const Timestamp& ts) override {
        return _syncSourceSelector->chooseNewSyncSource(ts);
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        _syncSourceSelector->blacklistSyncSource(host, until);
    }
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& metadata) override {
        return _syncSourceSelector->shouldChangeSyncSource(currentSource, metadata);
    }
    SyncSourceResolverResponse selectSyncSource(OperationContext* txn,
                                                const OpTime& lastOpTimeFetched) override {
        return SyncSourceResolverResponse();
    }

    void scheduleNetworkResponse(const BSONObj& obj) {
        NetworkInterfaceMock* net = getNet();
        ASSERT_TRUE(net->hasReadyRequests());
        scheduleNetworkResponse(net->getNextReadyRequest(), obj);
    }

    void scheduleNetworkResponse(NetworkInterfaceMock::NetworkOperationIterator noi,
                                 const BSONObj& obj) {
        NetworkInterfaceMock* net = getNet();
        Milliseconds millis(0);
        RemoteCommandResponse response(obj, BSONObj(), millis);
        ReplicationExecutor::ResponseStatus responseStatus(response);
        net->scheduleResponse(noi, net->now(), responseStatus);
    }

    void scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
        NetworkInterfaceMock* net = getNet();
        ASSERT_TRUE(net->hasReadyRequests());
        ReplicationExecutor::ResponseStatus responseStatus(code, reason);
        net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
    }

    void processNetworkResponse(const BSONObj& obj) {
        scheduleNetworkResponse(obj);
        finishProcessingNetworkResponse();
    }

    void processNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
        scheduleNetworkResponse(code, reason);
        finishProcessingNetworkResponse();
    }

    void finishProcessingNetworkResponse() {
        getNet()->runReadyNetworkOperations();
        ASSERT_FALSE(getNet()->hasReadyRequests());
    }

    DataReplicator& getDR() {
        return *_dr;
    }

    DataReplicatorExternalStateMock* getExternalState() {
        return _externalState;
    }

protected:
    void setUp() override {
        ReplicationExecutorTest::setUp();
        StorageInterface::set(getGlobalServiceContext(), stdx::make_unique<StorageInterfaceMock>());
        Client::initThreadIfNotAlready();
        reset();

        launchExecutorThread();

        _myLastOpTime = OpTime({3, 0}, 1);

        DataReplicatorOptions options;
        options.initialSyncRetryWait = Milliseconds(0);
        options.rollbackFn = [this](OperationContext* txn,
                                    const OpTime& lastOpTimeWritten,
                                    const HostAndPort& syncSource) -> Status {
            return _rollbackFn(txn, lastOpTimeWritten, syncSource);
        };

        options.prepareReplSetUpdatePositionCommandFn =
            [](ReplicationCoordinator::ReplSetUpdatePositionCommandStyle commandStyle)
            -> StatusWith<BSONObj> { return BSON(UpdatePositionArgs::kCommandFieldName << 1); };
        options.getMyLastOptime = [this]() { return _myLastOpTime; };
        options.setMyLastOptime = [this](const OpTime& opTime) { _setMyLastOptime(opTime); };
        options.setFollowerMode = [this](const MemberState& state) {
            _memberState = state;
            return true;
        };
        options.getSlaveDelay = [this]() { return Seconds(0); };
        options.syncSourceSelector = this;

        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.poolName = "replication";
        threadPoolOptions.minThreads = 1U;
        threadPoolOptions.maxThreads = 1U;
        threadPoolOptions.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName.c_str());
        };
        // This task executor is used by the MultiApplier only and should not be used to schedule
        // remote commands.
        _applierTaskExecutor = stdx::make_unique<executor::ThreadPoolTaskExecutor>(
            stdx::make_unique<ThreadPool>(threadPoolOptions),
            executor::makeNetworkInterface("DataReplicatorTest-ASIO"));
        _applierTaskExecutor->startup();

        auto dataReplicatorExternalState = stdx::make_unique<DataReplicatorExternalStateMock>();
        dataReplicatorExternalState->taskExecutor = _applierTaskExecutor.get();
        dataReplicatorExternalState->currentTerm = 1LL;
        dataReplicatorExternalState->lastCommittedOpTime = _myLastOpTime;
        {
            ReplicaSetConfig config;
            ASSERT_OK(config.initialize(BSON("_id"
                                             << "myset"
                                             << "version"
                                             << 1
                                             << "protocolVersion"
                                             << 1
                                             << "members"
                                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                      << "localhost:12345"))
                                             << "settings"
                                             << BSON("electionTimeoutMillis" << 10000))));
            dataReplicatorExternalState->replSetConfig = config;
        };
        _externalState = dataReplicatorExternalState.get();

        try {
            _dr.reset(new DataReplicator(
                options, std::move(dataReplicatorExternalState), &(getReplExecutor())));
        } catch (...) {
            ASSERT_OK(exceptionToStatus());
        }
    }

    void tearDown() override {
        ReplicationExecutorTest::tearDown();
        _dr.reset();
        _applierTaskExecutor->shutdown();
        _applierTaskExecutor->join();
        // Executor may still invoke callback before shutting down.
    }

    DataReplicatorOptions::RollbackFn _rollbackFn;
    DataReplicatorOptions::SetMyLastOptimeFn _setMyLastOptime;
    OpTime _myLastOpTime;
    MemberState _memberState;
    std::unique_ptr<SyncSourceSelector> _syncSourceSelector;
    std::unique_ptr<executor::TaskExecutor> _applierTaskExecutor;

private:
    DataReplicatorExternalStateMock* _externalState;
    std::unique_ptr<DataReplicator> _dr;
};

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

TEST_F(DataReplicatorTest, CreateDestroy) {}

TEST_F(DataReplicatorTest, StartOk) {
    ASSERT_OK(getDR().start(makeOpCtx().get()));
}

TEST_F(DataReplicatorTest, CannotInitialSyncAfterStart) {
    auto txn = makeOpCtx();
    ASSERT_OK(getDR().start(txn.get()));
    ASSERT_EQ(ErrorCodes::AlreadyInitialized, getDR().initialSync(txn.get()));
}

// Used to run a Initial Sync in a separate thread, to avoid blocking test execution.
class InitialSyncBackgroundRunner {
public:
    InitialSyncBackgroundRunner(DataReplicator* dr)
        : _dr(dr), _result(Status(ErrorCodes::BadValue, "failed to set status")) {}

    ~InitialSyncBackgroundRunner() {
        if (_thread) {
            _thread->join();
        }
    }

    // Could block if _sgr has not finished
    TimestampStatus getResult() {
        _thread->join();
        _thread.reset();
        return _result;
    }

    void run() {
        _thread.reset(new stdx::thread(stdx::bind(&InitialSyncBackgroundRunner::_run, this)));
    }

private:
    void _run() {
        setThreadName("InitialSyncRunner");
        Client::initThreadIfNotAlready();
        auto txn = getGlobalServiceContext()->makeOperationContext(&cc());
        _result = _dr->initialSync(txn.get());  // blocking
    }

    DataReplicator* _dr;
    TimestampStatus _result;
    std::unique_ptr<stdx::thread> _thread;
};

class InitialSyncTest : public DataReplicatorTest {
public:
    InitialSyncTest()
        : _insertCollectionFn([&](OperationContext* txn,
                                  const NamespaceString& theNss,
                                  const std::vector<BSONObj>& theDocuments) {
              log() << "insertDoc for " << theNss.toString();
              LockGuard lk(_collectionCountMutex);
              ++(_collectionCounts[theNss.toString()]);
              return Status::OK();
          }),
          _beginCollectionFn([&](OperationContext* txn,
                                 const NamespaceString& theNss,
                                 const CollectionOptions& theOptions,
                                 const std::vector<BSONObj>& theIndexSpecs) {
              log() << "beginCollection for " << theNss.toString();
              LockGuard lk(_collectionCountMutex);
              _collectionCounts[theNss.toString()] = 0;
              return Status::OK();
          }){};

protected:
    void setStorageFuncs(ClonerStorageInterfaceMock::InsertCollectionFn ins,
                         ClonerStorageInterfaceMock::BeginCollectionFn beg) {
        _insertCollectionFn = ins;
        _beginCollectionFn = beg;
    }

    void setResponses(std::vector<BSONObj> resps) {
        _responses = resps;
    }

    void startSync() {
        DataReplicator* dr = &(getDR());

        _storage.beginCollectionFn = _beginCollectionFn;
        _storage.insertDocumentsFn = _insertCollectionFn;
        _storage.insertMissingDocFn = [&](OperationContext* txn,
                                          const NamespaceString& nss,
                                          const BSONObj& doc) { return Status::OK(); };

        dr->_setInitialSyncStorageInterface(&_storage);
        _isbr.reset(new InitialSyncBackgroundRunner(dr));
        _isbr->run();
    }


    void playResponses(bool isLastBatchOfResponses) {
        // TODO: Handle network responses
        NetworkInterfaceMock* net = getNet();
        int processedRequests(0);
        const int expectedResponses(_responses.size());

        // counter for oplog entries
        int c(1);
        while (true) {
            net->enterNetwork();
            if (!net->hasReadyRequests() && processedRequests < expectedResponses) {
                net->exitNetwork();
                continue;
            }
            NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();

            const BSONObj reqBSON = noi->getRequest().cmdObj;
            const BSONElement cmdElem = reqBSON.firstElement();
            const bool isGetMore = cmdElem.fieldNameStringData().equalCaseInsensitive("getmore");
            const long long cursorId = cmdElem.numberLong();
            if (isGetMore && cursorId == 1LL) {
                // process getmore requests from the oplog fetcher
                auto respBSON =
                    fromjson(str::stream() << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs'"
                                              " , nextBatch:[{ts:Timestamp("
                                           << ++c
                                           << ",1), h:1, ns:'test.a', v:"
                                           << OplogEntry::kOplogVersion
                                           << ", op:'u', o2:{_id:"
                                           << c
                                           << "}, o:{$set:{a:1}}}]}}");
                net->scheduleResponse(
                    noi,
                    net->now(),
                    ResponseStatus(RemoteCommandResponse(respBSON, BSONObj(), Milliseconds(10))));
                net->runReadyNetworkOperations();
                net->exitNetwork();
                continue;
            } else if (isGetMore) {
                // TODO: return more data
            }

            // process fixed set of responses
            log() << "processing network request: " << noi->getRequest().dbname << "."
                  << noi->getRequest().cmdObj.toString();
            net->scheduleResponse(noi,
                                  net->now(),
                                  ResponseStatus(RemoteCommandResponse(
                                      _responses[processedRequests], BSONObj(), Milliseconds(10))));
            net->runReadyNetworkOperations();
            net->exitNetwork();
            if (++processedRequests >= expectedResponses) {
                log() << "done processing expected requests ";
                break;  // once we have processed all requests, continue;
            }
        }

        if (!isLastBatchOfResponses) {
            return;
        }

        net->enterNetwork();
        if (net->hasReadyRequests()) {
            log() << "There are unexpected requests left";
            log() << "next cmd: " << net->getNextReadyRequest()->getRequest().cmdObj.toString();
            ASSERT_FALSE(net->hasReadyRequests());
        }
        net->exitNetwork();
    }

    void verifySync(Status s = Status::OK()) {
        verifySync(s.code());
    }

    void verifySync(ErrorCodes::Error code) {
        // Check result
        ASSERT_EQ(_isbr->getResult().getStatus().code(), code) << "status codes differ";
    }

    std::map<std::string, int> getLocalCollectionCounts() {
        return _collectionCounts;
    }

private:
    ClonerStorageInterfaceMock::InsertCollectionFn _insertCollectionFn;
    ClonerStorageInterfaceMock::BeginCollectionFn _beginCollectionFn;
    std::vector<BSONObj> _responses;
    std::unique_ptr<InitialSyncBackgroundRunner> _isbr;
    std::map<std::string, int> _collectionCounts;  // counts of inserts during cloning
    mutex _collectionCountMutex;                   // used to protect the collectionCount map
    ClonerStorageInterfaceMock _storage;
};

TEST_F(InitialSyncTest, Complete) {
    /**
     * Initial Sync will issue these query/commands
     *   - startTS = oplog.rs->find().sort({$natural:-1}).limit(-1).next()["ts"]
     *   - listDatabases (foreach db do below)
     *   -- cloneDatabase (see DatabaseCloner tests).
     *   - endTS = oplog.rs->find().sort({$natural:-1}).limit(-1).next()["ts"]
     *   - ops = oplog.rs->find({ts:{$gte: startTS}}) (foreach op)
     *   -- if local doc is missing, getCollection(op.ns).findOne(_id:op.o2._id)
     *   - if any retries were done in the previous loop, endTS query again for minvalid
     *
     */

    const std::vector<BSONObj> responses =
        {
            // get rollback id
            fromjson(str::stream() << "{ok: 1, rbid:1}"),
            // get latest oplog ts
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                        "{ts:Timestamp(1,1), h:1, ns:'a.a', v:"
                     << OplogEntry::kOplogVersion
                     << ", op:'i', o:{_id:1, a:1}}]}}"),
            // oplog fetcher find
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                        "{ts:Timestamp(1,1), h:1, ns:'a.a', v:"
                     << OplogEntry::kOplogVersion
                     << ", op:'i', o:{_id:1, a:1}}]}}"),
            // Clone Start
            // listDatabases
            fromjson("{ok:1, databases:[{name:'a'}]}"),
            // listCollections for "a"
            fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                     "{name:'a', options:{}} "
                     "]}}"),
            // listIndexes:a
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                        "{v:"
                     << OplogEntry::kOplogVersion
                     << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}"),
            // find:a
            fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', firstBatch:["
                     "{_id:1, a:1} "
                     "]}}"),
            // Clone Done
            // get latest oplog ts
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                        "{ts:Timestamp(2,2), h:1, ns:'b.c', v:"
                     << OplogEntry::kOplogVersion
                     << ", op:'i', o:{_id:1, c:1}}]}}"),
            // Applier starts ...
            // check for rollback
            fromjson(str::stream() << "{ok: 1, rbid:1}"),
        };

    // Initial sync flag should not be set before starting.
    auto txn = makeOpCtx();
    ASSERT_FALSE(StorageInterface::get(getGlobalServiceContext())->getInitialSyncFlag(txn.get()));

    startSync();

    // Play first response to ensure data replicator has entered initial sync state.
    setResponses({responses.begin(), responses.begin() + 1});
    playResponses(false);

    // Initial sync flag should be set.
    ASSERT_TRUE(StorageInterface::get(getGlobalServiceContext())->getInitialSyncFlag(txn.get()));

    // Play rest of the responses after checking initial sync flag.
    setResponses({responses.begin() + 1, responses.end()});
    playResponses(true);

    verifySync();

    // Initial sync flag should not be set after completion.
    ASSERT_FALSE(StorageInterface::get(getGlobalServiceContext())->getInitialSyncFlag(txn.get()));
}

TEST_F(InitialSyncTest, MissingDocOnMultiApplyCompletes) {
    DataReplicatorOptions opts;
    int applyCounter{0};
    getExternalState()->multiApplyFn = [&](OperationContext*,
                                           const MultiApplier::Operations& ops,
                                           MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        if (++applyCounter == 1) {
            return Status(ErrorCodes::NoMatchingDocument, "failed: missing doc.");
        }
        return ops.back().getOpTime();
    };

    const std::vector<BSONObj> responses =
        {
            // get rollback id
            fromjson(str::stream() << "{ok: 1, rbid:1}"),
            // get latest oplog ts
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                        "{ts:Timestamp(1,1), h:1, ns:'a.a', v:"
                     << OplogEntry::kOplogVersion
                     << ", op:'i', o:{_id:1, a:1}}]}}"),
            // oplog fetcher find
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                        "{ts:Timestamp(1,1), h:1, ns:'a.a', v:"
                     << OplogEntry::kOplogVersion
                     << ", op:'u', o2:{_id:1}, o:{$set:{a:1}}}]}}"),
            // Clone Start
            // listDatabases
            fromjson("{ok:1, databases:[{name:'a'}]}"),
            // listCollections for "a"
            fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                     "{name:'a', options:{}} "
                     "]}}"),
            // listIndexes:a
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                        "{v:"
                     << OplogEntry::kOplogVersion
                     << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}"),
            // find:a -- empty
            fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', firstBatch:[]}}"),
            // Clone Done
            // get latest oplog ts
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                        "{ts:Timestamp(2,2), h:1, ns:'b.c', v:"
                     << OplogEntry::kOplogVersion
                     << ", op:'i', o:{_id:1, c:1}}]}}"),
            // Applier starts ...
            // missing doc fetch -- find:a {_id:1}
            fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', firstBatch:["
                     "{_id:1, a:1} "
                     "]}}"),
            // check for rollback
            fromjson(str::stream() << "{ok: 1, rbid:1}"),
        };
    startSync();
    setResponses(responses);
    playResponses(true);
    verifySync(ErrorCodes::OK);
}

TEST_F(InitialSyncTest, Failpoint) {
    mongo::getGlobalFailPointRegistry()
        ->getFailPoint("failInitialSyncWithBadHost")
        ->setMode(FailPoint::alwaysOn);

    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")));

    Timestamp time1(100, 1);
    OpTime opTime1(time1, OpTime::kInitialTerm);
    _myLastOpTime = opTime1;
    _memberState = MemberState::RS_SECONDARY;

    DataReplicator* dr = &(getDR());
    InitialSyncBackgroundRunner isbr(dr);
    isbr.run();
    ASSERT_EQ(isbr.getResult().getStatus().code(), ErrorCodes::InitialSyncFailure);

    mongo::getGlobalFailPointRegistry()
        ->getFailPoint("failInitialSyncWithBadHost")
        ->setMode(FailPoint::off);
}

TEST_F(InitialSyncTest, FailsOnClone) {
    const std::vector<BSONObj> responses = {
        // get rollback id
        fromjson(str::stream() << "{ok: 1, rbid:1}"),
        // get latest oplog ts
        fromjson(
            str::stream() << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                             "{ts:Timestamp(1,1), h:1, ns:'a.a', v:"
                          << OplogEntry::kOplogVersion
                          << ", op:'i', o:{_id:1, a:1}}]}}"),
        // oplog fetcher find
        fromjson(
            str::stream() << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                             "{ts:Timestamp(1,1), h:1, ns:'a.a', v:"
                          << OplogEntry::kOplogVersion
                          << ", op:'i', o:{_id:1, a:1}}]}}"),
        // Clone Start
        // listDatabases
        fromjson("{ok:0}"),
        // get rollback id
        fromjson(str::stream() << "{ok: 1, rbid:1}"),
    };
    startSync();
    setResponses(responses);
    playResponses(true);
    verifySync(ErrorCodes::InitialSyncFailure);
}

TEST_F(InitialSyncTest, FailOnRollback) {
    const std::vector<BSONObj> responses =
        {
            // get rollback id
            fromjson(str::stream() << "{ok: 1, rbid:1}"),
            // get latest oplog ts
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                        "{ts:Timestamp(1,1), h:1, ns:'a.a', v:"
                     << OplogEntry::kOplogVersion
                     << ", op:'i', o:{_id:1, a:1}}]}}"),
            // oplog fetcher find
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                        "{ts:Timestamp(1,1), h:1, ns:'a.a', v:"
                     << OplogEntry::kOplogVersion
                     << ", op:'i', o:{_id:1, a:1}}]}}"),
            // Clone Start
            // listDatabases
            fromjson("{ok:1, databases:[{name:'a'}]}"),
            // listCollections for "a"
            fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                     "{name:'a', options:{}} "
                     "]}}"),
            // listIndexes:a
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                        "{v:"
                     << OplogEntry::kOplogVersion
                     << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}"),
            // find:a
            fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', firstBatch:["
                     "{_id:1, a:1} "
                     "]}}"),
            // Clone Done
            // get latest oplog ts
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                        "{ts:Timestamp(2,2), h:1, ns:'b.c', v:"
                     << OplogEntry::kOplogVersion
                     << ", op:'i', o:{_id:1, c:1}}]}}"),
            // Applier starts ...
            // check for rollback
            fromjson(str::stream() << "{ok: 1, rbid:2}"),
        };

    startSync();
    setResponses({responses});
    playResponses(true);
    verifySync(ErrorCodes::InitialSyncFailure);
}


class TestSyncSourceSelector2 : public SyncSourceSelector {
public:
    void clearSyncSourceBlacklist() override {}
    HostAndPort chooseNewSyncSource(const Timestamp& ts) override {
        LockGuard lk(_mutex);
        auto result = HostAndPort(str::stream() << "host-" << _nextSourceNum++, -1);
        _condition.notify_all();
        return result;
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        LockGuard lk(_mutex);
        _blacklistedSource = host;
    }
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& metadata) override {
        return false;
    }
    SyncSourceResolverResponse selectSyncSource(OperationContext* txn,
                                                const OpTime& lastOpTimeFetched) override {
        return SyncSourceResolverResponse();
    }
    mutable stdx::mutex _mutex;
    stdx::condition_variable _condition;
    int _nextSourceNum{0};
    HostAndPort _blacklistedSource;
};

class SteadyStateTest : public DataReplicatorTest {
protected:
    void _setUpOplogFetcherFailed() {
        DataReplicator& dr = getDR();
        _syncSourceSelector.reset(new TestSyncSourceSelector2());
        _memberState = MemberState::RS_UNKNOWN;
        auto net = getNet();
        net->enterNetwork();
        ASSERT_OK(dr.start(makeOpCtx().get()));
    }

    void _testOplogFetcherFailed(const BSONObj& oplogFetcherResponse,
                                 const Status& rollbackStatus,
                                 const HostAndPort& expectedRollbackSource,
                                 const HostAndPort& expectedBlacklistedSource,
                                 const HostAndPort& expectedFinalSource,
                                 const MemberState& expectedFinalState,
                                 const DataReplicatorState& expectedDataReplicatorState,
                                 int expectedNextSourceNum) {
        OperationContext* rollbackTxn = nullptr;
        HostAndPort rollbackSource;
        DataReplicatorState stateDuringRollback = DataReplicatorState::Uninitialized;
        // Rollback happens on network thread now instead of DB worker thread previously.
        _rollbackFn = [&](OperationContext* txn,
                          const OpTime& lastOpTimeWritten,
                          const HostAndPort& syncSource) -> Status {
            rollbackTxn = txn;
            rollbackSource = syncSource;
            stateDuringRollback = getDR().getState();
            return rollbackStatus;
        };

        auto net = getNet();
        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        ASSERT_EQUALS("find", std::string(noi->getRequest().cmdObj.firstElementFieldName()));
        scheduleNetworkResponse(noi, oplogFetcherResponse);
        net->runReadyNetworkOperations();

        // Replicator state should be ROLLBACK before rollback function returns.
        ASSERT_EQUALS(toString(DataReplicatorState::Rollback), toString(stateDuringRollback));
        ASSERT_TRUE(rollbackTxn);
        ASSERT_EQUALS(expectedRollbackSource, rollbackSource);

        auto&& dr = getDR();
        dr.waitForState(expectedDataReplicatorState);

        // Wait for data replicator to request a new sync source if rollback is expected to fail.
        if (!rollbackStatus.isOK()) {
            TestSyncSourceSelector2* syncSourceSelector =
                static_cast<TestSyncSourceSelector2*>(_syncSourceSelector.get());
            UniqueLock lk(syncSourceSelector->_mutex);
            while (syncSourceSelector->_nextSourceNum < expectedNextSourceNum) {
                syncSourceSelector->_condition.wait(lk);
            }
            ASSERT_EQUALS(expectedBlacklistedSource, syncSourceSelector->_blacklistedSource);
        }

        ASSERT_EQUALS(expectedFinalSource, dr.getSyncSource());
        ASSERT_EQUALS(expectedFinalState.toString(), _memberState.toString());
    }
};

TEST_F(SteadyStateTest, StartWhenInSteadyState) {
    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto txn = makeOpCtx();
    ASSERT_OK(dr.start(txn.get()));
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, dr.start(txn.get()));
}

TEST_F(SteadyStateTest, ShutdownAfterStart) {
    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto net = getNet();
    net->enterNetwork();
    auto txn = makeOpCtx();
    ASSERT_OK(dr.start(txn.get()));
    ASSERT_TRUE(net->hasReadyRequests());
    getReplExecutor().shutdown();
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, dr.start(txn.get()));
}

TEST_F(SteadyStateTest, RequestShutdownAfterStart) {
    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto net = getNet();
    net->enterNetwork();
    auto txn = makeOpCtx();
    ASSERT_OK(dr.start(txn.get()));
    ASSERT_TRUE(net->hasReadyRequests());
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
    // Simulating an invalid remote oplog query response. This will invalidate the existing
    // sync source but that's fine because we're not testing oplog processing.
    scheduleNetworkResponse(BSON("ok" << 0));
    net->runReadyNetworkOperations();
    ASSERT_OK(dr.scheduleShutdown(txn.get()));
    net->exitNetwork();  // runs work item scheduled in 'scheduleShutdown()).
    dr.waitForShutdown();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
}

class ShutdownExecutorSyncSourceSelector : public SyncSourceSelector {
public:
    ShutdownExecutorSyncSourceSelector(ReplicationExecutor* exec) : _exec(exec) {}
    void clearSyncSourceBlacklist() override {}
    HostAndPort chooseNewSyncSource(const Timestamp& ts) override {
        _exec->shutdown();
        return HostAndPort();
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {}
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& metadata) override {
        return false;
    }
    SyncSourceResolverResponse selectSyncSource(OperationContext* txn,
                                                const OpTime& lastOpTimeFetched) override {
        return SyncSourceResolverResponse();
    }
    ReplicationExecutor* _exec;
};

TEST_F(SteadyStateTest, ScheduleNextActionFailsAfterChoosingEmptySyncSource) {
    _syncSourceSelector.reset(new ShutdownExecutorSyncSourceSelector(&getReplExecutor()));

    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto net = getNet();
    net->enterNetwork();
    ASSERT_OK(dr.start(makeOpCtx().get()));
    ASSERT_EQUALS(HostAndPort(), dr.getSyncSource());
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
}

TEST_F(SteadyStateTest, ChooseNewSyncSourceAfterFailedNetworkRequest) {
    TestSyncSourceSelector2* testSyncSourceSelector = new TestSyncSourceSelector2();
    _syncSourceSelector.reset(testSyncSourceSelector);

    _memberState = MemberState::RS_UNKNOWN;
    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto net = getNet();
    net->enterNetwork();
    ASSERT_OK(dr.start(makeOpCtx().get()));
    ASSERT_TRUE(net->hasReadyRequests());
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
    // Simulating an invalid remote oplog query response to cause the data replicator to
    // blacklist the existing sync source and request a new one.
    scheduleNetworkResponse(BSON("ok" << 0));
    net->runReadyNetworkOperations();

    // Wait for data replicator to request a new sync source.
    {
        UniqueLock lk(testSyncSourceSelector->_mutex);
        while (testSyncSourceSelector->_nextSourceNum < 2) {
            testSyncSourceSelector->_condition.wait(lk);
        }
        ASSERT_EQUALS(HostAndPort("host-0", -1), testSyncSourceSelector->_blacklistedSource);
    }
    ASSERT_EQUALS(HostAndPort("host-1", -1), dr.getSyncSource());
    ASSERT_EQUALS(MemberState(MemberState::RS_UNKNOWN).toString(), _memberState.toString());
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
}

TEST_F(SteadyStateTest, RemoteOplogEmptyRollbackSucceeded) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse =
        fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch: []}}");
    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status::OK(),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort(),              // sync source should not be blacklisted.
                            HostAndPort("host-0", -1),
                            MemberState::RS_SECONDARY,
                            DataReplicatorState::Steady,
                            2);
}

TEST_F(SteadyStateTest, RemoteOplogEmptyRollbackFailed) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse =
        fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch: []}}");
    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort("host-0", -1),
                            HostAndPort("host-1", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            2);
}

TEST_F(SteadyStateTest, RemoteOplogFirstOperationMissingTimestampRollbackFailed) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse =
        fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch: [{}]}}");
    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort("host-0", -1),
                            HostAndPort("host-1", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            2);
}

TEST_F(SteadyStateTest, RemoteOplogFirstOperationTimestampDoesNotMatchRollbackFailed) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse = fromjson(
        "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:[{ts:Timestamp(1,1)}]}}");
    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort("host-0", -1),
                            HostAndPort("host-1", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            2);
}

TEST_F(SteadyStateTest, RollbackTwoSyncSourcesBothFailed) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse =
        fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch: []}}");

    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort("host-0", -1),
                            HostAndPort("host-1", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            2);

    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-1", -1),  // rollback source
                            HostAndPort("host-1", -1),
                            HostAndPort("host-2", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            3);
}

TEST_F(SteadyStateTest, RollbackTwoSyncSourcesSecondRollbackSucceeds) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse =
        fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch: []}}");

    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort("host-0", -1),
                            HostAndPort("host-1", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            2);

    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status::OK(),
                            HostAndPort("host-1", -1),  // rollback source
                            HostAndPort("host-0", -1),  // blacklisted source unchanged
                            HostAndPort("host-1", -1),
                            MemberState::RS_SECONDARY,
                            DataReplicatorState::Steady,
                            2);  // not used when rollback is expected to succeed
}

TEST_F(SteadyStateTest, PauseDataReplicator) {
    auto lastOperationApplied = BSON("op"
                                     << "a"
                                     << "v"
                                     << OplogEntry::kOplogVersion
                                     << "ts"
                                     << Timestamp(Seconds(123), 0));

    auto operationToApply = BSON("op"
                                 << "a"
                                 << "v"
                                 << OplogEntry::kOplogVersion
                                 << "ts"
                                 << Timestamp(Seconds(456), 0));

    stdx::mutex mutex;
    unittest::Barrier barrier(2U);
    Timestamp lastTimestampApplied;
    BSONObj operationApplied;
    getExternalState()->multiApplyFn = [&](OperationContext*,
                                           const MultiApplier::Operations& ops,
                                           MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        operationApplied = ops.back().raw;
        barrier.countDownAndWait();
        return ops.back().getOpTime();
    };
    DataReplicatorOptions::SetMyLastOptimeFn oldSetMyLastOptime = _setMyLastOptime;
    _setMyLastOptime = [&](const OpTime& opTime) {
        oldSetMyLastOptime(opTime);
        stdx::lock_guard<stdx::mutex> lock(mutex);
        lastTimestampApplied = opTime.getTimestamp();
        barrier.countDownAndWait();
    };

    auto& dr = getDR();
    _myLastOpTime = OpTime(lastOperationApplied["ts"].timestamp(), OpTime::kInitialTerm);
    _memberState = MemberState::RS_SECONDARY;

    auto net = getNet();
    net->enterNetwork();

    ASSERT_OK(dr.start(makeOpCtx().get()));

    ASSERT_TRUE(net->hasReadyRequests());
    {
        auto networkRequest = net->getNextReadyRequest();
        auto commandResponse =
            BSON("ok" << 1 << "cursor"
                      << BSON("id" << 1LL << "ns"
                                   << "local.oplog.rs"
                                   << "firstBatch"
                                   << BSON_ARRAY(lastOperationApplied << operationToApply)));
        scheduleNetworkResponse(networkRequest, commandResponse);
    }

    dr.pause();

    ASSERT_EQUALS(0U, dr.getOplogBufferCount());

    // Data replication will process the fetcher response but will not schedule the applier.
    net->runReadyNetworkOperations();
    ASSERT_EQUALS(operationToApply["ts"].timestamp(), dr.getLastTimestampFetched());

    // Schedule a bogus work item to ensure that the operation applier function
    // is not scheduled.
    auto& exec = getReplExecutor();
    exec.scheduleWork(
        [&barrier](const executor::TaskExecutor::CallbackArgs&) { barrier.countDownAndWait(); });


    // Wake up executor thread and wait for bogus work callback to be invoked.
    net->exitNetwork();
    barrier.countDownAndWait();

    // Oplog buffer should contain fetched operations since applier is not scheduled.
    ASSERT_EQUALS(1U, dr.getOplogBufferCount());

    dr.resume();

    // Wait for applier function.
    barrier.countDownAndWait();
    // Run scheduleWork() work item scheduled in DataReplicator::_onApplyBatchFinish().
    net->exitNetwork();

    // Wait for batch completion callback.
    barrier.countDownAndWait();

    ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY).toString(), _memberState.toString());
    {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        ASSERT_EQUALS(operationToApply, operationApplied);
        ASSERT_EQUALS(operationToApply["ts"].timestamp(), lastTimestampApplied);
    }
}

TEST_F(SteadyStateTest, ApplyOneOperation) {
    auto lastOperationApplied = BSON("op"
                                     << "a"
                                     << "v"
                                     << OplogEntry::kOplogVersion
                                     << "ts"
                                     << Timestamp(Seconds(123), 0));

    auto operationToApply = BSON("op"
                                 << "a"
                                 << "v"
                                 << OplogEntry::kOplogVersion
                                 << "ts"
                                 << Timestamp(Seconds(456), 0));

    stdx::mutex mutex;
    unittest::Barrier barrier(2U);
    Timestamp lastTimestampApplied;
    BSONObj operationApplied;
    getExternalState()->multiApplyFn = [&](OperationContext*,
                                           const MultiApplier::Operations& ops,
                                           MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        operationApplied = ops.back().raw;
        barrier.countDownAndWait();
        return ops.back().getOpTime();
    };
    DataReplicatorOptions::SetMyLastOptimeFn oldSetMyLastOptime = _setMyLastOptime;
    _setMyLastOptime = [&](const OpTime& opTime) {
        oldSetMyLastOptime(opTime);
        stdx::lock_guard<stdx::mutex> lock(mutex);
        lastTimestampApplied = opTime.getTimestamp();
        barrier.countDownAndWait();
    };

    _myLastOpTime = OpTime(lastOperationApplied["ts"].timestamp(), OpTime::kInitialTerm);
    _memberState = MemberState::RS_SECONDARY;

    auto net = getNet();
    net->enterNetwork();

    auto& dr = getDR();
    ASSERT_OK(dr.start(makeOpCtx().get()));

    ASSERT_TRUE(net->hasReadyRequests());
    {
        auto networkRequest = net->getNextReadyRequest();
        auto commandResponse =
            BSON("ok" << 1 << "cursor"
                      << BSON("id" << 1LL << "ns"
                                   << "local.oplog.rs"
                                   << "firstBatch"
                                   << BSON_ARRAY(lastOperationApplied << operationToApply)));
        scheduleNetworkResponse(networkRequest, commandResponse);
    }
    ASSERT_EQUALS(0U, dr.getOplogBufferCount());

    // Oplog buffer should be empty because contents are transferred to applier.
    net->runReadyNetworkOperations();
    ASSERT_EQUALS(0U, dr.getOplogBufferCount());

    // Wait for applier function.
    barrier.countDownAndWait();
    ASSERT_EQUALS(operationToApply["ts"].timestamp(), dr.getLastTimestampFetched());
    // Run scheduleWork() work item scheduled in DataReplicator::_onApplyBatchFinish().
    net->exitNetwork();

    // Wait for batch completion callback.
    barrier.countDownAndWait();

    ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY).toString(), _memberState.toString());
    {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        ASSERT_EQUALS(operationToApply, operationApplied);
        ASSERT_EQUALS(operationToApply["ts"].timestamp(), lastTimestampApplied);
    }

    // Ensure that we send position information upstream after completing batch.
    net->enterNetwork();
    bool found = false;
    while (net->hasReadyRequests()) {
        auto networkRequest = net->getNextReadyRequest();
        auto commandRequest = networkRequest->getRequest();
        const auto& cmdObj = commandRequest.cmdObj;
        if (str::equals(cmdObj.firstElementFieldName(), UpdatePositionArgs::kCommandFieldName) &&
            commandRequest.dbname == "admin") {
            found = true;
            break;
        } else {
            net->blackHole(networkRequest);
        }
    }
    ASSERT_TRUE(found);
}

}  // namespace
