/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#define MS_CLASS "webrtc::ProbeController"
#define MS_LOG_DEV_LEVEL 3

#include "modules/congestion_controller/goog_cc/probe_controller.h"
#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "rtc_base/numerics/safe_conversions.h"

#include "DepLibUV.hpp"
#include "Logger.hpp"

#include <absl/memory/memory.h>
#include <algorithm>
#include <initializer_list>
#include <string>

namespace webrtc {

namespace {
// Maximum waiting time from the time of initiating probing to getting
// the measured results back.
constexpr TimeDelta kMaxWaitingTimeForProbingResult = TimeDelta::Seconds<1>();

// Default probing bitrate limit. Applied only when the application didn't
// specify max bitrate.
constexpr DataRate kDefaultMaxProbingBitrate = DataRate::KilobitsPerSec<5000>();

// If the bitrate drops to a factor |kBitrateDropThreshold| or lower
// and we recover within |kBitrateDropTimeoutMs|, then we'll send
// a probe at a fraction |kProbeFractionAfterDrop| of the original bitrate.
constexpr double kBitrateDropThreshold = 0.66;
constexpr TimeDelta kBitrateDropTimeout = TimeDelta::Seconds<5>();
constexpr double kProbeFractionAfterDrop = 0.85;

// Timeout for probing after leaving ALR. If the bitrate drops significantly,
// (as determined by the delay based estimator) and we leave ALR, then we will
// send a probe if we recover within `kLeftAlrTimeoutMs` ms.
constexpr TimeDelta kAlrEndedTimeout = TimeDelta::Seconds<3>();

// The expected uncertainty of probe result (as a fraction of the target probe
// This is a limit on how often probing can be done when there is a BW
// drop detected in ALR.
constexpr TimeDelta kMinTimeBetweenAlrProbes = TimeDelta::Seconds<5>();

// bitrate). Used to avoid probing if the probe bitrate is close to our current
// estimate.
constexpr double kProbeUncertainty = 0.05;

// Use probing to recover faster after large bitrate estimate drops.
constexpr char kBweRapidRecoveryExperiment[] =
    "WebRTC-BweRapidRecoveryExperiment";

void MaybeLogProbeClusterCreated(const ProbeClusterConfig& probe) {
#if MS_LOG_DEV_LEVEL == 3
  size_t min_bytes = static_cast<int32_t>(probe.target_data_rate.bps() *
                                          probe.target_duration.ms() / 8000);
#endif

  MS_DEBUG_DEV(
    "probe cluster created [id:%d, target data rate(bps):%lld, target probe count:%d, min_bytes:%zu]",
    probe.id, probe.target_data_rate.bps(), probe.target_probe_count, min_bytes);
}

}  // namespace

ProbeControllerConfig::ProbeControllerConfig(
    const WebRtcKeyValueConfig* key_value_config)
    : first_exponential_probe_scale("p1", 3.0),
      second_exponential_probe_scale("p2", 6.0),
      further_exponential_probe_scale("step_size", 2),
      further_probe_threshold("further_probe_threshold", 0.7),
      alr_probing_interval("alr_interval", TimeDelta::seconds(5)),
      alr_probe_scale("alr_scale", 2),
      network_state_estimate_probing_interval("network_state_interval",
                                              TimeDelta::PlusInfinity()),
      probe_if_estimate_lower_than_network_state_estimate_ratio(
          "est_lower_than_network_ratio",
          0),
      estimate_lower_than_network_state_estimate_probing_interval(
          "est_lower_than_network_interval",
          TimeDelta::seconds(3)),
      network_state_probe_scale("network_state_scale", 1.0),
      network_state_probe_duration("network_state_probe_duration",
                                   TimeDelta::Millis<15>()),

