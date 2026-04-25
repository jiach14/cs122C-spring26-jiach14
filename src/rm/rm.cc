#include "src/include/rm.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {
    using namespace PeterDB;

    const std::string TABLES_TABLE_NAME = "Tables";
    const std::string COLUMNS_TABLE_NAME = "Columns";

    constexpr int32_t TABLES_TABLE_ID = 1;
    constexpr int32_t COLUMNS_TABLE_ID = 2;
    constexpr uint32_t VARCHAR_CATALOG_LENGTH = 50;
    constexpr uint32_t INT_SIZE = 4;

    bool isSystemTable(const std::string &tableName) {
        return tableName == TABLES_TABLE_NAME || tableName == COLUMNS_TABLE_NAME;
    }

    uint32_t getNullIndicatorSize(uint32_t fieldCount) {
        return (fieldCount + 7) / 8;    // Round up to the nearest byte
    }

    bool fieldIsNull(const unsigned char *nullIndicator, uint32_t i) {
        return nullIndicator[i / 8] & static_cast<unsigned char>(1 << (7 - (i % 8)));
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

    void prepareVarcharValue(const std::string &value, void *buffer) {
        auto *output = static_cast<char *>(buffer);
        uint32_t length = static_cast<uint32_t>(value.size());
        memcpy(output, &length, INT_SIZE);
        memcpy(output + INT_SIZE, value.data(), length);
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

    void prepareTablesRecord(int32_t tableId,
                             const std::string &tableName,
                             const std::string &fileName,
                             void *buffer,
                             uint32_t &recordSize) {
        const uint32_t nullBytes = getNullIndicatorSize(3);
        auto *output = static_cast<char *>(buffer);
        memset(output, 0, nullBytes);

        uint32_t offset = nullBytes;
        appendIntField(output, offset, tableId);
        appendVarcharField(output, offset, tableName);
        appendVarcharField(output, offset, fileName);

        recordSize = offset;
    }

    void prepareColumnsRecord(int32_t tableId,
                              const std::string &columnName,
                              AttrType columnType,
                              AttrLength columnLength,
                              int32_t columnPosition,
                              void *buffer,
                              uint32_t &recordSize) {
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

    RC readProjectedInt(const void *data, int32_t &value, uint32_t fieldIndex = 0) {
        const auto *input = static_cast<const char *>(data);
        uint32_t nullBytes = getNullIndicatorSize(fieldIndex + 1);
        const auto *nullIndicator = reinterpret_cast<const unsigned char *>(input);

        if (fieldIsNull(nullIndicator, fieldIndex)) {
            return -1;
        }

        uint32_t offset = nullBytes;
        memcpy(&value, input + offset, INT_SIZE);
        return 0;
    }

    RC getTableIdAndFileName(const std::string &tableName,
                             int32_t &tableId,
                             std::string &fileName) {
        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        FileHandle tablesFileHandle;
        if (rbfm.openFile(TABLES_TABLE_NAME, tablesFileHandle) != 0) {
            return -1;
        }

        void *value = malloc(PAGE_SIZE);
        void *data = malloc(PAGE_SIZE);
        if (value == nullptr || data == nullptr) {
            free(value);
            free(data);
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        prepareVarcharValue(tableName, value);

        RBFM_ScanIterator iterator;
        std::vector<std::string> projectedAttrs = {"table-id", "file-name"};

        if (rbfm.scan(tablesFileHandle,
                      getTablesDescriptor(),
                      "table-name",
                      EQ_OP,
                      value,
                      projectedAttrs,
                      iterator) != 0) {
            free(value);
            free(data);
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        RID rid{};
        RC rc = iterator.getNextRecord(rid, data);
        if (rc != 0) {
            iterator.close();
            free(value);
            free(data);
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        const char *input = static_cast<const char *>(data);
        const auto *nullIndicator = reinterpret_cast<const unsigned char *>(input);
        if (fieldIsNull(nullIndicator, 0) || fieldIsNull(nullIndicator, 1)) {
            iterator.close();
            free(value);
            free(data);
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        uint32_t offset = getNullIndicatorSize(2);

        memcpy(&tableId, input + offset, INT_SIZE);
        offset += INT_SIZE;

        uint32_t fileNameLength = 0;
        memcpy(&fileNameLength, input + offset, INT_SIZE);
        offset += INT_SIZE;

        fileName.assign(input + offset, input + offset + fileNameLength);

        iterator.close();
        free(value);
        free(data);

        RC closeRc = rbfm.closeFile(tablesFileHandle);
        return closeRc;
    }

    RC getNextTableId(int32_t &nextTableId) {
        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        FileHandle tablesFileHandle;
        if (rbfm.openFile(TABLES_TABLE_NAME, tablesFileHandle) != 0) {
            return -1;
        }

        RBFM_ScanIterator iterator;
        std::vector<std::string> projectedAttrs = {"table-id"};

        if (rbfm.scan(tablesFileHandle,
                      getTablesDescriptor(),
                      "",
                      NO_OP,
                      nullptr,
                      projectedAttrs,
                      iterator) != 0) {
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        void *data = malloc(PAGE_SIZE);
        if (data == nullptr) {
            iterator.close();
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        RID rid{};
        int32_t maxTableId = 0;

        while (iterator.getNextRecord(rid, data) != RBFM_EOF) {
            int32_t tableId = 0;
            if (readProjectedInt(data, tableId) == 0) {
                maxTableId = std::max(maxTableId, tableId);
            }
        }

        free(data);
        iterator.close();

        if (rbfm.closeFile(tablesFileHandle) != 0) {
            return -1;
        }

        nextTableId = maxTableId + 1;
        return 0;
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

        if (rbfm.openFile(TABLES_TABLE_NAME, tablesFileHandle) != 0) {
            rbfm.destroyFile(COLUMNS_TABLE_NAME);
            rbfm.destroyFile(TABLES_TABLE_NAME);
            return -1;
        }

        if (rbfm.openFile(COLUMNS_TABLE_NAME, columnsFileHandle) != 0) {
            rbfm.closeFile(tablesFileHandle);
            rbfm.destroyFile(COLUMNS_TABLE_NAME);
            rbfm.destroyFile(TABLES_TABLE_NAME);
            return -1;
        }

        void *buffer = malloc(PAGE_SIZE);
        if (buffer == nullptr) {
            rbfm.closeFile(columnsFileHandle);
            rbfm.closeFile(tablesFileHandle);
            rbfm.destroyFile(COLUMNS_TABLE_NAME);
            rbfm.destroyFile(TABLES_TABLE_NAME);
            return -1;
        }

        auto fail = [&]() -> RC {
            free(buffer);
            rbfm.closeFile(columnsFileHandle);
            rbfm.closeFile(tablesFileHandle);
            rbfm.destroyFile(COLUMNS_TABLE_NAME);
            rbfm.destroyFile(TABLES_TABLE_NAME);
            return -1;
        };

        auto insertTablesRow = [&](int32_t tableId,
                                   const std::string &tableName,
                                   const std::string &fileName) -> RC {
            uint32_t recordSize = 0;
            RID rid{};
            prepareTablesRecord(tableId, tableName, fileName, buffer, recordSize);
            return rbfm.insertRecord(tablesFileHandle, tablesDescriptor, buffer, rid);
        };

        auto insertColumnsRow = [&](int32_t tableId,
                                    const Attribute &attribute,
                                    int32_t position) -> RC {
            uint32_t recordSize = 0;
            RID rid{};
            prepareColumnsRecord(tableId,
                                 attribute.name,
                                 attribute.type,
                                 attribute.length,
                                 position,
                                 buffer,
                                 recordSize);
            return rbfm.insertRecord(columnsFileHandle, columnsDescriptor, buffer, rid);
        };

        if (insertTablesRow(TABLES_TABLE_ID, TABLES_TABLE_NAME, TABLES_TABLE_NAME) != 0 ||
            insertTablesRow(COLUMNS_TABLE_ID, COLUMNS_TABLE_NAME, COLUMNS_TABLE_NAME) != 0) {
            return fail();
        }

        for (uint32_t i = 0; i < tablesDescriptor.size(); i++) {
            if (insertColumnsRow(TABLES_TABLE_ID, tablesDescriptor[i], static_cast<int32_t>(i + 1)) != 0) {
                return fail();
            }
        }

        for (uint32_t i = 0; i < columnsDescriptor.size(); i++) {
            if (insertColumnsRow(COLUMNS_TABLE_ID, columnsDescriptor[i], static_cast<int32_t>(i + 1)) != 0) {
                return fail();
            }
        }

        free(buffer);

        if (rbfm.closeFile(columnsFileHandle) != 0) {
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        if (rbfm.closeFile(tablesFileHandle) != 0) {
            return -1;
        }

        return 0;
    }

    RC RelationManager::deleteCatalog() {
        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        RC rc1 = rbfm.destroyFile(TABLES_TABLE_NAME);
        RC rc2 = rbfm.destroyFile(COLUMNS_TABLE_NAME);

        return (rc1 == 0 && rc2 == 0) ? 0 : -1;
    }

    RC RelationManager::createTable(const std::string &tableName, const std::vector<Attribute> &attrs) {
        if (tableName.empty() || isSystemTable(tableName)) {
            return -1;
        }

        int32_t existingTableId = 0;
        std::string existingFileName;
        if (getTableIdAndFileName(tableName, existingTableId, existingFileName) == 0) {
            return -1;
        }

        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        if (rbfm.createFile(tableName) != 0) {
            return -1;
        }

        int32_t tableId = 0;
        if (getNextTableId(tableId) != 0) {
            rbfm.destroyFile(tableName);
            return -1;
        }

        FileHandle tablesFileHandle;
        FileHandle columnsFileHandle;

        if (rbfm.openFile(TABLES_TABLE_NAME, tablesFileHandle) != 0) {
            rbfm.destroyFile(tableName);
            return -1;
        }

        if (rbfm.openFile(COLUMNS_TABLE_NAME, columnsFileHandle) != 0) {
            rbfm.closeFile(tablesFileHandle);
            rbfm.destroyFile(tableName);
            return -1;
        }

        void *buffer = malloc(PAGE_SIZE);
        if (buffer == nullptr) {
            rbfm.closeFile(columnsFileHandle);
            rbfm.closeFile(tablesFileHandle);
            rbfm.destroyFile(tableName);
            return -1;
        }

        uint32_t recordSize = 0;
        RID rid{};

        prepareTablesRecord(tableId, tableName, tableName, buffer, recordSize);
        if (rbfm.insertRecord(tablesFileHandle, getTablesDescriptor(), buffer, rid) != 0) {
            free(buffer);
            rbfm.closeFile(columnsFileHandle);
            rbfm.closeFile(tablesFileHandle);
            rbfm.destroyFile(tableName);
            return -1;
        }

        for (uint32_t i = 0; i < attrs.size(); i++) {
            prepareColumnsRecord(tableId,
                                 attrs[i].name,
                                 attrs[i].type,
                                 attrs[i].length,
                                 static_cast<int32_t>(i + 1),
                                 buffer,
                                 recordSize);

            if (rbfm.insertRecord(columnsFileHandle, getColumnsDescriptor(), buffer, rid) != 0) {
                free(buffer);
                rbfm.closeFile(columnsFileHandle);
                rbfm.closeFile(tablesFileHandle);
                rbfm.destroyFile(tableName);
                return -1;
            }
        }

        free(buffer);

        if (rbfm.closeFile(columnsFileHandle) != 0) {
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        if (rbfm.closeFile(tablesFileHandle) != 0) {
            return -1;
        }

        return 0;
    }

    RC RelationManager::deleteTable(const std::string &tableName) {
        if (tableName.empty() || isSystemTable(tableName)) {
            return -1;
        }

        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        int32_t tableId = 0;
        std::string fileName;
        if (getTableIdAndFileName(tableName, tableId, fileName) != 0) {
            return -1;
        }

        FileHandle tablesFileHandle;
        FileHandle columnsFileHandle;

        if (rbfm.openFile(TABLES_TABLE_NAME, tablesFileHandle) != 0) {
            return -1;
        }

        if (rbfm.openFile(COLUMNS_TABLE_NAME, columnsFileHandle) != 0) {
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        void *data = malloc(PAGE_SIZE);
        void *value = malloc(PAGE_SIZE);
        if (data == nullptr || value == nullptr) {
            free(data);
            free(value);
            rbfm.closeFile(columnsFileHandle);
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        prepareVarcharValue(tableName, value);

        RBFM_ScanIterator tablesIterator;
        if (rbfm.scan(tablesFileHandle,
                      getTablesDescriptor(),
                      "table-name",
                      EQ_OP,
                      value,
                      {"table-id"},
                      tablesIterator) != 0) {
            free(data);
            free(value);
            rbfm.closeFile(columnsFileHandle);
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        RID rid{};
        while (tablesIterator.getNextRecord(rid, data) != RBFM_EOF) {
            if (rbfm.deleteRecord(tablesFileHandle, getTablesDescriptor(), rid) != 0) {
                tablesIterator.close();
                free(data);
                free(value);
                rbfm.closeFile(columnsFileHandle);
                rbfm.closeFile(tablesFileHandle);
                return -1;
            }
        }
        tablesIterator.close();

        memcpy(value, &tableId, INT_SIZE);

        RBFM_ScanIterator columnsIterator;
        if (rbfm.scan(columnsFileHandle,
                      getColumnsDescriptor(),
                      "table-id",
                      EQ_OP,
                      value,
                      {"column-position"},
                      columnsIterator) != 0) {
            free(data);
            free(value);
            rbfm.closeFile(columnsFileHandle);
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        while (columnsIterator.getNextRecord(rid, data) != RBFM_EOF) {
            if (rbfm.deleteRecord(columnsFileHandle, getColumnsDescriptor(), rid) != 0) {
                columnsIterator.close();
                free(data);
                free(value);
                rbfm.closeFile(columnsFileHandle);
                rbfm.closeFile(tablesFileHandle);
                return -1;
            }
        }
        columnsIterator.close();

        free(data);
        free(value);

        if (rbfm.closeFile(columnsFileHandle) != 0) {
            rbfm.closeFile(tablesFileHandle);
            return -1;
        }

        if (rbfm.closeFile(tablesFileHandle) != 0) {
            return -1;
        }

        return rbfm.destroyFile(fileName);
    }

    RC RelationManager::getAttributes(const std::string &tableName, std::vector<Attribute> &attrs) {
        attrs.clear();

        int32_t tableId = 0;
        std::string fileName;
        if (getTableIdAndFileName(tableName, tableId, fileName) != 0) {
            return -1;
        }

        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        FileHandle columnsFileHandle;
        if (rbfm.openFile(COLUMNS_TABLE_NAME, columnsFileHandle) != 0) {
            return -1;
        }

        void *value = malloc(PAGE_SIZE);
        void *data = malloc(PAGE_SIZE);
        if (value == nullptr || data == nullptr) {
            free(value);
            free(data);
            rbfm.closeFile(columnsFileHandle);
            return -1;
        }

        memcpy(value, &tableId, INT_SIZE);

        RBFM_ScanIterator iterator;
        std::vector<std::string> projectedAttrs = {
            "column-name",
            "column-type",
            "column-length",
            "column-position"
        };

        if (rbfm.scan(columnsFileHandle,
                      getColumnsDescriptor(),
                      "table-id",
                      EQ_OP,
                      value,
                      projectedAttrs,
                      iterator) != 0) {
            free(value);
            free(data);
            rbfm.closeFile(columnsFileHandle);
            return -1;
        }

        struct ColumnEntry {
            Attribute attr;
            int32_t position;
        };

        std::vector<ColumnEntry> columns;
        RID rid{};

        while (iterator.getNextRecord(rid, data) != RBFM_EOF) {
            const char *input = static_cast<const char *>(data);
            const auto *nullIndicator = reinterpret_cast<const unsigned char *>(input);

            if (fieldIsNull(nullIndicator, 0) ||
                fieldIsNull(nullIndicator, 1) ||
                fieldIsNull(nullIndicator, 2) ||
                fieldIsNull(nullIndicator, 3)) {
                continue;
            }

            uint32_t offset = getNullIndicatorSize(4);

            uint32_t nameLength = 0;
            memcpy(&nameLength, input + offset, INT_SIZE);
            offset += INT_SIZE;

            std::string columnName(input + offset, input + offset + nameLength);
            offset += nameLength;

            int32_t columnType = 0;
            int32_t columnLength = 0;
            int32_t columnPosition = 0;

            memcpy(&columnType, input + offset, INT_SIZE);
            offset += INT_SIZE;

            memcpy(&columnLength, input + offset, INT_SIZE);
            offset += INT_SIZE;

            memcpy(&columnPosition, input + offset, INT_SIZE);

            Attribute attr;
            attr.name = columnName;
            attr.type = static_cast<AttrType>(columnType);
            attr.length = static_cast<AttrLength>(columnLength);

            columns.push_back({attr, columnPosition});
        }

        iterator.close();
        free(value);
        free(data);

        if (rbfm.closeFile(columnsFileHandle) != 0) {
            return -1;
        }

        std::sort(columns.begin(), columns.end(),
                  [](const ColumnEntry &a, const ColumnEntry &b) {
                      return a.position < b.position;
                  });

        for (const auto &column : columns) {
            attrs.push_back(column.attr);
        }

        return attrs.empty() ? -1 : 0;
    }

    RC RelationManager::insertTuple(const std::string &tableName, const void *data, RID &rid) {
        if (isSystemTable(tableName)) {
            return -1;
        }

        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) {
            return -1;
        }

        int32_t tableId = 0;
        std::string fileName;
        if (getTableIdAndFileName(tableName, tableId, fileName) != 0) {
            return -1;
        }

        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        FileHandle fileHandle;
        if (rbfm.openFile(fileName, fileHandle) != 0) {
            return -1;
        }

        RC rc = rbfm.insertRecord(fileHandle, attrs, data, rid);

        if (rbfm.closeFile(fileHandle) != 0) {
            return -1;
        }

        return rc;
    }

    RC RelationManager::deleteTuple(const std::string &tableName, const RID &rid) {
        if (isSystemTable(tableName)) {
            return -1;
        }

        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) {
            return -1;
        }

        int32_t tableId = 0;
        std::string fileName;
        if (getTableIdAndFileName(tableName, tableId, fileName) != 0) {
            return -1;
        }

        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        FileHandle fileHandle;
        if (rbfm.openFile(fileName, fileHandle) != 0) {
            return -1;
        }

        RC rc = rbfm.deleteRecord(fileHandle, attrs, rid);

        if (rbfm.closeFile(fileHandle) != 0) {
            return -1;
        }

        return rc;
    }

    RC RelationManager::updateTuple(const std::string &tableName, const void *data, const RID &rid) {
        if (isSystemTable(tableName)) {
            return -1;
        }

        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) {
            return -1;
        }

        int32_t tableId = 0;
        std::string fileName;
        if (getTableIdAndFileName(tableName, tableId, fileName) != 0) {
            return -1;
        }

        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        FileHandle fileHandle;
        if (rbfm.openFile(fileName, fileHandle) != 0) {
            return -1;
        }

        RC rc = rbfm.updateRecord(fileHandle, attrs, data, rid);

        if (rbfm.closeFile(fileHandle) != 0) {
            return -1;
        }

        return rc;
    }

    RC RelationManager::readTuple(const std::string &tableName, const RID &rid, void *data) {
        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) {
            return -1;
        }

        int32_t tableId = 0;
        std::string fileName;
        if (getTableIdAndFileName(tableName, tableId, fileName) != 0) {
            return -1;
        }

        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        FileHandle fileHandle;
        if (rbfm.openFile(fileName, fileHandle) != 0) {
            return -1;
        }

        RC rc = rbfm.readRecord(fileHandle, attrs, rid, data);

        if (rbfm.closeFile(fileHandle) != 0) {
            return -1;
        }

        return rc;
    }

    RC RelationManager::printTuple(const std::vector<Attribute> &attrs, const void *data, std::ostream &out) {
        return RecordBasedFileManager::instance().printRecord(attrs, data, out);
    }

    RC RelationManager::readAttribute(const std::string &tableName,
                                      const RID &rid,
                                      const std::string &attributeName,
                                      void *data) {
        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) {
            return -1;
        }

        int32_t tableId = 0;
        std::string fileName;
        if (getTableIdAndFileName(tableName, tableId, fileName) != 0) {
            return -1;
        }

        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        FileHandle fileHandle;
        if (rbfm.openFile(fileName, fileHandle) != 0) {
            return -1;
        }

        RC rc = rbfm.readAttribute(fileHandle, attrs, rid, attributeName, data);

        if (rbfm.closeFile(fileHandle) != 0) {
            return -1;
        }

        return rc;
    }

    RC RelationManager::scan(const std::string &tableName,
                             const std::string &conditionAttribute,
                             const CompOp compOp,
                             const void *value,
                             const std::vector<std::string> &attributeNames,
                             RM_ScanIterator &rm_ScanIterator) {
        rm_ScanIterator.close();

        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) {
            return -1;
        }

        int32_t tableId = 0;
        std::string fileName;
        if (getTableIdAndFileName(tableName, tableId, fileName) != 0) {
            return -1;
        }

        RecordBasedFileManager &rbfm = RecordBasedFileManager::instance();

        if (rbfm.openFile(fileName, rm_ScanIterator.fileHandle) != 0) {
            return -1;
        }

        if (rbfm.scan(rm_ScanIterator.fileHandle,
                      attrs,
                      conditionAttribute,
                      compOp,
                      value,
                      attributeNames,
                      rm_ScanIterator.rbfmIterator) != 0) {
            rbfm.closeFile(rm_ScanIterator.fileHandle);
            return -1;
        }

        rm_ScanIterator.isOpen = true;
        return 0;
    }

    RM_ScanIterator::RM_ScanIterator() = default;

    RM_ScanIterator::~RM_ScanIterator() {
        close();
    }

    RC RM_ScanIterator::getNextTuple(RID &rid, void *data) {
        if (!isOpen) {
            return RM_EOF;
        }

        RC rc = rbfmIterator.getNextRecord(rid, data);
        return rc == RBFM_EOF ? RM_EOF : rc;
    }

    RC RM_ScanIterator::close() {
        if (!isOpen) {
            return 0;
        }

        rbfmIterator.close();

        if (RecordBasedFileManager::instance().closeFile(fileHandle) != 0) {
            isOpen = false;
            return -1;
        }

        isOpen = false;
        return 0;
    }

    // Extra credit work
    RC RelationManager::dropAttribute(const std::string &tableName, const std::string &attributeName) {
        return -1;
    }

    // Extra credit work
    RC RelationManager::addAttribute(const std::string &tableName, const Attribute &attr) {
        return -1;
    }

    // QE IX related
    RC RelationManager::createIndex(const std::string &tableName, const std::string &attributeName) {
        return -1;
    }

    RC RelationManager::destroyIndex(const std::string &tableName, const std::string &attributeName) {
        return -1;
    }

    RC RelationManager::indexScan(const std::string &tableName,
                                  const std::string &attributeName,
                                  const void *lowKey,
                                  const void *highKey,
                                  bool lowKeyInclusive,
                                  bool highKeyInclusive,
                                  RM_IndexScanIterator &rm_IndexScanIterator) {
        return -1;
    }

    RM_IndexScanIterator::RM_IndexScanIterator() = default;

    RM_IndexScanIterator::~RM_IndexScanIterator() = default;

    RC RM_IndexScanIterator::getNextEntry(RID &rid, void *key) {
        return -1;
    }

    RC RM_IndexScanIterator::close() {
        return -1;
    }

} // namespace PeterDB