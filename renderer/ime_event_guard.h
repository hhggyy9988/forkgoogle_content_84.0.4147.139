// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_RENDERER_IME_EVENT_GUARD_H_
#define CONTENT_RENDERER_IME_EVENT_GUARD_H_

#include "base/memory/weak_ptr.h"

namespace content {
class RenderWidget;

// Simple RAII object for guarding IME updates. Calls OnImeGuardStart on
// construction and OnImeGuardFinish on destruction.
class ImeEventGuard {
 public:
  // The ImeEventGuard is intended to be allocated on the stack. A WeakPtr is
  // used because the associated RenderWidget may be destroyed while this
  // variable is on the stack. (i.e. inside a nested event loop).
  explicit ImeEventGuard(base::WeakPtr<RenderWidget> widget);
  ~ImeEventGuard();

  bool show_virtual_keyboard() const { return show_virtual_keyboard_; }
  void set_show_virtual_keyboard(bool show) { show_virtual_keyboard_ = show; }

 private:
  base::WeakPtr<RenderWidget> widget_;
  bool show_virtual_keyboard_ = false;
};
}

#endif  // CONTENT_RENDERER_IME_EVENT_GUARD_H_
