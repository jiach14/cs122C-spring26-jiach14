#include "src/include/ix.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace {
    const uint32_t IX_MAGIC = 0x49584254;   // IXBT
    const uint32_t NODE_MAGIC = 0x49584E44; // IXND
    const PeterDB::PageNum INVALID_PAGE = std::numeric_limits<PeterDB::PageNum>::max();

    struct MetaPage {
        uint32_t magic = IX_MAGIC;
        uint32_t initialized = 0;
        PeterDB::PageNum rootPage = INVALID_PAGE;
        uint32_t height = 0;
    };

    struct LeafEntry {
        std::vector<char> key;
        PeterDB::RID rid{};
    };

    struct Node {
        bool isLeaf = true;
        PeterDB::PageNum nextLeaf = INVALID_PAGE;
        std::vector<LeafEntry> entries;
        std::vector<std::vector<char> > keys;
        std::vector<PeterDB::PageNum> children;
    };

    struct SplitResult {
        bool didSplit = false;
        std::vector<char> promotedKey;
        PeterDB::PageNum rightPage = INVALID_PAGE;
    };

    struct LevelItem {
        PeterDB::PageNum page = INVALID_PAGE;
        std::vector<char> firstKey;

        LevelItem() = default;
        LevelItem(PeterDB::PageNum page, const std::vector<char> &firstKey) : page(page), firstKey(firstKey) {}
    };

    uint16_t readUInt16(const char *page, size_t &offset) {
        uint16_t value;
        std::memcpy(&value, page + offset, sizeof(value));
        offset += sizeof(value);
        return value;
    }

    uint32_t readUInt32(const char *page, size_t &offset) {
        uint32_t value;
        std::memcpy(&value, page + offset, sizeof(value));
        offset += sizeof(value);
        return value;
    }

    void writeUInt16(char *page, size_t &offset, uint16_t value) {
        std::memcpy(page + offset, &value, sizeof(value));
        offset += sizeof(value);
    }

    void writeUInt32(char *page, size_t &offset, uint32_t value) {
        std::memcpy(page + offset, &value, sizeof(value));
        offset += sizeof(value);
    }

    int keySize(const PeterDB::Attribute &attribute, const void *key) {
        if (key == nullptr) return 0;
        if (attribute.type == PeterDB::TypeInt || attribute.type == PeterDB::TypeReal) return 4;
        uint32_t len;
        std::memcpy(&len, key, sizeof(len));
        return 4 + static_cast<int>(len);
    }

    std::vector<char> copyKey(const PeterDB::Attribute &attribute, const void *key) {
        const int size = keySize(attribute, key);
        const char *begin = static_cast<const char *>(key);
        return std::vector<char>(begin, begin + size);
    }

    int compareKey(const PeterDB::Attribute &attribute, const std::vector<char> &a, const std::vector<char> &b) {
        if (attribute.type == PeterDB::TypeInt) {
            int av, bv;
            std::memcpy(&av, a.data(), sizeof(av));
            std::memcpy(&bv, b.data(), sizeof(bv));
            if (av < bv) return -1;
            if (av > bv) return 1;
            return 0;
        }
        if (attribute.type == PeterDB::TypeReal) {
            float av, bv;
            std::memcpy(&av, a.data(), sizeof(av));
            std::memcpy(&bv, b.data(), sizeof(bv));
            if (av < bv) return -1;
            if (av > bv) return 1;
            return 0;
        }

        uint32_t alen, blen;
        std::memcpy(&alen, a.data(), sizeof(alen));
        std::memcpy(&blen, b.data(), sizeof(blen));
        const int cmp = std::memcmp(a.data() + 4, b.data() + 4, std::min(alen, blen));
        if (cmp < 0) return -1;
        if (cmp > 0) return 1;
        if (alen < blen) return -1;
        if (alen > blen) return 1;
        return 0;
    }

    int compareRid(const PeterDB::RID &a, const PeterDB::RID &b) {
        if (a.pageNum < b.pageNum) return -1;
        if (a.pageNum > b.pageNum) return 1;
        if (a.slotNum < b.slotNum) return -1;
        if (a.slotNum > b.slotNum) return 1;
        return 0;
    }

    int compareEntry(const PeterDB::Attribute &attribute, const LeafEntry &a, const LeafEntry &b) {
        const int keyCmp = compareKey(attribute, a.key, b.key);
        if (keyCmp != 0) return keyCmp;
        return compareRid(a.rid, b.rid);
    }

    bool sameRid(const PeterDB::RID &a, const PeterDB::RID &b) {
        return a.pageNum == b.pageNum && a.slotNum == b.slotNum;
    }

    std::string keyToString(const PeterDB::Attribute &attribute, const std::vector<char> &key) {
        std::ostringstream out;
        if (attribute.type == PeterDB::TypeInt) {
            int value;
            std::memcpy(&value, key.data(), sizeof(value));
            out << value;
        } else if (attribute.type == PeterDB::TypeReal) {
            float value;
            std::memcpy(&value, key.data(), sizeof(value));
            out << std::setprecision(6) << value;
        } else {
            uint32_t len;
            std::memcpy(&len, key.data(), sizeof(len));
            for (uint32_t i = 0; i < len; ++i) {
                const char c = key[4 + i];
                if (c == '"' || c == '\\') out << '\\';
                out << c;
            }
        }
        return out.str();
    }

    size_t serializedNodeSize(const Node &node) {
        size_t size = 4 + 1 + 2 + 4;
        if (node.isLeaf) {
            for (const auto &entry: node.entries) {
                size += 2 + entry.key.size() + 4 + 2;
            }
        } else {
            size += 2 + node.children.size() * 4;
            for (const auto &key: node.keys) {
                size += 2 + key.size();
            }
        }
        return size;
    }

    bool nodeFits(const Node &node) {
        return serializedNodeSize(node) <= PAGE_SIZE;
    }

    void serializeNode(const Node &node, void *data) {
        char *page = static_cast<char *>(data);
        std::memset(page, 0, PAGE_SIZE);
        size_t offset = 0;
        writeUInt32(page, offset, NODE_MAGIC);
        page[offset++] = node.isLeaf ? 1 : 0;
        writeUInt16(page, offset, static_cast<uint16_t>(node.isLeaf ? node.entries.size() : node.keys.size()));
        writeUInt32(page, offset, node.nextLeaf);

        if (node.isLeaf) {
            for (const auto &entry: node.entries) {
                writeUInt16(page, offset, static_cast<uint16_t>(entry.key.size()));
                std::memcpy(page + offset, entry.key.data(), entry.key.size());
                offset += entry.key.size();
                writeUInt32(page, offset, entry.rid.pageNum);
                writeUInt16(page, offset, entry.rid.slotNum);
            }
        } else {
            writeUInt16(page, offset, static_cast<uint16_t>(node.children.size()));
            for (PeterDB::PageNum child: node.children) {
                writeUInt32(page, offset, child);
            }
            for (const auto &key: node.keys) {
                writeUInt16(page, offset, static_cast<uint16_t>(key.size()));
                std::memcpy(page + offset, key.data(), key.size());
                offset += key.size();
            }
        }
    }

    PeterDB::RC deserializeNode(const void *data, Node &node) {
        const char *page = static_cast<const char *>(data);
        size_t offset = 0;
        const uint32_t magic = readUInt32(page, offset);
        if (magic != NODE_MAGIC) return -1;

        node = Node{};
        node.isLeaf = page[offset++] != 0;
        const uint16_t count = readUInt16(page, offset);
        node.nextLeaf = readUInt32(page, offset);

        if (node.isLeaf) {
            node.entries.reserve(count);
            for (uint16_t i = 0; i < count; ++i) {
                const uint16_t keyLen = readUInt16(page, offset);
                LeafEntry entry;
                entry.key.assign(page + offset, page + offset + keyLen);
                offset += keyLen;
                entry.rid.pageNum = readUInt32(page, offset);
                entry.rid.slotNum = readUInt16(page, offset);
                node.entries.push_back(entry);
            }
        } else {
            const uint16_t childCount = readUInt16(page, offset);
            node.children.reserve(childCount);
            for (uint16_t i = 0; i < childCount; ++i) {
                node.children.push_back(readUInt32(page, offset));
            }
            node.keys.reserve(count);
            for (uint16_t i = 0; i < count; ++i) {
                const uint16_t keyLen = readUInt16(page, offset);
                node.keys.emplace_back(page + offset, page + offset + keyLen);
                offset += keyLen;
            }
        }
        return 0;
    }

    PeterDB::RC readMeta(PeterDB::IXFileHandle &handle, MetaPage &meta) {
        if (!handle.isOpen || handle.fileHandle.getNumberOfPages() == 0) return -1;
        char page[PAGE_SIZE];
        if (handle.fileHandle.readPage(0, page) != 0) return -1;
        std::memcpy(&meta, page, sizeof(MetaPage));
        return meta.magic == IX_MAGIC ? 0 : -1;
    }

    PeterDB::RC writeMeta(PeterDB::IXFileHandle &handle, const MetaPage &meta) {
        if (!handle.isOpen || handle.fileHandle.getNumberOfPages() == 0) return -1;
        char page[PAGE_SIZE];
        std::memset(page, 0, PAGE_SIZE);
        std::memcpy(page, &meta, sizeof(MetaPage));
        return handle.fileHandle.writePage(0, page);
    }

    PeterDB::RC readNode(PeterDB::IXFileHandle &handle, PeterDB::PageNum pageNum, Node &node) {
        char page[PAGE_SIZE];
        if (handle.fileHandle.readPage(pageNum, page) != 0) return -1;
        return deserializeNode(page, node);
    }

    PeterDB::RC writeNode(PeterDB::IXFileHandle &handle, PeterDB::PageNum pageNum, const Node &node) {
        char page[PAGE_SIZE];
        serializeNode(node, page);
        return handle.fileHandle.writePage(pageNum, page);
    }

    PeterDB::RC appendNode(PeterDB::IXFileHandle &handle, const Node &node, PeterDB::PageNum &pageNum) {
        char page[PAGE_SIZE];
        serializeNode(node, page);
        pageNum = handle.fileHandle.getNumberOfPages();
        return handle.fileHandle.appendPage(page);
    }

    uint32_t chooseChild(const PeterDB::Attribute &attribute, const Node &node, const std::vector<char> &key) {
        uint32_t i = 0;
        while (i < node.keys.size() && compareKey(attribute, key, node.keys[i]) >= 0) ++i;
        return i;
    }

    uint32_t chooseChildForDelete(const PeterDB::Attribute &attribute,
                          const Node &node,
                          const std::vector<char> &key) {
        uint32_t i = 0;
        while (i < node.keys.size() && compareKey(attribute, key, node.keys[i]) > 0) ++i;
        return i;
    }

    void sortLeafEntries(const PeterDB::Attribute &attribute, std::vector<LeafEntry> &entries) {
        std::sort(entries.begin(), entries.end(), [&](const LeafEntry &a, const LeafEntry &b) {
            return compareEntry(attribute, a, b) < 0;
        });
    }

    PeterDB::RC insertRecursive(PeterDB::IXFileHandle &handle, const PeterDB::Attribute &attribute,
                                PeterDB::PageNum pageNum, const LeafEntry &entry, SplitResult &split) {
        Node node;
        if (readNode(handle, pageNum, node) != 0) return -1;

        if (node.isLeaf) {
            node.entries.push_back(entry);
            sortLeafEntries(attribute, node.entries);

            auto duplicate = std::adjacent_find(node.entries.begin(), node.entries.end(),
                                                [&](const LeafEntry &a, const LeafEntry &b) {
                                                    return compareKey(attribute, a.key, b.key) == 0 &&
                                                           sameRid(a.rid, b.rid);
                                                });
            if (duplicate != node.entries.end()) return -1;

            if (nodeFits(node)) {
                split.didSplit = false;
                return writeNode(handle, pageNum, node);
            }

            Node right;
            right.isLeaf = true;
            const size_t splitAt = node.entries.size() / 2;
            right.entries.assign(node.entries.begin() + splitAt, node.entries.end());
            node.entries.erase(node.entries.begin() + splitAt, node.entries.end());
            right.nextLeaf = node.nextLeaf;

            PeterDB::PageNum rightPage;
            if (appendNode(handle, right, rightPage) != 0) return -1;
            node.nextLeaf = rightPage;
            if (writeNode(handle, pageNum, node) != 0) return -1;

            split.didSplit = true;
            split.promotedKey = right.entries.front().key;
            split.rightPage = rightPage;
            return 0;
        }

        const uint32_t childIndex = chooseChild(attribute, node, entry.key);
        SplitResult childSplit;
        if (insertRecursive(handle, attribute, node.children[childIndex], entry, childSplit) != 0) return -1;
        if (!childSplit.didSplit) {
            split.didSplit = false;
            return 0;
        }

        node.keys.insert(node.keys.begin() + childIndex, childSplit.promotedKey);
        node.children.insert(node.children.begin() + childIndex + 1, childSplit.rightPage);

        if (nodeFits(node)) {
            split.didSplit = false;
            return writeNode(handle, pageNum, node);
        }

        const size_t mid = node.keys.size() / 2;
        Node right;
        right.isLeaf = false;
        right.keys.assign(node.keys.begin() + mid + 1, node.keys.end());
        right.children.assign(node.children.begin() + mid + 1, node.children.end());

        std::vector<char> promoted = node.keys[mid];
        node.keys.erase(node.keys.begin() + mid, node.keys.end());
        node.children.erase(node.children.begin() + mid + 1, node.children.end());

        PeterDB::PageNum rightPage;
        if (appendNode(handle, right, rightPage) != 0) return -1;
        if (writeNode(handle, pageNum, node) != 0) return -1;

        split.didSplit = true;
        split.promotedKey = promoted;
        split.rightPage = rightPage;
        return 0;
    }

    PeterDB::PageNum leftmostLeaf(PeterDB::IXFileHandle &handle, PeterDB::PageNum rootPage) {
        Node node;
        PeterDB::PageNum page = rootPage;
        while (readNode(handle, page, node) == 0 && !node.isLeaf) {
            page = node.children.front();
        }
        return page;
    }

    PeterDB::RC collectSmallTreeEntries(PeterDB::IXFileHandle &handle, PeterDB::PageNum rootPage,
                                        size_t limit, std::vector<LeafEntry> &entries, bool &tooLarge) {
        entries.clear();
        tooLarge = false;

        PeterDB::PageNum page = leftmostLeaf(handle, rootPage);
        while (page != INVALID_PAGE) {
            Node leaf;
            if (readNode(handle, page, leaf) != 0 || !leaf.isLeaf) return -1;

            for (const LeafEntry &entry: leaf.entries) {
                entries.push_back(entry);
                if (entries.size() > limit) {
                    tooLarge = true;
                    return 0;
                }
            }
            page = leaf.nextLeaf;
        }
        return 0;
    }

    PeterDB::RC rebuildCompactTree(PeterDB::IXFileHandle &handle, const PeterDB::Attribute &attribute,
                                   const std::vector<LeafEntry> &sourceEntries, MetaPage &meta) {
        std::vector<LeafEntry> entries = sourceEntries;
        sortLeafEntries(attribute, entries);

        if (entries.empty()) {
            return 0;
        }

        std::vector<Node> leaves;
        Node currentLeaf;
        currentLeaf.isLeaf = true;

        for (const LeafEntry &entry: entries) {
            currentLeaf.entries.push_back(entry);
            if (!nodeFits(currentLeaf) && currentLeaf.entries.size() > 1) {
                currentLeaf.entries.pop_back();
                leaves.push_back(currentLeaf);

                currentLeaf = Node{};
                currentLeaf.isLeaf = true;
                currentLeaf.entries.push_back(entry);
            }
        }
        leaves.push_back(currentLeaf);

        const PeterDB::PageNum firstLeafPage = handle.fileHandle.getNumberOfPages();
        std::vector<LevelItem> level;
        level.reserve(leaves.size());
        for (size_t i = 0; i < leaves.size(); ++i) {
            leaves[i].nextLeaf = (i + 1 < leaves.size())
                                 ? static_cast<PeterDB::PageNum>(firstLeafPage + i + 1)
                                 : INVALID_PAGE;
            PeterDB::PageNum pageNum;
            if (appendNode(handle, leaves[i], pageNum) != 0) return -1;
            level.push_back({pageNum, leaves[i].entries.front().key});
        }

        uint32_t height = 0;
        while (level.size() > 1) {
            std::vector<Node> parents;
            std::vector<std::vector<char> > parentFirstKeys;

            Node parent;
            parent.isLeaf = false;
            std::vector<char> parentFirstKey;

            for (const LevelItem &child: level) {
                if (parent.children.empty()) {
                    parent.children.push_back(child.page);
                    parentFirstKey = child.firstKey;
                    continue;
                }

                parent.children.push_back(child.page);
                parent.keys.push_back(child.firstKey);

                if (!nodeFits(parent) && parent.children.size() > 2) {
                    parent.children.pop_back();
                    parent.keys.pop_back();
                    parents.push_back(parent);
                    parentFirstKeys.push_back(parentFirstKey);

                    parent = Node{};
                    parent.isLeaf = false;
                    parent.children.push_back(child.page);
                    parentFirstKey = child.firstKey;
                }
            }

            if (!parent.children.empty()) {
                parents.push_back(parent);
                parentFirstKeys.push_back(parentFirstKey);
            }

            std::vector<LevelItem> nextLevel;
            nextLevel.reserve(parents.size());
            for (size_t i = 0; i < parents.size(); ++i) {
                PeterDB::PageNum pageNum;
                if (appendNode(handle, parents[i], pageNum) != 0) return -1;
                nextLevel.push_back({pageNum, parentFirstKeys[i]});
            }

            level = nextLevel;
            ++height;
        }

        meta.rootPage = level.front().page;
        meta.height = height;
        return writeMeta(handle, meta);
    }

    PeterDB::RC compactSmallTreeAfterDelete(PeterDB::IXFileHandle &handle, const PeterDB::Attribute &attribute,
                                            MetaPage &meta) {
        if (meta.height == 0) return 0;
        if (handle.fileHandle.getNumberOfPages() > 256) return 0;

        const size_t rebuildLimit = 64;
        std::vector<LeafEntry> entries;
        bool tooLarge = false;
        if (collectSmallTreeEntries(handle, meta.rootPage, rebuildLimit, entries, tooLarge) != 0) return -1;
        if (tooLarge || entries.empty()) return 0;

        return rebuildCompactTree(handle, attribute, entries, meta);
    }

    bool belowLow(const PeterDB::IX_ScanIterator &it, const std::vector<char> &key) {
        if (!it.hasLowKey) return false;
        const int cmp = compareKey(it.attribute, key, it.lowKey);
        return it.lowKeyInclusive ? cmp < 0 : cmp <= 0;
    }

    bool aboveHigh(const PeterDB::IX_ScanIterator &it, const std::vector<char> &key) {
        if (!it.hasHighKey) return false;
        const int cmp = compareKey(it.attribute, key, it.highKey);
        return it.highKeyInclusive ? cmp > 0 : cmp >= 0;
    }

    bool afterLastReturned(const PeterDB::IX_ScanIterator &it, const LeafEntry &entry) {
        if (!it.hasLastReturned) return true;
        const int cmp = compareKey(it.attribute, entry.key, it.lastKey);
        if (cmp > 0) return true;
        if (cmp < 0) return false;
        return compareRid(entry.rid, it.lastRid) > 0;
    }

    void copyKeyOut(const PeterDB::Attribute &, const std::vector<char> &key, void *out) {
        std::memcpy(out, key.data(), key.size());
    }

    void printNode(PeterDB::IXFileHandle &handle, const PeterDB::Attribute &attribute,
                   PeterDB::PageNum pageNum, std::ostream &out) {
        Node node;
        readNode(handle, pageNum, node);

        out << "{\"keys\":[";
        if (node.isLeaf) {
            for (size_t i = 0; i < node.entries.size();) {
                if (i != 0) out << ",";
                out << "\"" << keyToString(attribute, node.entries[i].key) << ":[";
                size_t j = i;
                while (j < node.entries.size() &&
                       compareKey(attribute, node.entries[i].key, node.entries[j].key) == 0) {
                    if (j != i) out << ",";
                    out << "(" << node.entries[j].rid.pageNum << "," << node.entries[j].rid.slotNum << ")";
                    ++j;
                }
                out << "]\"";
                i = j;
            }
            out << "]}";
            return;
        }

        for (size_t i = 0; i < node.keys.size(); ++i) {
            if (i != 0) out << ",";
            out << "\"" << keyToString(attribute, node.keys[i]) << "\"";
        }
        out << "],\"children\":[";
        for (size_t i = 0; i < node.children.size(); ++i) {
            if (i != 0) out << ",";
            printNode(handle, attribute, node.children[i], out);
        }
        out << "]}";
    }
}

