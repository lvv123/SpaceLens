#include "ntfs.hpp"

#include <cstring>

namespace cs::ntfs {
namespace {

inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// Walk the attribute chain of a FILE record; stops at kAttrEnd / out of bounds.
struct AttrIterator {
    const uint8_t* rec;
    size_t size;
    size_t pos;
    size_t end;

    AttrIterator(const uint8_t* r, size_t s) : rec(r), size(s), pos(0), end(s) {
        if (s >= 24) {
            const uint32_t used = rd32(r + 24);
            uint32_t start = rd16(r + 20);
            if (start < 48 || start >= s) start = 48;
            end = (used >= start && used <= s) ? used : s;
            pos = start;
        }
    }

    bool next(const uint8_t*& attr, uint32_t& type, uint32_t& length, bool& nonResident) {
        while (pos + 16 <= end) {
            type = rd32(rec + pos);
            if (type == kAttrEnd || type == 0) return false;
            length = rd32(rec + pos + 4);
            if (length < 16 || pos + length > end) return false;
            nonResident = rec[pos + 8] != 0;
            attr = rec + pos;
            pos += length;
            return true;
        }
        return false;
    }
};

}  // namespace

bool find_data_extent_in_attribute_list(const uint8_t* data, uint32_t length, uint64_t& ref) {
    bool found = false;
    uint64_t bestVcn = 0;
    uint64_t bestRef = 0;
    size_t p = 0;
    while (p + 26 <= length) {
        const uint32_t t = rd32(data + p);
        const uint16_t elen = rd16(data + p + 4);
        if (elen < 26 || p + elen > length) break;
        const uint8_t nlen = data[p + 6];
        const uint64_t vcn = rd64(data + p + 8);
        const uint64_t r = rd64(data + p + 16);
        if (t == kAttrData && nlen == 0 && (!found || vcn < bestVcn)) {
            bestVcn = vcn;
            bestRef = r;
            found = true;
        }
        p += elen;
    }
    ref = bestRef;
    return found;
}

bool parse_boot_sector(const uint8_t* data, size_t size, BootSector& out) {
    out = BootSector{};
    if (size < 72) return false;
    if (std::memcmp(data + 3, "NTFS    ", 8) != 0) return false;

    out.bytesPerSector    = rd16(data + 11);
    out.sectorsPerCluster = data[13];
    const int8_t cpr      = static_cast<int8_t>(data[64]);
    out.mftLcn            = static_cast<int64_t>(rd64(data + 48));

    if (out.bytesPerSector == 0 || out.sectorsPerCluster == 0) return false;
    out.bytesPerCluster = static_cast<uint32_t>(out.bytesPerSector) * out.sectorsPerCluster;

    if (cpr > 0) {
        out.bytesPerFileRecord = static_cast<uint32_t>(cpr) * out.bytesPerCluster;
    } else if (cpr < 0) {
        out.bytesPerFileRecord = 1u << (-cpr);
    } else {
        return false;
    }
    out.valid = true;
    return true;
}

bool decode_run_list(const uint8_t* runs, size_t size, std::vector<Run>& out) {
    out.clear();
    uint64_t vcn = 0;
    int64_t lcn = 0;
    size_t i = 0;
    while (i < size) {
        const uint8_t header = runs[i++];
        if (header == 0) return true;  // end marker
        const unsigned lenBytes = header & 0x0F;
        const unsigned offBytes = header >> 4;
        if (lenBytes == 0 || lenBytes > 8 || offBytes > 8) return false;
        if (i + lenBytes + offBytes > size) return false;

        uint64_t length = 0;
        for (unsigned b = 0; b < lenBytes; ++b)
            length |= static_cast<uint64_t>(runs[i + b]) << (8 * b);
        i += lenBytes;

        const bool sparse = (offBytes == 0);
        int64_t delta = 0;
        if (!sparse) {
            uint64_t raw = 0;
            for (unsigned b = 0; b < offBytes; ++b)
                raw |= static_cast<uint64_t>(runs[i + b]) << (8 * b);
            const unsigned bits = 8 * offBytes;
            if (bits < 64 && (raw & (1ull << (bits - 1))))
                raw |= ~((1ull << bits) - 1);  // sign extend
            delta = static_cast<int64_t>(raw);
            i += offBytes;
        }

        Run r;
        r.vcn = vcn;
        r.lengthClusters = length;
        r.sparse = sparse;
        if (sparse) {
            r.lcn = 0;
        } else {
            lcn += delta;
            if (lcn < 0) return false;
            r.lcn = static_cast<uint64_t>(lcn);
        }
        out.push_back(r);
        vcn += length;
    }
    return true;
}

bool apply_fixups(uint8_t* record, size_t recordSize, uint32_t bytesPerSector) {
    if (recordSize < 48 || bytesPerSector == 0) return false;
    const uint16_t usaOffset = rd16(record + 4);
    const uint16_t usaCount  = rd16(record + 6);
    if (usaCount == 0) return true;
    const size_t usaBytes = static_cast<size_t>(usaCount) * 2;
    if (static_cast<size_t>(usaOffset) + usaBytes > recordSize) return false;

    const uint16_t usn = rd16(record + usaOffset);
    for (uint16_t i = 1; i < usaCount; ++i) {
        const size_t sectorEnd = static_cast<size_t>(i) * bytesPerSector;
        if (sectorEnd < 2 || sectorEnd > recordSize) return false;
        const size_t pos = sectorEnd - 2;
        if (rd16(record + pos) != usn) return false;
        const uint16_t original = rd16(record + usaOffset + i * 2);
        std::memcpy(record + pos, &original, 2);
    }
    return true;
}

bool parse_file_record(const uint8_t* record, size_t recordSize,
                       uint32_t recordNumber, uint32_t bytesPerSector,
                       FileRecord& out) {
    (void)bytesPerSector;
    out.valid = false;
    out.inUse = false;
    out.isDirectory = false;
    out.sequence = 0;
    out.hardLinkCount = 0;
    out.recordNumber = recordNumber;
    out.realSize = 0;
    out.allocatedSize = 0;
    out.hasStandardInformation = false;
    out.dataExtentRef = 0;
    out.dataExtentRefValid = false;
    out.attrListRuns.clear();
    out.hasAttrListRuns = false;
    out.attrListRealSize = 0;
    out.names.clear();

    if (recordSize < 48) return false;
    if (std::memcmp(record, "FILE", 4) != 0) return false;

    const uint16_t flags = rd16(record + 22);
    out.inUse       = (flags & 0x0001) != 0;
    out.isDirectory = (flags & 0x0002) != 0;
    out.sequence    = rd16(record + 16);
    out.hardLinkCount = rd16(record + 18);

    uint64_t dataReal = 0;
    uint64_t dataAlloc = 0;
    uint64_t dataBestVcn = 0;
    bool hasData = false;
    uint64_t fnReal = 0;
    uint64_t fnAlloc = 0;
    bool hasFileNameSize = false;
    uint64_t attrListDataRef = 0;
    bool attrListFound = false;

    AttrIterator it(record, recordSize);
    const uint8_t* attr = nullptr;
    uint32_t type = 0, length = 0;
    bool nonResident = false;
    while (it.next(attr, type, length, nonResident)) {
        const uint8_t attrNameLen = attr[9];
        if (type == kAttrStandardInformation && !nonResident) {
            // $STANDARD_INFORMATION carries no size fields; only note its presence.
            out.hasStandardInformation = true;
        } else if (type == kAttrFileName && !nonResident) {
            const uint32_t contentLen = rd32(attr + 16);
            const uint16_t contentOff = rd16(attr + 20);
            if (static_cast<uint32_t>(contentOff) + contentLen <= length && contentLen >= 0x42) {
                const uint8_t* fn = attr + contentOff;
                const uint8_t nameLen = fn[0x40];
                const uint8_t nameSpace = fn[0x41];
                if (static_cast<size_t>(0x42) + static_cast<size_t>(nameLen) * 2 <= contentLen) {
                    FileNameEntry e;
                    e.parentRef  = rd64(fn + 0);
                    e.name       = reinterpret_cast<const wchar_t*>(fn + 0x42);
                    e.nameLength = nameLen;
                    e.nameSpace  = nameSpace;
                    if (e.nameLength > 0) out.names.push_back(e);
                    if (rd32(fn + 0x38) & 0x10000000u) out.isDirectory = true;
                    if (!hasFileNameSize) {
                        fnAlloc = rd64(fn + 0x28);
                        fnReal  = rd64(fn + 0x30);
                        hasFileNameSize = true;
                    }
                }
            }
        } else if (type == kAttrAttributeList) {
            if (!nonResident) {
                const uint32_t contentLen = rd32(attr + 16);
                const uint16_t contentOff = rd16(attr + 20);
                if (static_cast<uint32_t>(contentOff) + contentLen <= length) {
                    attrListFound = find_data_extent_in_attribute_list(attr + contentOff,
                                                                       contentLen, attrListDataRef);
                }
            } else {
                const uint16_t runOff = rd16(attr + 32);
                if (runOff < length) {
                    decode_run_list(attr + runOff, length - runOff, out.attrListRuns);
                    out.attrListRealSize = rd64(attr + 48);
                    out.hasAttrListRuns = !out.attrListRuns.empty();
                }
            }
        } else if (type == kAttrData && attrNameLen == 0) {
            // The unnamed $DATA stream is the authoritative size source. When the
            // attribute is split, keep the extent with the lowest VCN (its header
            // carries the real size).
            const uint64_t vcn = nonResident ? rd64(attr + 0x10) : 0;
            if (!hasData || vcn < dataBestVcn) {
                dataBestVcn = vcn;
                dataAlloc = nonResident ? rd64(attr + 0x28) : rd32(attr + 16);
                dataReal  = nonResident ? rd64(attr + 0x30) : rd32(attr + 16);
                hasData = true;
            }
        }
    }

    out.dataExtentRef = attrListDataRef;
    out.dataExtentRefValid = attrListFound;

    if (hasData) {
        out.realSize = dataReal;
        out.allocatedSize = dataAlloc;
    } else if (hasFileNameSize) {
        out.realSize = fnReal;
        out.allocatedSize = fnAlloc;
    }

    out.valid = true;
    return true;
}

bool decode_data_attribute(const uint8_t* record, size_t recordSize,
                           std::vector<Run>& runs, uint64_t& realSize,
                           uint64_t& allocatedSize) {
    runs.clear();
    realSize = 0;
    allocatedSize = 0;
    if (recordSize < 48) return false;
    if (std::memcmp(record, "FILE", 4) != 0) return false;

    AttrIterator it(record, recordSize);
    const uint8_t* attr = nullptr;
    uint32_t type = 0, length = 0;
    bool nonResident = false;
    while (it.next(attr, type, length, nonResident)) {
        if (type != kAttrData) continue;
        if (attr[9] != 0) continue;  // named stream
        if (!nonResident) continue;  // no runs
        const uint16_t runOff = rd16(attr + 32);
        if (runOff >= length) return false;
        realSize      = rd64(attr + 48);
        allocatedSize = rd64(attr + 40);
        return decode_run_list(attr + runOff, length - runOff, runs);
    }
    return false;
}

}  // namespace cs::ntfs
