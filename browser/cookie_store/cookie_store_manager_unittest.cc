// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include "base/bind.h"
#include "base/files/scoped_temp_dir.h"
#include "base/macros.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/bind_test_util.h"
#include "base/test/scoped_feature_list.h"
#include "content/browser/cookie_store/cookie_store_context.h"
#include "content/browser/cookie_store/cookie_store_manager.h"
#include "content/browser/service_worker/embedded_worker_test_helper.h"
#include "content/browser/service_worker/fake_embedded_worker_instance_client.h"
#include "content/browser/service_worker/fake_service_worker.h"
#include "content/browser/service_worker/service_worker_context_wrapper.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "mojo/public/cpp/test_support/test_utils.h"
#include "net/base/features.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_util.h"
#include "services/network/public/cpp/features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/service_worker/service_worker.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_event_status.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_registration.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {

namespace {

// Synchronous proxies to a wrapped CookieStore service's methods.
class CookieStoreSync {
 public:
  using Subscriptions = std::vector<blink::mojom::CookieChangeSubscriptionPtr>;

  // The caller must ensure that the CookieStore service outlives this.
  explicit CookieStoreSync(blink::mojom::CookieStore* cookie_store_service)
      : cookie_store_service_(cookie_store_service) {}
  ~CookieStoreSync() = default;

  bool AddSubscriptions(int64_t service_worker_registration_id,
                        Subscriptions subscriptions) {
    bool success;
    base::RunLoop run_loop;
    cookie_store_service_->AddSubscriptions(
        service_worker_registration_id, std::move(subscriptions),
        base::BindLambdaForTesting([&](bool service_success) {
          success = service_success;
          run_loop.Quit();
        }));
    run_loop.Run();
    return success;
  }

  bool RemoveSubscriptions(int64_t service_worker_registration_id,
                           Subscriptions subscriptions) {
    bool success;
    base::RunLoop run_loop;
    cookie_store_service_->RemoveSubscriptions(
        service_worker_registration_id, std::move(subscriptions),
        base::BindLambdaForTesting([&](bool service_success) {
          success = service_success;
          run_loop.Quit();
        }));
    run_loop.Run();
    return success;
  }

  base::Optional<Subscriptions> GetSubscriptions(
      int64_t service_worker_registration_id) {
    base::Optional<Subscriptions> result;
    base::RunLoop run_loop;
    cookie_store_service_->GetSubscriptions(
        service_worker_registration_id,
        base::BindLambdaForTesting(
            [&](Subscriptions service_result, bool service_success) {
              if (service_success)
                result = std::move(service_result);
              run_loop.Quit();
            }));
    run_loop.Run();
    return result;
  }

 private:
  blink::mojom::CookieStore* cookie_store_service_;

  DISALLOW_COPY_AND_ASSIGN(CookieStoreSync);
};

const char kExampleScope[] = "https://example.com/a";
const char kExampleWorkerScript[] = "https://example.com/a/script.js";
const char kGoogleScope[] = "https://google.com/a";
const char kGoogleWorkerScript[] = "https://google.com/a/script.js";
const char kLegacyScope[] = "https://legacy.com/a";
const char kLegacyWorkerScript[] = "https://legacy.com/a/script.js";

// Mocks a service worker that uses the cookieStore API.
class CookieStoreWorkerTestHelper : public EmbeddedWorkerTestHelper {
 public:
  using EmbeddedWorkerTestHelper::EmbeddedWorkerTestHelper;

  explicit CookieStoreWorkerTestHelper(
      const base::FilePath& user_data_directory)
      : EmbeddedWorkerTestHelper(user_data_directory) {}
  ~CookieStoreWorkerTestHelper() override = default;

  class ServiceWorker : public FakeServiceWorker {
   public:
    explicit ServiceWorker(CookieStoreWorkerTestHelper* worker_helper)
        : FakeServiceWorker(worker_helper), worker_helper_(worker_helper) {}
    ~ServiceWorker() override = default;

    // Used to implement WaitForActivateEvent().
    void DispatchActivateEvent(
        DispatchActivateEventCallback callback) override {
      if (worker_helper_->quit_on_activate_) {
        worker_helper_->quit_on_activate_->Quit();
        worker_helper_->quit_on_activate_ = nullptr;
      }

      FakeServiceWorker::DispatchActivateEvent(std::move(callback));
    }

    void DispatchCookieChangeEvent(
        const net::CookieChangeInfo& change,
        DispatchCookieChangeEventCallback callback) override {
      worker_helper_->changes_.emplace_back(change);
      std::move(callback).Run(
          blink::mojom::ServiceWorkerEventStatus::COMPLETED);
    }

   private:
    CookieStoreWorkerTestHelper* const worker_helper_;

    DISALLOW_COPY_AND_ASSIGN(ServiceWorker);
  };

  std::unique_ptr<FakeServiceWorker> CreateServiceWorker() override {
    return std::make_unique<ServiceWorker>(this);
  }

  // Spins inside a run loop until a service worker activate event is received.
  void WaitForActivateEvent() {
    base::RunLoop run_loop;
    quit_on_activate_ = &run_loop;
    run_loop.Run();
  }

  // The data in the CookieChangeEvents received by the worker.
  std::vector<net::CookieChangeInfo>& changes() { return changes_; }

 private:
  // Set by WaitForActivateEvent(), used in OnActivateEvent().
  base::RunLoop* quit_on_activate_ = nullptr;

  // Collects the changes reported to OnCookieChangeEvent().
  std::vector<net::CookieChangeInfo> changes_;
};

}  // namespace

