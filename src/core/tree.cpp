#include "tree.hpp"

#include <algorithm>
#include <cwchar>

namespace cs {
namespace {

bool iequals_ext(const wchar_t* ext, size_t len, const char* ascii) {
    for (size_t i = 0; i < len; ++i) {
        char c = ascii[i];
        if (c == 0) return false;
        wchar_t w = ext[i];
        if (w >= L'A' && w <= L'Z') w = static_cast<wchar_t>(w - L'A' + L'a');
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (w != static_cast<wchar_t>(c)) return false;
    }
    return ascii[len] == 0;
}

bool in_list(const wchar_t* ext, size_t len, const char* const* list) {
    for (const char* const* p = list; *p; ++p) {
        if (iequals_ext(ext, len, *p)) return true;
    }
    return false;
}

}  // namespace

Category categorize(const wchar_t* name, uint32_t length) {
    if (length == 0) return Category::Other;
    uint32_t dot = length;
    for (uint32_t i = length; i > 0; --i) {
        if (name[i - 1] == L'.') { dot = i - 1; break; }
    }
    if (dot == length) return Category::Other;  // no extension
    const wchar_t* ext = name + dot + 1;
    const size_t len = length - dot - 1;
    if (len == 0) return Category::Other;

    static const char* const kVideo[] = {"mp4","mkv","avi","mov","wmv","flv","webm","m4v","mpg","mpeg","ts","m2ts",nullptr};
    static const char* const kImage[] = {"jpg","jpeg","png","gif","bmp","tif","tiff","webp","heic","svg","ico","raw","cr2","nef",nullptr};
    static const char* const kAudio[] = {"mp3","wav","flac","aac","ogg","wma","m4a","opus","aiff",nullptr};
    static const char* const kDoc[]   = {"pdf","doc","docx","xls","xlsx","ppt","pptx","txt","md","rtf","odt","ods","csv","epub",nullptr};
    static const char* const kArchive[] = {"zip","rar","7z","tar","gz","bz2","xz","iso","cab","tgz",nullptr};
    static const char* const kExec[]  = {"exe","dll","msi","sys","com","bin","so","dylib","scr","cpl",nullptr};
    static const char* const kCode[]  = {"c","cpp","cc","cxx","h","hpp","cs","java","py","js","ts","tsx","jsx","html","css","json","xml","go","rs","sh","bat","ps1","yml","yaml",nullptr};

    if (in_list(ext, len, kVideo))   return Category::Video;
    if (in_list(ext, len, kImage))   return Category::Image;
    if (in_list(ext, len, kAudio))   return Category::Audio;
    if (in_list(ext, len, kDoc))     return Category::Document;
    if (in_list(ext, len, kArchive)) return Category::Archive;
    if (in_list(ext, len, kExec))    return Category::Executable;
    if (in_list(ext, len, kCode))    return Category::Code;
    return Category::Other;
}

void VolumeTree::clear() {
    nodes_.clear();
    names_.clear();
    root_ = kInvalid;
}

void VolumeTree::reserve(size_t nodeCount, size_t nameChars) {
    nodes_.reserve(nodeCount);
    names_.reserve(nameChars);
}

uint32_t VolumeTree::addNode(const wchar_t* name, uint32_t nameLength, uint64_t size,
                             uint64_t alloc, bool isDirectory, uint32_t recordIndex) {
    Node n;
    n.parent = kInvalid;
    n.firstChild = kInvalid;
    n.nextSibling = kInvalid;
    n.nameOffset = static_cast<uint32_t>(names_.size());
    n.nameLength = static_cast<uint16_t>(nameLength);
    n.flags = isDirectory ? 1 : 0;
    n.category = static_cast<uint8_t>(isDirectory ? Category::Other
                                                  : categorize(name, nameLength));
    n.totalSize = size;
    n.totalAlloc = alloc;
    n.childCount = 0;
    n.recordIndex = recordIndex;
    names_.insert(names_.end(), name, name + nameLength);
    names_.push_back(L'\0');  // keep it printable without copying

    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(n);
    return index;
}

void VolumeTree::link(uint32_t parent, uint32_t child) {
    if (!valid(parent) || !valid(child) || parent == child) return;
    Node& c = nodes_[child];
    c.parent = parent;
    Node& p = nodes_[parent];
    c.nextSibling = p.firstChild;
    p.firstChild = child;
    ++p.childCount;
}

std::wstring VolumeTree::nameString(uint32_t i) const {
    if (!valid(i)) return {};
    const Node& n = nodes_[i];
    return std::wstring(names_.data() + n.nameOffset, n.nameLength);
}

void VolumeTree::aggregate() {
    if (!valid(root_)) return;
    std::vector<uint32_t> order;
    order.reserve(nodes_.size());
    std::vector<uint32_t> stack;
    stack.push_back(root_);
    while (!stack.empty()) {
        const uint32_t u = stack.back();
        stack.pop_back();
        order.push_back(u);
        for (uint32_t c = nodes_[u].firstChild; c != kInvalid; c = nodes_[c].nextSibling)
            stack.push_back(c);
    }
    // Reverse preorder => every child is processed before its parent.
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const uint32_t u = *it;
        Node& n = nodes_[u];
        uint64_t size = n.totalSize;   // currently holds the node's own size
        uint64_t alloc = n.totalAlloc;
        for (uint32_t c = n.firstChild; c != kInvalid; c = nodes_[c].nextSibling) {
            size += nodes_[c].totalSize;
            alloc += nodes_[c].totalAlloc;
        }
        n.totalSize = size;
        n.totalAlloc = alloc;
    }
}

void VolumeTree::sortChildrenBySizeDesc() {
    std::vector<uint32_t> children;
    for (uint32_t i = 0; i < nodes_.size(); ++i) {
        Node& n = nodes_[i];
        if (n.childCount < 2) continue;
        children.clear();
        for (uint32_t c = n.firstChild; c != kInvalid; c = nodes_[c].nextSibling)
            children.push_back(c);
        std::sort(children.begin(), children.end(), [this](uint32_t a, uint32_t b) {
            if (nodes_[a].totalSize != nodes_[b].totalSize)
                return nodes_[a].totalSize > nodes_[b].totalSize;
            return a < b;
        });
        n.firstChild = children.empty() ? kInvalid : children.front();
        for (size_t k = 0; k < children.size(); ++k) {
            nodes_[children[k]].nextSibling = (k + 1 < children.size()) ? children[k + 1] : kInvalid;
        }
    }
}

}  // namespace cs
