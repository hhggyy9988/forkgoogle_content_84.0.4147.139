// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/tracing/cros_tracing_agent.h"

#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/check_op.h"
#include "base/memory/ref_counted_memory.h"
#include "base/no_destructor.h"
#include "base/sequence_checker.h"
#include "base/task/post_task.h"
#include "base/task/thread_pool.h"
#include "base/trace_event/trace_config.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/debug_daemon/debug_daemon_client.h"
#include "content/public/browser/browser_task_traits.h"
#include "services/tracing/public/cpp/perfetto/perfetto_traced_process.h"
#include "services/tracing/public/cpp/perfetto/system_trace_writer.h"
#include "services/tracing/public/mojom/constants.mojom.h"
#include "services/tracing/public/mojom/perfetto_service.mojom.h"

namespace content {

class CrOSSystemTracingSession {
 public:
  using SuccessCallback = base::OnceCallback<void(bool)>;
  using TraceDataCallback = base::OnceCallback<void(
      const scoped_refptr<base::RefCountedString>& events)>;

  CrOSSystemTracingSession() = default;

  // Begin tracing if configured in |config|. Calls |success_callback| with
  // |true| if tracing was started and |false| otherwise.
  void StartTracing(const std::string& config, SuccessCallback callback) {
    DCHECK(!is_tracing_);
    if (!chromeos::DBusThreadManager::IsInitialized()) {
      if (callback)
        std::move(callback).Run(/*success=*/false);
      return;
    }

    base::trace_event::TraceConfig trace_config(config);
    debug_daemon_ = chromeos::DBusThreadManager::Get()->GetDebugDaemonClient();
    if (!trace_config.IsSystraceEnabled() || !debug_daemon_) {
      if (callback)
        std::move(callback).Run(/*success=*/false);
      return;
    }
    debug_daemon_->SetStopAgentTracingTaskRunner(
        base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(),
             base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN}));
    debug_daemon_->StartAgentTracing(
        trace_config,
        base::BindOnce(&CrOSSystemTracingSession::StartTracingCallbackProxy,
                       base::Unretained(this), std::move(callback)));
  }

  void StopTracing(TraceDataCallback callback) {
    if (!is_tracing_) {
      std::move(callback).Run(nullptr);
      return;
    }
    DCHECK(debug_daemon_);
    is_tracing_ = false;
    debug_daemon_->StopAgentTracing(
        base::BindOnce(&CrOSSystemTracingSession::OnTraceData,
                       base::Unretained(this), std::move(callback)));
  }

 private:
  void StartTracingCallbackProxy(SuccessCallback success_callback,
                                 const std::string& agent_name,
                                 bool success) {
    is_tracing_ = success;
    if (success_callback)
      std::move(success_callback).Run(success);
  }

  void OnTraceData(TraceDataCallback callback,
                   const std::string& event_name,
                   const std::string& events_label,
                   const scoped_refptr<base::RefCountedString>& events) {
    std::move(callback).Run(events);
  }

  bool is_tracing_ = false;
  chromeos::DebugDaemonClient* debug_daemon_ = nullptr;
};

namespace {

class CrOSDataSource : public tracing::PerfettoTracedProcess::DataSourceBase {
 public:
  static CrOSDataSource* GetInstance() {
    static base::NoDestructor<CrOSDataSource> instance;
    return instance.get();
  }

  // Called from the tracing::PerfettoProducer on its sequence.
  void StartTracing(
      tracing::PerfettoProducer* perfetto_producer,
      const perfetto::DataSourceConfig& data_source_config) override {
    base::PostTask(FROM_HERE, {BrowserThread::UI},
                   base::BindOnce(&CrOSDataSource::StartTracingOnUI,
                                  base::Unretained(this), perfetto_producer,
                                  data_source_config));
  }

  // Called from the tracing::PerfettoProducer on its sequence.
  void StopTracing(base::OnceClosure stop_complete_callback) override {
    base::PostTask(
        FROM_HERE, {BrowserThread::UI},
        base::BindOnce(&CrOSDataSource::StopTracingOnUI, base::Unretained(this),
                       std::move(stop_complete_callback)));
  }