namespace PeterDB {
    IndexManager &IndexManager::instance() {
        static IndexManager _index_manager = IndexManager();
        return _index_manager;
    }

    RC IndexManager::createFile(const std::string &fileName) {
        return PagedFileManager::instance().createFile(fileName);
    }

    RC IndexManager::destroyFile(const std::string &fileName) {
        return PagedFileManager::instance().destroyFile(fileName);
    }

    RC IndexManager::openFile(const std::string &fileName, IXFileHandle &ixFileHandle) {
        if (ixFileHandle.isOpen) return -1;
        if (PagedFileManager::instance().openFile(fileName, ixFileHandle.fileHandle) != 0) return -1;
        ixFileHandle.isOpen = true;
        ixFileHandle.collectCounterValues(ixFileHandle.ixReadPageCounter, ixFileHandle.ixWritePageCounter,
                                           ixFileHandle.ixAppendPageCounter);
        return 0;
    }

    RC IndexManager::closeFile(IXFileHandle &ixFileHandle) {
        if (!ixFileHandle.isOpen) return -1;
        if (PagedFileManager::instance().closeFile(ixFileHandle.fileHandle) != 0) return -1;
        ixFileHandle.isOpen = false;
        return 0;
    }

    RC IndexManager::insertEntry(IXFileHandle &ixFileHandle, const Attribute &attribute, const void *key, const RID &rid) {
        if (!ixFileHandle.isOpen || key == nullptr) return -1;

        LeafEntry entry;
        entry.key = copyKey(attribute, key);
        entry.rid = rid;

        if (ixFileHandle.fileHandle.getNumberOfPages() == 0) {
            MetaPage meta;
            char metaPage[PAGE_SIZE];
            std::memset(metaPage, 0, PAGE_SIZE);
            std::memcpy(metaPage, &meta, sizeof(MetaPage));
            if (ixFileHandle.fileHandle.appendPage(metaPage) != 0) return -1;

            Node root;
            root.isLeaf = true;
            root.entries.push_back(entry);
            PeterDB::PageNum rootPage;
            if (appendNode(ixFileHandle, root, rootPage) != 0) return -1;

            meta.initialized = 1;
            meta.rootPage = rootPage;
            meta.height = 0;
            return writeMeta(ixFileHandle, meta);
        }

        MetaPage meta;
        if (readMeta(ixFileHandle, meta) != 0) return -1;
        if (!meta.initialized) return -1;

        SplitResult split;
        if (insertRecursive(ixFileHandle, attribute, meta.rootPage, entry, split) != 0) return -1;
        if (!split.didSplit) return 0;

        Node newRoot;
        newRoot.isLeaf = false;
        newRoot.keys.push_back(split.promotedKey);
        newRoot.children.push_back(meta.rootPage);
        newRoot.children.push_back(split.rightPage);
        PeterDB::PageNum newRootPage;
        if (appendNode(ixFileHandle, newRoot, newRootPage) != 0) return -1;

        meta.rootPage = newRootPage;
        meta.height += 1;
        return writeMeta(ixFileHandle, meta);
    }