// This class cannot be in an anonymous namespace because it needs to be a
// friend of StoragePartitionImpl, to access its constructor.
class CookieStoreManagerTest
    : public testing::Test,
      public testing::WithParamInterface<bool /* reset_context */> {
 public:
  CookieStoreManagerTest()
      : task_environment_(BrowserTaskEnvironment::IO_MAINLOOP) {
    // Enable SameSiteByDefaultCookies because the default CookieAccessSemantics
    // setting is based on the state of this feature, and we want a consistent
    // expected value in the tests for domains without a custom setting.
    feature_list_.InitAndEnableFeature(
        net::features::kSameSiteByDefaultCookies);
  }

  void SetUp() override {
    // Use an on-disk service worker storage to test saving and loading.
    ASSERT_TRUE(user_data_directory_.CreateUniqueTempDir());

    SetUpServiceWorkerContext();
  }

  void TearDown() override { TearDownServiceWorkerContext(); }

  void ResetServiceWorkerContext() {
    TearDownServiceWorkerContext();
    SetUpServiceWorkerContext();
  }

  // Returns the new service worker's registration id.
  //
  // Spins in a nested RunLoop until the new service worker is activated. The
  // new service worker is guaranteed to be running when the method returns.
  int64_t RegisterServiceWorker(const char* scope, const char* script_url) {
    bool success = false;
    int64_t registration_id;
    blink::mojom::ServiceWorkerRegistrationOptions options;
    options.scope = GURL(scope);
    base::RunLoop run_loop;
    worker_test_helper_->context()->RegisterServiceWorker(
        GURL(script_url), options,
        blink::mojom::FetchClientSettingsObject::New(),
        base::BindLambdaForTesting([&](blink::ServiceWorkerStatusCode status,
                                       const std::string& status_message,
                                       int64_t service_worker_registration_id) {
          success = (status == blink::ServiceWorkerStatusCode::kOk);
          registration_id = service_worker_registration_id;
          EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status)
              << blink::ServiceWorkerStatusToString(status);
          run_loop.Quit();
        }));
    run_loop.Run();
    if (!success)
      return kInvalidRegistrationId;

    worker_test_helper_->WaitForActivateEvent();
    return registration_id;
  }

  // The given service worker will be running after the method returns.
  //
  // RegisterServiceWorker() also guarantees that the newly created SW is
  // running. EnsureServiceWorkerStarted() is only necessary when calling APIs
  // that require a live registration after ResetServiceWorkerContext().
  //
  // Returns true on success. Spins in a nested RunLoop until the service worker
  // is started.
  bool EnsureServiceWorkerStarted(int64_t registration_id) {
    bool success = false;
    scoped_refptr<ServiceWorkerRegistration> registration;

    {
      base::RunLoop run_loop;
      worker_test_helper_->context_wrapper()->FindReadyRegistrationForIdOnly(
          registration_id,
          base::BindLambdaForTesting(
              [&](blink::ServiceWorkerStatusCode status,
                  scoped_refptr<ServiceWorkerRegistration> found_registration) {
                success = (status == blink::ServiceWorkerStatusCode::kOk);
                registration = std::move(found_registration);
                EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status)
                    << blink::ServiceWorkerStatusToString(status);
                run_loop.Quit();
              }));
      run_loop.Run();
    }
    if (!success)
      return false;

    scoped_refptr<ServiceWorkerVersion> active_version =
        registration->active_version();
    EXPECT_TRUE(active_version);
    if (!active_version)
      return false;
    if (active_version->running_status() == EmbeddedWorkerStatus::RUNNING)
      return true;
    {
      base::RunLoop run_loop;
      active_version->RunAfterStartWorker(
          ServiceWorkerMetrics::EventType::COOKIE_CHANGE,
          base::BindLambdaForTesting(
              [&](blink::ServiceWorkerStatusCode status) {
                success = (status == blink::ServiceWorkerStatusCode::kOk);
                EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status)
                    << blink::ServiceWorkerStatusToString(status);
                run_loop.Quit();
              }));
      run_loop.Run();
    }
    return success;
  }

  // Synchronous helper for CookieManager::SetCanonicalCookie().
  bool SetCanonicalCookie(const net::CanonicalCookie& cookie) {
    base::RunLoop run_loop;
    bool success = false;
    cookie_manager_->SetCanonicalCookie(
        cookie, net::cookie_util::SimulatedCookieSource(cookie, "https"),
        net::CookieOptions::MakeAllInclusive(),
        base::BindLambdaForTesting(
            [&](net::CanonicalCookie::CookieInclusionStatus service_status) {
              success = service_status.IsInclude();
              run_loop.Quit();
            }));
    run_loop.Run();
    return success;
  }

  // Simplified helper for SetCanonicalCookie.
  //
  // Creates a CanonicalCookie that is not secure, not http-only,
  // and not restricted to first parties. Returns false if creation fails.
  bool SetSessionCookie(const char* name,
                        const char* value,
                        const char* domain,
                        const char* path) {
    return SetCanonicalCookie(net::CanonicalCookie(
        name, value, domain, path, base::Time(), base::Time(), base::Time(),
        /* secure = */ true,
        /* httponly = */ false, net::CookieSameSite::NO_RESTRICTION,
        net::COOKIE_PRIORITY_DEFAULT));
  }

  bool DeleteCookie(const char* name, const char* domain, const char* path) {
    return SetCanonicalCookie(net::CanonicalCookie(
        name, /* value = */ "", domain, path, /* creation = */ base::Time(),
        /* expiration = */ base::Time::Min(), /* last_access = */ base::Time(),
        /* secure = */ true, /* httponly = */ false,
        net::CookieSameSite::NO_RESTRICTION, net::COOKIE_PRIORITY_DEFAULT));
  }

  // Designates a closure for preparing the cookie store for the current test.
  //
  // The closure will be run immediately. If the service worker context is
  // reset, the closure will be run again after the new CookieManager is set up.
  void SetCookieStoreInitializer(base::RepeatingClosure initializer) {
    DCHECK(!cookie_store_initializer_) << __func__ << " already called";
    DCHECK(initializer);
    cookie_store_initializer_ = std::move(initializer);
    cookie_store_initializer_.Run();
  }

  bool reset_context_during_test() const { return GetParam(); }

  static constexpr const int64_t kInvalidRegistrationId = -1;

 protected:
  void SetUpServiceWorkerContext() {
    worker_test_helper_ = std::make_unique<CookieStoreWorkerTestHelper>(
        user_data_directory_.GetPath());

    cookie_store_context_ = base::MakeRefCounted<CookieStoreContext>();
    cookie_store_context_->Initialize(worker_test_helper_->context_wrapper(),
                                      base::BindOnce([](bool success) {
                                        CHECK(success) << "Initialize failed";
                                      }));
    storage_partition_impl_ = StoragePartitionImpl::Create(
        worker_test_helper_->browser_context(), true /* in_memory */,
        base::FilePath() /* relative_partition_path */,
        std::string() /* partition_domain */);
    storage_partition_impl_->Initialize();
    ::network::mojom::NetworkContext* network_context =
        storage_partition_impl_->GetNetworkContext();
    network_context->GetCookieManager(
        cookie_manager_.BindNewPipeAndPassReceiver());
    if (cookie_store_initializer_)
      cookie_store_initializer_.Run();
    cookie_store_context_->ListenToCookieChanges(
        network_context, base::BindOnce([](bool success) {
          CHECK(success) << "ListenToCookieChanges failed";
        }));

    cookie_store_context_->CreateServiceForTesting(
        url::Origin::Create(GURL(kExampleScope)),
        example_service_remote_.BindNewPipeAndPassReceiver());
    example_service_ =
        std::make_unique<CookieStoreSync>(example_service_remote_.get());

    cookie_store_context_->CreateServiceForTesting(
        url::Origin::Create(GURL(kGoogleScope)),
        google_service_remote_.BindNewPipeAndPassReceiver());
    google_service_ =
        std::make_unique<CookieStoreSync>(google_service_remote_.get());

    cookie_store_context_->CreateServiceForTesting(
        url::Origin::Create(GURL(kLegacyScope)),
        legacy_service_remote_.BindNewPipeAndPassReceiver());
    legacy_service_ =
        std::make_unique<CookieStoreSync>(legacy_service_remote_.get());

    // Set Legacy cookie access setting for legacy.com to test
    // CookieAccessSemantics.
    std::vector<ContentSettingPatternSource> legacy_settings;
    legacy_settings.emplace_back(
        ContentSettingsPattern::FromString("[*.]legacy.com"),
        ContentSettingsPattern::FromString("*"),
        base::Value(ContentSetting::CONTENT_SETTING_ALLOW), std::string(),
        false /* incognito */);
    cookie_manager_->SetContentSettingsForLegacyCookieAccess(
        std::move(legacy_settings));
    cookie_manager_.FlushForTesting();
  }

  void TearDownServiceWorkerContext() {
    // Let the service worker context cleanly shut down, so its storage can be
    // safely opened again if the test will continue.
    worker_test_helper_->ShutdownContext();
    task_environment_.RunUntilIdle();

    // Smart pointers are reset manually in destruction order because this is
    // called by ResetServiceWorkerContext().
    example_service_.reset();
    google_service_.reset();
    legacy_service_.reset();
    example_service_remote_.reset();
    google_service_remote_.reset();
    legacy_service_remote_.reset();
    cookie_manager_.reset();
    cookie_store_context_.reset();
    storage_partition_impl_.reset();
    worker_test_helper_.reset();
  }

  BrowserTaskEnvironment task_environment_;
  base::test::ScopedFeatureList feature_list_;
  base::ScopedTempDir user_data_directory_;
  std::unique_ptr<CookieStoreWorkerTestHelper> worker_test_helper_;
  std::unique_ptr<StoragePartitionImpl> storage_partition_impl_;
  scoped_refptr<CookieStoreContext> cookie_store_context_;
  mojo::Remote<::network::mojom::CookieManager> cookie_manager_;
  base::RepeatingClosure cookie_store_initializer_;

  mojo::Remote<blink::mojom::CookieStore> example_service_remote_,
      google_service_remote_, legacy_service_remote_;
  std::unique_ptr<CookieStoreSync> example_service_, google_service_,
      legacy_service_;
};

