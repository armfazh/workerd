// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// APIs that an Actor (Durable Object) uses to access its own state.
//
// See actor.h for APIs used by other Workers to talk to Actors.

#include <workerd/jsg/jsg.h>
#include <workerd/io/io-context.h>
#include <workerd/io/actor-storage.capnp.h>
#include <kj/async.h>
#include <v8.h>
#include <workerd/io/actor-cache.h>

namespace workerd::api {
class SqlStorage;

// Forward-declared to avoid dependency cycle (actor.h -> http.h -> basics.h -> actor-state.h)
class DurableObjectId;
class WebSocket;

kj::Array<kj::byte> serializeV8Value(v8::Local<v8::Value> value, v8::Isolate* isolate);

v8::Local<v8::Value> deserializeV8Value(
    kj::ArrayPtr<const char> key, kj::ArrayPtr<const kj::byte> buf, v8::Isolate* isolate);

class DurableObjectStorageOperations {
  // Common implementation of DurableObjectStorage and DurableObjectTransaction. This class is
  // designed to be used as a mixin.

public:
  struct GetOptions {
    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> noCache;

    inline operator ActorCacheOps::ReadOptions() const {
      return {
        .noCache = noCache.orDefault(false)
      };
    }

    JSG_STRUCT(allowConcurrency, noCache);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectGetOptions); // Rename from DurableObjectStorageOperationsGetOptions
  };

  jsg::Promise<jsg::Value> get(
      kj::OneOf<kj::String, kj::Array<kj::String>> keys, jsg::Optional<GetOptions> options,
      v8::Isolate* isolate);

  struct GetAlarmOptions {
    jsg::Optional<bool> allowConcurrency;

    JSG_STRUCT(allowConcurrency);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectGetAlarmOptions); // Rename from DurableObjectStorageOperationsGetAlarmOptions
  };

  jsg::Promise<kj::Maybe<double>> getAlarm(jsg::Optional<GetAlarmOptions> options, v8::Isolate* isolate);

  struct ListOptions {
    jsg::Optional<kj::String> start;
    jsg::Optional<kj::String> startAfter;
    jsg::Optional<kj::String> end;
    jsg::Optional<kj::String> prefix;
    jsg::Optional<bool> reverse;
    jsg::Optional<int> limit;

    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> noCache;

    inline operator ActorCacheOps::ReadOptions() const {
      return {
        .noCache = noCache.orDefault(false)
      };
    }

    JSG_STRUCT(start, startAfter, end, prefix, reverse, limit, allowConcurrency, noCache);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectListOptions); // Rename from DurableObjectStorageOperationsListOptions
  };

  jsg::Promise<jsg::Value> list(jsg::Optional<ListOptions> options, v8::Isolate* isolate);

  struct PutOptions {
    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> allowUnconfirmed;
    jsg::Optional<bool> noCache;

    inline operator ActorCacheOps::WriteOptions() const {
      return {
        .allowUnconfirmed = allowUnconfirmed.orDefault(false),
        .noCache = noCache.orDefault(false)
      };
    }

    JSG_STRUCT(allowConcurrency, allowUnconfirmed, noCache);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectPutOptions); // Rename from DurableObjectStorageOperationsPutOptions
  };

  jsg::Promise<void> put(jsg::Lock& js,
      kj::OneOf<kj::String, jsg::Dict<v8::Local<v8::Value>>> keyOrEntries,
      jsg::Optional<v8::Local<v8::Value>> value, jsg::Optional<PutOptions> options,
      v8::Isolate* isolate, const jsg::TypeHandler<PutOptions>& optionsTypeHandler);

  kj::OneOf<jsg::Promise<bool>, jsg::Promise<int>> delete_(
      kj::OneOf<kj::String, kj::Array<kj::String>> keys, jsg::Optional<PutOptions> options,
      v8::Isolate* isolate);

  struct SetAlarmOptions {
    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> allowUnconfirmed;
    // We don't allow noCache for alarm puts.

    inline operator ActorCacheOps::WriteOptions() const {
      return {
        .allowUnconfirmed = allowUnconfirmed.orDefault(false),
      };
    }

    JSG_STRUCT(allowConcurrency, allowUnconfirmed);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectSetAlarmOptions); // Rename from DurableObjectStorageOperationsSetAlarmOptions
  };

  jsg::Promise<void> setAlarm(kj::Date scheduledTime, jsg::Optional<SetAlarmOptions> options,
      v8::Isolate* isolate);
  jsg::Promise<void> deleteAlarm(jsg::Optional<SetAlarmOptions> options, v8::Isolate* isolate);

