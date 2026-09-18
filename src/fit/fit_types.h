// FIT protocol constants: base types, record header bits, well-known
// global message numbers. See the FIT Protocol specification (Garmin).
#pragma once
#include <cstddef>
#include <cstdint>

namespace fit {

// Base type identifiers as they appear in definition messages. The low 5
// bits index kBaseTypeSize; bit 7 marks multi-byte (endian-sensitive) types.
enum BaseType : uint8_t {
  kEnum = 0x00,
  kSint8 = 0x01,
  kUint8 = 0x02,
  kSint16 = 0x83,
  kUint16 = 0x84,
  kSint32 = 0x85,
  kUint32 = 0x86,
  kString = 0x07,
  kFloat32 = 0x88,
  kFloat64 = 0x89,
  kUint8z = 0x0A,
  kUint16z = 0x8B,
  kUint32z = 0x8C,
  kByte = 0x0D,
  kSint64 = 0x8E,
  kUint64 = 0x8F,
  kUint64z = 0x90,
};

constexpr uint8_t kBaseTypeIndexMask = 0x1F;
constexpr size_t kBaseTypeCount = 17;

// Byte size per base type index (0..16).
constexpr uint8_t kBaseTypeSize[kBaseTypeCount] = {1, 1, 1, 2, 2, 4, 4, 1, 4,
                                                   8, 1, 2, 4, 1, 8, 8, 8};

inline bool base_type_valid(uint8_t bt) { return (bt & kBaseTypeIndexMask) < kBaseTypeCount; }
inline uint8_t base_type_size(uint8_t bt) { return kBaseTypeSize[bt & kBaseTypeIndexMask]; }

// Record header layout.
constexpr uint8_t kHdrCompressedBit = 0x80;
constexpr uint8_t kHdrDefinitionBit = 0x40;
constexpr uint8_t kHdrDeveloperBit = 0x20;
constexpr uint8_t kHdrLocalMask = 0x0F;
constexpr uint8_t kHdrCompLocalMask = 0x60;
constexpr uint8_t kHdrCompOffsetMask = 0x1F;

constexpr size_t kLocalMessageCount = 16;
constexpr size_t kMaxFieldsPerMessage = 255;  // num_fields is a uint8
constexpr uint8_t kTimestampFieldNum = 253;
constexpr uint8_t kMessageIndexFieldNum = 254;

// Global message numbers we care about (FIT Profile "mesg_num").
enum MesgNum : uint16_t {
  kMesgFileId = 0,
  kMesgDeviceSettings = 2,
  kMesgUserProfile = 3,
  kMesgSession = 18,
  kMesgLap = 19,
  kMesgRecord = 20,
  kMesgEvent = 21,
  kMesgDeviceInfo = 23,
  kMesgWeightScale = 30,
  kMesgActivity = 34,
  kMesgFileCreator = 49,
  kMesgMonitoring = 55,
  kMesgHrv = 78,
  kMesgMonitoringInfo = 103,
  kMesgFieldDescription = 206,
  kMesgDeveloperDataId = 207,
  kMesgMonitoringHrData = 211,
  kMesgStressLevel = 227,
  kMesgSpo2Data = 269,
  kMesgSleepLevel = 275,
  kMesgRespirationRate = 297,
  kMesgSleepAssessment = 346,
  kMesgHrvStatusSummary = 370,
  kMesgHrvValue = 371,
  kMesgRawBbi = 372,
};

// file_id.type values.
enum FileType : uint8_t {
  kFileDevice = 1,
  kFileSettings = 2,
  kFileSport = 3,
  kFileActivity = 4,
  kFileWorkout = 5,
  kFileCourse = 6,
  kFileWeight = 9,
  kFileTotals = 10,
  kFileGoals = 11,
  kFileMonitoringA = 15,
  kFileActivitySummary = 20,
  kFileMonitoringDaily = 28,
  kFileMonitoringB = 32,
  kFileSegment = 34,
  kFileSleep = 49,
};

}  // namespace fit