const int64_t CookieStoreManagerTest::kInvalidRegistrationId;

namespace {

// Useful for sorting a vector of cookie change subscriptions.
bool CookieChangeSubscriptionLessThan(
    const blink::mojom::CookieChangeSubscriptionPtr& lhs,
    const blink::mojom::CookieChangeSubscriptionPtr& rhs) {
  return std::tie(lhs->name, lhs->match_type, lhs->url) <
         std::tie(rhs->name, rhs->match_type, rhs->url);
}

TEST_P(CookieStoreManagerTest, NoSubscriptions) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  EXPECT_EQ(0u, all_subscriptions_opt.value().size());
}

TEST_P(CookieStoreManagerTest, AddSubscriptions_EmptyInput) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  EXPECT_EQ(0u, all_subscriptions_opt.value().size());
}

TEST_P(CookieStoreManagerTest, AddSubscriptions_OneSubscription) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie_name_prefix";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);

  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions all_subscriptions =
      std::move(all_subscriptions_opt).value();
  ASSERT_EQ(1u, all_subscriptions.size());
  EXPECT_EQ("cookie_name_prefix", all_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            all_subscriptions[0]->match_type);
  EXPECT_EQ(GURL(kExampleScope), all_subscriptions[0]->url);
}

TEST_P(CookieStoreManagerTest, AddSubscriptions_WrongScopeOrigin) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kGoogleScope);
  mojo::test::BadMessageObserver bad_mesage_observer;
  EXPECT_FALSE(example_service_->AddSubscriptions(registration_id,
                                                  std::move(subscriptions)));
  EXPECT_EQ("Invalid subscription URL",
            bad_mesage_observer.WaitForBadMessage());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  EXPECT_EQ(0u, all_subscriptions_opt.value().size());

  ASSERT_TRUE(
      SetSessionCookie("cookie-name", "cookie-value", "google.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(0u, worker_test_helper_->changes().size());
}

TEST_P(CookieStoreManagerTest, AddSubscriptions_NonexistentRegistrationId) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie_name_prefix";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);

  EXPECT_FALSE(example_service_->AddSubscriptions(registration_id + 100,
                                                  std::move(subscriptions)));

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  EXPECT_EQ(0u, all_subscriptions_opt.value().size());
}

