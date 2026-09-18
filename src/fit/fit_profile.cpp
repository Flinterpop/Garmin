#include "fit/fit_profile.h"

#include "fit/fit_types.h"
#include "util/assert.h"

namespace fit {

namespace {

// Shorthand initialisers: F = plain field, T = date_time field.
#define F(num, name, scale, offset, units) {num, name, scale, offset, units, false}
#define T(num, name) {num, name, 1.0, 0.0, "s", true}

constexpr FieldInfo kFileId[] = {
    F(0, "type", 1, 0, ""),          F(1, "manufacturer", 1, 0, ""),
    F(2, "product", 1, 0, ""),       F(3, "serial_number", 1, 0, ""),
    T(4, "time_created"),            F(5, "number", 1, 0, ""),
    F(8, "product_name", 1, 0, ""),
};

constexpr FieldInfo kFileCreator[] = {
    F(0, "software_version", 1, 0, ""),
    F(1, "hardware_version", 1, 0, ""),
};

constexpr FieldInfo kUserProfile[] = {
    F(0, "friendly_name", 1, 0, ""),    F(1, "gender", 1, 0, ""),
    F(2, "age", 1, 0, "years"),         F(3, "height", 100, 0, "m"),
    F(4, "weight", 10, 0, "kg"),        F(8, "resting_heart_rate", 1, 0, "bpm"),
    F(11, "default_max_heart_rate", 1, 0, "bpm"),
};

constexpr FieldInfo kDeviceInfo[] = {
    T(253, "timestamp"),
    F(0, "device_index", 1, 0, ""),
    F(1, "device_type", 1, 0, ""),
    F(2, "manufacturer", 1, 0, ""),
    F(3, "serial_number", 1, 0, ""),
    F(4, "product", 1, 0, ""),
    F(5, "software_version", 100, 0, ""),
    F(6, "hardware_version", 1, 0, ""),
    F(7, "cum_operating_time", 1, 0, "s"),
    F(10, "battery_voltage", 256, 0, "V"),
    F(11, "battery_status", 1, 0, ""),
    F(18, "sensor_position", 1, 0, ""),
    F(19, "descriptor", 1, 0, ""),
    F(20, "ant_transmission_type", 1, 0, ""),
    F(21, "ant_device_number", 1, 0, ""),
    F(22, "ant_network", 1, 0, ""),
    F(25, "source_type", 1, 0, ""),
    F(27, "product_name", 1, 0, ""),
    F(32, "battery_level", 1, 0, "%"),
};

constexpr FieldInfo kEvent[] = {
    T(253, "timestamp"),        F(0, "event", 1, 0, ""),  F(1, "event_type", 1, 0, ""),
    F(2, "data16", 1, 0, ""),   F(3, "data", 1, 0, ""),   F(4, "event_group", 1, 0, ""),
};

constexpr FieldInfo kSession[] = {
    T(253, "timestamp"),
    F(0, "event", 1, 0, ""),
    F(1, "event_type", 1, 0, ""),
    T(2, "start_time"),
    F(3, "start_position_lat", 1, 0, "semicircles"),
    F(4, "start_position_long", 1, 0, "semicircles"),
    F(5, "sport", 1, 0, ""),
    F(6, "sub_sport", 1, 0, ""),
    F(7, "total_elapsed_time", 1000, 0, "s"),
    F(8, "total_timer_time", 1000, 0, "s"),
    F(9, "total_distance", 100, 0, "m"),
    F(10, "total_cycles", 1, 0, "cycles"),
    F(11, "total_calories", 1, 0, "kcal"),
    F(13, "total_fat_calories", 1, 0, "kcal"),
    F(14, "avg_speed", 1000, 0, "m/s"),
    F(15, "max_speed", 1000, 0, "m/s"),
    F(16, "avg_heart_rate", 1, 0, "bpm"),
    F(17, "max_heart_rate", 1, 0, "bpm"),
    F(18, "avg_cadence", 1, 0, "rpm"),
    F(19, "max_cadence", 1, 0, "rpm"),
    F(20, "avg_power", 1, 0, "W"),
    F(21, "max_power", 1, 0, "W"),
    F(22, "total_ascent", 1, 0, "m"),
    F(23, "total_descent", 1, 0, "m"),
    F(24, "total_training_effect", 10, 0, ""),
    F(25, "first_lap_index", 1, 0, ""),
    F(26, "num_laps", 1, 0, ""),
    F(28, "trigger", 1, 0, ""),
    F(34, "normalized_power", 1, 0, "W"),
    F(35, "training_stress_score", 10, 0, "tss"),
    F(36, "intensity_factor", 1000, 0, "if"),
    F(41, "avg_stroke_count", 10, 0, "strokes/lap"),
    F(42, "avg_stroke_distance", 100, 0, "m"),
    F(43, "swim_stroke", 1, 0, ""),
    F(44, "pool_length", 100, 0, "m"),
    F(45, "threshold_power", 1, 0, "W"),
    F(47, "num_active_lengths", 1, 0, "lengths"),
    F(48, "total_work", 1, 0, "J"),
    F(49, "avg_altitude", 5, 500, "m"),
    F(50, "max_altitude", 5, 500, "m"),
    F(52, "avg_grade", 100, 0, "%"),
    F(57, "avg_temperature", 1, 0, "C"),
    F(58, "max_temperature", 1, 0, "C"),
    F(59, "total_moving_time", 1000, 0, "s"),
    F(64, "min_heart_rate", 1, 0, "bpm"),
    F(71, "min_altitude", 5, 500, "m"),
    F(89, "avg_vertical_oscillation", 10, 0, "mm"),
    F(90, "avg_stance_time_percent", 100, 0, "%"),
    F(91, "avg_stance_time", 10, 0, "ms"),
    F(92, "avg_fractional_cadence", 128, 0, "rpm"),
    F(124, "enhanced_avg_speed", 1000, 0, "m/s"),
    F(125, "enhanced_max_speed", 1000, 0, "m/s"),
    F(126, "enhanced_avg_altitude", 5, 500, "m"),
    F(127, "enhanced_min_altitude", 5, 500, "m"),
    F(128, "enhanced_max_altitude", 5, 500, "m"),
    F(132, "avg_vertical_ratio", 100, 0, "%"),
    F(133, "avg_stance_time_balance", 100, 0, "%"),
    F(134, "avg_step_length", 10, 0, "mm"),
    F(137, "total_anaerobic_training_effect", 10, 0, ""),
    F(139, "avg_vam", 1000, 0, "m/s"),
};

constexpr FieldInfo kLap[] = {
    T(253, "timestamp"),
    F(0, "event", 1, 0, ""),
    F(1, "event_type", 1, 0, ""),
    T(2, "start_time"),
    F(3, "start_position_lat", 1, 0, "semicircles"),
    F(4, "start_position_long", 1, 0, "semicircles"),
    F(5, "end_position_lat", 1, 0, "semicircles"),
    F(6, "end_position_long", 1, 0, "semicircles"),
    F(7, "total_elapsed_time", 1000, 0, "s"),
    F(8, "total_timer_time", 1000, 0, "s"),
    F(9, "total_distance", 100, 0, "m"),
    F(10, "total_cycles", 1, 0, "cycles"),
    F(11, "total_calories", 1, 0, "kcal"),
    F(12, "total_fat_calories", 1, 0, "kcal"),
    F(13, "avg_speed", 1000, 0, "m/s"),
    F(14, "max_speed", 1000, 0, "m/s"),
    F(15, "avg_heart_rate", 1, 0, "bpm"),
    F(16, "max_heart_rate", 1, 0, "bpm"),
    F(17, "avg_cadence", 1, 0, "rpm"),
    F(18, "max_cadence", 1, 0, "rpm"),
    F(19, "avg_power", 1, 0, "W"),
    F(20, "max_power", 1, 0, "W"),
    F(21, "total_ascent", 1, 0, "m"),
    F(22, "total_descent", 1, 0, "m"),
    F(23, "intensity", 1, 0, ""),
    F(24, "lap_trigger", 1, 0, ""),
    F(25, "sport", 1, 0, ""),
    F(32, "num_lengths", 1, 0, "lengths"),
    F(33, "normalized_power", 1, 0, "W"),
    F(39, "sub_sport", 1, 0, ""),
    F(41, "total_work", 1, 0, "J"),
    F(42, "avg_altitude", 5, 500, "m"),
    F(43, "max_altitude", 5, 500, "m"),
    F(45, "avg_grade", 100, 0, "%"),
    F(50, "avg_temperature", 1, 0, "C"),
    F(51, "max_temperature", 1, 0, "C"),
    F(52, "total_moving_time", 1000, 0, "s"),
    F(62, "min_altitude", 5, 500, "m"),
    F(63, "min_heart_rate", 1, 0, "bpm"),
    F(77, "avg_vertical_oscillation", 10, 0, "mm"),
    F(78, "avg_stance_time_percent", 100, 0, "%"),
    F(79, "avg_stance_time", 10, 0, "ms"),
    F(80, "avg_fractional_cadence", 128, 0, "rpm"),
    F(110, "enhanced_avg_speed", 1000, 0, "m/s"),
    F(111, "enhanced_max_speed", 1000, 0, "m/s"),
    F(112, "enhanced_avg_altitude", 5, 500, "m"),
    F(113, "enhanced_min_altitude", 5, 500, "m"),
    F(114, "enhanced_max_altitude", 5, 500, "m"),
    F(118, "avg_vertical_ratio", 100, 0, "%"),
    F(119, "avg_stance_time_balance", 100, 0, "%"),
    F(120, "avg_step_length", 10, 0, "mm"),
};

constexpr FieldInfo kRecord[] = {
    T(253, "timestamp"),
    F(0, "position_lat", 1, 0, "semicircles"),
    F(1, "position_long", 1, 0, "semicircles"),
    F(2, "altitude", 5, 500, "m"),
    F(3, "heart_rate", 1, 0, "bpm"),
    F(4, "cadence", 1, 0, "rpm"),
    F(5, "distance", 100, 0, "m"),
    F(6, "speed", 1000, 0, "m/s"),
    F(7, "power", 1, 0, "W"),
    F(9, "grade", 100, 0, "%"),
    F(10, "resistance", 1, 0, ""),
    F(11, "time_from_course", 1000, 0, "s"),
    F(12, "cycle_length", 100, 0, "m"),
    F(13, "temperature", 1, 0, "C"),
    F(17, "speed_1s", 16, 0, "m/s"),
    F(18, "cycles", 1, 0, "cycles"),
    F(19, "total_cycles", 1, 0, "cycles"),
    F(29, "accumulated_power", 1, 0, "W"),
    F(30, "left_right_balance", 1, 0, ""),
    F(31, "gps_accuracy", 1, 0, "m"),
    F(32, "vertical_speed", 1000, 0, "m/s"),
    F(33, "calories", 1, 0, "kcal"),
    F(39, "vertical_oscillation", 10, 0, "mm"),
    F(40, "stance_time_percent", 100, 0, "%"),
    F(41, "stance_time", 10, 0, "ms"),
    F(42, "activity_type", 1, 0, ""),
    F(43, "left_torque_effectiveness", 2, 0, "%"),
    F(44, "right_torque_effectiveness", 2, 0, "%"),
    F(45, "left_pedal_smoothness", 2, 0, "%"),
    F(46, "right_pedal_smoothness", 2, 0, "%"),
    F(47, "combined_pedal_smoothness", 2, 0, "%"),
    F(53, "fractional_cadence", 128, 0, "rpm"),
    F(54, "total_hemoglobin_conc", 100, 0, "g/dL"),
    F(57, "saturated_hemoglobin_percent", 10, 0, "%"),
    F(62, "device_index", 1, 0, ""),
    F(73, "enhanced_speed", 1000, 0, "m/s"),
    F(78, "enhanced_altitude", 5, 500, "m"),
    F(81, "battery_soc", 2, 0, "%"),
    F(82, "motor_power", 1, 0, "W"),
    F(83, "vertical_ratio", 100, 0, "%"),
    F(84, "stance_time_balance", 100, 0, "%"),
    F(85, "step_length", 10, 0, "mm"),
    F(91, "absolute_pressure", 1, 0, "Pa"),
    F(92, "depth", 1000, 0, "m"),
    F(99, "respiration_rate", 1, 0, "breaths/min"),
    F(108, "enhanced_respiration_rate", 100, 0, "breaths/min"),
    F(114, "grit", 1, 0, ""),
    F(115, "flow", 1, 0, ""),
    F(116, "current_stress", 100, 0, ""),
    F(139, "core_temperature", 100, 0, "C"),
};

constexpr FieldInfo kActivity[] = {
    T(253, "timestamp"),
    F(0, "total_timer_time", 1000, 0, "s"),
    F(1, "num_sessions", 1, 0, ""),
    F(2, "type", 1, 0, ""),
    F(3, "event", 1, 0, ""),
    F(4, "event_type", 1, 0, ""),
    T(5, "local_timestamp"),
    F(6, "event_group", 1, 0, ""),
};

constexpr FieldInfo kMonitoring[] = {
    T(253, "timestamp"),
    F(0, "device_index", 1, 0, ""),
    F(1, "calories", 1, 0, "kcal"),
    F(2, "distance", 100, 0, "m"),
    F(3, "cycles", 2, 0, "cycles"),
    F(4, "active_time", 1000, 0, "s"),
    F(5, "activity_type", 1, 0, ""),
    F(6, "activity_subtype", 1, 0, ""),
    F(7, "activity_level", 1, 0, ""),
    F(8, "distance_16", 1, 0, "100*m"),
    F(9, "cycles_16", 1, 0, "2*cycles"),
    F(10, "active_time_16", 1, 0, "s"),
    T(11, "local_timestamp"),
    F(12, "temperature", 100, 0, "C"),
    F(14, "temperature_min", 100, 0, "C"),
    F(15, "temperature_max", 100, 0, "C"),
    F(16, "activity_time", 1, 0, "min"),
    F(19, "active_calories", 1, 0, "kcal"),
    F(24, "current_activity_type_intensity", 1, 0, ""),
    F(25, "timestamp_min_8", 1, 0, "min"),
    F(26, "timestamp_16", 1, 0, "s"),
    F(27, "heart_rate", 1, 0, "bpm"),
    F(28, "intensity", 10, 0, ""),
    F(29, "duration_min", 1, 0, "min"),
    F(30, "duration", 1, 0, "s"),
    F(31, "ascent", 1000, 0, "m"),
    F(32, "descent", 1000, 0, "m"),
    F(33, "moderate_activity_minutes", 1, 0, "min"),
    F(34, "vigorous_activity_minutes", 1, 0, "min"),
};

constexpr FieldInfo kMonitoringInfo[] = {
    T(253, "timestamp"),
    T(0, "local_timestamp"),
    F(1, "activity_type", 1, 0, ""),
    F(3, "cycles_to_distance", 5000, 0, "m/cycle"),
    F(4, "cycles_to_calories", 5000, 0, "kcal/cycle"),
    F(5, "resting_metabolic_rate", 1, 0, "kcal/day"),
};

constexpr FieldInfo kMonitoringHrData[] = {
    T(253, "timestamp"),
    F(0, "resting_heart_rate", 1, 0, "bpm"),
    F(1, "current_day_resting_heart_rate", 1, 0, "bpm"),
};

constexpr FieldInfo kStressLevel[] = {
    F(0, "stress_level_value", 1, 0, ""),
    T(1, "stress_level_time"),
};

constexpr FieldInfo kSleepLevel[] = {
    T(253, "timestamp"),
    F(0, "sleep_level", 1, 0, ""),
};

constexpr FieldInfo kSpo2Data[] = {
    T(253, "timestamp"),
    F(0, "reading_spo2", 1, 0, "%"),
    F(1, "reading_confidence", 1, 0, ""),
    F(2, "mode", 1, 0, ""),
};

constexpr FieldInfo kRespirationRate[] = {
    T(253, "timestamp"),
    F(0, "respiration_rate", 100, 0, "breaths/min"),
};

constexpr FieldInfo kHrv[] = {
    F(0, "time", 1000, 0, "s"),
};

constexpr FieldInfo kHrvStatusSummary[] = {
    T(253, "timestamp"),
    F(0, "weekly_average", 128, 0, "ms"),
    F(1, "last_night_average", 128, 0, "ms"),
    F(2, "last_night_5_min_high", 128, 0, "ms"),
    F(3, "baseline_low_upper", 128, 0, "ms"),
    F(4, "baseline_balanced_lower", 128, 0, "ms"),
    F(5, "baseline_balanced_upper", 128, 0, "ms"),
    F(6, "status", 1, 0, ""),
};

constexpr FieldInfo kHrvValue[] = {
    T(253, "timestamp"),
    F(0, "value", 128, 0, "ms"),
};

constexpr FieldInfo kRawBbi[] = {
    T(253, "timestamp"),
    F(0, "timestamp_ms", 1, 0, "ms"),
    F(1, "data", 1, 0, ""),
    F(2, "time", 1, 0, "ms"),
    F(3, "quality", 1, 0, ""),
    F(4, "gap", 1, 0, ""),
};

constexpr FieldInfo kWeightScale[] = {
    T(253, "timestamp"),
    F(0, "weight", 100, 0, "kg"),
    F(1, "percent_fat", 100, 0, "%"),
    F(2, "percent_hydration", 100, 0, "%"),
    F(3, "visceral_fat_mass", 100, 0, "kg"),
    F(4, "bone_mass", 100, 0, "kg"),
    F(5, "muscle_mass", 100, 0, "kg"),
    F(7, "basal_met", 4, 0, "kcal/day"),
    F(8, "physique_rating", 1, 0, ""),
    F(9, "active_met", 4, 0, "kcal/day"),
    F(10, "metabolic_age", 1, 0, "years"),
    F(11, "visceral_fat_rating", 1, 0, ""),
    F(12, "user_profile_index", 1, 0, ""),
    F(13, "bmi", 10, 0, "kg/m^2"),
};

constexpr FieldInfo kSleepAssessment[] = {
    F(0, "combined_awake_score", 1, 0, ""),
    F(1, "awake_time_score", 1, 0, ""),
    F(2, "awakenings_count_score", 1, 0, ""),
    F(3, "deep_sleep_score", 1, 0, ""),
    F(4, "sleep_duration_score", 1, 0, ""),
    F(5, "light_sleep_score", 1, 0, ""),
    F(6, "overall_sleep_score", 1, 0, ""),
    F(7, "sleep_quality_score", 1, 0, ""),
    F(8, "sleep_recovery_score", 1, 0, ""),
    F(9, "rem_sleep_score", 1, 0, ""),
    F(10, "sleep_restlessness_score", 1, 0, ""),
    F(11, "awakenings_count", 1, 0, ""),
    F(14, "interruptions_score", 1, 0, ""),
    F(15, "average_stress_during_sleep", 100, 0, ""),
};

constexpr FieldInfo kFieldDescription[] = {
    F(0, "developer_data_index", 1, 0, ""),
    F(1, "field_definition_number", 1, 0, ""),
    F(2, "fit_base_type_id", 1, 0, ""),
    F(3, "field_name", 1, 0, ""),
    F(4, "array", 1, 0, ""),
    F(5, "components", 1, 0, ""),
    F(6, "scale", 1, 0, ""),
    F(7, "offset", 1, 0, ""),
    F(8, "units", 1, 0, ""),
    F(13, "fit_base_unit_id", 1, 0, ""),
    F(14, "native_mesg_num", 1, 0, ""),
    F(15, "native_field_num", 1, 0, ""),
};

constexpr FieldInfo kDeveloperDataId[] = {
    F(0, "developer_id", 1, 0, ""),
    F(1, "application_id", 1, 0, ""),
    F(2, "manufacturer_id", 1, 0, ""),
    F(3, "developer_data_index", 1, 0, ""),
    F(4, "application_version", 1, 0, ""),
};

#undef F
#undef T

#define M(num, name, arr) {num, name, arr, sizeof(arr) / sizeof(arr[0])}

constexpr MesgInfo kMesgs[] = {
    M(kMesgFileId, "file_id", kFileId),
    M(kMesgUserProfile, "user_profile", kUserProfile),
    M(kMesgSession, "session", kSession),
    M(kMesgLap, "lap", kLap),
    M(kMesgRecord, "record", kRecord),
    M(kMesgEvent, "event", kEvent),
    M(kMesgDeviceInfo, "device_info", kDeviceInfo),
    M(kMesgWeightScale, "weight_scale", kWeightScale),
    M(kMesgActivity, "activity", kActivity),
    M(kMesgFileCreator, "file_creator", kFileCreator),
    M(kMesgMonitoring, "monitoring", kMonitoring),
    M(kMesgHrv, "hrv", kHrv),
    M(kMesgMonitoringInfo, "monitoring_info", kMonitoringInfo),
    M(kMesgFieldDescription, "field_description", kFieldDescription),
    M(kMesgDeveloperDataId, "developer_data_id", kDeveloperDataId),
    M(kMesgMonitoringHrData, "monitoring_hr_data", kMonitoringHrData),
    M(kMesgStressLevel, "stress_level", kStressLevel),
    M(kMesgSpo2Data, "spo2_data", kSpo2Data),
    M(kMesgSleepLevel, "sleep_level", kSleepLevel),
    M(kMesgRespirationRate, "respiration_rate", kRespirationRate),
    M(kMesgSleepAssessment, "sleep_assessment", kSleepAssessment),
    M(kMesgHrvStatusSummary, "hrv_status_summary", kHrvStatusSummary),
    M(kMesgHrvValue, "hrv_value", kHrvValue),
    M(kMesgRawBbi, "raw_bbi", kRawBbi),
};

#undef M

constexpr size_t kMesgCount = sizeof(kMesgs) / sizeof(kMesgs[0]);

}  // namespace

const MesgInfo* find_mesg(uint16_t mesg_num) {
  for (size_t i = 0; i < kMesgCount; ++i) {
    if (kMesgs[i].num == mesg_num) return &kMesgs[i];
  }
  return nullptr;
}

const FieldInfo* find_field(uint16_t mesg_num, uint8_t field_num) {
  const MesgInfo* m = find_mesg(mesg_num);
  G_REQUIRE_RET(m != nullptr, nullptr);
  G_ASSERT(m->field_count <= kMaxFieldsPerMessage);
  for (size_t i = 0; i < m->field_count; ++i) {
    if (m->fields[i].num == field_num) return &m->fields[i];
  }
  return nullptr;
}

const char* file_type_name(uint8_t type) {
  switch (type) {
    case kFileDevice: return "device";
    case kFileSettings: return "settings";
    case kFileSport: return "sport";
    case kFileActivity: return "activity";
    case kFileWorkout: return "workout";
    case kFileCourse: return "course";
    case kFileWeight: return "weight";
    case kFileTotals: return "totals";
    case kFileGoals: return "goals";
    case kFileMonitoringA: return "monitoring_a";
    case kFileActivitySummary: return "activity_summary";
    case kFileMonitoringDaily: return "monitoring_daily";
    case kFileMonitoringB: return "monitoring_b";
    case kFileSegment: return "segment";
    case kFileSleep: return "sleep";
    default: return "unknown";
  }
}

const char* sport_name(uint8_t sport) {
  switch (sport) {
    case 0: return "generic";
    case 1: return "running";
    case 2: return "cycling";
    case 3: return "transition";
    case 4: return "fitness_equipment";
    case 5: return "swimming";
    case 6: return "basketball";
    case 7: return "soccer";
    case 8: return "tennis";
    case 9: return "american_football";
    case 10: return "training";
    case 11: return "walking";
    case 12: return "cross_country_skiing";
    case 13: return "alpine_skiing";
    case 14: return "snowboarding";
    case 15: return "rowing";
    case 16: return "mountaineering";
    case 17: return "hiking";
    case 18: return "multisport";
    case 19: return "paddling";
    case 20: return "flying";
    case 21: return "e_biking";
    case 22: return "motorcycling";
    case 23: return "boating";
    case 24: return "driving";
    case 25: return "golf";
    case 26: return "hang_gliding";
    case 27: return "horseback_riding";
    case 28: return "hunting";
    case 29: return "fishing";
    case 30: return "inline_skating";
    case 31: return "rock_climbing";
    case 32: return "sailing";
    case 33: return "ice_skating";
    case 34: return "sky_diving";
    case 35: return "snowshoeing";
    case 36: return "snowmobiling";
    case 37: return "stand_up_paddleboarding";
    case 38: return "surfing";
    case 39: return "wakeboarding";
    case 40: return "water_skiing";
    case 41: return "kayaking";
    case 42: return "rafting";
    case 43: return "windsurfing";
    case 44: return "kitesurfing";
    case 45: return "tactical";
    case 46: return "jumpmaster";
    case 47: return "boxing";
    case 48: return "floor_climbing";
    case 53: return "diving";
    case 62: return "hiit";
    case 64: return "racket";
    case 76: return "water_tubing";
    case 77: return "wakesurfing";
    default: return "unknown";
  }
}

const char* sleep_level_name(uint8_t level) {
  switch (level) {
    case 0: return "unmeasurable";
    case 1: return "awake";
    case 2: return "light";
    case 3: return "deep";
    case 4: return "rem";
    default: return "unknown";
  }
}

double semicircles_to_degrees(int32_t semicircles) {
  return static_cast<double>(semicircles) * (180.0 / 2147483648.0);
}

}  // namespace fit