    RC IndexManager::deleteEntry(IXFileHandle &ixFileHandle,
                                 const Attribute &attribute,
                                 const void *key,
                                 const RID &rid) {
        if (!ixFileHandle.isOpen || key == nullptr) return -1;

        MetaPage meta;
        if (readMeta(ixFileHandle, meta) != 0 || !meta.initialized) return -1;

        const std::vector<char> targetKey = copyKey(attribute, key);

        PageNum page = meta.rootPage;
        Node node;

        while (true) {
            if (readNode(ixFileHandle, page, node) != 0) return -1;

            if (node.isLeaf) break;

            uint32_t childIndex = chooseChildForDelete(attribute, node, targetKey);
            if (childIndex >= node.children.size()) return -1;

            page = node.children[childIndex];
        }

        while (page != INVALID_PAGE) {
            Node leaf = node;

            if (!leaf.isLeaf) return -1;

            for (auto it = leaf.entries.begin(); it != leaf.entries.end(); ++it) {
                const int cmp = compareKey(attribute, it->key, targetKey);

                if (cmp > 0) return -1;

                if (cmp == 0 && sameRid(it->rid, rid)) {
                    leaf.entries.erase(it);
                    if (writeNode(ixFileHandle, page, leaf) != 0) return -1;
                    return compactSmallTreeAfterDelete(ixFileHandle, attribute, meta);
                }
            }

            if (!leaf.entries.empty()) {
                const int lastCmp = compareKey(attribute, leaf.entries.back().key, targetKey);
                if (lastCmp > 0) return -1;
            }

            page = leaf.nextLeaf;
            if (page == INVALID_PAGE) break;

            if (readNode(ixFileHandle, page, node) != 0) return -1;
        }

        return -1;
    }

