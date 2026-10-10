#pragma once

// Low-level NTFS on-disk structures and pure parsing helpers.
// Everything here is side-effect free and unit-testable with synthetic buffers.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cs::ntfs {

inline constexpr uint32_t kAttrStandardInformation = 0x10;
inline constexpr uint32_t kAttrFileName            = 0x30;
inline constexpr uint32_t kAttrData                = 0x80;
inline constexpr uint32_t kAttrEnd                 = 0xFFFFFFFF;

// Fields from the NTFS volume boot sector needed to locate and size the MFT.
struct BootSector {
    uint16_t bytesPerSector     = 0;
    uint8_t  sectorsPerCluster  = 0;
    uint32_t bytesPerCluster    = 0;
    uint32_t bytesPerFileRecord = 0;
    int64_t  mftLcn             = 0;
    bool     valid              = false;
};

bool parse_boot_sector(const uint8_t* data, size_t size, BootSector& out);

// One physical extent of an NTFS data stream.
struct Run {
    uint64_t vcn           = 0;
    uint64_t lcn           = 0;   // valid only when !sparse
    uint64_t lengthClusters = 0;
    bool     sparse        = false;
};

// Decode a run list byte stream. Returns false on malformed input.
bool decode_run_list(const uint8_t* runs, size_t size, std::vector<Run>& out);

// One FILE_NAME attribute value.
struct FileNameEntry {
    uint64_t       parentRef  = 0;   // full 64-bit file reference of the parent
    const wchar_t* name       = nullptr;  // points into the parsed record buffer
    uint16_t       nameLength = 0;       // in wchar_t units
    uint8_t        nameSpace  = 0;       // 0=POSIX 1=Win32 2=DOS 3=Win32&DOS
};

struct FileRecord {
    bool     valid         = false;  // signature present and bounds sane
    bool     inUse         = false;
    bool     isDirectory   = false;
    uint16_t sequence      = 0;
    uint16_t hardLinkCount = 0;
    uint32_t recordNumber  = 0;
    uint64_t realSize      = 0;      // logical size (unnamed $DATA; fallback $FILE_NAME)
    uint64_t allocatedSize = 0;      // size on disk (same sources)
    bool     hasStandardInformation = false;
    std::vector<FileNameEntry> names;  // reused across calls (capacity is kept)
};

// Undo the update-sequence-array fixups in place. `record` must be mutable.
bool apply_fixups(uint8_t* record, size_t recordSize, uint32_t bytesPerSector);

// Parse one FILE record. Fixups must already have been applied to `record`.
// Name pointers in `out` alias `record`, so it must outlive their use.
bool parse_file_record(const uint8_t* record, size_t recordSize,
                       uint32_t recordNumber, uint32_t bytesPerSector,
                       FileRecord& out);

// Find the unnamed, non-resident $DATA attribute and decode its run list.
// `record` must already be fixup-corrected.
bool decode_data_attribute(const uint8_t* record, size_t recordSize,
                           std::vector<Run>& runs, uint64_t& realSize,
                           uint64_t& allocatedSize);

}  // namespace cs::ntfs
