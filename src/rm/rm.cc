#include "src/include/rm.h"

#include <cstdint>
#include <cstring>

namespace {
    using namespace PeterDB;

    const std::string TABLES_TABLE_NAME = "Tables";
    const std::string COLUMNS_TABLE_NAME = "Columns";

    constexpr int32_t TABLES_TABLE_ID = 1;
    constexpr int32_t COLUMNS_TABLE_ID = 2;
    constexpr uint32_t VARCHAR_CATALOG_LENGTH = 50;
    constexpr uint32_t INT_SIZE = 4;

    uint32_t getNullIndicatorSize(uint32_t fieldCount) {
        return (fieldCount + 7) / 8;    // Round up to the nearest byte
    }

    void appendIntField(char *buffer, uint32_t &offset, int32_t value) {
        memcpy(buffer + offset, &value, INT_SIZE);
        offset += INT_SIZE;
    }

    void appendVarcharField(char *buffer, uint32_t &offset, const std::string &value) {
        uint32_t length = static_cast<uint32_t>(value.size());
        memcpy(buffer + offset, &length, INT_SIZE);
        offset += INT_SIZE;

        memcpy(buffer + offset, value.data(), length);
        offset += length;
    }

    std::vector<Attribute> getTablesDescriptor() {
        return {
            {"table-id", TypeInt, INT_SIZE},
            {"table-name", TypeVarChar, VARCHAR_CATALOG_LENGTH},
            {"file-name", TypeVarChar, VARCHAR_CATALOG_LENGTH}
        };
    }

    std::vector<Attribute> getColumnsDescriptor() {
        return {
            {"table-id", TypeInt, INT_SIZE},
            {"column-name", TypeVarChar, VARCHAR_CATALOG_LENGTH},
            {"column-type", TypeInt, INT_SIZE},
            {"column-length", TypeInt, INT_SIZE},
            {"column-position", TypeInt, INT_SIZE}
        };
    }

    void prepareTablesRecord(int32_t tableId, const std::string &tableName, const std::string &fileName,
                             void *buffer, uint32_t &recordSize) {
        const uint32_t nullBytes = getNullIndicatorSize(3);
        auto *output = static_cast<char *>(buffer);
        memset(output, 0, nullBytes);

        uint32_t offset = nullBytes;
        appendIntField(output, offset, tableId);
        appendVarcharField(output, offset, tableName);
        appendVarcharField(output, offset, fileName);

        recordSize = offset;
    }

    void prepareColumnsRecord(int32_t tableId, const std::string &columnName, AttrType columnType,
                              AttrLength columnLength, int32_t columnPosition,
                              void *buffer, uint32_t &recordSize) {
        const uint32_t nullBytes = getNullIndicatorSize(5);
        auto *output = static_cast<char *>(buffer);
        memset(output, 0, nullBytes);

        uint32_t offset = nullBytes;
        appendIntField(output, offset, tableId);
        appendVarcharField(output, offset, columnName);
        appendIntField(output, offset, static_cast<int32_t>(columnType));
        appendIntField(output, offset, static_cast<int32_t>(columnLength));
        appendIntField(output, offset, columnPosition);

        recordSize = offset;
    }
}

namespace PeterDB {
    RelationManager &RelationManager::instance() {
        static RelationManager _relation_manager = RelationManager();
        return _relation_manager;
    }

    RelationManager::RelationManager() = default;

    RelationManager::~RelationManager() = default;

    RelationManager::RelationManager(const RelationManager &) = default;

    RelationManager &RelationManager::operator=(const RelationManager &) = default;