    RC IndexManager::scan(IXFileHandle &ixFileHandle,
                          const Attribute &attribute,
                          const void *lowKey,
                          const void *highKey,
                          bool lowKeyInclusive,
                          bool highKeyInclusive,
                          IX_ScanIterator &ix_ScanIterator) {
        if (!ixFileHandle.isOpen) return -1;
        ix_ScanIterator.close();

        ix_ScanIterator.ixFileHandle = &ixFileHandle;
        ix_ScanIterator.attribute = attribute;
        ix_ScanIterator.hasLowKey = lowKey != nullptr;
        ix_ScanIterator.hasHighKey = highKey != nullptr;
        ix_ScanIterator.lowKeyInclusive = lowKeyInclusive;
        ix_ScanIterator.highKeyInclusive = highKeyInclusive;
        ix_ScanIterator.lowKey.clear();
        ix_ScanIterator.highKey.clear();
        ix_ScanIterator.lastKey.clear();
        ix_ScanIterator.hasLastReturned = false;
        if (lowKey != nullptr) ix_ScanIterator.lowKey = copyKey(attribute, lowKey);
        if (highKey != nullptr) ix_ScanIterator.highKey = copyKey(attribute, highKey);

        MetaPage meta;
        if (readMeta(ixFileHandle, meta) != 0 || !meta.initialized) {
            ix_ScanIterator.currentLeafPage = INVALID_PAGE;
        } else {
            ix_ScanIterator.currentLeafPage = leftmostLeaf(ixFileHandle, meta.rootPage);
        }
        ix_ScanIterator.currentEntryIndex = 0;
        ix_ScanIterator.isOpen = true;
        return 0;
    }

