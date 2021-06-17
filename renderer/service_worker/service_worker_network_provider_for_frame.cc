// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/service_worker/service_worker_network_provider_for_frame.h"

#include <utility>

#include "base/memory/ptr_util.h"
#include "content/common/service_worker/service_worker_utils.h"
#include "content/public/common/origin_util.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/renderer/loader/web_url_loader_impl.h"
#include "content/renderer/render_frame_impl.h"
#include "content/renderer/render_thread_impl.h"
#include "content/renderer/service_worker/service_worker_provider_context.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "third_party/blink/public/web/web_local_frame.h"

namespace content {

class ServiceWorkerNetworkProviderForFrame::NewDocumentObserver
    : public RenderFrameObserver {
 public:
  NewDocumentObserver(ServiceWorkerNetworkProviderForFrame* owner,
                      RenderFrameImpl* frame)
      : RenderFrameObserver(frame), owner_(owner) {}

  void DidCreateNewDocument() override {
    blink::WebLocalFrame* web_frame = render_frame()->GetWebFrame();
    blink::WebDocumentLoader* web_loader =
        render_frame()->GetWebFrame()->GetDocumentLoader();
    DCHECK_EQ(owner_, web_loader->GetServiceWorkerNetworkProvider());

    if (web_frame->GetSecurityOrigin().IsOpaque()) {
      // At navigation commit we thought the document was eligible to use
      // service workers so created the network provider, but it turns out it is
      // not eligible because it is CSP sandboxed.
      web_loader->SetServiceWorkerNetworkProvider(
          ServiceWorkerNetworkProviderForFrame::CreateInvalidInstance());
      // |this| and its owner are destroyed.
      return;
    }

    owner_->NotifyExecutionReady();
  }

  void ReportFeatureUsage(blink::mojom::WebFeature feature) {
    render_frame()->GetWebFrame()->BlinkFeatureUsageReport(feature);
  }

  void OnDestruct() override {
    // Deletes |this|.
    owner_->observer_.reset();
  }

 private:
  ServiceWorkerNetworkProviderForFrame* owner_;
};

// static
std::unique_ptr<ServiceWorkerNetworkProviderForFrame>
ServiceWorkerNetworkProviderForFrame::Create(
    RenderFrameImpl* frame,
    blink::mojom::ServiceWorkerProviderInfoForClientPtr provider_info,
    blink::mojom::ControllerServiceWorkerInfoPtr controller_info,
    scoped_refptr<network::SharedURLLoaderFactory> fallback_loader_factory) {
  DCHECK(provider_info);

  auto provider =
      base::WrapUnique(new ServiceWorkerNetworkProviderForFrame(frame));
  provider->context_ = base::MakeRefCounted<ServiceWorkerProviderContext>(
      blink::mojom::ServiceWorkerContainerType::kForWindow,
      std::move(provider_info->client_receiver),
      std::move(provider_info->host_remote), std::move(controller_info),
      std::move(fallback_loader_factory));

  return provider;
}

// static
std::unique_ptr<ServiceWorkerNetworkProviderForFrame>
ServiceWorkerNetworkProviderForFrame::CreateInvalidInstance() {
  return base::WrapUnique(new ServiceWorkerNetworkProviderForFrame(nullptr));
}

ServiceWorkerNetworkProviderForFrame::ServiceWorkerNetworkProviderForFrame(
    RenderFrameImpl* frame) {
  if (frame)
    observer_ = std::make_unique<NewDocumentObserver>(this, frame);
}

ServiceWorkerNetworkProviderForFrame::~ServiceWorkerNetworkProviderForFrame() {
  if (context())
    context()->OnNetworkProviderDestroyed();
}

void ServiceWorkerNetworkProviderForFrame::WillSendRequest(
    blink::WebURLRequest& request) {
  // Inject this frame's fetch window id into the request.
  if (context())
    request.SetFetchWindowId(context()->fetch_request_window_id());
}

std::unique_ptr<blink::WebURLLoader>
ServiceWorkerNetworkProviderForFrame::CreateURLLoader(
    const blink::WebURLRequest& request,
    std::unique_ptr<blink::scheduler::WebResourceLoadingTaskRunnerHandle>
        task_runner_handle) {
  // RenderThreadImpl is nullptr in some tests.
  if (!RenderThreadImpl::current())
    return nullptr;

  // We need SubresourceLoaderFactory populated in order to create our own
  // URLLoader for subresource loading.
  if (!context() || !context()->GetSubresourceLoaderFactory())
    return nullptr;

  // If the URL is not http(s) or otherwise whitelisted, do not intercept the
  // request. Schemes like 'blob' and 'file' are not eligible to be intercepted
  // by service workers.
  // TODO(falken): Let ServiceWorkerSubresourceLoaderFactory handle the request
  // and move this check there (i.e., for such URLs, it should use its fallback
  // factory).
  const GURL gurl(request.Url());
  if (!gurl.SchemeIsHTTPOrHTTPS() && !OriginCanAccessServiceWorkers(gurl))
    return nullptr;

  // If GetSkipServiceWorker() returns true, do not intercept the request.
  if (request.GetSkipServiceWorker())
    return nullptr;

  // Record use counter for intercepting requests from opaque stylesheets.
  // TODO(crbug.com/898497): Remove this feature usage once we have enough data.
  if (observer_ && request.IsFromOriginDirtyStyleSheet()) {
    observer_->ReportFeatureUsage(
        blink::mojom::WebFeature::
            kServiceWorkerInterceptedRequestFromOriginDirtyStyleSheet);
  }

  mojo::PendingRemote<mojom::KeepAliveHandle> keep_alive_handle;
  if (request.GetKeepalive()) {
    // This cast is safe because NewDocumentObserver is always created with a
    // RenderFrameImpl.
    auto* render_frame_impl =
        static_cast<RenderFrameImpl*>(observer_->render_frame());
    render_frame_impl->GetFrameHost()->IssueKeepAliveHandle(
        keep_alive_handle.InitWithNewPipeAndPassReceiver());
  }

  // Create our own SubresourceLoader to route the request to the controller
  // ServiceWorker.
  return std::make_unique<WebURLLoaderImpl>(
      RenderThreadImpl::current()->resource_dispatcher(),
      std::move(task_runner_handle), context()->GetSubresourceLoaderFactory(),
      std::move(keep_alive_handle));
}

blink::mojom::ControllerServiceWorkerMode
ServiceWorkerNetworkProviderForFrame::GetControllerServiceWorkerMode() {
  if (!context())
    return blink::mojom::ControllerServiceWorkerMode::kNoController;
  return context()->GetControllerServiceWorkerMode();
}

int64_t ServiceWorkerNetworkProviderForFrame::ControllerServiceWorkerID() {
  if (!context())
    return blink::mojom::kInvalidServiceWorkerVersionId;
  return context()->GetControllerVersionId();
}

void ServiceWorkerNetworkProviderForFrame::DispatchNetworkQuiet() {
  if (!context())
    return;
  context()->DispatchNetworkQuiet();
}

mojo::ScopedMessagePipeHandle
ServiceWorkerNetworkProviderForFrame::TakePendingWorkerTimingReceiver(
    int request_id) {
  if (!context())
    return {};
  auto worker_timing_receiver =
      context()->TakePendingWorkerTimingReceiver(request_id);
  return worker_timing_receiver.PassPipe();
}

void ServiceWorkerNetworkProviderForFrame::NotifyExecutionReady() {
  if (context())
    context()->NotifyExecutionReady();
}

}  // namespace content
