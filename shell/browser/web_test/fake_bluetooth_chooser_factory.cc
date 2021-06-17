// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/shell/browser/web_test/fake_bluetooth_chooser_factory.h"

#include "content/shell/browser/web_test/fake_bluetooth_chooser.h"

namespace content {

FakeBluetoothChooserFactory::~FakeBluetoothChooserFactory() {}

void FakeBluetoothChooserFactory::CreateFakeBluetoothChooser(
    mojo::PendingReceiver<mojom::FakeBluetoothChooser> receiver,
    mojo::PendingAssociatedRemote<mojom::FakeBluetoothChooserClient> client) {
  DCHECK(!next_fake_bluetooth_chooser_);
  next_fake_bluetooth_chooser_ = std::make_unique<FakeBluetoothChooser>(
      std::move(receiver), std::move(client));
}

std::unique_ptr<FakeBluetoothChooser>
FakeBluetoothChooserFactory::GetNextFakeBluetoothChooser() {
  return std::move(next_fake_bluetooth_chooser_);
}

FakeBluetoothChooserFactory::FakeBluetoothChooserFactory(
    mojo::PendingReceiver<mojom::FakeBluetoothChooserFactory> receiver)
    : receiver_(this, std::move(receiver)) {}

}  // namespace content
