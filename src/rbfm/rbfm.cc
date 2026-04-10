#include "src/include/rbfm.h"

#include <cstdint>

namespace {
    using namespace PeterDB;

    // Deleted slots are reusable and should not return records.
    constexpr int16_t DELETED_SLOT = -1;
    // Tombstone slots store a forwarding RID for records moved by updateRecord.
    constexpr int16_t TOMBSTONE_SLOT = -2;
    constexpr int16_t INVALID_OFFSET = -1;
    // The project spec defines an RID as 4-byte pageNum + 2-byte slotNum.
    constexpr int16_t FORWARDING_RID_SIZE = 6;

    struct PageHeader {
        uint16_t freeSpaceOffset;
        uint16_t slotCount;
    };

    struct SlotEntry {
        int16_t offset;
        int16_t length;
    };

    inline uint32_t getNullIndicatorSize(uint32_t fieldCount) {
        return (fieldCount + 7) / 8;
    }

    inline bool fieldIsNull(const unsigned char *nullIndicator, uint32_t i) {
        return nullIndicator[i / 8] & (1 << (7 - (i % 8)));
    }

    inline void initializeNewPage(void *page) {
        memset(page, 0, PAGE_SIZE);
        auto *header = reinterpret_cast<PageHeader *>(page);
        header->freeSpaceOffset = sizeof(PageHeader);
        header->slotCount = 0;
    }

    inline PageHeader *getPageHeader(void *page) {
        return reinterpret_cast<PageHeader *>(page);
    }

    inline const PageHeader *getPageHeader(const void *page) {
        return reinterpret_cast<const PageHeader *>(page);
    }

    inline SlotEntry *getSlotEntry(void *page, uint16_t slotNum) {
        char *base = static_cast<char *>(page);
        return reinterpret_cast<SlotEntry *>(
            base + PAGE_SIZE - static_cast<int>((slotNum + 1) * sizeof(SlotEntry))
        );
    }

    inline const SlotEntry *getSlotEntry(const void *page, uint16_t slotNum) {
        const char *base = static_cast<const char *>(page);
        return reinterpret_cast<const SlotEntry *>(
            base + PAGE_SIZE - static_cast<int>((slotNum + 1) * sizeof(SlotEntry))
        );
    }

    inline uint32_t getFreeSpaceSize(const void *page) {
        const PageHeader *header = getPageHeader(page);
        uint32_t slotDirectoryStart = PAGE_SIZE - header->slotCount * sizeof(SlotEntry);
        return slotDirectoryStart - header->freeSpaceOffset;
    }

    inline bool findDeletedSlot(const void *page, uint16_t &slotNum) {
        const PageHeader *header = getPageHeader(page);
        for (uint16_t i = 0; i < header->slotCount; i++) {
            const SlotEntry *slot = getSlotEntry(page, i);
            if (slot->length == DELETED_SLOT) {
                slotNum = i;
                return true;
            }
        }

        return false;
    }

    inline void writeForwardingRID(void *page, const SlotEntry *slot, const RID &rid) {
        char *ptr = static_cast<char *>(page) + slot->offset;
        uint32_t pageNum = static_cast<uint32_t>(rid.pageNum);
        uint16_t slotNum = static_cast<uint16_t>(rid.slotNum);
        memcpy(ptr, &pageNum, sizeof(pageNum));
        memcpy(ptr + sizeof(pageNum), &slotNum, sizeof(slotNum));
    }

    inline RID readForwardingRID(const void *page, const SlotEntry *slot) {
        const char *ptr = static_cast<const char *>(page) + slot->offset;
        uint32_t pageNum = 0;
        uint16_t slotNum = 0;
        memcpy(&pageNum, ptr, sizeof(pageNum));
        memcpy(&slotNum, ptr + sizeof(pageNum), sizeof(slotNum));
        return RID{pageNum, slotNum};
    }