protected:
  typedef kj::StringPtr OpName;
  static constexpr OpName OP_GET = "get()"_kj;
  static constexpr OpName OP_GET_ALARM = "getAlarm()"_kj;
  static constexpr OpName OP_LIST = "list()"_kj;
  static constexpr OpName OP_PUT = "put()"_kj;
  static constexpr OpName OP_PUT_ALARM = "setAlarm()"_kj;
  static constexpr OpName OP_DELETE = "delete()"_kj;
  static constexpr OpName OP_DELETE_ALARM = "deleteAlarm()"_kj;
  static constexpr OpName OP_RENAME = "rename()"_kj;
  static constexpr OpName OP_ROLLBACK = "rollback()"_kj;

  static bool readOnlyOp(OpName op) {
    return op == OP_GET || op == OP_LIST || op == OP_ROLLBACK;
  }

  virtual ActorCacheOps& getCache(OpName op) = 0;

  virtual bool useDirectIo() = 0;
  // Whether to skip caching and allow concurrency on all operations.

  template <typename T>
  T configureOptions(T&& options) {
    // Method that should be called at the start of each storage operation to override any of the
    // options as appropriate.
    if (useDirectIo()) {
      options.allowConcurrency = true;
      options.noCache = true;
    }
    return kj::mv(options);
  }

private:
  jsg::Promise<jsg::Value> getOne(kj::String key, const GetOptions& options, v8::Isolate* isolate);
  jsg::Promise<jsg::Value> getMultiple(kj::Array<kj::String> keys, const GetOptions& options,
                                         v8::Isolate* isolate);

  jsg::Promise<void> putOne(kj::String key, v8::Local<v8::Value> value, const PutOptions& options,
                             v8::Isolate* isolate);
  jsg::Promise<void> putMultiple(jsg::Dict<v8::Local<v8::Value>> entries,
                                  const PutOptions& options, v8::Isolate* isolate);

  jsg::Promise<bool> deleteOne(kj::String key, const PutOptions& options, v8::Isolate* isolate);
  jsg::Promise<int> deleteMultiple(kj::Array<kj::String> keys, const PutOptions& options,
                                    v8::Isolate* isolate);
};

class DurableObjectTransaction;

class DurableObjectStorage: public jsg::Object, public DurableObjectStorageOperations {
public:
  DurableObjectStorage(IoPtr<ActorCacheInterface> cache)
    : cache(kj::mv(cache)) {}

  ActorCacheInterface& getActorCacheInterface() { return *cache; }

  struct TransactionOptions {
    jsg::Optional<kj::Date> asOfTime;
    jsg::Optional<bool> lowPriority;

    JSG_STRUCT(asOfTime, lowPriority);
    JSG_STRUCT_TS_OVERRIDE(type TransactionOptions = never);
    // Omit from definitions
  };

  jsg::Promise<jsg::Value> transaction(jsg::Lock& js,
      jsg::Function<jsg::Promise<jsg::Value>(jsg::Ref<DurableObjectTransaction>)> closure,
      jsg::Optional<TransactionOptions> options);

  jsg::Value transactionSync(jsg::Lock& js, jsg::Function<jsg::Value()> callback);

  jsg::Promise<void> deleteAll(jsg::Lock& js, jsg::Optional<PutOptions> options);

  jsg::Promise<void> sync(jsg::Lock& js);

  jsg::Ref<SqlStorage> getSql(jsg::Lock& js);

  kj::Promise<kj::String> getCurrentBookmark();
  // Get a bookmark for the current state of the database. Note that since this is async, the
  // bookmark will include any writes in the current atomic batch, including writes that are
  // performed after this call begins. It could also include concurrent writes that haven't happned
  // yet, unless blockConcurrencyWhile() is used to prevent them.