  void Flush(base::RepeatingClosure flush_complete_callback) override {
    // CrOS's DebugDaemon doesn't support flushing while recording.
    flush_complete_callback.Run();
  }

 private:
  friend class base::NoDestructor<CrOSDataSource>;

  CrOSDataSource()
      : DataSourceBase(tracing::mojom::kSystemTraceDataSourceName) {
    DETACH_FROM_SEQUENCE(ui_sequence_checker_);
  }

  void StartTracingOnUI(tracing::PerfettoProducer* producer,
                        const perfetto::DataSourceConfig& data_source_config) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
    DCHECK(!session_);
    target_buffer_ = data_source_config.target_buffer();
    session_ = std::make_unique<CrOSSystemTracingSession>();
    session_->StartTracing(
        data_source_config.chrome_config().trace_config(),
        base::BindOnce(&CrOSDataSource::SystemTracerStartedOnUI,
                       base::Unretained(this)));
  }

  void SystemTracerStartedOnUI(bool success) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
    session_started_ = true;
    if (on_session_started_callback_)
      std::move(on_session_started_callback_).Run();
  }

  void StopTracingOnUI(base::OnceClosure stop_complete_callback) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
    DCHECK(producer_);
    DCHECK(session_);
    if (!session_started_) {
      on_session_started_callback_ =
          base::BindOnce(&CrOSDataSource::StopTracing, base::Unretained(this),
                         std::move(stop_complete_callback));
      return;
    }

    session_->StopTracing(base::BindOnce(&CrOSDataSource::OnTraceData,
                                         base::Unretained(this),
                                         std::move(stop_complete_callback)));
  }

  // Called on any thread.
  void OnTraceData(base::OnceClosure stop_complete_callback,
                   const scoped_refptr<base::RefCountedString>& events) {
    if (!events || events->data().empty()) {
      OnTraceDataCommitted(std::move(stop_complete_callback));
      return;
    }

    trace_writer_ = std::make_unique<
        tracing::SystemTraceWriter<scoped_refptr<base::RefCountedString>>>(
        producer_, target_buffer_,
        tracing::SystemTraceWriter<
            scoped_refptr<base::RefCountedString>>::TraceType::kFTrace);
    trace_writer_->WriteData(events);
    trace_writer_->Flush(base::BindOnce(&CrOSDataSource::OnTraceDataCommitted,
                                        base::Unretained(this),
                                        std::move(stop_complete_callback)));
  }

  void OnTraceDataCommitted(base::OnceClosure stop_complete_callback) {
    trace_writer_.reset();

    // Destruction and reset of fields should happen on the UI thread.
    base::PostTask(
        FROM_HERE, {BrowserThread::UI},
        base::BindOnce(&CrOSDataSource::OnTraceDataOnUI, base::Unretained(this),
                       std::move(stop_complete_callback)));
  }

  void OnTraceDataOnUI(base::OnceClosure stop_complete_callback) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
    session_.reset();
    session_started_ = false;
    producer_ = nullptr;

    tracing::PerfettoTracedProcess::Get()
        ->GetTaskRunner()
        ->GetOrCreateTaskRunner()
        ->PostTask(FROM_HERE, std::move(stop_complete_callback));
  }

  SEQUENCE_CHECKER(ui_sequence_checker_);
  std::unique_ptr<CrOSSystemTracingSession> session_;
  bool session_started_ = false;
  base::OnceClosure on_session_started_callback_;
  uint32_t target_buffer_ = 0;
  std::unique_ptr<
      tracing::SystemTraceWriter<scoped_refptr<base::RefCountedString>>>
      trace_writer_;

  DISALLOW_COPY_AND_ASSIGN(CrOSDataSource);
};

}  // namespace

CrOSTracingAgent::CrOSTracingAgent() {
  tracing::PerfettoTracedProcess::Get()->AddDataSource(
      CrOSDataSource::GetInstance());
}

CrOSTracingAgent::~CrOSTracingAgent() = default;

}  // namespace content