    RC RelationManager::createCatalog() {
        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();
        const std::vector<Attribute> tablesDescriptor = getTablesDescriptor();
        const std::vector<Attribute> columnsDescriptor = getColumnsDescriptor();

        if (rbfm.createFile(TABLES_TABLE_NAME) != 0) {
            return -1;
        }

        if (rbfm.createFile(COLUMNS_TABLE_NAME) != 0) {
            rbfm.destroyFile(TABLES_TABLE_NAME);
            return -1;
        }

        FileHandle tablesFileHandle;
        FileHandle columnsFileHandle;
        bool tablesOpened = false;
        bool columnsOpened = false;

        if (rbfm.openFile(TABLES_TABLE_NAME, tablesFileHandle) != 0) {
            rbfm.destroyFile(COLUMNS_TABLE_NAME);
            rbfm.destroyFile(TABLES_TABLE_NAME);
            return -1;
        }
        tablesOpened = true;

        if (rbfm.openFile(COLUMNS_TABLE_NAME, columnsFileHandle) != 0) {
            rbfm.closeFile(tablesFileHandle);
            rbfm.destroyFile(COLUMNS_TABLE_NAME);
            rbfm.destroyFile(TABLES_TABLE_NAME);
            return -1;
        }
        columnsOpened = true;

        auto cleanupFailure = [&]() -> RC {
            if (columnsOpened) {
                rbfm.closeFile(columnsFileHandle);
            }
            if (tablesOpened) {
                rbfm.closeFile(tablesFileHandle);
            }
            rbfm.destroyFile(COLUMNS_TABLE_NAME);
            rbfm.destroyFile(TABLES_TABLE_NAME);
            return -1;
        };

        void *buffer = malloc(PAGE_SIZE);
        if (buffer == nullptr) {
            return cleanupFailure();
        }

        auto insertTablesRow = [&](int32_t tableId, const std::string &tableName, const std::string &fileName) -> RC {
            uint32_t recordSize = 0;
            RID rid{};
            prepareTablesRecord(tableId, tableName, fileName, buffer, recordSize);
            return rbfm.insertRecord(tablesFileHandle, tablesDescriptor, buffer, rid);
        };

        auto insertColumnsRow = [&](int32_t tableId, const Attribute &attribute, int32_t position) -> RC {
            uint32_t recordSize = 0;
            RID rid{};
            prepareColumnsRecord(tableId, attribute.name, attribute.type, attribute.length, position, buffer, recordSize);
            return rbfm.insertRecord(columnsFileHandle, columnsDescriptor, buffer, rid);
        };

        if (insertTablesRow(TABLES_TABLE_ID, TABLES_TABLE_NAME, TABLES_TABLE_NAME) != 0 ||
            insertTablesRow(COLUMNS_TABLE_ID, COLUMNS_TABLE_NAME, COLUMNS_TABLE_NAME) != 0) {
            free(buffer);
            return cleanupFailure();
        }

        for (uint32_t i = 0; i < tablesDescriptor.size(); i++) {
            if (insertColumnsRow(TABLES_TABLE_ID, tablesDescriptor[i], static_cast<int32_t>(i + 1)) != 0) {
                free(buffer);
                return cleanupFailure();
            }
        }

        for (uint32_t i = 0; i < columnsDescriptor.size(); i++) {
            if (insertColumnsRow(COLUMNS_TABLE_ID, columnsDescriptor[i], static_cast<int32_t>(i + 1)) != 0) {
                free(buffer);
                return cleanupFailure();
            }
        }

        free(buffer);

        if (rbfm.closeFile(columnsFileHandle) != 0 || rbfm.closeFile(tablesFileHandle) != 0) {
            return cleanupFailure();
        }

        return 0;
    }

    RC RelationManager::deleteCatalog() {
        return -1;
    }

    RC RelationManager::createTable(const std::string &tableName, const std::vector<Attribute> &attrs) {
        return -1;
    }

    RC RelationManager::deleteTable(const std::string &tableName) {
        return -1;
    }

    RC RelationManager::getAttributes(const std::string &tableName, std::vector<Attribute> &attrs) {
        return -1;
    }

    RC RelationManager::insertTuple(const std::string &tableName, const void *data, RID &rid) {
        return -1;
    }

    RC RelationManager::deleteTuple(const std::string &tableName, const RID &rid) {
        return -1;
    }

    RC RelationManager::updateTuple(const std::string &tableName, const void *data, const RID &rid) {
        return -1;
    }

    RC RelationManager::readTuple(const std::string &tableName, const RID &rid, void *data) {
        return -1;
    }

    RC RelationManager::printTuple(const std::vector<Attribute> &attrs, const void *data, std::ostream &out) {
        return -1;
    }

    RC RelationManager::readAttribute(const std::string &tableName, const RID &rid, const std::string &attributeName,
                                      void *data) {
        return -1;
    }

    RC RelationManager::scan(const std::string &tableName,
                             const std::string &conditionAttribute,
                             const CompOp compOp,
                             const void *value,
                             const std::vector<std::string> &attributeNames,
                             RM_ScanIterator &rm_ScanIterator) {
        return -1;
    }

    RM_ScanIterator::RM_ScanIterator() = default;

    RM_ScanIterator::~RM_ScanIterator() = default;

    RC RM_ScanIterator::getNextTuple(RID &rid, void *data) { return RM_EOF; }

    RC RM_ScanIterator::close() { return -1; }

    // Extra credit work
    RC RelationManager::dropAttribute(const std::string &tableName, const std::string &attributeName) {
        return -1;
    }

    // Extra credit work
    RC RelationManager::addAttribute(const std::string &tableName, const Attribute &attr) {
        return -1;
    }

    // QE IX related
    RC RelationManager::createIndex(const std::string &tableName, const std::string &attributeName){
        return -1;
    }

    RC RelationManager::destroyIndex(const std::string &tableName, const std::string &attributeName){
        return -1;
    }

    // indexScan returns an iterator to allow the caller to go through qualified entries in index
    RC RelationManager::indexScan(const std::string &tableName,
                 const std::string &attributeName,
                 const void *lowKey,
                 const void *highKey,
                 bool lowKeyInclusive,
                 bool highKeyInclusive,
                 RM_IndexScanIterator &rm_IndexScanIterator){
        return -1;
    }


    RM_IndexScanIterator::RM_IndexScanIterator() = default;

    RM_IndexScanIterator::~RM_IndexScanIterator() = default;

    RC RM_IndexScanIterator::getNextEntry(RID &rid, void *key){
        return -1;
    }

    RC RM_IndexScanIterator::close(){
        return -1;
    }

} // namespace PeterDB
