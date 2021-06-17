// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

self.addEventListener('install', evt => {
  evt.waitUntil(async function() {
    return Promise.all([
      fetch('./foo/1'),
      fetch('./foo/2'),
      fetch('./foo/3'),
    ]);
  }());
});
