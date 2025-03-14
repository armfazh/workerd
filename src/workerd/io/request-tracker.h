#pragma once

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/refcount.h>

namespace workerd {

class RequestTracker final: public kj::Refcounted {
  // This class is used to track a number of associated requests so that some desired behavior
  // is carried out once all requests have completed. `activeRequests` is incremented each time a
  // new request is created, and then decremented once it completes.

public:
  class Hooks {
  public:
    virtual void active() = 0;
    virtual void inactive() = 0;
  };

  class ActiveRequest {
  public:
    // An object that should be associated with (attached to) a request.
    // On creation, if the parent RequestTracker has 0 active requests, we call the `active()` hook.
    // On destruction, if the RequestTracker has 0 active requests, we call the `inactive()` hook.
    // Otherwise, we just increment/decrement the count on creation/destruction respectively.
    ActiveRequest(kj::Badge<RequestTracker>, RequestTracker& parent);
    ActiveRequest(ActiveRequest&& other) = default;
    KJ_DISALLOW_COPY(ActiveRequest);
    ~ActiveRequest() noexcept(false);

  private:
    kj::Maybe<kj::Own<RequestTracker>> maybeParent;
  };

  RequestTracker(Hooks& hooks);
  ~RequestTracker() noexcept(false);
  KJ_DISALLOW_COPY(RequestTracker);

  ActiveRequest startRequest();
  // Returns a new ActiveRequest, thereby bumping the count of active requests associated with the
  // RequestTracker. The ActiveRequest must be attached to the lifetime of the request such that we
  // destroy the ActiveRequest when the request is finished. On destruction, we decrement the count
  // of active requests associated with the RequestTracker, and if there are no more active requests
  // we call the `inactive()` hook.

  void shutdown() {
    // We want to prevent any hooks from running after this point.
    hooks = nullptr;
  }

  kj::Own<RequestTracker> addRef()  {
    return kj::addRef(*this);
  }

private:
  void requestActive();
  void requestInactive();

  int activeRequests = 0;
  kj::Maybe<Hooks&> hooks;
};

} // namespace workerd