  kj::Promise<kj::String> getBookmarkForTime(kj::Date timestamp);
  // Get a bookmark representing approximately the given timestamp, which is a time up to 30 days
  // in the past (or whatever the backup retention period is).

  kj::Promise<kj::String> onNextSessionRestoreBookmark(kj::String bookmark);
  // Arrange that the next time the Durable Object restarts, the database will be restored to
  // the state represented by the given bookmark. This returns a bookmark string which represents
  // the state immediately before the restoration takes place, and thus can be used to undo the
  // restore. (This bookmark technically refers to a *future* state -- it specifies the state the
  // object will have at the end of the current session.)
  //
  // It is up to the caller to force a restart in order to complete the restoration, for instance
  // by calling state.abort() or by throwing from a blockConcurrencyWhile() callback.

  JSG_RESOURCE_TYPE(DurableObjectStorage, CompatibilityFlags::Reader flags) {
    JSG_METHOD(get);
    JSG_METHOD(list);
    JSG_METHOD(put);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(deleteAll);
    JSG_METHOD(transaction);
    JSG_METHOD(getAlarm);
    JSG_METHOD(setAlarm);
    JSG_METHOD(deleteAlarm);
    JSG_METHOD(sync);

    if (flags.getWorkerdExperimental()) {
      JSG_LAZY_INSTANCE_PROPERTY(sql, getSql);
      JSG_METHOD(transactionSync);

      JSG_METHOD(getCurrentBookmark);
      JSG_METHOD(getBookmarkForTime);
      JSG_METHOD(onNextSessionRestoreBookmark);
    }

    JSG_TS_OVERRIDE({
      get<T = unknown>(key: string, options?: DurableObjectGetOptions): Promise<T | undefined>;
      get<T = unknown>(keys: string[], options?: DurableObjectGetOptions): Promise<Map<string, T>>;

      list<T = unknown>(options?: DurableObjectListOptions): Promise<Map<string, T>>;

      put<T>(key: string, value: T, options?: DurableObjectPutOptions): Promise<void>;
      put<T>(entries: Record<string, T>, options?: DurableObjectPutOptions): Promise<void>;

      delete(key: string, options?: DurableObjectPutOptions): Promise<boolean>;
      delete(keys: string[], options?: DurableObjectPutOptions): Promise<number>;

      transaction<T>(closure: (txn: DurableObjectTransaction) => Promise<T>): Promise<T>;
      transactionSync<T>(closure: () => T): T;
    });
  }

protected:
  ActorCacheOps& getCache(kj::StringPtr op) override;

  bool useDirectIo() override {
    return false;
  }

private:
  IoPtr<ActorCacheInterface> cache;
  uint transactionSyncDepth = 0;
};

class DurableObjectTransaction final: public jsg::Object, public DurableObjectStorageOperations {
public:
  DurableObjectTransaction(IoOwn<ActorCacheInterface::Transaction> cacheTxn)
    : cacheTxn(kj::mv(cacheTxn)) {}

  kj::Promise<void> maybeCommit();
  void maybeRollback();
  // Called from C++, not JS, after the transaction callback has completed (successfully or not).
  // These methods do nothing if the transaction is already committed / rolled back.

  void rollback();  // called from JS

  void deleteAll();
  // Just throws an exception saying this isn't supported.

  JSG_RESOURCE_TYPE(DurableObjectTransaction) {
    JSG_METHOD(get);
    JSG_METHOD(list);
    JSG_METHOD(put);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(deleteAll);
    JSG_METHOD(rollback);
    JSG_METHOD(getAlarm);
    JSG_METHOD(setAlarm);
    JSG_METHOD(deleteAlarm);

    JSG_TS_OVERRIDE({
      get<T = unknown>(key: string, options?: DurableObjectGetOptions): Promise<T | undefined>;
      get<T = unknown>(keys: string[], options?: DurableObjectGetOptions): Promise<Map<string, T>>;

      list<T = unknown>(options?: DurableObjectListOptions): Promise<Map<string, T>>;

      put<T>(key: string, value: T, options?: DurableObjectPutOptions): Promise<void>;
      put<T>(entries: Record<string, T>, options?: DurableObjectPutOptions): Promise<void>;

      delete(key: string, options?: DurableObjectPutOptions): Promise<boolean>;
      delete(keys: string[], options?: DurableObjectPutOptions): Promise<number>;

      deleteAll: never;
    });
  }

protected:
  ActorCacheOps& getCache(kj::StringPtr op) override;

