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
    constexpr uint32_t INT_FIELD_SIZE = 4;
    constexpr uint32_t REAL_FIELD_SIZE = 4;
    constexpr uint32_t VARCHAR_LENGTH_PREFIX_SIZE = 4;
    constexpr uint32_t FIELD_OFFSET_SIZE = sizeof(uint16_t);

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

    uint32_t getApiRecordSize(const std::vector<Attribute> &recordDescriptor, const void *data) {
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
                    offset += INT_FIELD_SIZE;
                    break;
                case TypeReal:
                    offset += REAL_FIELD_SIZE;
                    break;
                case TypeVarChar: {
                    uint32_t len = 0;
                    memcpy(&len, ptr + offset, VARCHAR_LENGTH_PREFIX_SIZE);
                    offset += VARCHAR_LENGTH_PREFIX_SIZE + len;
                    break;
                }
                default:
                    break;
            }
        }

        return offset;
    }

    inline uint32_t getInternalHeaderSize(uint32_t fieldCount) {
        return getNullIndicatorSize(fieldCount) + fieldCount * FIELD_OFFSET_SIZE;
    }

    inline void setFieldEnd(char *record, uint32_t nullBytes, uint32_t fieldNum, uint16_t fieldEnd) {
        memcpy(record + nullBytes + fieldNum * FIELD_OFFSET_SIZE, &fieldEnd, FIELD_OFFSET_SIZE);
    }

    inline uint16_t getFieldEnd(const char *record, uint32_t nullBytes, uint32_t fieldNum) {
        uint16_t fieldEnd = 0;
        memcpy(&fieldEnd, record + nullBytes + fieldNum * FIELD_OFFSET_SIZE, FIELD_OFFSET_SIZE);
        return fieldEnd;
    }

    uint32_t getInternalRecordSize(const std::vector<Attribute> &recordDescriptor, const void *data) {
        return getInternalHeaderSize(static_cast<uint32_t>(recordDescriptor.size())) +
               getApiRecordSize(recordDescriptor, data) -
               getNullIndicatorSize(static_cast<uint32_t>(recordDescriptor.size()));
    }

    void formatRecordForStorage(const std::vector<Attribute> &recordDescriptor, const void *data, void *record) {
        uint32_t fieldCount = static_cast<uint32_t>(recordDescriptor.size());
        uint32_t nullBytes = getNullIndicatorSize(fieldCount);
        uint32_t dataOffset = getInternalHeaderSize(fieldCount);

        const char *input = static_cast<const char *>(data);
        char *output = static_cast<char *>(record);
        const unsigned char *nullIndicator = reinterpret_cast<const unsigned char *>(input);

        memcpy(output, input, nullBytes);

        uint32_t inputOffset = nullBytes;

        // Each end offset lets a field be located directly without scanning prior fields.
        for (uint32_t i = 0; i < fieldCount; i++) {
            if (!fieldIsNull(nullIndicator, i)) {
                uint32_t fieldSize = 0;
                switch (recordDescriptor[i].type) {
                    case TypeInt:
                        fieldSize = INT_FIELD_SIZE;
                        break;
                    case TypeReal:
                        fieldSize = REAL_FIELD_SIZE;
                        break;
                    case TypeVarChar: {
                        uint32_t len = 0;
                        memcpy(&len, input + inputOffset, VARCHAR_LENGTH_PREFIX_SIZE);
                        fieldSize = VARCHAR_LENGTH_PREFIX_SIZE + len;
                        break;
                    }
                    default:
                        break;
                }

                memcpy(output + dataOffset, input + inputOffset, fieldSize);
                inputOffset += fieldSize;
                dataOffset += fieldSize;
            }

            setFieldEnd(output, nullBytes, i, static_cast<uint16_t>(dataOffset));
        }
    }

    void formatRecordForApi(const std::vector<Attribute> &recordDescriptor, const void *record, void *data) {
        uint32_t fieldCount = static_cast<uint32_t>(recordDescriptor.size());
        uint32_t nullBytes = getNullIndicatorSize(fieldCount);
        uint32_t apiOffset = nullBytes;

        const char *input = static_cast<const char *>(record);
        char *output = static_cast<char *>(data);
        const unsigned char *nullIndicator = reinterpret_cast<const unsigned char *>(input);

        memcpy(output, input, nullBytes);

        for (uint32_t i = 0; i < fieldCount; i++) {
            uint32_t fieldStart = (i == 0) ? getInternalHeaderSize(fieldCount) : getFieldEnd(input, nullBytes, i - 1);
            uint32_t fieldEnd = getFieldEnd(input, nullBytes, i);
            uint32_t fieldSize = fieldEnd - fieldStart;

            if (!fieldIsNull(nullIndicator, i)) {
                memcpy(output + apiOffset, input + fieldStart, fieldSize);
                apiOffset += fieldSize;
            }
        }
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

        uint32_t recordSize = getInternalRecordSize(recordDescriptor, data);
        if (recordSize + sizeof(PageHeader) + sizeof(SlotEntry) > PAGE_SIZE) {
            return -1;
        }

        void *record = malloc(recordSize);
        if (record == nullptr) {
            return -1;
        }
        formatRecordForStorage(recordDescriptor, data, record);

        int targetPageNum = findPageWithSpace(fileHandle, recordSize);

        void *page = malloc(PAGE_SIZE);
        if (page == nullptr) {
            free(record);
            return -1;
        }

        if (targetPageNum == -1) {
            // need a new page
            initializeNewPage(page);

            PageHeader *header = getPageHeader(page);
            uint16_t slotNum = header->slotCount;

            memcpy(static_cast<char *>(page) + header->freeSpaceOffset, record, recordSize);

            SlotEntry *slot = getSlotEntry(page, slotNum);
            slot->offset = static_cast<int16_t>(header->freeSpaceOffset);
            slot->length = static_cast<int16_t>(recordSize);

            header->freeSpaceOffset = static_cast<uint16_t>(header->freeSpaceOffset + recordSize);
            header->slotCount = static_cast<uint16_t>(header->slotCount + 1);

            if (fileHandle.appendPage(page) != 0) {
                free(record);
                free(page);
                return -1;
            }

            rid.pageNum = fileHandle.getNumberOfPages() - 1;
            rid.slotNum = slotNum;
            free(record);
            free(page);
            return 0;
        }

        // insert into existing page
        if (fileHandle.readPage(static_cast<PageNum>(targetPageNum), page) != 0) {
            free(record);
            free(page);
            return -1;
        }

        PageHeader *header = getPageHeader(page);
        uint16_t slotNum = header->slotCount;
        bool reuseDeletedSlot = findDeletedSlot(page, slotNum);

        memcpy(static_cast<char *>(page) + header->freeSpaceOffset, record, recordSize);

        SlotEntry *slot = getSlotEntry(page, slotNum);
        slot->offset = static_cast<int16_t>(header->freeSpaceOffset);
        slot->length = static_cast<int16_t>(recordSize);

        header->freeSpaceOffset = static_cast<uint16_t>(header->freeSpaceOffset + recordSize);
        if (!reuseDeletedSlot) {
            header->slotCount = static_cast<uint16_t>(header->slotCount + 1);
        }

        if (fileHandle.writePage(static_cast<PageNum>(targetPageNum), page) != 0) {
            free(record);
            free(page);
            return -1;
        }

        rid.pageNum = static_cast<PageNum>(targetPageNum);
        rid.slotNum = slotNum;

        free(record);
        free(page);
        return 0;
    }

    RC RecordBasedFileManager::readRecord(FileHandle &fileHandle, 
                                        const std::vector<Attribute> &recordDescriptor,
                                        const RID &rid, void *data) {
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

        formatRecordForApi(recordDescriptor, static_cast<char *>(page) + slot->offset, data);

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

        int16_t deletedOffset = slot->offset;
        int16_t deletedLength = slot->length;
        if (slot->length == TOMBSTONE_SLOT) {
            RID forwardedRID = readForwardingRID(page, slot);
            if (deleteRecord(fileHandle, recordDescriptor, forwardedRID) != 0) {
                free(page);
                return -1;
            }

            if (forwardedRID.pageNum == rid.pageNum && fileHandle.readPage(rid.pageNum, page) != 0) {
                free(page);
                return -1;
            }

            header = getPageHeader(page);
            slot = getSlotEntry(page, rid.slotNum);
            deletedOffset = slot->offset;
            deletedLength = FORWARDING_RID_SIZE;
        }


        int16_t bytesToMove = static_cast<int16_t>(header->freeSpaceOffset - (deletedOffset + deletedLength));
        if (bytesToMove > 0) {
            // Close the hole left by the deleted record or tombstone payload.
            memmove(static_cast<char *>(page) + deletedOffset,                      
                    static_cast<char *>(page) + deletedOffset + deletedLength,
                    bytesToMove);
        }

        for (uint16_t i = 0; i < header->slotCount; i++) {
            if (i == rid.slotNum) {
                continue;
            }

            SlotEntry *otherSlot = getSlotEntry(page, i);
            if (otherSlot->length == DELETED_SLOT || otherSlot->offset < static_cast<int16_t>(sizeof(PageHeader))) { // skip deleted slots
                continue;
            }

            if (otherSlot->offset > deletedOffset) {
                otherSlot->offset -= deletedLength;
            }
        }

        header->freeSpaceOffset -= deletedLength;   // update free space offset after shifting records
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
                    memcpy(&value, ptr + offset, INT_FIELD_SIZE);
                    offset += INT_FIELD_SIZE;
                    out << value;
                    break;
            }

            case TypeReal: {
                    float value = 0.0f;
                    memcpy(&value, ptr + offset, REAL_FIELD_SIZE);
                    offset += REAL_FIELD_SIZE;
                    out << value;
                    break;
            }

            case TypeVarChar: {
                    uint32_t len = 0;
                    memcpy(&len, ptr + offset, VARCHAR_LENGTH_PREFIX_SIZE);
                    offset += VARCHAR_LENGTH_PREFIX_SIZE;

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

        uint32_t recordSize = getInternalRecordSize(recordDescriptor, data);
        if (recordSize + sizeof(PageHeader) + sizeof(SlotEntry) > PAGE_SIZE) {
            return -1;
        }

        void *record = malloc(recordSize);
        if (record == nullptr) {
            return -1;
        }
        formatRecordForStorage(recordDescriptor, data, record);

        void *page = malloc(PAGE_SIZE);
        if (page == nullptr) {
            free(record);
            return -1;
        }

        if (fileHandle.readPage(rid.pageNum, page) != 0) {
            free(record);
            free(page);
            return -1;
        }

        PageHeader *header = getPageHeader(page);
        if (rid.slotNum >= header->slotCount) {
            free(record);
            free(page);
            return -1;
        }

        SlotEntry *slot = getSlotEntry(page, rid.slotNum);
        if (slot->length == DELETED_SLOT || slot->offset < static_cast<int16_t>(sizeof(PageHeader))) {
            free(record);
            free(page);
            return -1;
        }

        if (slot->length == TOMBSTONE_SLOT) {
            RID forwardedRID = readForwardingRID(page, slot);
            free(record);
            free(page);
            return updateRecord(fileHandle, recordDescriptor, data, forwardedRID);
        }

        if (recordSize <= static_cast<uint32_t>(slot->length)) {
            memcpy(static_cast<char *>(page) + slot->offset, record, recordSize);
            slot->length = static_cast<int16_t>(recordSize);

            RC rc = fileHandle.writePage(rid.pageNum, page);
            free(record);
            free(page);
            return rc;
        }

        if (slot->length < FORWARDING_RID_SIZE) {
            free(record);
            free(page);
            return -1;
        }

        // Grow the record by inserting the new bytes elsewhere, then leave
        // this original RID as a tombstone pointing to the new RID.
        RID forwardedRID;
        if (insertRecord(fileHandle, recordDescriptor, data, forwardedRID) != 0) {
            free(record);
            free(page);
            return -1;
        }

        if (forwardedRID.pageNum == rid.pageNum && fileHandle.readPage(rid.pageNum, page) != 0) {
            free(record);
            free(page);
            return -1;
        }

        slot = getSlotEntry(page, rid.slotNum);
        writeForwardingRID(page, slot, forwardedRID);
        slot->length = TOMBSTONE_SLOT;

        RC rc = fileHandle.writePage(rid.pageNum, page);
        free(record);
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
