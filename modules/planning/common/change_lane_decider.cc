/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 **/

#include "modules/planning/common/change_lane_decider.h"

#include <limits>

#include "modules/common/time/time.h"
#include "modules/planning/common/planning_context.h"
#include "modules/planning/common/planning_gflags.h"

namespace apollo {
namespace planning {

using common::SLPoint;
using common::time::Clock;

void ChangeLaneDecider::UpdateStatus(ChangeLaneStatus::Status status_code,
                                     const std::string& path_id) {
  UpdateStatus(Clock::NowInSeconds(), status_code, path_id);
}

void ChangeLaneDecider::UpdateStatus(double timestamp,
                                     ChangeLaneStatus::Status status_code,
                                     const std::string& path_id) {
  auto* change_lane_status = PlanningContext::Instance()
                                 ->mutable_planning_status()
                                 ->mutable_change_lane();
  change_lane_status->set_timestamp(timestamp);
  change_lane_status->set_path_id(path_id);
  change_lane_status->set_status(status_code);
}

void ChangeLaneDecider::PrioritizeChangeLane(
    std::list<ReferenceLineInfo>* reference_line_info) const {
  if (reference_line_info->empty()) {
    AERROR << "Reference line info empty";
    return;
  }
  auto iter = reference_line_info->begin();
  while (iter != reference_line_info->end()) {
    if (iter->IsChangeLanePath()) {
      reference_line_info->splice(reference_line_info->begin(),
                                  *reference_line_info, iter);
      break;
    }
    ++iter;
  }
}

void ChangeLaneDecider::RemoveChangeLane(
    std::list<ReferenceLineInfo>* reference_line_info) const {
  auto iter = reference_line_info->begin();
  while (iter != reference_line_info->end()) {
    if (iter->IsChangeLanePath()) {
      iter = reference_line_info->erase(iter);
    } else {
      ++iter;
    }
  }
}

std::string GetCurrentPathId(
    const std::list<ReferenceLineInfo>& reference_line_info) {
  for (const auto& info : reference_line_info) {
    if (!info.IsChangeLanePath()) {
      return info.Lanes().Id();
    }
  }
  return "";
}

bool ChangeLaneDecider::Apply(
    std::list<ReferenceLineInfo>* reference_line_info) {
  if (reference_line_info->empty()) {
    AERROR << "Reference lines empty";
    return false;
  }

  if (FLAGS_reckless_change_lane) {
    PrioritizeChangeLane(reference_line_info);
    return true;
  }

  auto* prev_status = PlanningContext::Instance()
                          ->mutable_planning_status()
                          ->mutable_change_lane();
  double now = Clock::NowInSeconds();

  if (!prev_status->has_status()) {
    UpdateStatus(now, ChangeLaneStatus::CHANGE_LANE_SUCCESS,
                 GetCurrentPathId(*reference_line_info));
    return true;
  }

  bool has_change_lane = reference_line_info->size() > 1;
  if (!has_change_lane) {
    const auto& path_id = reference_line_info->front().Lanes().Id();
    if (prev_status->status() == ChangeLaneStatus::CHANGE_LANE_SUCCESS) {
    } else if (prev_status->status() == ChangeLaneStatus::IN_CHANGE_LANE) {
      UpdateStatus(now, ChangeLaneStatus::CHANGE_LANE_SUCCESS, path_id);
    } else if (prev_status->status() == ChangeLaneStatus::CHANGE_LANE_FAILED) {
    } else {
      AERROR << "Unknown state: " << prev_status->ShortDebugString();
      return false;
    }
    return true;
  } else {  // has change lane in reference lines.
    auto current_path_id = GetCurrentPathId(*reference_line_info);
    if (current_path_id.empty()) {
      AERROR << "The vehicle is not on any reference line";
      return false;
    }
    if (prev_status->status() == ChangeLaneStatus::IN_CHANGE_LANE) {
      if (prev_status->path_id() == current_path_id) {
        PrioritizeChangeLane(reference_line_info);
      } else {
        RemoveChangeLane(reference_line_info);
        UpdateStatus(now, ChangeLaneStatus::CHANGE_LANE_SUCCESS,
                     current_path_id);
      }
      return true;
    } else if (prev_status->status() == ChangeLaneStatus::CHANGE_LANE_FAILED) {
      if (now - prev_status->timestamp() < FLAGS_change_lane_fail_freeze_time) {
        RemoveChangeLane(reference_line_info);
      } else {
        UpdateStatus(now, ChangeLaneStatus::IN_CHANGE_LANE, current_path_id);
      }
      return true;
    } else if (prev_status->status() == ChangeLaneStatus::CHANGE_LANE_SUCCESS) {
      if (now - prev_status->timestamp() <
          FLAGS_change_lane_success_freeze_time) {
        RemoveChangeLane(reference_line_info);
      } else {
        PrioritizeChangeLane(reference_line_info);
        UpdateStatus(now, ChangeLaneStatus::IN_CHANGE_LANE, current_path_id);
      }
    } else {
      AERROR << "Unknown state: " << prev_status->ShortDebugString();
      return false;
    }
  }

  return true;
}

bool ChangeLaneDecider::IsClearToChangeLane(
    ReferenceLineInfo* reference_line_info) {
  double ego_start_s = reference_line_info->AdcSlBoundary().start_s();
  double ego_end_s = reference_line_info->AdcSlBoundary().end_s();
  double ego_v =
      std::abs(reference_line_info->vehicle_state().linear_velocity());

  for (auto* obstacle :
       reference_line_info->path_decision()->obstacles().Items()) {
    if (obstacle->IsVirtual() || obstacle->IsStatic()) {
      ADEBUG << "skip one virtual or static obstacle";
      continue;
    }

    double start_s = std::numeric_limits<double>::max();
    double end_s = -std::numeric_limits<double>::max();
    double start_l = std::numeric_limits<double>::max();
    double end_l = -std::numeric_limits<double>::max();

    for (const auto& p : obstacle->PerceptionPolygon().points()) {
      SLPoint sl_point;
      reference_line_info->reference_line().XYToSL({p.x(), p.y()}, &sl_point);
      start_s = std::fmin(start_s, sl_point.s());
      end_s = std::fmax(end_s, sl_point.s());

      start_l = std::fmin(start_l, sl_point.l());
      end_l = std::fmax(end_l, sl_point.l());
    }

    if (reference_line_info->IsChangeLanePath()) {
      constexpr double kLateralShift = 2.5;
      if (end_l < -kLateralShift || start_l > kLateralShift) {
        continue;
      }
    }

    // Raw estimation on whether same direction with ADC or not based on
    // prediction trajectory
    bool same_direction = true;
    if (obstacle->HasTrajectory()) {
      double obstacle_moving_direction =
          obstacle->Trajectory().trajectory_point(0).path_point().theta();
      const auto& vehicle_state = reference_line_info->vehicle_state();
      double vehicle_moving_direction = vehicle_state.heading();
      if (vehicle_state.gear() == canbus::Chassis::GEAR_REVERSE) {
        vehicle_moving_direction =
            common::math::NormalizeAngle(vehicle_moving_direction + M_PI);
      }
      double heading_difference = std::abs(common::math::NormalizeAngle(
          obstacle_moving_direction - vehicle_moving_direction));
      same_direction = heading_difference < (M_PI / 2.0);
    }

    // TODO(All) move to confs
    constexpr double kSafeTimeOnSameDirection = 3.0;
    constexpr double kSafeTimeOnOppositeDirection = 5.0;
    constexpr double kForwardMinSafeDistanceOnSameDirection = 6.0;
    constexpr double kBackwardMinSafeDistanceOnSameDirection = 8.0;
    constexpr double kForwardMinSafeDistanceOnOppositeDirection = 50.0;
    constexpr double kBackwardMinSafeDistanceOnOppositeDirection = 1.0;
    constexpr double kDistanceBuffer = 0.5;

    double kForwardSafeDistance = 0.0;
    double kBackwardSafeDistance = 0.0;
    if (same_direction) {
      kForwardSafeDistance =
          std::fmax(kForwardMinSafeDistanceOnSameDirection,
                    (ego_v - obstacle->speed()) * kSafeTimeOnSameDirection);
      kBackwardSafeDistance =
          std::fmax(kBackwardMinSafeDistanceOnSameDirection,
                    (obstacle->speed() - ego_v) * kSafeTimeOnSameDirection);
    } else {
      kForwardSafeDistance =
          std::fmax(kForwardMinSafeDistanceOnOppositeDirection,
                    (ego_v + obstacle->speed()) * kSafeTimeOnOppositeDirection);
      kBackwardSafeDistance = kBackwardMinSafeDistanceOnOppositeDirection;
    }

    if (HysteresisFilter(ego_start_s - end_s, kBackwardSafeDistance,
                         kDistanceBuffer, obstacle->IsLaneChangeBlocking()) &&
        HysteresisFilter(start_s - ego_end_s, kForwardSafeDistance,
                         kDistanceBuffer, obstacle->IsLaneChangeBlocking())) {
      reference_line_info->path_decision()
          ->Find(obstacle->Id())
          ->SetLaneChangeBlocking(true);
      ADEBUG << "Lane Change is blocked by obstacle" << obstacle->Id();
      return false;
    } else {
      reference_line_info->path_decision()
          ->Find(obstacle->Id())
          ->SetLaneChangeBlocking(false);
    }
  }
  return true;
}

bool ChangeLaneDecider::HysteresisFilter(const double obstacle_distance,
                                         const double safe_distance,
                                         const double distance_buffer,
                                         const bool is_obstacle_blocking) {
  if (is_obstacle_blocking) {
    return obstacle_distance < safe_distance + distance_buffer;
  } else {
    return obstacle_distance < safe_distance - distance_buffer;
  }
}

}  // namespace planning
}  // namespace apollo