  bool useDirectIo() override {
    return false;
  }

private:
  kj::Maybe<IoOwn<ActorCacheInterface::Transaction>> cacheTxn;
  // Becomes null when committed or rolled back.

  bool rolledBack = false;

  friend DurableObjectStorage;
};

class ActorState: public jsg::Object {
  // The type placed in event.actorState (pre-modules API).
  // NOTE: It hasn't been renamed under the assumption that it will only be
  // used for colo-local namespaces.
  // TODO(cleanup): Remove getPersistent method that isn't supported for colo-local actors anymore.

public:
  ActorState(Worker::Actor::Id actorId, kj::Maybe<jsg::Value> transient,
      kj::Maybe<jsg::Ref<DurableObjectStorage>> persistent);

  kj::OneOf<jsg::Ref<DurableObjectId>, kj::StringPtr> getId();

  jsg::Optional<v8::Local<v8::Value>> getTransient(v8::Isolate* isolate) {
    return transient.map([&](jsg::Value& v) { return v.getHandle(isolate); });
  }

  jsg::Optional<jsg::Ref<DurableObjectStorage>> getPersistent() {
    return persistent.map([&](jsg::Ref<DurableObjectStorage>& p) { return p.addRef(); });
  }

  JSG_RESOURCE_TYPE(ActorState) {
    JSG_READONLY_INSTANCE_PROPERTY(id, getId);
    JSG_READONLY_INSTANCE_PROPERTY(transient, getTransient);
    JSG_READONLY_INSTANCE_PROPERTY(persistent, getPersistent);

    JSG_TS_OVERRIDE(type ActorState = never);
  }

private:
  Worker::Actor::Id id;
  kj::Maybe<jsg::Value> transient;
  kj::Maybe<jsg::Ref<DurableObjectStorage>> persistent;
};

class WebSocketRequestResponsePair: public jsg::Object {
public:
  WebSocketRequestResponsePair(kj::String request, kj::String response)
      : request(kj::mv(request)), response(kj::mv(response)) {};

  static jsg::Ref<WebSocketRequestResponsePair> constructor(kj::String request, kj::String response) {
    return jsg::alloc<WebSocketRequestResponsePair>(kj::mv(request),kj::mv(response));
  };

  kj::StringPtr getRequest() { return request.asPtr(); }
  kj::StringPtr getResponse() { return response.asPtr(); }

  JSG_RESOURCE_TYPE(WebSocketRequestResponsePair) {
    JSG_READONLY_PROTOTYPE_PROPERTY(request, getRequest);
    JSG_READONLY_PROTOTYPE_PROPERTY(response, getResponse);
  }

private:
  kj::String request;
  kj::String response;
};

class DurableObjectState: public jsg::Object {
  // The type passed as the first parameter to durable object class's constructor.

public:
  DurableObjectState(Worker::Actor::Id actorId, kj::Maybe<jsg::Ref<DurableObjectStorage>> storage);

  void waitUntil(kj::Promise<void> promise);

  kj::OneOf<jsg::Ref<DurableObjectId>, kj::StringPtr> getId();

  jsg::Optional<jsg::Ref<DurableObjectStorage>> getStorage() {
    return storage.map([&](jsg::Ref<DurableObjectStorage>& p) { return p.addRef(); });
  }

  jsg::Promise<jsg::Value> blockConcurrencyWhile(jsg::Lock& js,
      jsg::Function<jsg::Promise<jsg::Value>()> callback);

  void abort(jsg::Optional<kj::String> reason);
  // Reset the object, including breaking the output gate and canceling any writes that haven't
  // been committed yet.