    uint32_t getRecordSize(const std::vector<Attribute> &recordDescriptor, const void *data) {
        uint32_t fieldCount = static_cast<uint32_t>(recordDescriptor.size());
        uint32_t nullBytes = getNullIndicatorSize(fieldCount);

        const char *ptr = static_cast<const char *>(data);
        const unsigned char *nullIndicator = reinterpret_cast<const unsigned char *>(ptr);

        uint32_t offset = nullBytes;

        for (uint32_t i = 0; i < fieldCount; i++) {
            if (fieldIsNull(nullIndicator, i)) {
                continue;
            }

            switch (recordDescriptor[i].type) {
                case TypeInt:
                case TypeReal:
                    offset += 4;
                    break;
                case TypeVarChar: {
                    uint32_t len = 0;
                    memcpy(&len, ptr + offset, 4);
                    offset += 4 + len;
                    break;
                }
                default:
                    break;
            }
        }

        return offset;
    }

    int findPageWithSpace(FileHandle &fileHandle, uint32_t recordSize) {
        unsigned numPages = fileHandle.getNumberOfPages();
        void *page = malloc(PAGE_SIZE);
        if (page == nullptr) {
            return -1;
        }

        if (numPages > 0) {
            PageNum lastPage = numPages - 1;
            if (fileHandle.readPage(lastPage, page) == 0) {
                uint16_t deletedSlot = 0;
                uint32_t neededSpace = findDeletedSlot(page, deletedSlot)
                                       ? recordSize
                                       : recordSize + sizeof(SlotEntry);
                if (getFreeSpaceSize(page) >= neededSpace) {
                    free(page);
                    return static_cast<int>(lastPage);
                }
            }
        }

        for (PageNum i = 0; i < numPages; i++) {
            if (numPages > 0 && i == numPages - 1) {
                continue; // already checked last page
            }

            if (fileHandle.readPage(i, page) != 0) {
                continue;
            }

            uint16_t deletedSlot = 0;
            uint32_t neededSpace = findDeletedSlot(page, deletedSlot)
                                   ? recordSize
                                   : recordSize + sizeof(SlotEntry);
            if (getFreeSpaceSize(page) >= neededSpace) {
                free(page);
                return static_cast<int>(i);
            }
        }

        free(page);
        return -1;
    }
}





namespace PeterDB {
    RecordBasedFileManager &RecordBasedFileManager::instance() {
        static RecordBasedFileManager _rbf_manager = RecordBasedFileManager();
        return _rbf_manager;
    }

    RecordBasedFileManager::RecordBasedFileManager() = default;

    RecordBasedFileManager::~RecordBasedFileManager() = default;

    RecordBasedFileManager::RecordBasedFileManager(const RecordBasedFileManager &) = default;

    RecordBasedFileManager &RecordBasedFileManager::operator=(const RecordBasedFileManager &) = default;

    RC RecordBasedFileManager::createFile(const std::string &fileName) {
        return PagedFileManager::instance().createFile(fileName);
    }

    RC RecordBasedFileManager::destroyFile(const std::string &fileName) {
        return PagedFileManager::instance().destroyFile(fileName);
    }

    RC RecordBasedFileManager::openFile(const std::string &fileName, FileHandle &fileHandle) {
        return PagedFileManager::instance().openFile(fileName, fileHandle);
    }

    RC RecordBasedFileManager::closeFile(FileHandle &fileHandle) {
        return PagedFileManager::instance().closeFile(fileHandle);
    }

