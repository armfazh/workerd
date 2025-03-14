// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Defines abstract interfaces for observing the activity of various components of the system,
// e.g. to collect logs and metrics.

#include <kj/string.h>
#include <kj/refcount.h>
#include <kj/exception.h>
#include <kj/time.h>
#include <kj/compat/http.h>
#include <workerd/io/trace.h>

namespace workerd {

class WorkerInterface;
class LimitEnforcer;
class TimerChannel;

class RequestObserver: public kj::Refcounted {
  // Observes a specific request to a specific worker. Also observes outgoing subrequests.
  //
  // Observing anything is optional. Default implementations of all methods observe nothing.

public:
  virtual void delivered() {};
  // Invoked when the request is actually delivered.
  //
  // If, for some reason, this is not invoked before the object is destroyed, this indicate that
  // the event was canceled for some reason before delivery. No JavaScript was invoked. In this
  // case, the request should not be billed.

  virtual void jsDone() {}
  // Call when no more JavaScript will run on behalf of this request. Note that deferred proxying
  // may still be in progress.

  virtual void setIsPrewarm() {}
  // Called to indicate this was a prewarm request. Normal request metrics won't be logged, but
  // the prewarm metric will be incremented.

  virtual void reportFailure(const kj::Exception& e) {}
  // Report that the request failed with the given exception. This only needs to be called in
  // cases where the wrapper created with wrapWorkerInterface() wouldn't otherwise see the
  // exception, e.g. because it has been replaced with an HTTP error response or because it
  // occurred asynchronously.

  virtual WorkerInterface& wrapWorkerInterface(WorkerInterface& worker) { return worker; }
  // Wrap the given WorkerInterface with a version that collects metrics. This method may only be
  // called once, and only one method call may be made to the returned interface.
  //
  // The returned reference remains valid as long as the observer and `worker` both remain live.

  virtual kj::Own<WorkerInterface> wrapSubrequestClient(kj::Own<WorkerInterface> client) {
    // Wrap an HttpClient so that its usage is counted in the request's subrequest stats.
    return kj::mv(client);
  }

  virtual kj::Own<WorkerInterface> wrapActorSubrequestClient(kj::Own<WorkerInterface> client) {
    // Wrap an HttpClient so that its usage is counted in the request's actor subrequest count.
    return kj::mv(client);
  }

  virtual SpanParent getSpan() { return nullptr; }

  virtual void addedContextTask() {}
  virtual void finishedContextTask() {}
  virtual void addedWaitUntilTask() {}
  virtual void finishedWaitUntilTask() {}

  virtual void setFailedOpen(bool value) {}
};

class IsolateObserver: public kj::AtomicRefcounted {
public:
  virtual void created() {};
  // Called when Worker::Isolate is created.

  virtual void evicted() {}
  // Called when the owning Worker::Script is being destroyed. The IsolateObserver may
  // live a while longer to handle deferred proxy requests.

  virtual void teardownStarted() {}
  virtual void teardownLockAcquired() {}
  virtual void teardownFinished() {}

  enum class StartType: uint8_t {
    // Describes why a worker was started.

    COLD,
    // Cold start with active request waiting.

    PREWARM,
    // Started due to prewarm hint (e.g. from TLS SNI); a real request is expected soon.

    PRELOAD
    // Started due to preload at process startup.
  };

  class Parse {
    // Created while parsing a script, to record related metrics.
  public:
    virtual void done() {}
    // Marks the ScriptReplica as finished parsing, which starts reporting of isolate metrics.
  };

  virtual kj::Own<Parse> parse(StartType startType) const {
    class FinalParse final: public Parse {};
    return kj::heap<FinalParse>();
  }

  class LockTiming {
  public:
    virtual void waitingForOtherIsolate(kj::StringPtr id) {}
    // Called by `Isolate::takeAsyncLock()` when it is blocked by a different isolate lock on the
    // same thread.

    virtual void reportAsyncInfo(uint currentLoad, bool threadWaitingSameLock,
        uint threadWaitingDifferentLockCount) {}
    // Call if this is an async lock attempt, before constructing LockRecord.
    //
    // TODO(cleanup): Should be able to get this data at `tryCreateLockTiming()` time. It'd be
    //   easier if IsolateObserver were an AOP class, and thus had access to the real isolate.