  void acceptWebSocket(jsg::Ref<WebSocket> ws, jsg::Optional<kj::Array<kj::String>> tags);
  // Adds a WebSocket to the set attached to this object.
  // `ws.accept()` must NOT have been called separately.
  // Once called, any incoming messages will be delivered
  // by calling the Durable Object's webSocketMessage()
  // handler, and webSocketClose() will be invoked upon
  // disconnect.
  //
  // After calling this, the WebSocket is accepted, so
  // its send() and close() methods can be used to send
  // messages. It should be noted that calling addEventListener()
  // on the websocket does nothing, since inbound events will
  // automatically be delivered to one of the webSocketMessage()/
  // webSocketClose()/webSocketError() handlers. No inbound events
  // to a WebSocket accepted via acceptWebSocket() will ever be
  // delivered to addEventListener(), so there is no reason to call it.
  //
  // `tags` are string tags which can be used to look up
  // the WebSocket with getWebSockets().

  kj::Array<jsg::Ref<api::WebSocket>> getWebSockets(jsg::Lock& js, jsg::Optional<kj::String> tag);
  // Gets an array of accepted WebSockets matching the given tag.
  // If no tag is provided, an array of all accepted WebSockets is returned.
  // Disconnected WebSockets are automatically removed from the list.

  void setWebSocketAutoResponse(jsg::Optional<jsg::Ref<api::WebSocketRequestResponsePair>> maybeReqResp);
  // Sets an object-wide websocket auto response message for a specific
  // request string. All websockets belonging to the same object must
  // reply to the request with the matching response, then store the timestamp at which
  // the request was received.
  // If maybeReqResp is not set, we consider it as unset and remove any set request response pair.

  kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> getWebSocketAutoResponse();
  // Gets the currently set object-wide websocket auto response.

  kj::Maybe<kj::Date> getWebSocketAutoResponseTimestamp(jsg::Ref<WebSocket> ws);
  // Get the last auto response timestamp or null

  JSG_RESOURCE_TYPE(DurableObjectState, CompatibilityFlags::Reader flags) {
    JSG_METHOD(waitUntil);
    JSG_READONLY_INSTANCE_PROPERTY(id, getId);
    JSG_READONLY_INSTANCE_PROPERTY(storage, getStorage);
    JSG_METHOD(blockConcurrencyWhile);
    JSG_METHOD(acceptWebSocket);
    JSG_METHOD(getWebSockets);
    JSG_METHOD(setWebSocketAutoResponse);
    JSG_METHOD(getWebSocketAutoResponse);
    JSG_METHOD(getWebSocketAutoResponseTimestamp);

    if (flags.getWorkerdExperimental()) {
      // TODO(someday): This currently exists for testing purposes only but maybe it could be
      //   useful to apps in actual production? It's a convenient way to bail out when you discover
      //   your state is inconsistent.
      JSG_METHOD(abort);
    }

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE({
      readonly id: DurableObjectId;
      readonly storage: DurableObjectStorage;
      blockConcurrencyWhile<T>(callback: () => Promise<T>): Promise<T>;
    });
    // Make `storage` non-optional
  }

private:
  Worker::Actor::Id id;
  kj::Maybe<jsg::Ref<DurableObjectStorage>> storage;
  const size_t MAX_TAGS_PER_CONNECTION = 10;
  const size_t MAX_TAG_LENGTH = 256;
  // Limits for Hibernatable WebSocket tags.
};

#define EW_ACTOR_STATE_ISOLATE_TYPES                     \
  api::ActorState,                                       \
  api::DurableObjectState,                               \
  api::DurableObjectTransaction,                         \
  api::DurableObjectStorage,                             \
  api::DurableObjectStorage::TransactionOptions,         \
  api::DurableObjectStorageOperations::ListOptions,      \
  api::DurableObjectStorageOperations::GetOptions,       \
  api::DurableObjectStorageOperations::GetAlarmOptions,  \
  api::DurableObjectStorageOperations::PutOptions,       \
  api::DurableObjectStorageOperations::SetAlarmOptions,  \
  api::WebSocketRequestResponsePair

}  // namespace workerd::api