    RC IndexManager::printBTree(IXFileHandle &ixFileHandle, const Attribute &attribute, std::ostream &out) const {
        if (!ixFileHandle.isOpen) return -1;
        MetaPage meta;
        if (readMeta(ixFileHandle, meta) != 0 || !meta.initialized) {
            out << "{\"keys\":[]}";
            return 0;
        }
        printNode(ixFileHandle, attribute, meta.rootPage, out);
        return 0;
    }

    IX_ScanIterator::IX_ScanIterator() = default;

    IX_ScanIterator::~IX_ScanIterator() {
        close();
    }

    RC IX_ScanIterator::getNextEntry(RID &rid, void *key) {
        if (!isOpen || ixFileHandle == nullptr || key == nullptr) return IX_EOF;

        while (currentLeafPage != INVALID_PAGE) {
            Node leaf;
            if (readNode(*ixFileHandle, currentLeafPage, leaf) != 0 || !leaf.isLeaf) return IX_EOF;

            size_t startIndex = currentEntryIndex;
            if (hasLastReturned && currentLeafPage == lastLeafPage) {
                bool foundLast = false;
                for (size_t i = 0; i < leaf.entries.size(); ++i) {
                    if (compareKey(attribute, leaf.entries[i].key, lastKey) == 0 &&
                        sameRid(leaf.entries[i].rid, lastRid)) {
                        startIndex = i + 1;
                        foundLast = true;
                        break;
                    }
                }
                if (!foundLast && currentEntryIndex > 0) {
                    startIndex = currentEntryIndex - 1;
                }
            }

            for (size_t i = startIndex; i < leaf.entries.size(); ++i) {
                const LeafEntry &entry = leaf.entries[i];
                if (belowLow(*this, entry.key)) continue;
                if (aboveHigh(*this, entry.key)) return IX_EOF;

                rid = entry.rid;
                copyKeyOut(attribute, entry.key, key);
                lastKey = entry.key;
                lastRid = entry.rid;
                lastLeafPage = currentLeafPage;
                hasLastReturned = true;
                currentEntryIndex = static_cast<uint32_t>(i + 1);
                return 0;
            }

            currentLeafPage = leaf.nextLeaf;
            currentEntryIndex = 0;
        }
        return IX_EOF;
    }

    RC IX_ScanIterator::close() {
        ixFileHandle = nullptr;
        lowKey.clear();
        highKey.clear();
        lastKey.clear();
        hasLowKey = false;
        hasHighKey = false;
        hasLastReturned = false;
        lastLeafPage = INVALID_PAGE;
        currentLeafPage = INVALID_PAGE;
        currentEntryIndex = 0;
        isOpen = false;
        return 0;
    }

    IXFileHandle::IXFileHandle() {
        ixReadPageCounter = 0;
        ixWritePageCounter = 0;
        ixAppendPageCounter = 0;
    }

    IXFileHandle::~IXFileHandle() = default;

    RC
    IXFileHandle::collectCounterValues(unsigned &readPageCount, unsigned &writePageCount, unsigned &appendPageCount) {
        if (!isOpen) return -1;
        if (fileHandle.collectCounterValues(readPageCount, writePageCount, appendPageCount) != 0) return -1;
        ixReadPageCounter = readPageCount;
        ixWritePageCounter = writePageCount;
        ixAppendPageCounter = appendPageCount;
        return 0;
    }



} // namespace PeterDB
