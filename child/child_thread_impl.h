// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_CHILD_CHILD_THREAD_IMPL_H_
#define CONTENT_CHILD_CHILD_THREAD_IMPL_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>

#include "base/macros.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/metrics/field_trial.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "build/build_config.h"
#include "components/variations/child_process_field_trial_syncer.h"
#include "content/common/associated_interfaces.mojom.h"
#include "content/common/child_process.mojom.h"
#include "content/common/content_export.h"
#include "content/public/child/child_thread.h"
#include "ipc/ipc.mojom.h"
#include "ipc/ipc_buildflags.h"  // For BUILDFLAG(IPC_MESSAGE_LOG_ENABLED).
#include "ipc/ipc_platform_file.h"
#include "ipc/message_router.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/shared_remote.h"
#include "services/service_manager/public/mojom/service.mojom.h"
#include "services/tracing/public/mojom/background_tracing_agent.mojom.h"
#include "third_party/blink/public/mojom/associated_interfaces/associated_interfaces.mojom.h"

#if defined(OS_WIN)
#include "content/public/common/font_cache_win.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"
#endif

namespace IPC {
class MessageFilter;
class SyncChannel;
class SyncMessageFilter;
}  // namespace IPC

namespace mojo {
class OutgoingInvitation;
namespace core {
class ScopedIPCSupport;
}  // namespace core
}  // namespace mojo

namespace tracing {
class BackgroundTracingAgentProviderImpl;
}  // namespace tracing

namespace content {
class InProcessChildThreadParams;
class ThreadSafeSender;

// The main thread of a child process derives from this class.
class CONTENT_EXPORT ChildThreadImpl
    : public IPC::Listener,
      virtual public ChildThread,
      private base::FieldTrialList::Observer,
      public mojom::RouteProvider,
      public blink::mojom::AssociatedInterfaceProvider {
 public:
  struct CONTENT_EXPORT Options;

  // Creates the thread.
  explicit ChildThreadImpl(base::RepeatingClosure quit_closure);
  // Allow to be used for single-process mode and for in process gpu mode via
  // options.
  ChildThreadImpl(base::RepeatingClosure quit_closure, const Options& options);
  // ChildProcess::main_thread() is reset after Shutdown(), and before the
  // destructor, so any subsystem that relies on ChildProcess::main_thread()
  // must be terminated before Shutdown returns. In particular, if a subsystem
  // has a thread that post tasks to ChildProcess::main_thread(), that thread
  // should be joined in Shutdown().
  ~ChildThreadImpl() override;
  virtual void Shutdown();
  // Returns true if the thread should be destroyed.
  virtual bool ShouldBeDestroyed();

  // IPC::Sender implementation:
  bool Send(IPC::Message* msg) override;

  // ChildThread implementation:
#if defined(OS_WIN)
  void PreCacheFont(const LOGFONT& log_font) override;
  void ReleaseCachedFonts() override;
#endif
  void RecordAction(const base::UserMetricsAction& action) override;
  void RecordComputedAction(const std::string& action) override;
  void BindHostReceiver(mojo::GenericPendingReceiver receiver) override;
  scoped_refptr<base::SingleThreadTaskRunner> GetIOTaskRunner() override;
  void SetFieldTrialGroup(const std::string& trial_name,
                          const std::string& group_name) override;

  // base::FieldTrialList::Observer:
  void OnFieldTrialGroupFinalized(const std::string& trial_name,
                                  const std::string& group_name) override;

  IPC::SyncChannel* channel() { return channel_.get(); }

  IPC::MessageRouter* GetRouter();

  mojom::RouteProvider* GetRemoteRouteProvider();

  IPC::SyncMessageFilter* sync_message_filter() const {
    return sync_message_filter_.get();
  }

  // The getter should only be called on the main thread, however the
  // IPC::Sender it returns may be safely called on any thread including
  // the main thread.
  ThreadSafeSender* thread_safe_sender() const {
    return thread_safe_sender_.get();
  }

  scoped_refptr<base::SingleThreadTaskRunner> main_thread_runner() const {
    return main_thread_runner_;
  }

  // Returns the one child thread. Can only be called on the main thread.
  static ChildThreadImpl* current();

  void GetBackgroundTracingAgentProvider(
      mojo::PendingReceiver<tracing::mojom::BackgroundTracingAgentProvider>
          receiver);

  // Returns a reference to the thread-safe SharedRemote<ChildProcessHost>
  // interface endpoint.
  const mojo::SharedRemote<mojom::ChildProcessHost>& child_process_host()
      const {
    return child_process_host_;
  }

  virtual void RunService(
      const std::string& service_name,
      mojo::PendingReceiver<service_manager::mojom::Service> receiver);

  virtual void BindServiceInterface(mojo::GenericPendingReceiver receiver);

  virtual void OnBindReceiver(mojo::GenericPendingReceiver receiver);

 protected:
  friend class ChildProcess;

  // Called when the process refcount is 0.
  virtual void OnProcessFinalRelease();

  // Must be called by subclasses during initialization if and only if they set
  // |Options::expose_interfaces_to_browser| to |true|. This makes |binders|
  // available to handle incoming interface requests from the browser.
  void ExposeInterfacesToBrowser(mojo::BinderMap binders);

  virtual bool OnControlMessageReceived(const IPC::Message& msg);
  // IPC::Listener implementation:
  bool OnMessageReceived(const IPC::Message& msg) override;
  void OnAssociatedInterfaceRequest(
      const std::string& interface_name,
      mojo::ScopedInterfaceEndpointHandle handle) override;
  void OnChannelConnected(int32_t peer_pid) override;
  void OnChannelError() override;
  bool on_channel_error_called() const { return on_channel_error_called_; }

  bool IsInBrowserProcess() const;

 private:
  class IOThreadState;

  class ChildThreadMessageRouter : public IPC::MessageRouter {
   public:
    // |sender| must outlive this object.
    explicit ChildThreadMessageRouter(IPC::Sender* sender);
    bool Send(IPC::Message* msg) override;

    // MessageRouter overrides.
    bool RouteMessage(const IPC::Message& msg) override;

   private:
    IPC::Sender* const sender_;
  };

  void Init(const Options& options);

  // IPC message handlers.

  void EnsureConnected();

  // mojom::RouteProvider:
  void GetRoute(
      int32_t routing_id,
      mojo::PendingAssociatedReceiver<blink::mojom::AssociatedInterfaceProvider>
          receiver) override;

  // blink::mojom::AssociatedInterfaceProvider:
  void GetAssociatedInterface(
      const std::string& name,
      mojo::PendingAssociatedReceiver<blink::mojom::AssociatedInterface>
          receiver) override;

#if defined(OS_WIN)
  const mojo::Remote<mojom::FontCacheWin>& GetFontCacheWin();
#endif

  base::Thread mojo_ipc_thread_{"Mojo IPC"};
  std::unique_ptr<mojo::core::ScopedIPCSupport> mojo_ipc_support_;

  mojo::AssociatedReceiver<mojom::RouteProvider> route_provider_receiver_{this};
  mojo::AssociatedReceiverSet<blink::mojom::AssociatedInterfaceProvider,
                              int32_t>
      associated_interface_provider_receivers_;
  mojo::AssociatedRemote<mojom::RouteProvider> remote_route_provider_;
#if defined(OS_WIN)
  mutable mojo::Remote<mojom::FontCacheWin> font_cache_win_;
#endif

  std::unique_ptr<IPC::SyncChannel> channel_;

  // Allows threads other than the main thread to send sync messages.
  scoped_refptr<IPC::SyncMessageFilter> sync_message_filter_;

  scoped_refptr<ThreadSafeSender> thread_safe_sender_;

  // Implements message routing functionality to the consumers of
  // ChildThreadImpl.
  ChildThreadMessageRouter router_;

  // The OnChannelError() callback was invoked - the channel is dead, don't
  // attempt to communicate.
  bool on_channel_error_called_;

  // TaskRunner to post tasks to the main thread.
  scoped_refptr<base::SingleThreadTaskRunner> main_thread_runner_;

  // Used to quit the main thread.
  base::RepeatingClosure quit_closure_;

  std::unique_ptr<tracing::BackgroundTracingAgentProviderImpl>
      background_tracing_agent_provider_;

  scoped_refptr<base::SingleThreadTaskRunner> browser_process_io_runner_;

  std::unique_ptr<variations::ChildProcessFieldTrialSyncer> field_trial_syncer_;

  std::unique_ptr<base::WeakPtrFactory<ChildThreadImpl>>
      channel_connected_factory_;

  scoped_refptr<base::SingleThreadTaskRunner> ipc_task_runner_;

  // An interface to the browser's process host object.
  mojo::SharedRemote<mojom::ChildProcessHost> child_process_host_;

  // ChlidThreadImpl state which lives on the IO thread, including its
  // implementation of the mojom ChildProcess interface.
  scoped_refptr<IOThreadState> io_thread_state_;

  base::WeakPtrFactory<ChildThreadImpl> weak_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(ChildThreadImpl);
};

struct ChildThreadImpl::Options {
  Options(const Options& other);
  ~Options();