    virtual void start() {}
    virtual void stop() {}

    virtual void locked() {}
    virtual void gcPrologue() {}
    virtual void gcEpilogue() {}
  };

  virtual kj::Maybe<kj::Own<LockTiming>> tryCreateLockTiming(
      kj::OneOf<SpanParent, kj::Maybe<RequestObserver&>> parentOrRequest) const { return nullptr; }
  // Construct a LockTiming if config.reportScriptLockTiming is true, or if the
  // request (if any) is being traced.

  class LockRecord {
    // Use like so:
    //
    //   auto lockTiming = MetricsCollector::ScriptReplica::LockTiming::tryCreate(script, maybeRequest);
    //   MetricsCollector::ScriptReplica::LockRecord record(lockTiming);
    //   JsgWorkerIsolate::Lock lock(isolate);
    //   record.locked();
    //
    // And `record()` will report the time spent waiting for the lock (including any asynchronous
    // time you might insert between the construction of `lockTiming` and `LockRecord()`), plus
    // the time spent holding the lock for the given ScriptReplica.
    //
    // This is a thin wrapper around LockTiming which efficiently handles the case where we don't
    // want to track timing.

  public:
    explicit LockRecord(kj::Maybe<kj::Own<LockTiming>> lockTimingParam)
        : lockTiming(kj::mv(lockTimingParam)) {
      KJ_IF_MAYBE(l, lockTiming) l->get()->start();
    }
    ~LockRecord() noexcept(false) {
      KJ_IF_MAYBE(l, lockTiming) l->get()->stop();
    }
    KJ_DISALLOW_COPY_AND_MOVE(LockRecord);

    void locked() { KJ_IF_MAYBE(l, lockTiming) l->get()->locked(); }
    void gcPrologue() { KJ_IF_MAYBE(l, lockTiming) l->get()->gcPrologue(); }
    void gcEpilogue() { KJ_IF_MAYBE(l, lockTiming) l->get()->gcEpilogue(); }

  private:
    kj::Maybe<kj::Own<LockTiming>> lockTiming;
    // The presence of `lockTiming` determines whether or not we need to record timing data. If
    // we have no `lockTiming`, then this LockRecord wrapper is just a big nothingburger.
  };
};

class WorkerObserver: public kj::AtomicRefcounted {
public:
  class Startup {
    // Created while executing a script's global scope, to record related metrics.
  public:
    virtual void done() {}
  };

  virtual kj::Own<Startup> startup(IsolateObserver::StartType startType) const {
    class FinalStartup final: public Startup {};
    return kj::heap<FinalStartup>();
  }

  virtual void teardownStarted() {}
  virtual void teardownLockAcquired() {}
  virtual void teardownFinished() {}
};

class ActorObserver: public kj::Refcounted {
public:
  virtual kj::Promise<void> flushLoop(TimerChannel& timer, LimitEnforcer& limitEnforcer) {
    // Allows the observer to run in the background, periodically making observations. Owner must
    // call this and store the promise. `limitEnforcer` is used to collect CPU usage metrics, it
    // must remain valid as long as the loop is running.

    return kj::NEVER_DONE;
  }

  virtual void startRequest() {}
  virtual void endRequest() {}

  virtual void webSocketAccepted() {}
  virtual void webSocketClosed() {}
  virtual void receivedWebSocketMessage(size_t bytes) {}
  virtual void sentWebSocketMessage(size_t bytes) {}

  virtual void addCachedStorageReadUnits(uint32_t units) {}
  virtual void addUncachedStorageReadUnits(uint32_t units) {}
  virtual void addStorageWriteUnits(uint32_t units) {}
  virtual void addStorageDeletes(uint32_t count) {}

  virtual void inputGateLocked() {}
  virtual void inputGateReleased() {}
  virtual void inputGateWaiterAdded() {}
  virtual void inputGateWaiterRemoved() {}
  virtual void outputGateLocked() {}
  virtual void outputGateReleased() {}
  virtual void outputGateWaiterAdded() {}
  virtual void outputGateWaiterRemoved() {}

  virtual void shutdown(uint16_t reasonCode, LimitEnforcer& limitEnforcer) {}
};

}  // namespace workerd
