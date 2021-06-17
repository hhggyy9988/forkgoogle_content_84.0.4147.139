// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_MEDIA_SESSION_MOCK_MEDIA_SESSION_PLAYER_OBSERVER_H_
#define CONTENT_BROWSER_MEDIA_SESSION_MOCK_MEDIA_SESSION_PLAYER_OBSERVER_H_

#include <stddef.h>
#include <vector>

#include "base/time/time.h"
#include "content/browser/media/session/media_session_player_observer.h"
#include "services/media_session/public/cpp/media_position.h"

namespace content {

// MockMediaSessionPlayerObserver is a mock implementation of
// MediaSessionPlayerObserver to be used in tests.
class MockMediaSessionPlayerObserver : public MediaSessionPlayerObserver {
 public:
  explicit MockMediaSessionPlayerObserver(
      RenderFrameHost* render_frame_host = nullptr);
  ~MockMediaSessionPlayerObserver() override;

  // Implements MediaSessionPlayerObserver.
  void OnSuspend(int player_id) override;
  void OnResume(int player_id) override;
  void OnSeekForward(int player_id, base::TimeDelta seek_time) override;
  void OnSeekBackward(int player_id, base::TimeDelta seek_time) override;
  void OnSetVolumeMultiplier(int player_id, double volume_multiplier) override;
  void OnEnterPictureInPicture(int player_id) override;
  void OnExitPictureInPicture(int player_id) override;
  base::Optional<media_session::MediaPosition> GetPosition(
      int player_id) const override;
  bool IsPictureInPictureAvailable(int player_id) const override;
  RenderFrameHost* render_frame_host() const override;
  bool HasVideo(int player_id) const override;

  // Simulate that a new player started.
  // Returns the player_id.
  int StartNewPlayer();

  // Returns whether |player_id| is playing.
  bool IsPlaying(size_t player_id);

  // Returns the volume multiplier of |player_id|.
  double GetVolumeMultiplier(size_t player_id);

  // Simulate a play state change for |player_id|.
  void SetPlaying(size_t player_id, bool playing);

  // Set the position for |player_id|.
  void SetPosition(size_t player_id, media_session::MediaPosition& position);

  int received_suspend_calls() const;
  int received_resume_calls() const;
  int received_seek_forward_calls() const;
  int received_seek_backward_calls() const;
  int received_enter_picture_in_picture_calls() const;
  int received_exit_picture_in_picture_calls() const;

 private:
  // Internal representation of the players to keep track of their statuses.
  struct MockPlayer {
   public:
    MockPlayer(bool is_playing = true, double volume_multiplier = 1.0f);
    ~MockPlayer();
    MockPlayer(const MockPlayer&);

    bool is_playing_;
    double volume_multiplier_;
    base::Optional<media_session::MediaPosition> position_;
    bool is_in_picture_in_picture_;
  };

  // Basic representation of the players. The position in the vector is the
  // player_id. The value of the vector is the playing status and volume.
  std::vector<MockPlayer> players_;

  RenderFrameHost* render_frame_host_;

  int received_resume_calls_ = 0;
  int received_suspend_calls_ = 0;
  int received_seek_forward_calls_ = 0;
  int received_seek_backward_calls_ = 0;
  int received_enter_picture_in_picture_calls_ = 0;
  int received_exit_picture_in_picture_calls_ = 0;
};

}  // namespace content

#endif  // CONTENT_BROWSER_MEDIA_SESSION_MOCK_MEDIA_SESSION_PLAYER_OBSERVER_H_