TEST_P(CookieStoreManagerTest, AddSubscriptions_WrongRegistrationOrigin) {
  int64_t example_registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(example_registration_id, kInvalidRegistrationId);

  int64_t google_registration_id =
      RegisterServiceWorker(kGoogleScope, kGoogleWorkerScript);
  ASSERT_NE(google_registration_id, kInvalidRegistrationId);
  EXPECT_NE(example_registration_id, google_registration_id);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kGoogleScope);
  mojo::test::BadMessageObserver bad_mesage_observer;
  EXPECT_FALSE(example_service_->AddSubscriptions(google_registration_id,
                                                  std::move(subscriptions)));
  EXPECT_EQ("Invalid service worker", bad_mesage_observer.WaitForBadMessage());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      google_service_->GetSubscriptions(google_registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  EXPECT_EQ(0u, all_subscriptions_opt.value().size());

  ASSERT_TRUE(
      SetSessionCookie("cookie-name", "cookie-value", "google.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(0u, worker_test_helper_->changes().size());
}

TEST_P(CookieStoreManagerTest, AddSubscriptionsMultipleWorkers) {
  int64_t example_registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(example_registration_id, kInvalidRegistrationId);
  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "cookie_name_prefix";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL(kExampleScope);

    EXPECT_TRUE(example_service_->AddSubscriptions(example_registration_id,
                                                   std::move(subscriptions)));
  }

  int64_t google_registration_id =
      RegisterServiceWorker(kGoogleScope, kGoogleWorkerScript);
  ASSERT_NE(google_registration_id, kInvalidRegistrationId);
  EXPECT_NE(example_registration_id, google_registration_id);
  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "cookie_name";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::EQUALS;
    subscriptions.back()->url = GURL(kGoogleScope);

    EXPECT_TRUE(google_service_->AddSubscriptions(google_registration_id,
                                                  std::move(subscriptions)));
  }

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> example_subscriptions_opt =
      example_service_->GetSubscriptions(example_registration_id);
  ASSERT_TRUE(example_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions example_subscriptions =
      std::move(example_subscriptions_opt).value();
  ASSERT_EQ(1u, example_subscriptions.size());
  EXPECT_EQ("cookie_name_prefix", example_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            example_subscriptions[0]->match_type);
  EXPECT_EQ(GURL(kExampleScope), example_subscriptions[0]->url);

  base::Optional<CookieStoreSync::Subscriptions> google_subscriptions_opt =
      google_service_->GetSubscriptions(google_registration_id);
  ASSERT_TRUE(google_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions google_subscriptions =
      std::move(google_subscriptions_opt).value();
  ASSERT_EQ(1u, google_subscriptions.size());
  EXPECT_EQ("cookie_name", google_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::EQUALS,
            google_subscriptions[0]->match_type);
  EXPECT_EQ(GURL(kGoogleScope), google_subscriptions[0]->url);
}

TEST_P(CookieStoreManagerTest, AddSubscriptions_MultipleSubscriptions) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);
  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name1";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/1");
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name2";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::EQUALS;
    subscriptions.back()->url = GURL("https://example.com/a/2");
    EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                   std::move(subscriptions)));
  }
  {
    CookieStoreSync::Subscriptions subscriptions;
    EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                   std::move(subscriptions)));
  }
  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name3";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/3");
    EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                   std::move(subscriptions)));
  }
  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions all_subscriptions =
      std::move(all_subscriptions_opt).value();

  std::sort(all_subscriptions.begin(), all_subscriptions.end(),
            CookieChangeSubscriptionLessThan);

  ASSERT_EQ(3u, all_subscriptions.size());
  EXPECT_EQ("name1", all_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            all_subscriptions[0]->match_type);
  EXPECT_EQ(GURL("https://example.com/a/1"), all_subscriptions[0]->url);
  EXPECT_EQ("name2", all_subscriptions[1]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::EQUALS,
            all_subscriptions[1]->match_type);
  EXPECT_EQ(GURL("https://example.com/a/2"), all_subscriptions[1]->url);
  EXPECT_EQ("name3", all_subscriptions[2]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            all_subscriptions[2]->match_type);
  EXPECT_EQ(GURL("https://example.com/a/3"), all_subscriptions[2]->url);
}

TEST_P(CookieStoreManagerTest, AddSubscriptions_MultipleAddsAcrossRestart) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);
  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name1";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/1");
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name2";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::EQUALS;
    subscriptions.back()->url = GURL("https://example.com/a/2");
    EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                   std::move(subscriptions)));
  }
  {
    CookieStoreSync::Subscriptions subscriptions;
    EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                   std::move(subscriptions)));
  }

  if (reset_context_during_test()) {
    ResetServiceWorkerContext();
    EXPECT_TRUE(EnsureServiceWorkerStarted(registration_id));
  }

  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name3";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/3");
    EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                   std::move(subscriptions)));
  }

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions all_subscriptions =
      std::move(all_subscriptions_opt).value();

  std::sort(all_subscriptions.begin(), all_subscriptions.end(),
            CookieChangeSubscriptionLessThan);

  ASSERT_EQ(3u, all_subscriptions.size());
  EXPECT_EQ("name1", all_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            all_subscriptions[0]->match_type);
  EXPECT_EQ(GURL("https://example.com/a/1"), all_subscriptions[0]->url);
  EXPECT_EQ("name2", all_subscriptions[1]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::EQUALS,
            all_subscriptions[1]->match_type);
  EXPECT_EQ(GURL("https://example.com/a/2"), all_subscriptions[1]->url);
  EXPECT_EQ("name3", all_subscriptions[2]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            all_subscriptions[2]->match_type);
  EXPECT_EQ(GURL("https://example.com/a/3"), all_subscriptions[2]->url);
}

