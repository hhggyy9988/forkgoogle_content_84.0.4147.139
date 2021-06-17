// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_renderer_host.h"
#include "content/test/test_render_frame_host.h"
#include "third_party/blink/public/common/feature_policy/feature_policy.h"
#include "third_party/blink/public/common/frame/frame_policy.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {

// Integration tests for feature policy setup and querying through a RFH. These
// tests are not meant to cover every edge case as the FeaturePolicy class
// itself is tested thoroughly in feature_policy_unittest.cc. Instead they are
// meant to ensure that integration with RenderFrameHost works correctly.
class RenderFrameHostFeaturePolicyTest
    : public content::RenderViewHostTestHarness {
 protected:
  static constexpr const char* kOrigin1 = "https://google.com";
  static constexpr const char* kOrigin2 = "https://maps.google.com";
  static constexpr const char* kOrigin3 = "https://example.com";
  static constexpr const char* kOrigin4 = "https://test.com";

  static const blink::mojom::FeaturePolicyFeature kDefaultEnabledFeature =
      blink::mojom::FeaturePolicyFeature::kDocumentWrite;
  static const blink::mojom::FeaturePolicyFeature kDefaultSelfFeature =
      blink::mojom::FeaturePolicyFeature::kGeolocation;

  RenderFrameHost* GetMainRFH(const char* origin) {
    RenderFrameHost* result = web_contents()->GetMainFrame();
    RenderFrameHostTester::For(result)->InitializeRenderFrameIfNeeded();
    SimulateNavigation(&result, GURL(origin));
    return result;
  }

  RenderFrameHost* AddChildRFH(RenderFrameHost* parent, const char* origin) {
    RenderFrameHost* result =
        RenderFrameHostTester::For(parent)->AppendChild("");
    RenderFrameHostTester::For(result)->InitializeRenderFrameIfNeeded();
    SimulateNavigation(&result, GURL(origin));
    return result;
  }

  // The header policy should only be set once on page load, so we refresh the
  // page to simulate that.
  void RefreshPageAndSetHeaderPolicy(RenderFrameHost** rfh,
                                     blink::mojom::FeaturePolicyFeature feature,
                                     const std::vector<std::string>& origins) {
    RenderFrameHost* current = *rfh;
    SimulateNavigation(&current, current->GetLastCommittedURL());
    static_cast<TestRenderFrameHost*>(current)->DidSetFramePolicyHeaders(
        network::mojom::WebSandboxFlags::kNone,
        CreateFPHeader(feature, origins), {} /* document_policy_header */);
    *rfh = current;
  }

  void SetContainerPolicy(RenderFrameHost* parent,
                          RenderFrameHost* child,
                          blink::mojom::FeaturePolicyFeature feature,
                          const std::vector<std::string>& origins) {
    static_cast<TestRenderFrameHost*>(parent)->OnDidChangeFramePolicy(
        child->GetRoutingID(), {network::mojom::WebSandboxFlags::kNone,
                                CreateFPHeader(feature, origins),
                                {} /* required_document_policy */});
  }

  void SimulateNavigation(RenderFrameHost** rfh, const GURL& url) {
    auto navigation_simulator =
        NavigationSimulator::CreateRendererInitiated(url, *rfh);
    navigation_simulator->Commit();
    *rfh = navigation_simulator->GetFinalRenderFrameHost();
  }

 private:
  blink::ParsedFeaturePolicy CreateFPHeader(
      blink::mojom::FeaturePolicyFeature feature,
      const std::vector<std::string>& origins) {
    blink::ParsedFeaturePolicy result(1);
    result[0].feature = feature;
    for (auto const& origin : origins)
      result[0].allowed_origins.push_back(url::Origin::Create(GURL(origin)));
    return result;
  }
};

TEST_F(RenderFrameHostFeaturePolicyTest, DefaultPolicy) {
  RenderFrameHost* parent = GetMainRFH(kOrigin1);
  RenderFrameHost* child = AddChildRFH(parent, kOrigin2);

  EXPECT_TRUE(parent->IsFeatureEnabled(kDefaultEnabledFeature));
  EXPECT_TRUE(parent->IsFeatureEnabled(kDefaultSelfFeature));
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultEnabledFeature));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));
}

TEST_F(RenderFrameHostFeaturePolicyTest, HeaderPolicy) {
  RenderFrameHost* parent = GetMainRFH(kOrigin1);

  // Enable the feature for the child in the parent frame.
  RefreshPageAndSetHeaderPolicy(&parent, kDefaultSelfFeature,
                                {std::string(kOrigin1), std::string(kOrigin2)});

  // Create the child.
  RenderFrameHost* child = AddChildRFH(parent, kOrigin2);

  EXPECT_TRUE(parent->IsFeatureEnabled(kDefaultSelfFeature));
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Set an empty allowlist in the child to test that the policies combine
  // correctly.
  RefreshPageAndSetHeaderPolicy(&child, kDefaultSelfFeature,
                                std::vector<std::string>());

  EXPECT_TRUE(parent->IsFeatureEnabled(kDefaultSelfFeature));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Re-enable the feature in the child.
  RefreshPageAndSetHeaderPolicy(&child, kDefaultSelfFeature,
                                {std::string(kOrigin2)});
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Navigate the child. Check that the feature is disabled.
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));
}

TEST_F(RenderFrameHostFeaturePolicyTest, ContainerPolicy) {
  RenderFrameHost* parent = GetMainRFH(kOrigin1);
  RenderFrameHost* child = AddChildRFH(parent, kOrigin2);

  // Set a container policy on origin 3 to give it the feature. It should not
  // be enabled because container policy will only take effect after navigation.
  SetContainerPolicy(parent, child, kDefaultSelfFeature,
                     {std::string(kOrigin2), std::string(kOrigin3)});
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Navigate the child so that the container policy takes effect.
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Navigate the child again, the feature should not be enabled.
  SimulateNavigation(&child, GURL(kOrigin4));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));
}

TEST_F(RenderFrameHostFeaturePolicyTest, HeaderAndContainerPolicy) {
  RenderFrameHost* parent = GetMainRFH(kOrigin1);

  // Set a header policy and container policy. Check that they both take effect.
  RefreshPageAndSetHeaderPolicy(&parent, kDefaultSelfFeature,
                                {std::string(kOrigin1), std::string(kOrigin2)});

  RenderFrameHost* child = AddChildRFH(parent, kOrigin2);
  SetContainerPolicy(parent, child, kDefaultSelfFeature,
                     {std::string(kOrigin3)});

  // The feature should be enabled in kOrigin2, kOrigin3 but not kOrigin4.
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultSelfFeature));
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultSelfFeature));
  SimulateNavigation(&child, GURL(kOrigin4));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Change the header policy to turn off the feature. It should be disabled in
  // all children.
  RefreshPageAndSetHeaderPolicy(&parent, kDefaultSelfFeature,
                                std::vector<std::string>());
  child = AddChildRFH(parent, kOrigin2);
  SetContainerPolicy(parent, child, kDefaultSelfFeature,
                     {std::string(kOrigin3)});

  SimulateNavigation(&child, GURL(kOrigin2));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));
}

}  // namespace content