  class Builder;

  bool connect_to_browser;
  scoped_refptr<base::SingleThreadTaskRunner> browser_process_io_runner;
  std::vector<IPC::MessageFilter*> startup_filters;
  mojo::OutgoingInvitation* mojo_invitation;
  scoped_refptr<base::SingleThreadTaskRunner> ipc_task_runner;

  // Indicates that this child process exposes one or more Mojo interfaces to
  // the browser process. Subclasses which initialize this to |true| must
  // explicitly call |ExposeInterfacesToBrowser()| some time during
  // initialization.
  bool exposes_interfaces_to_browser = false;

  using ServiceBinder =
      base::RepeatingCallback<void(mojo::GenericPendingReceiver*)>;
  ServiceBinder service_binder;

 private:
  Options();
};

class ChildThreadImpl::Options::Builder {
 public:
  Builder();

  Builder& InBrowserProcess(const InProcessChildThreadParams& params);
  Builder& ConnectToBrowser(bool connect_to_browser);
  Builder& AddStartupFilter(IPC::MessageFilter* filter);
  Builder& IPCTaskRunner(
      scoped_refptr<base::SingleThreadTaskRunner> ipc_task_runner);
  Builder& ServiceBinder(ServiceBinder binder);
  Builder& ExposesInterfacesToBrowser();

  Options Build();

 private:
  struct Options options_;

  DISALLOW_COPY_AND_ASSIGN(Builder);
};

}  // namespace content

#endif  // CONTENT_CHILD_CHILD_THREAD_IMPL_H_