TEST_P(CookieStoreManagerTest, RemoveSubscriptions_EmptyVector) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie_name_prefix";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);

  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));

  subscriptions.clear();
  EXPECT_TRUE(example_service_->RemoveSubscriptions(registration_id,
                                                    std::move(subscriptions)));

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions all_subscriptions =
      std::move(all_subscriptions_opt).value();
  ASSERT_EQ(1u, all_subscriptions.size());
  EXPECT_EQ("cookie_name_prefix", all_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            all_subscriptions[0]->match_type);
  EXPECT_EQ(GURL(kExampleScope), all_subscriptions[0]->url);
}

TEST_P(CookieStoreManagerTest, RemoveSubscriptions_OneExistingSubscription) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie_name_prefix";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);

  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));

  subscriptions.clear();
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie_name_prefix";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);
  EXPECT_TRUE(example_service_->RemoveSubscriptions(registration_id,
                                                    std::move(subscriptions)));

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions all_subscriptions =
      std::move(all_subscriptions_opt).value();
  EXPECT_EQ(0u, all_subscriptions.size());
}

TEST_P(CookieStoreManagerTest, RemoveSubscriptions_OneNonexistingSubscription) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie_name_prefix";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);

  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));

  subscriptions.clear();
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "wrong_cookie_name_prefix";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);
  EXPECT_TRUE(example_service_->RemoveSubscriptions(registration_id,
                                                    std::move(subscriptions)));

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions all_subscriptions =
      std::move(all_subscriptions_opt).value();
  ASSERT_EQ(1u, all_subscriptions.size());
  EXPECT_EQ("cookie_name_prefix", all_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            all_subscriptions[0]->match_type);
  EXPECT_EQ(GURL(kExampleScope), all_subscriptions[0]->url);
}

TEST_P(CookieStoreManagerTest, RemoveSubscriptions_NonexistentRegistrationId) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie_name_prefix";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);

  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));

  subscriptions.clear();
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie_name_prefix";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);
  EXPECT_FALSE(example_service_->RemoveSubscriptions(registration_id + 100,
                                                     std::move(subscriptions)));

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions all_subscriptions =
      std::move(all_subscriptions_opt).value();
  ASSERT_EQ(1u, all_subscriptions.size());
  EXPECT_EQ("cookie_name_prefix", all_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            all_subscriptions[0]->match_type);
  EXPECT_EQ(GURL(kExampleScope), all_subscriptions[0]->url);
}

