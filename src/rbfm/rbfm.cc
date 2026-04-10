#include "src/include/rbfm.h"

namespace {
    using namespace PeterDB;

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

    int findPageWithSpace(FileHandle &fileHandle, uint32_t neededSpace) {
        unsigned numPages = fileHandle.getNumberOfPages();
        void *page = malloc(PAGE_SIZE);
        if (page == nullptr) {
            return -1;
        }

        if (numPages > 0) {
            PageNum lastPage = numPages - 1;
            if (fileHandle.readPage(lastPage, page) == 0) {
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
        uint32_t neededSpace = recordSize + sizeof(SlotEntry);

        if (neededSpace + sizeof(PageHeader) > PAGE_SIZE) {
            return -1;
        }

        int targetPageNum = findPageWithSpace(fileHandle, neededSpace);

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

        memcpy(static_cast<char *>(page) + header->freeSpaceOffset, data, recordSize);

        SlotEntry *slot = getSlotEntry(page, slotNum);
        slot->offset = static_cast<int16_t>(header->freeSpaceOffset);
        slot->length = static_cast<int16_t>(recordSize);

        header->freeSpaceOffset = static_cast<uint16_t>(header->freeSpaceOffset + recordSize);
        header->slotCount = static_cast<uint16_t>(header->slotCount + 1);

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
        return -1;
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
        return -1;
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