    RC RecordBasedFileManager::insertRecord(FileHandle &fileHandle, 
                                            const std::vector<Attribute> &recordDescriptor,
                                            const void *data, RID &rid) {
        if (data == nullptr) {
            return -1;
        }

        uint32_t recordSize = getRecordSize(recordDescriptor, data);
        if (recordSize + sizeof(PageHeader) + sizeof(SlotEntry) > PAGE_SIZE) {
            return -1;
        }

        int targetPageNum = findPageWithSpace(fileHandle, recordSize);

        void *page = malloc(PAGE_SIZE);
        if (page == nullptr) {
            return -1;
        }

        if (targetPageNum == -1) {
            // need a new page
            initializeNewPage(page);

            PageHeader *header = getPageHeader(page);
            uint16_t slotNum = header->slotCount;

            memcpy(static_cast<char *>(page) + header->freeSpaceOffset, data, recordSize);

            SlotEntry *slot = getSlotEntry(page, slotNum);
            slot->offset = static_cast<int16_t>(header->freeSpaceOffset);
            slot->length = static_cast<int16_t>(recordSize);

            header->freeSpaceOffset = static_cast<uint16_t>(header->freeSpaceOffset + recordSize);
            header->slotCount = static_cast<uint16_t>(header->slotCount + 1);

            if (fileHandle.appendPage(page) != 0) {
                free(page);
                return -1;
            }

            rid.pageNum = fileHandle.getNumberOfPages() - 1;
            rid.slotNum = slotNum;
            free(page);
            return 0;
        }

        // insert into existing page
        if (fileHandle.readPage(static_cast<PageNum>(targetPageNum), page) != 0) {
            free(page);
            return -1;
        }

        PageHeader *header = getPageHeader(page);
        uint16_t slotNum = header->slotCount;
        bool reuseDeletedSlot = findDeletedSlot(page, slotNum);

        memcpy(static_cast<char *>(page) + header->freeSpaceOffset, data, recordSize);

        SlotEntry *slot = getSlotEntry(page, slotNum);
        slot->offset = static_cast<int16_t>(header->freeSpaceOffset);
        slot->length = static_cast<int16_t>(recordSize);

        header->freeSpaceOffset = static_cast<uint16_t>(header->freeSpaceOffset + recordSize);
        if (!reuseDeletedSlot) {
            header->slotCount = static_cast<uint16_t>(header->slotCount + 1);
        }

        if (fileHandle.writePage(static_cast<PageNum>(targetPageNum), page) != 0) {
            free(page);
            return -1;
        }

        rid.pageNum = static_cast<PageNum>(targetPageNum);
        rid.slotNum = slotNum;

        free(page);
        return 0;
    }

    RC RecordBasedFileManager::readRecord(FileHandle &fileHandle, 
                                        const std::vector<Attribute> &recordDescriptor,
                                        const RID &rid, void *data) {
        (void)recordDescriptor;

        if (data == nullptr) {
            return -1;
        }

        void *page = malloc(PAGE_SIZE);
        if (page == nullptr) {
            return -1;
        }

        if (fileHandle.readPage(rid.pageNum, page) != 0) {
            free(page);
            return -1;
        }

        PageHeader *header = getPageHeader(page);
        if (rid.slotNum >= header->slotCount) {
            free(page);
            return -1;
        }

        SlotEntry *slot = getSlotEntry(page, rid.slotNum);
        if (slot->length == TOMBSTONE_SLOT) {
            RID forwardedRID = readForwardingRID(page, slot);
            free(page);
            return readRecord(fileHandle, recordDescriptor, forwardedRID, data);
        }

        if (slot->length <= 0 || slot->offset < static_cast<int16_t>(sizeof(PageHeader))) {
            free(page);
            return -1;
        }

        memcpy(data, static_cast<char *>(page) + slot->offset, slot->length);

        free(page);
        return 0;
    }

    RC RecordBasedFileManager::deleteRecord(FileHandle &fileHandle, const std::vector<Attribute> &recordDescriptor,
                                            const RID &rid) {
        void *page = malloc(PAGE_SIZE);
        if (page == nullptr) {
            return -1;
        }

        if (fileHandle.readPage(rid.pageNum, page) != 0) {
            free(page);
            return -1;
        }

        PageHeader *header = getPageHeader(page);
        if (rid.slotNum >= header->slotCount) {
            free(page);
            return -1;
        }

        SlotEntry *slot = getSlotEntry(page, rid.slotNum);
        if (slot->length == DELETED_SLOT || slot->offset < static_cast<int16_t>(sizeof(PageHeader))) {
            free(page);
            return -1;
        }

        if (slot->length == TOMBSTONE_SLOT) {
            RID forwardedRID = readForwardingRID(page, slot);
            deleteRecord(fileHandle, recordDescriptor, forwardedRID);
        }

        slot->offset = INVALID_OFFSET;
        slot->length = DELETED_SLOT;

        RC rc = fileHandle.writePage(rid.pageNum, page);
        free(page);
        return rc;
    }