TEST_P(CookieStoreManagerTest, RemoveSubscriptions_WrongRegistrationOrigin) {
  int64_t example_registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(example_registration_id, kInvalidRegistrationId);
  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "cookie_name_prefix";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL(kExampleScope);

    EXPECT_TRUE(example_service_->AddSubscriptions(example_registration_id,
                                                   std::move(subscriptions)));
  }

  int64_t google_registration_id =
      RegisterServiceWorker(kGoogleScope, kGoogleWorkerScript);
  ASSERT_NE(google_registration_id, kInvalidRegistrationId);
  EXPECT_NE(example_registration_id, google_registration_id);
  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "cookie_name";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::EQUALS;
    subscriptions.back()->url = GURL(kGoogleScope);

    EXPECT_TRUE(google_service_->AddSubscriptions(google_registration_id,
                                                  std::move(subscriptions)));
  }

  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "cookie_name";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::EQUALS;
    subscriptions.back()->url = GURL(kGoogleScope);

    mojo::test::BadMessageObserver bad_mesage_observer;
    EXPECT_FALSE(example_service_->RemoveSubscriptions(
        google_registration_id, std::move(subscriptions)));
    EXPECT_EQ("Invalid service worker",
              bad_mesage_observer.WaitForBadMessage());
  }

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> example_subscriptions_opt =
      example_service_->GetSubscriptions(example_registration_id);
  ASSERT_TRUE(example_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions example_subscriptions =
      std::move(example_subscriptions_opt).value();
  ASSERT_EQ(1u, example_subscriptions.size());
  EXPECT_EQ("cookie_name_prefix", example_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            example_subscriptions[0]->match_type);
  EXPECT_EQ(GURL(kExampleScope), example_subscriptions[0]->url);

  base::Optional<CookieStoreSync::Subscriptions> google_subscriptions_opt =
      google_service_->GetSubscriptions(google_registration_id);
  ASSERT_TRUE(google_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions google_subscriptions =
      std::move(google_subscriptions_opt).value();
  ASSERT_EQ(1u, google_subscriptions.size());
  EXPECT_EQ("cookie_name", google_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::EQUALS,
            google_subscriptions[0]->match_type);
  EXPECT_EQ(GURL(kGoogleScope), google_subscriptions[0]->url);
}

TEST_P(CookieStoreManagerTest, RemoveSubscriptions_MultipleWorkers) {
  int64_t example_registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(example_registration_id, kInvalidRegistrationId);
  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "cookie_name_prefix";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL(kExampleScope);

    EXPECT_TRUE(example_service_->AddSubscriptions(example_registration_id,
                                                   std::move(subscriptions)));
  }

  int64_t google_registration_id =
      RegisterServiceWorker(kGoogleScope, kGoogleWorkerScript);
  ASSERT_NE(google_registration_id, kInvalidRegistrationId);
  EXPECT_NE(example_registration_id, google_registration_id);
  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "cookie_name";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::EQUALS;
    subscriptions.back()->url = GURL(kGoogleScope);

    EXPECT_TRUE(google_service_->AddSubscriptions(google_registration_id,
                                                  std::move(subscriptions)));
  }

  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "cookie_name_prefix";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL(kExampleScope);

    EXPECT_TRUE(example_service_->RemoveSubscriptions(
        example_registration_id, std::move(subscriptions)));
  }

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> example_subscriptions_opt =
      example_service_->GetSubscriptions(example_registration_id);
  ASSERT_TRUE(example_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions example_subscriptions =
      std::move(example_subscriptions_opt).value();
  EXPECT_EQ(0u, example_subscriptions.size());

  base::Optional<CookieStoreSync::Subscriptions> google_subscriptions_opt =
      google_service_->GetSubscriptions(google_registration_id);
  ASSERT_TRUE(google_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions google_subscriptions =
      std::move(google_subscriptions_opt).value();
  ASSERT_EQ(1u, google_subscriptions.size());
  EXPECT_EQ("cookie_name", google_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::EQUALS,
            google_subscriptions[0]->match_type);
  EXPECT_EQ(GURL(kGoogleScope), google_subscriptions[0]->url);
}

TEST_P(CookieStoreManagerTest, RemoveSubscriptions_MultipleSubscriptionsLeft) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);
  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name1";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/1");
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name2";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::EQUALS;
    subscriptions.back()->url = GURL("https://example.com/a/2");
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name3";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/3");
    EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                   std::move(subscriptions)));
  }

  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "wrong_name3";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/3");
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "wrong_name1";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/1");
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name2";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::EQUALS;
    subscriptions.back()->url = GURL("https://example.com/a/2");
    EXPECT_TRUE(example_service_->RemoveSubscriptions(
        registration_id, std::move(subscriptions)));
  }

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions all_subscriptions =
      std::move(all_subscriptions_opt).value();

  std::sort(all_subscriptions.begin(), all_subscriptions.end(),
            CookieChangeSubscriptionLessThan);

  ASSERT_EQ(2u, all_subscriptions.size());
  EXPECT_EQ("name1", all_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            all_subscriptions[0]->match_type);
  EXPECT_EQ(GURL("https://example.com/a/1"), all_subscriptions[0]->url);
  EXPECT_EQ("name3", all_subscriptions[1]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            all_subscriptions[1]->match_type);
  EXPECT_EQ(GURL("https://example.com/a/3"), all_subscriptions[1]->url);
}

TEST_P(CookieStoreManagerTest, RemoveSubscriptions_OneSubscriptionLeft) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);
  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name1";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/1");
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name2";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::EQUALS;
    subscriptions.back()->url = GURL("https://example.com/a/2");
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name3";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/3");
    EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                   std::move(subscriptions)));
  }

  {
    CookieStoreSync::Subscriptions subscriptions;
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name3";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/3");
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "wrong_name1";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::STARTS_WITH;
    subscriptions.back()->url = GURL("https://example.com/a/1");
    subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
    subscriptions.back()->name = "name2";
    subscriptions.back()->match_type =
        ::network::mojom::CookieMatchType::EQUALS;
    subscriptions.back()->url = GURL("https://example.com/a/2");
    EXPECT_TRUE(example_service_->RemoveSubscriptions(
        registration_id, std::move(subscriptions)));
  }

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  CookieStoreSync::Subscriptions all_subscriptions =
      std::move(all_subscriptions_opt).value();

  ASSERT_EQ(1u, all_subscriptions.size());
  EXPECT_EQ("name1", all_subscriptions[0]->name);
  EXPECT_EQ(::network::mojom::CookieMatchType::STARTS_WITH,
            all_subscriptions[0]->match_type);
  EXPECT_EQ(GURL("https://example.com/a/1"), all_subscriptions[0]->url);
}

