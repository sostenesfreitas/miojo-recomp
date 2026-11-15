/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Renderer log protocol consumed by host crash/freeze records. Keep these in
   step with checkpoints_report (vk_draw.cpp) and replay_fatal (vk_replay.cpp).
   Only a prefix at the start of a line counts, never a quoted mention. */
#define HOS_REPORT_DEVICE_LOST_PREFIX "vk: device lost:"
#define HOS_REPORT_FATAL_PREFIX "vk: FATAL:"