    RC RecordBasedFileManager::printRecord(const std::vector<Attribute> &recordDescriptor, const void *data,
                                           std::ostream &out) {
        if (data == nullptr) {
            return -1;
        }

        uint32_t fieldCount = static_cast<uint32_t>(recordDescriptor.size());
        uint32_t nullBytes = getNullIndicatorSize(fieldCount);

        const char *ptr = static_cast<const char *>(data);
        const unsigned char *nullIndicator = reinterpret_cast<const unsigned char *>(ptr);

        uint32_t offset = nullBytes;

        for (uint32_t i = 0; i < fieldCount; i++) {
            if (i > 0) {
                out << ", ";
            }

            out << recordDescriptor[i].name << ": ";

            if (fieldIsNull(nullIndicator, i)) {
                out << "NULL";
                continue;
            }

            switch (recordDescriptor[i].type) {
            case TypeInt: {
                    int value = 0;
                    memcpy(&value, ptr + offset, 4);
                    offset += 4;
                    out << value;
                    break;
            }

            case TypeReal: {
                    float value = 0.0f;
                    memcpy(&value, ptr + offset, 4);
                    offset += 4;
                    out << value;
                    break;
            }

            case TypeVarChar: {
                    uint32_t len = 0;
                    memcpy(&len, ptr + offset, 4);
                    offset += 4;

                    std::string s(ptr + offset, ptr + offset + len);
                    offset += len;

                    out << s;
                    break;
            }

            default:
                return -1;
            }
        }

        out << "\n";
        return 0;
    }

    RC RecordBasedFileManager::updateRecord(FileHandle &fileHandle, const std::vector<Attribute> &recordDescriptor,
                                            const void *data, const RID &rid) {
        if (data == nullptr) {
            return -1;
        }

        uint32_t recordSize = getRecordSize(recordDescriptor, data);
        if (recordSize + sizeof(PageHeader) + sizeof(SlotEntry) > PAGE_SIZE) {
            return -1;
        }

        void *page = malloc(PAGE_SIZE);
        if (page == nullptr) {
            return -1;
        }

        if (fileHandle.readPage(rid.pageNum, page) != 0) {
            free(page);
            return -1;
        }

        PageHeader *header = getPageHeader(page);
        if (rid.slotNum >= header->slotCount) {
            free(page);
            return -1;
        }

        SlotEntry *slot = getSlotEntry(page, rid.slotNum);
        if (slot->length == DELETED_SLOT || slot->offset < static_cast<int16_t>(sizeof(PageHeader))) {
            free(page);
            return -1;
        }

        if (slot->length == TOMBSTONE_SLOT) {
            RID forwardedRID = readForwardingRID(page, slot);
            free(page);
            return updateRecord(fileHandle, recordDescriptor, data, forwardedRID);
        }

        if (recordSize <= static_cast<uint32_t>(slot->length)) {
            memcpy(static_cast<char *>(page) + slot->offset, data, recordSize);
            slot->length = static_cast<int16_t>(recordSize);

            RC rc = fileHandle.writePage(rid.pageNum, page);
            free(page);
            return rc;
        }

        if (slot->length < FORWARDING_RID_SIZE) {
            free(page);
            return -1;
        }

        // Grow the record by inserting the new bytes elsewhere, then leave
        // this original RID as a tombstone pointing to the new RID.
        RID forwardedRID;
        if (insertRecord(fileHandle, recordDescriptor, data, forwardedRID) != 0) {
            free(page);
            return -1;
        }

        if (forwardedRID.pageNum == rid.pageNum && fileHandle.readPage(rid.pageNum, page) != 0) {
            free(page);
            return -1;
        }

        slot = getSlotEntry(page, rid.slotNum);
        writeForwardingRID(page, slot, forwardedRID);
        slot->length = TOMBSTONE_SLOT;

        RC rc = fileHandle.writePage(rid.pageNum, page);
        free(page);
        return rc;
    }

    RC RecordBasedFileManager::readAttribute(FileHandle &fileHandle, const std::vector<Attribute> &recordDescriptor,
                                             const RID &rid, const std::string &attributeName, void *data) {
        return -1;
    }

    RC RecordBasedFileManager::scan(FileHandle &fileHandle, const std::vector<Attribute> &recordDescriptor,
                                    const std::string &conditionAttribute, const CompOp compOp, const void *value,
                                    const std::vector<std::string> &attributeNames,
                                    RBFM_ScanIterator &rbfm_ScanIterator) {
        return -1;
    }

} // namespace PeterDB