TEST_P(CookieStoreManagerTest, OneCookieChange) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);

  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  ASSERT_EQ(1u, all_subscriptions_opt.value().size());

  ASSERT_TRUE(
      SetSessionCookie("cookie-name", "cookie-value", "example.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("example.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // example.com does not have a custom access semantics setting, so it defaults
  // to NONLEGACY, because the FeatureList has SameSiteByDefaultCookies enabled.
  EXPECT_EQ(net::CookieAccessSemantics::NONLEGACY,
            worker_test_helper_->changes()[0].access_semantics);
}

// Same as above except this tests that the LEGACY access semantics for
// legacy.com cookies is correctly reflected in the change info.
TEST_P(CookieStoreManagerTest, OneCookieChangeLegacy) {
  int64_t registration_id =
      RegisterServiceWorker(kLegacyScope, kLegacyWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kLegacyScope);

  EXPECT_TRUE(legacy_service_->AddSubscriptions(registration_id,
                                                std::move(subscriptions)));
  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      legacy_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  ASSERT_EQ(1u, all_subscriptions_opt.value().size());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  ASSERT_TRUE(
      SetSessionCookie("cookie-name", "cookie-value", "legacy.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("legacy.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // legacy.com has a custom Legacy setting.
  EXPECT_EQ(net::CookieAccessSemantics::LEGACY,
            worker_test_helper_->changes()[0].access_semantics);
}

TEST_P(CookieStoreManagerTest, CookieChangeNameStartsWith) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie-name-2";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);

  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));
  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  ASSERT_EQ(1u, all_subscriptions_opt.value().size());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  ASSERT_TRUE(
      SetSessionCookie("cookie-name-1", "cookie-value-1", "example.com", "/"));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0u, worker_test_helper_->changes().size());

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(
      SetSessionCookie("cookie-name-2", "cookie-value-2", "example.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name-2", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value-2", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("example.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // example.com does not have a custom access semantics setting, so it defaults
  // to NONLEGACY, because the FeatureList has SameSiteByDefaultCookies enabled.
  EXPECT_EQ(net::CookieAccessSemantics::NONLEGACY,
            worker_test_helper_->changes()[0].access_semantics);

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(SetSessionCookie("cookie-name-22", "cookie-value-22",
                               "example.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name-22", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value-22",
            worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("example.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // example.com does not have a custom access semantics setting, so it defaults
  // to NONLEGACY, because the FeatureList has SameSiteByDefaultCookies enabled.
  EXPECT_EQ(net::CookieAccessSemantics::NONLEGACY,
            worker_test_helper_->changes()[0].access_semantics);
}

// Same as above except this tests that the LEGACY access semantics for
// legacy.com cookies is correctly reflected in the change info.
TEST_P(CookieStoreManagerTest, CookieChangeNameStartsWithLegacy) {
  int64_t registration_id =
      RegisterServiceWorker(kLegacyScope, kLegacyWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie-name-2";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kLegacyScope);
  EXPECT_TRUE(legacy_service_->AddSubscriptions(registration_id,
                                                std::move(subscriptions)));

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      legacy_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  ASSERT_EQ(1u, all_subscriptions_opt.value().size());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  ASSERT_TRUE(
      SetSessionCookie("cookie-name-1", "cookie-value-1", "legacy.com", "/"));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0u, worker_test_helper_->changes().size());

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(
      SetSessionCookie("cookie-name-2", "cookie-value-2", "legacy.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name-2", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value-2", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("legacy.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // legacy.com has a custom Legacy setting.
  EXPECT_EQ(net::CookieAccessSemantics::LEGACY,
            worker_test_helper_->changes()[0].access_semantics);

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(
      SetSessionCookie("cookie-name-22", "cookie-value-22", "legacy.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name-22", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value-22",
            worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("legacy.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // legacy.com has a custom Legacy setting.
  EXPECT_EQ(net::CookieAccessSemantics::LEGACY,
            worker_test_helper_->changes()[0].access_semantics);
}

TEST_P(CookieStoreManagerTest, CookieChangeUrl) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);

  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));
  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  ASSERT_EQ(1u, all_subscriptions_opt.value().size());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  ASSERT_TRUE(
      SetSessionCookie("cookie-name-1", "cookie-value-1", "google.com", "/"));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(0u, worker_test_helper_->changes().size());

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(SetSessionCookie("cookie-name-2", "cookie-value-2", "example.com",
                               "/a/subpath"));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0u, worker_test_helper_->changes().size());

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(
      SetSessionCookie("cookie-name-3", "cookie-value-3", "example.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name-3", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value-3", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("example.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // example.com does not have a custom access semantics setting, so it defaults
  // to NONLEGACY, because the FeatureList has SameSiteByDefaultCookies enabled.
  EXPECT_EQ(net::CookieAccessSemantics::NONLEGACY,
            worker_test_helper_->changes()[0].access_semantics);

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(
      SetSessionCookie("cookie-name-4", "cookie-value-4", "example.com", "/a"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name-4", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value-4", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("example.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/a", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // example.com does not have a custom access semantics setting, so it defaults
  // to NONLEGACY, because the FeatureList has SameSiteByDefaultCookies enabled.
  EXPECT_EQ(net::CookieAccessSemantics::NONLEGACY,
            worker_test_helper_->changes()[0].access_semantics);
}

// Same as above except this tests that the LEGACY access semantics for
// legacy.com cookies is correctly reflected in the change info.
TEST_P(CookieStoreManagerTest, CookieChangeUrlLegacy) {
  int64_t registration_id =
      RegisterServiceWorker(kLegacyScope, kLegacyWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kLegacyScope);
  EXPECT_TRUE(legacy_service_->AddSubscriptions(registration_id,
                                                std::move(subscriptions)));

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      legacy_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  ASSERT_EQ(1u, all_subscriptions_opt.value().size());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  ASSERT_TRUE(
      SetSessionCookie("cookie-name-1", "cookie-value-1", "google.com", "/"));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(0u, worker_test_helper_->changes().size());

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(SetSessionCookie("cookie-name-2", "cookie-value-2", "legacy.com",
                               "/a/subpath"));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0u, worker_test_helper_->changes().size());

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(
      SetSessionCookie("cookie-name-3", "cookie-value-3", "legacy.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name-3", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value-3", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("legacy.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // legacy.com has a custom Legacy setting.
  EXPECT_EQ(net::CookieAccessSemantics::LEGACY,
            worker_test_helper_->changes()[0].access_semantics);

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(
      SetSessionCookie("cookie-name-4", "cookie-value-4", "legacy.com", "/a"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name-4", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value-4", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("legacy.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/a", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // legacy.com has a custom Legacy setting.
  EXPECT_EQ(net::CookieAccessSemantics::LEGACY,
            worker_test_helper_->changes()[0].access_semantics);
}

TEST_P(CookieStoreManagerTest, HttpOnlyCookieChange) {
  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);

  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));
  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  ASSERT_EQ(1u, all_subscriptions_opt.value().size());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  ASSERT_TRUE(SetCanonicalCookie(net::CanonicalCookie(
      "cookie-name-1", "cookie-value-1", "example.com", "/", base::Time(),
      base::Time(), base::Time(),
      /* secure = */ true,
      /* httponly = */ true, net::CookieSameSite::NO_RESTRICTION,
      net::COOKIE_PRIORITY_DEFAULT)));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0u, worker_test_helper_->changes().size());

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(SetCanonicalCookie(net::CanonicalCookie(
      "cookie-name-2", "cookie-value-2", "example.com", "/", base::Time(),
      base::Time(), base::Time(),
      /* secure = */ true,
      /* httponly = */ false, net::CookieSameSite::NO_RESTRICTION,
      net::COOKIE_PRIORITY_DEFAULT)));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name-2", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value-2", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("example.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // example.com does not have a custom access semantics setting, so it defaults
  // to NONLEGACY, because the FeatureList has SameSiteByDefaultCookies enabled.
  EXPECT_EQ(net::CookieAccessSemantics::NONLEGACY,
            worker_test_helper_->changes()[0].access_semantics);
}

// Same as above except this tests that the LEGACY access semantics for
// legacy.com cookies is correctly reflected in the change info.
TEST_P(CookieStoreManagerTest, HttpOnlyCookieChangeLegacy) {
  int64_t registration_id =
      RegisterServiceWorker(kLegacyScope, kLegacyWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kLegacyScope);
  EXPECT_TRUE(legacy_service_->AddSubscriptions(registration_id,
                                                std::move(subscriptions)));

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      legacy_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  ASSERT_EQ(1u, all_subscriptions_opt.value().size());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  ASSERT_TRUE(SetCanonicalCookie(net::CanonicalCookie(
      "cookie-name-1", "cookie-value-1", "legacy.com", "/", base::Time(),
      base::Time(), base::Time(),
      /* secure = */ false,
      /* httponly = */ true, net::CookieSameSite::NO_RESTRICTION,
      net::COOKIE_PRIORITY_DEFAULT)));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0u, worker_test_helper_->changes().size());

  worker_test_helper_->changes().clear();
  ASSERT_TRUE(SetCanonicalCookie(net::CanonicalCookie(
      "cookie-name-2", "cookie-value-2", "legacy.com", "/", base::Time(),
      base::Time(), base::Time(),
      /* secure = */ false,
      /* httponly = */ false, net::CookieSameSite::NO_RESTRICTION,
      net::COOKIE_PRIORITY_DEFAULT)));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name-2", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value-2", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("legacy.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
  // legacy.com has a custom Legacy setting.
  EXPECT_EQ(net::CookieAccessSemantics::LEGACY,
            worker_test_helper_->changes()[0].access_semantics);
}

TEST_P(CookieStoreManagerTest, CookieChangeForDeletion) {
  SetCookieStoreInitializer(base::BindLambdaForTesting([&]() {
    EXPECT_TRUE(
        SetSessionCookie("cookie-name", "cookie-value", "example.com", "/"));
  }));

  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);
  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  ASSERT_EQ(1u, all_subscriptions_opt.value().size());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  ASSERT_TRUE(DeleteCookie("cookie-name", "example.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("cookie-value", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("example.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::EXPIRED_OVERWRITE,
            worker_test_helper_->changes()[0].cause);
}

TEST_P(CookieStoreManagerTest, CookieChangeForOverwrite) {
  SetCookieStoreInitializer(base::BindLambdaForTesting([&]() {
    EXPECT_TRUE(
        SetSessionCookie("cookie-name", "cookie-value", "example.com", "/"));
  }));

  int64_t registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);
  EXPECT_TRUE(example_service_->AddSubscriptions(registration_id,
                                                 std::move(subscriptions)));

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  ASSERT_EQ(1u, all_subscriptions_opt.value().size());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  ASSERT_TRUE(SetSessionCookie("cookie-name", "new-value", "example.com", "/"));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1u, worker_test_helper_->changes().size());
  EXPECT_EQ("cookie-name", worker_test_helper_->changes()[0].cookie.Name());
  EXPECT_EQ("new-value", worker_test_helper_->changes()[0].cookie.Value());
  EXPECT_EQ("example.com", worker_test_helper_->changes()[0].cookie.Domain());
  EXPECT_EQ("/", worker_test_helper_->changes()[0].cookie.Path());
  EXPECT_EQ(net::CookieChangeCause::INSERTED,
            worker_test_helper_->changes()[0].cause);
}

TEST_P(CookieStoreManagerTest, GetSubscriptionsFromWrongOrigin) {
  int64_t example_registration_id =
      RegisterServiceWorker(kExampleScope, kExampleWorkerScript);
  ASSERT_NE(example_registration_id, kInvalidRegistrationId);

  CookieStoreSync::Subscriptions subscriptions;
  subscriptions.emplace_back(blink::mojom::CookieChangeSubscription::New());
  subscriptions.back()->name = "cookie_name_prefix";
  subscriptions.back()->match_type =
      ::network::mojom::CookieMatchType::STARTS_WITH;
  subscriptions.back()->url = GURL(kExampleScope);

  EXPECT_TRUE(example_service_->AddSubscriptions(example_registration_id,
                                                 std::move(subscriptions)));

  base::Optional<CookieStoreSync::Subscriptions> all_subscriptions_opt =
      example_service_->GetSubscriptions(example_registration_id);
  ASSERT_TRUE(all_subscriptions_opt.has_value());
  EXPECT_EQ(1u, all_subscriptions_opt.value().size());

  if (reset_context_during_test())
    ResetServiceWorkerContext();

  mojo::test::BadMessageObserver bad_mesage_observer;
  base::Optional<CookieStoreSync::Subscriptions> wrong_subscriptions_opt =
      google_service_->GetSubscriptions(example_registration_id);
  EXPECT_FALSE(wrong_subscriptions_opt.has_value());
  EXPECT_EQ("Invalid service worker", bad_mesage_observer.WaitForBadMessage());
}

INSTANTIATE_TEST_SUITE_P(All,
                         CookieStoreManagerTest,
                         testing::Bool() /* reset_context_during_test */);

}  // namespace

}  // namespace content