      probe_on_max_allocated_bitrate_change("probe_max_allocation", true),
      first_allocation_probe_scale("alloc_p1", 1),
      second_allocation_probe_scale("alloc_p2", 2),
      allocation_allow_further_probing("alloc_probe_further", false),
      allocation_probe_max("alloc_probe_max", DataRate::PlusInfinity()),
      min_probe_packets_sent("min_probe_packets_sent", 5),
      min_probe_duration("min_probe_duration", TimeDelta::Millis<15>()),
      limit_probe_target_rate_to_loss_bwe("limit_probe_target_rate_to_loss_bwe",
                                          false),
      loss_limited_probe_scale("loss_limited_scale", 1.5),
      skip_if_estimate_larger_than_fraction_of_max(
          "skip_if_est_larger_than_fraction_of_max",
          0.0) {
  ParseFieldTrial({&first_exponential_probe_scale,
                   &second_exponential_probe_scale,
                   &further_exponential_probe_scale,
                   &further_probe_threshold,
                   &alr_probing_interval,
                   &alr_probe_scale,
                   &probe_on_max_allocated_bitrate_change,
                   &first_allocation_probe_scale,
                   &second_allocation_probe_scale,
                   &allocation_allow_further_probing,
                   &min_probe_duration,
                   &network_state_estimate_probing_interval,
                   &probe_if_estimate_lower_than_network_state_estimate_ratio,
                   &estimate_lower_than_network_state_estimate_probing_interval,
                   &network_state_probe_scale,
                   &network_state_probe_duration,
                   &min_probe_packets_sent,
                   &limit_probe_target_rate_to_loss_bwe,
                   &loss_limited_probe_scale,
                   &skip_if_estimate_larger_than_fraction_of_max},
                  key_value_config->Lookup("WebRTC-Bwe-ProbingConfiguration"));

  // Specialized keys overriding subsets of WebRTC-Bwe-ProbingConfiguration
  ParseFieldTrial(
      {&first_exponential_probe_scale, &second_exponential_probe_scale},
      key_value_config->Lookup("WebRTC-Bwe-InitialProbing"));
  ParseFieldTrial({&further_exponential_probe_scale, &further_probe_threshold},
                  key_value_config->Lookup("WebRTC-Bwe-ExponentialProbing"));
  ParseFieldTrial(
      {&alr_probing_interval, &alr_probe_scale, &loss_limited_probe_scale},
      key_value_config->Lookup("WebRTC-Bwe-AlrProbing"));
  ParseFieldTrial(
      {&first_allocation_probe_scale, &second_allocation_probe_scale,
       &allocation_allow_further_probing, &allocation_probe_max},
      key_value_config->Lookup("WebRTC-Bwe-AllocationProbing"));
  ParseFieldTrial({&min_probe_packets_sent, &min_probe_duration},
                  key_value_config->Lookup("WebRTC-Bwe-ProbingBehavior"));
}

ProbeControllerConfig::ProbeControllerConfig(const ProbeControllerConfig&) =
    default;
ProbeControllerConfig::~ProbeControllerConfig() = default;

ProbeController::ProbeController(const WebRtcKeyValueConfig* key_value_config)
    : enable_periodic_alr_probing_(false),
      in_rapid_recovery_experiment_(
	    	key_value_config->Lookup(kBweRapidRecoveryExperiment).find("Enabled") == 0
	    ),
      //event_log_(event_log),
      config_(ProbeControllerConfig(key_value_config)) {
  Reset(Timestamp::Zero());
}

ProbeController::~ProbeController() {}

std::vector<ProbeClusterConfig> ProbeController::SetBitrates(
    DataRate min_bitrate,
    DataRate start_bitrate,
    DataRate max_bitrate,
    Timestamp at_time) {
  if (start_bitrate > DataRate::Zero()) {
    start_bitrate_ = start_bitrate;
    estimated_bitrate_ = start_bitrate;
  } else if (start_bitrate_.IsZero()) {
    start_bitrate_ = min_bitrate;
  }
/*	MS_DEBUG_DEV(
		"[old_max_bitrate_bps:%lld, max_bitrate_bps:%lld]",
		max_bitrate_.bps(),
		max_bitrate.bps());*/
  // The reason we use the variable `old_max_bitrate_pbs` is because we
  // need to set `max_bitrate_` before we call InitiateProbing.
  DataRate old_max_bitrate = max_bitrate_;
  max_bitrate_ =
      max_bitrate.IsFinite() ? max_bitrate : kDefaultMaxProbingBitrate;

  switch (state_) {
    case State::kInit:
      if (network_available_)
        return InitiateExponentialProbing(at_time);
      break;

    case State::kWaitingForProbingResult:
      break;

    case State::kProbingComplete:
      // If the new max bitrate is higher than both the old max bitrate and the
      // estimate then initiate probing.
      if (!estimated_bitrate_.IsZero() && old_max_bitrate < max_bitrate_ &&
          estimated_bitrate_ < max_bitrate_) {
        // The assumption is that if we jump more than 20% in the bandwidth
        // estimate or if the bandwidth estimate is within 90% of the new
        // max bitrate then the probing attempt was successful.
        mid_call_probing_succcess_threshold_ =
            std::min(estimated_bitrate_ * 1.2, max_bitrate_ * 0.9);
        mid_call_probing_waiting_for_result_ = true;
        mid_call_probing_bitrate_ = max_bitrate_;

        // RTC_HISTOGRAM_COUNTS_10000("WebRTC.BWE.MidCallProbing.Initiated",
                                   // max_bitrate_bps_ / 1000);

        return InitiateProbing(at_time, {max_bitrate_}, false);
      }
      break;
  }
  return std::vector<ProbeClusterConfig>();
}

std::vector<ProbeClusterConfig> ProbeController::OnMaxTotalAllocatedBitrate(
    DataRate max_total_allocated_bitrate,
    Timestamp at_time) {
  const bool in_alr = alr_start_time_.has_value();
  const bool allow_allocation_probe = in_alr;

  if (config_.probe_on_max_allocated_bitrate_change &&
      state_ == State::kProbingComplete &&
      max_total_allocated_bitrate != max_total_allocated_bitrate_ &&
      estimated_bitrate_ < max_bitrate_ &&
      estimated_bitrate_ < max_total_allocated_bitrate &&
      allow_allocation_probe) {
    max_total_allocated_bitrate_ = max_total_allocated_bitrate;

    if (!config_.first_allocation_probe_scale)
      return std::vector<ProbeClusterConfig>();

    DataRate first_probe_rate = max_total_allocated_bitrate *
                                config_.first_allocation_probe_scale.Value();
    DataRate probe_cap = config_.allocation_probe_max.Get();
    first_probe_rate = std::min(first_probe_rate, probe_cap);
    std::vector<DataRate> probes = {first_probe_rate};
    if (config_.second_allocation_probe_scale) {
      DataRate second_probe_rate =
          max_total_allocated_bitrate *
          config_.second_allocation_probe_scale.Value();
      second_probe_rate = std::min(second_probe_rate, probe_cap);
      if (second_probe_rate > first_probe_rate)
        probes.push_back(second_probe_rate);
    }
    return InitiateProbing(at_time, probes,
                           config_.allocation_allow_further_probing.Get());
  }
  max_total_allocated_bitrate_ = max_total_allocated_bitrate;
  return std::vector<ProbeClusterConfig>();
}

std::vector<ProbeClusterConfig> ProbeController::OnNetworkAvailability(
    NetworkAvailability msg) {
  network_available_ = msg.network_available;

  if (!network_available_ && state_ == State::kWaitingForProbingResult) {
    state_ = State::kProbingComplete;
    min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
  }

  if (network_available_ && state_ == State::kInit && !start_bitrate_.IsZero())
    return InitiateExponentialProbing(msg.at_time);
  return std::vector<ProbeClusterConfig>();
}

std::vector<ProbeClusterConfig> ProbeController::InitiateExponentialProbing(
    Timestamp at_time) {
  //RTC_DCHECK(network_available_);
  //RTC_DCHECK(state_ == State::kInit);
  //RTC_DCHECK_GT(start_bitrate_bps_, 0);
  MS_ASSERT(network_available_, "network not available");
  MS_ASSERT(state_ == State::kInit, "state_ must be State::kInit");
  MS_ASSERT(start_bitrate_ > DataRate::Zero(), "start_bitrate_bps_ must be > 0");

  // When probing at 1.8 Mbps ( 6x 300), this represents a threshold of
  // 1.2 Mbps to continue probing.
  std::vector<DataRate> probes = {config_.first_exponential_probe_scale *
                                  start_bitrate_};
  if (config_.second_exponential_probe_scale &&
      config_.second_exponential_probe_scale.GetOptional().value() > 0) {
    probes.push_back(config_.second_exponential_probe_scale.Value() *
                     start_bitrate_);
  }
  return InitiateProbing(at_time, probes, true);
}

std::vector<ProbeClusterConfig> ProbeController::SetEstimatedBitrate(
    DataRate bitrate,
    BandwidthLimitedCause bandwidth_limited_cause,
    Timestamp at_time) {
  bandwidth_limited_cause_ = bandwidth_limited_cause;
  if (bitrate < kBitrateDropThreshold * estimated_bitrate_) {
    time_of_last_large_drop_ = at_time;
    bitrate_before_last_large_drop_ = estimated_bitrate_;
  }
  estimated_bitrate_ = bitrate;

  if (mid_call_probing_waiting_for_result_ &&
      bitrate >= mid_call_probing_succcess_threshold_) {
    // RTC_HISTOGRAM_COUNTS_10000("WebRTC.BWE.MidCallProbing.Success",
                               // mid_call_probing_bitrate_bps_ / 1000);
    // RTC_HISTOGRAM_COUNTS_10000("WebRTC.BWE.MidCallProbing.ProbedKbps",
                               // bitrate_bps / 1000);
    mid_call_probing_waiting_for_result_ = false;
  }
  if (state_ == State::kWaitingForProbingResult) {
    // Continue probing if probing results indicate channel has greater
    // capacity.
    DataRate network_state_estimate_probe_further_limit =
        config_.network_state_estimate_probing_interval->IsFinite() &&
                network_estimate_
            ? network_estimate_->link_capacity_upper *
                  config_.further_probe_threshold
            : DataRate::PlusInfinity();

		// MS_DEBUG_DEV(
		//   "[measured bitrate:%" PRIi64 ", minimum to probe further:%" PRIi64 "]",
		//   bitrate_bps, min_bitrate_to_probe_further_bps_);
/*    RTC_LOG(LS_INFO) << "Measured bitrate: " << bitrate
                     << " Minimum to probe further: "
                     << min_bitrate_to_probe_further_ << " upper limit: "
                     << network_state_estimate_probe_further_limit;*/

    if (bitrate > min_bitrate_to_probe_further_ &&
        bitrate <= network_state_estimate_probe_further_limit) {
      return InitiateProbing(
          at_time, {config_.further_exponential_probe_scale * bitrate}, true);
    }
  }
  return {};
}

void ProbeController::EnablePeriodicAlrProbing(bool enable) {
  enable_periodic_alr_probing_ = enable;
}

void ProbeController::SetAlrStartTimeMs(
    absl::optional<int64_t> alr_start_time_ms) {
  if ((alr_start_time_ms.has_value() && !alr_start_time_.has_value()) ||
		(alr_start_time_ms.has_value() && alr_start_time_.has_value() && (alr_start_time_.value().ms() != alr_start_time_ms.value())))
	{
		MS_DEBUG_TAG(bwe, "ALR Start, start time %ld", alr_start_time_ms.value());
		alr_start_time_ = Timestamp::ms(alr_start_time_ms.value());
	}
  /*} else {
    alr_start_time_ = absl::nullopt;
  }*/
}
void ProbeController::SetAlrEndedTimeMs(int64_t alr_end_time_ms) {
	MS_DEBUG_TAG(bwe, "ALR End");
  alr_end_time_.emplace(Timestamp::ms(alr_end_time_ms));
}

std::vector<ProbeClusterConfig> ProbeController::RequestProbe(
    Timestamp at_time) {
  // Called once we have returned to normal state after a large drop in
  // estimated bandwidth. The current response is to initiate a single probe
  // session (if not already probing) at the previous bitrate.
  //
  // If the probe session fails, the assumption is that this drop was a
  // real one from a competing flow or a network change.
  bool in_alr = alr_start_time_.has_value();
  bool alr_ended_recently =
      (alr_end_time_.has_value() &&
       at_time - alr_end_time_.value() < kAlrEndedTimeout);
  if (in_alr || alr_ended_recently || in_rapid_recovery_experiment_) {
    if (state_ == State::kProbingComplete) {
      DataRate suggested_probe =
          kProbeFractionAfterDrop * bitrate_before_last_large_drop_;
      DataRate min_expected_probe_result =
          (1 - kProbeUncertainty) * suggested_probe;
      TimeDelta time_since_drop = at_time - time_of_last_large_drop_;
      TimeDelta time_since_probe = at_time - last_bwe_drop_probing_time_;
      if (min_expected_probe_result > estimated_bitrate_ &&
          time_since_drop < kBitrateDropTimeout &&
          time_since_probe > kMinTimeBetweenAlrProbes) {
				MS_WARN_TAG(bwe, "detected big bandwidth drop, start probing");
        // Track how often we probe in response to bandwidth drop in ALR.
        // RTC_HISTOGRAM_COUNTS_10000(
        //     "WebRTC.BWE.BweDropProbingIntervalInS",
        //     (at_time_ms - last_bwe_drop_probing_time_ms_) / 1000);
				last_bwe_drop_probing_time_ = at_time;
				return InitiateProbing(at_time, {suggested_probe}, false);
      }
    }
  }
  return std::vector<ProbeClusterConfig>();
}

void ProbeController::SetNetworkStateEstimate(
    webrtc::NetworkStateEstimate estimate) {
  network_estimate_ = estimate;
}

void ProbeController::Reset(Timestamp at_time) {
  network_available_ = true;
  bandwidth_limited_cause_ = BandwidthLimitedCause::kDelayBasedLimited;
  state_ = State::kInit;
  min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
  time_last_probing_initiated_ = Timestamp::Zero();
  estimated_bitrate_ = DataRate::Zero();
  network_estimate_ = absl::nullopt;
  start_bitrate_ = DataRate::Zero();
  max_bitrate_ = kDefaultMaxProbingBitrate;
  Timestamp now = at_time;
  last_bwe_drop_probing_time_ = now;
  alr_end_time_.reset();
  mid_call_probing_waiting_for_result_ = false;
  time_of_last_large_drop_ = now;
  bitrate_before_last_large_drop_ = DataRate::Zero();
  max_total_allocated_bitrate_ = DataRate::Zero();
}

bool ProbeController::TimeForAlrProbe(Timestamp at_time) const {
  if (enable_periodic_alr_probing_ && alr_start_time_) {
		//MS_DEBUG_TAG(bwe, "enable_periodic_alr_probing_: %d, alr_start_time_: %lld", enable_periodic_alr_probing_, alr_start_time_.value().ms());
    Timestamp next_probe_time =
        std::max(*alr_start_time_, time_last_probing_initiated_) +
        config_.alr_probing_interval;
    return at_time >= next_probe_time;
  }
  return false;
}

bool ProbeController::TimeForNetworkStateProbe(Timestamp at_time) const {
  if (!network_estimate_ ||
      network_estimate_->link_capacity_upper.IsInfinite()) {
    return false;
  }

  bool probe_due_to_low_estimate =
      bandwidth_limited_cause_ == BandwidthLimitedCause::kDelayBasedLimited &&
      estimated_bitrate_ <
          config_.probe_if_estimate_lower_than_network_state_estimate_ratio *
              network_estimate_->link_capacity_upper;
  if (probe_due_to_low_estimate &&
      config_.estimate_lower_than_network_state_estimate_probing_interval
          ->IsFinite()) {
    Timestamp next_probe_time =
        time_last_probing_initiated_ +
        config_.estimate_lower_than_network_state_estimate_probing_interval;
    return at_time >= next_probe_time;
  }

  bool periodic_probe =
      estimated_bitrate_ < network_estimate_->link_capacity_upper;
  if (periodic_probe &&
      config_.network_state_estimate_probing_interval->IsFinite()) {
    Timestamp next_probe_time = time_last_probing_initiated_ +
                                config_.network_state_estimate_probing_interval;
    return at_time >= next_probe_time;
  }

  return false;
}

std::vector<ProbeClusterConfig> ProbeController::Process(Timestamp at_time) {
  if (at_time - time_last_probing_initiated_ >
      kMaxWaitingTimeForProbingResult) {
    mid_call_probing_waiting_for_result_ = false;

    if (state_ == State::kWaitingForProbingResult) {
      MS_WARN_TAG(bwe, "kWaitingForProbingResult: timeout");
      state_ = State::kProbingComplete;
      min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
    }
  }
  if (estimated_bitrate_.IsZero() || state_ != State::kProbingComplete) {
    return {};
  }
  if (TimeForAlrProbe(at_time) || TimeForNetworkStateProbe(at_time)) {
    return InitiateProbing(
        at_time, {estimated_bitrate_ * config_.alr_probe_scale}, true);
  }
  return std::vector<ProbeClusterConfig>();
}

std::vector<ProbeClusterConfig> ProbeController::InitiateProbing(
    Timestamp now,
    std::vector<DataRate> bitrates_to_probe,
    bool probe_further) {
  if (config_.skip_if_estimate_larger_than_fraction_of_max > 0) {
    DataRate network_estimate = network_estimate_
                                    ? network_estimate_->link_capacity_upper
                                    : DataRate::PlusInfinity();
    DataRate max_probe_rate =
        max_total_allocated_bitrate_.IsZero()
            ? max_bitrate_
            : std::min(max_total_allocated_bitrate_, max_bitrate_);
    if (std::min(network_estimate, estimated_bitrate_) >
        config_.skip_if_estimate_larger_than_fraction_of_max * max_probe_rate) {
      state_ = State::kProbingComplete;
      min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
      return {};
    }
  }

  DataRate max_probe_bitrate = max_bitrate_;
  if (max_total_allocated_bitrate_ > DataRate::Zero()) {
    // If a max allocated bitrate has been configured, allow probing up to 2x
    // that rate. This allows some overhead to account for bursty streams,
    // which otherwise would have to ramp up when the overshoot is already in
    // progress.
    // It also avoids minor quality reduction caused by probes often being
    // received at slightly less than the target probe bitrate.
    max_probe_bitrate =
        std::min(max_probe_bitrate, max_total_allocated_bitrate_ * 2);
  }

  DataRate estimate_capped_bitrate = DataRate::PlusInfinity();
  if (config_.limit_probe_target_rate_to_loss_bwe) {
    switch (bandwidth_limited_cause_) {
      case BandwidthLimitedCause::kLossLimitedBweDecreasing:
        // If bandwidth estimate is decreasing because of packet loss, do not
        // send probes. Unless we are in ALR state, where we might not have traffic to estimate.
				// This might be terribly wrong when we have a big gap between factual and probation bitrate.
				if (!alr_start_time_) {
					MS_DEBUG_TAG(bwe, "State is BandwidthLimitedCause::kLossLimitedBweDecreasing");
					return {};
				}
				break;
      case BandwidthLimitedCause::kLossLimitedBweIncreasing:
				MS_DEBUG_TAG(bwe, "State is BandwidthLimitedCause::kLossLimitedBweIncreasing");
        estimate_capped_bitrate =
            std::min(max_probe_bitrate,
                     estimated_bitrate_ * config_.loss_limited_probe_scale);
        break;
      case BandwidthLimitedCause::kDelayBasedLimited:
				MS_DEBUG_TAG(bwe, "State is BandwidthLimitedCause::kDelayBasedLimited");
        break;
    }
  }
  if (config_.network_state_estimate_probing_interval->IsFinite() &&
      network_estimate_ && network_estimate_->link_capacity_upper.IsFinite()) {
    if (network_estimate_->link_capacity_upper.IsZero()) {
      // RTC_LOG(LS_INFO) << "Not sending probe, Network state estimate is zero";
      return {};
    }
    estimate_capped_bitrate =
        std::min({estimate_capped_bitrate, max_probe_bitrate,
                  network_estimate_->link_capacity_upper *
                      config_.network_state_probe_scale});
  }

  std::vector<ProbeClusterConfig> pending_probes;
  for (DataRate bitrate : bitrates_to_probe) {
    // RTC_DCHECK(!bitrate.IsZero());

    bitrate = std::min(bitrate, estimate_capped_bitrate);
    if (bitrate > max_probe_bitrate) {
      bitrate = max_probe_bitrate;
      probe_further = false;
    }

    ProbeClusterConfig config;
    config.at_time = now;
    config.target_data_rate = bitrate;
    if (network_estimate_ &&
        config_.network_state_estimate_probing_interval->IsFinite()) {
      config.target_duration = config_.network_state_probe_duration;
    } else {
      config.target_duration = config_.min_probe_duration;
    }

    config.target_probe_count = config_.min_probe_packets_sent;
    config.id = next_probe_cluster_id_;
    next_probe_cluster_id_++;
    MaybeLogProbeClusterCreated(config);
    pending_probes.push_back(config);
  }
  time_last_probing_initiated_ = now;
  if (probe_further) {
    state_ = State::kWaitingForProbingResult;
    // Dont expect probe results to be larger than a fraction of the actual
    // probe rate.
    min_bitrate_to_probe_further_ =
        std::min(estimate_capped_bitrate, (*(bitrates_to_probe.end() - 1))) *
        config_.further_probe_threshold;
  } else {
    state_ = State::kProbingComplete;
    min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
  }
  return pending_probes;
}

}  // namespace webrtc
