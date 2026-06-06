#include "src/include/qe.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {
    using namespace PeterDB;

    constexpr unsigned INT_SIZE = 4;
    constexpr unsigned REAL_SIZE = 4;
    constexpr unsigned VARCHAR_LENGTH_SIZE = 4;

    unsigned getNullIndicatorSize(unsigned fieldCount) {
        return (fieldCount + 7) / 8;
    }

    bool fieldIsNull(const void *data, unsigned i) {
        const auto *nulls = static_cast<const unsigned char *>(data);
        return nulls[i / 8] & static_cast<unsigned char>(1 << (7 - (i % 8)));
    }

    void setNullBit(void *data, unsigned i) {
        auto *nulls = static_cast<unsigned char *>(data);
        nulls[i / 8] |= static_cast<unsigned char>(1 << (7 - (i % 8)));
    }

    int findAttrIndex(const std::vector<Attribute> &attrs, const std::string &name) {
        for (unsigned i = 0; i < attrs.size(); i++) {
            if (attrs[i].name == name) {
                return static_cast<int>(i);
            }
        }

        for (unsigned i = 0; i < attrs.size(); i++) {
            size_t dot = attrs[i].name.find_last_of('.');
            if (dot != std::string::npos && attrs[i].name.substr(dot + 1) == name) {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    unsigned getFieldSize(const Attribute &attr, const char *ptr) {
        if (attr.type == TypeInt) {
            return INT_SIZE;
        }

        if (attr.type == TypeReal) {
            return REAL_SIZE;
        }

        uint32_t len = 0;
        memcpy(&len, ptr, VARCHAR_LENGTH_SIZE);
        return VARCHAR_LENGTH_SIZE + len;
    }

    unsigned getTupleSize(const std::vector<Attribute> &attrs, const void *data) {
        const char *input = static_cast<const char *>(data);
        unsigned offset = getNullIndicatorSize(attrs.size());

        for (unsigned i = 0; i < attrs.size(); i++) {
            if (fieldIsNull(data, i)) {
                continue;
            }

            unsigned size = getFieldSize(attrs[i], input + offset);
            offset += size;
        }

        return offset;
    }

    bool getAttrValue(const std::vector<Attribute> &attrs,
                      const void *tuple,
                      const std::string &attrName,
                      Attribute &attr,
                      std::vector<char> &value) {
        int targetIndex = findAttrIndex(attrs, attrName);
        if (targetIndex < 0) {
            return false;
        }

        const char *input = static_cast<const char *>(tuple);
        unsigned offset = getNullIndicatorSize(attrs.size());

        for (unsigned i = 0; i < attrs.size(); i++) {
            bool isNull = fieldIsNull(tuple, i);

            if (static_cast<int>(i) == targetIndex) {
                attr = attrs[i];

                if (isNull) {
                    value.clear();
                    return false;
                }

                unsigned size = getFieldSize(attrs[i], input + offset);
                value.assign(input + offset, input + offset + size);
                return true;
            }

            if (!isNull) {
                offset += getFieldSize(attrs[i], input + offset);
            }
        }

        return false;
    }

    int compareValues(AttrType type, const void *lhs, const void *rhs) {
        if (type == TypeInt) {
            int a = 0;
            int b = 0;
            memcpy(&a, lhs, INT_SIZE);
            memcpy(&b, rhs, INT_SIZE);

            if (a < b) return -1;
            if (a > b) return 1;
            return 0;
        }

        if (type == TypeReal) {
            float a = 0;
            float b = 0;
            memcpy(&a, lhs, REAL_SIZE);
            memcpy(&b, rhs, REAL_SIZE);

            if (a < b) return -1;
            if (a > b) return 1;
            return 0;
        }

        uint32_t lenA = 0;
        uint32_t lenB = 0;
        memcpy(&lenA, lhs, VARCHAR_LENGTH_SIZE);
        memcpy(&lenB, rhs, VARCHAR_LENGTH_SIZE);

        const char *strA = static_cast<const char *>(lhs) + VARCHAR_LENGTH_SIZE;
        const char *strB = static_cast<const char *>(rhs) + VARCHAR_LENGTH_SIZE;

        int cmp = std::memcmp(strA, strB, std::min(lenA, lenB));
        if (cmp < 0) return -1;
        if (cmp > 0) return 1;

        if (lenA < lenB) return -1;
        if (lenA > lenB) return 1;
        return 0;
    }

    bool applyCompOp(int cmp, CompOp op) {
        switch (op) {
            case EQ_OP:
                return cmp == 0;
            case LT_OP:
                return cmp < 0;
            case LE_OP:
                return cmp <= 0;
            case GT_OP:
                return cmp > 0;
            case GE_OP:
                return cmp >= 0;
            case NE_OP:
                return cmp != 0;
            case NO_OP:
                return true;
            default:
                return false;
        }
    }

    bool tupleSatisfiesCondition(const std::vector<Attribute> &attrs,
                                 const void *tuple,
                                 const Condition &condition) {
        if (condition.op == NO_OP) {
            return true;
        }

        Attribute lhsAttr;
        std::vector<char> lhsValue;

        if (!getAttrValue(attrs, tuple, condition.lhsAttr, lhsAttr, lhsValue)) {
            return false;
        }

        if (condition.bRhsIsAttr) {
            Attribute rhsAttr;
            std::vector<char> rhsValue;

            if (!getAttrValue(attrs, tuple, condition.rhsAttr, rhsAttr, rhsValue)) {
                return false;
            }

            int cmp = compareValues(lhsAttr.type, lhsValue.data(), rhsValue.data());
            return applyCompOp(cmp, condition.op);
        }

        if (condition.rhsValue.data == nullptr) {
            return false;
        }

        int cmp = compareValues(lhsAttr.type, lhsValue.data(), condition.rhsValue.data);
        return applyCompOp(cmp, condition.op);
    }

    RC projectTuple(const std::vector<Attribute> &inputAttrs,
                    const std::vector<Attribute> &outputAttrs,
                    const void *inputTuple,
                    void *outputTuple) {
        unsigned outputNullSize = getNullIndicatorSize(outputAttrs.size());
        memset(outputTuple, 0, outputNullSize);

        char *output = static_cast<char *>(outputTuple);
        unsigned outputOffset = outputNullSize;

        for (unsigned i = 0; i < outputAttrs.size(); i++) {
            Attribute attr;
            std::vector<char> value;

            if (!getAttrValue(inputAttrs, inputTuple, outputAttrs[i].name, attr, value)) {
                setNullBit(outputTuple, i);
                continue;
            }

            memcpy(output + outputOffset, value.data(), value.size());
            outputOffset += value.size();
        }

        return 0;
    }

    std::string aggregateOpToString(AggregateOp op) {
        switch (op) {
        case MIN: return "MIN";
        case MAX: return "MAX";
        case COUNT: return "COUNT";
        case SUM: return "SUM";
        case AVG: return "AVG";
        default: return "UNKNOWN";
        }
    }

    float getNumericValue(AttrType type, const void *data) {
        if (type == TypeInt) {
            int value = 0;
            memcpy(&value, data, INT_SIZE);
            return static_cast<float>(value);
        }

        float value = 0.0f;
        memcpy(&value, data, REAL_SIZE);
        return value;
    }

    std::string makeKeyString(AttrType type, const std::vector<char> &value) {
        std::string key;
        key.push_back(static_cast<char>(type));
        key.append(value.data(), value.size());
        return key;
    }

    std::vector<char> makeAggregateTuple(float value) {
        std::vector<char> tuple(1 + REAL_SIZE, 0);
        memcpy(tuple.data() + 1, &value, REAL_SIZE);
        return tuple;
    }

    std::vector<char> makeGroupAggregateTuple(const std::vector<char> &groupValue, float aggValue) {
        std::vector<char> tuple(1 + groupValue.size() + REAL_SIZE, 0);
        unsigned offset = 1;
        memcpy(tuple.data() + offset, groupValue.data(), groupValue.size());
        offset += groupValue.size();
        memcpy(tuple.data() + offset, &aggValue, REAL_SIZE);
        return tuple;
    }

    std::vector<char> copyTupleToVector(const std::vector<Attribute> &attrs, const void *tuple) {
        unsigned size = getTupleSize(attrs, tuple);
        const char *input = static_cast<const char *>(tuple);
        return std::vector<char>(input, input + size);
    }

    std::vector<char> mergeTuples(const std::vector<Attribute> &leftAttrs,
                                  const void *leftTuple,
                                  const std::vector<Attribute> &rightAttrs,
                                  const void *rightTuple) {
        unsigned totalAttrs = leftAttrs.size() + rightAttrs.size();
        unsigned outputNullSize = getNullIndicatorSize(totalAttrs);

        std::vector<char> output(PAGE_SIZE, 0);
        unsigned outputOffset = outputNullSize;

        for (unsigned i = 0; i < leftAttrs.size(); i++) {
            Attribute attr;
            std::vector<char> value;

            if (!getAttrValue(leftAttrs, leftTuple, leftAttrs[i].name, attr, value)) {
                output[i / 8] |= static_cast<char>(1 << (7 - (i % 8)));
                continue;
            }

            memcpy(output.data() + outputOffset, value.data(), value.size());
            outputOffset += value.size();
        }

        for (unsigned i = 0; i < rightAttrs.size(); i++) {
            Attribute attr;
            std::vector<char> value;
            unsigned outputIndex = leftAttrs.size() + i;

            if (!getAttrValue(rightAttrs, rightTuple, rightAttrs[i].name, attr, value)) {
                output[outputIndex / 8] |= static_cast<char>(1 << (7 - (outputIndex % 8)));
                continue;
            }

            memcpy(output.data() + outputOffset, value.data(), value.size());
            outputOffset += value.size();
        }

        output.resize(outputOffset);
        return output;
    }

    unsigned nextGHJoinId() {
        static unsigned id = 0;
        return ++id;
    }

    RC createTempPartitionFiles(unsigned joinId,
                                unsigned numPartitions,
                                std::vector<std::string> &tempFileNames) {
        PagedFileManager &pfm = PagedFileManager::instance();
        tempFileNames.clear();

        for (unsigned i = 0; i < numPartitions; i++) {
            for (const char *side : {"left", "right"}) {
                std::ostringstream name;
                name << side << "_join" << joinId << "_" << i;

                const std::string fileName = name.str();
                pfm.destroyFile(fileName);

                if (pfm.createFile(fileName) != 0) {
                    for (const std::string &createdFile : tempFileNames) {
                        pfm.destroyFile(createdFile);
                    }
                    tempFileNames.clear();
                    return -1;
                }

                tempFileNames.push_back(fileName);
            }
        }

        return 0;
    }

    void destroyTempPartitionFiles(std::vector<std::string> &tempFileNames) {
        PagedFileManager &pfm = PagedFileManager::instance();

        for (const std::string &fileName : tempFileNames) {
            pfm.destroyFile(fileName);
        }

        tempFileNames.clear();
    }
}

namespace PeterDB {
    Filter::Filter(Iterator *input, const Condition &condition) {
        this->input = input;
        this->condition = condition;
        this->attrs.clear();

        if (this->input != nullptr) {
            this->input->getAttributes(this->attrs);
        }
    }

    Filter::~Filter() = default;

    RC Filter::getNextTuple(void *data) {
        if (input == nullptr || data == nullptr) {
            return QE_EOF;
        }

        void *buffer = malloc(PAGE_SIZE);
        if (buffer == nullptr) {
            return QE_EOF;
        }

        while (input->getNextTuple(buffer) != QE_EOF) {
            if (tupleSatisfiesCondition(attrs, buffer, condition)) {
                unsigned size = getTupleSize(attrs, buffer);
                memcpy(data, buffer, size);
                free(buffer);
                return 0;
            }
        }

        free(buffer);
        return QE_EOF;
    }

    RC Filter::getAttributes(std::vector<Attribute> &attrs) const {
        attrs.clear();
        attrs = this->attrs;
        return 0;
    }

    Project::Project(Iterator *input, const std::vector<std::string> &attrNames) {
        this->input = input;
        this->attrNames = attrNames;
        this->inputAttrs.clear();
        this->outputAttrs.clear();

        if (this->input != nullptr) {
            this->input->getAttributes(this->inputAttrs);
        }

        for (const std::string &name : this->attrNames) {
            int index = findAttrIndex(this->inputAttrs, name);
            if (index >= 0) {
                this->outputAttrs.push_back(this->inputAttrs[index]);
            }
        }
    }

    Project::~Project() = default;

    RC Project::getNextTuple(void *data) {
        if (input == nullptr || data == nullptr) {
            return QE_EOF;
        }

        void *buffer = malloc(PAGE_SIZE);
        if (buffer == nullptr) {
            return QE_EOF;
        }

        RC rc = input->getNextTuple(buffer);
        if (rc == QE_EOF) {
            free(buffer);
            return QE_EOF;
        }

        projectTuple(inputAttrs, outputAttrs, buffer, data);

        free(buffer);
        return 0;
    }

    RC Project::getAttributes(std::vector<Attribute> &attrs) const {
        attrs.clear();
        attrs = this->outputAttrs;
        return 0;
    }

    BNLJoin::BNLJoin(Iterator *leftIn, TableScan *rightIn, const Condition &condition, const unsigned int numPages) {
        this->leftIn = leftIn;
        this->rightIn = rightIn;
        this->condition = condition;
        this->numPages = numPages;

        this->leftExhausted = false;
        this->blockLoaded = false;

        if (this->leftIn != nullptr) {
            this->leftIn->getAttributes(this->leftAttrs);
        }

        if (this->rightIn != nullptr) {
            this->rightIn->getAttributes(this->rightAttrs);
        }

        this->outputAttrs.clear();
        this->outputAttrs.insert(this->outputAttrs.end(), this->leftAttrs.begin(), this->leftAttrs.end());
        this->outputAttrs.insert(this->outputAttrs.end(), this->rightAttrs.begin(), this->rightAttrs.end());
    }

    BNLJoin::~BNLJoin() = default;

    RC BNLJoin::loadNextLeftBlock() {
        hashTable.clear();

        if (leftIn == nullptr || leftExhausted) {
            return QE_EOF;
        }

        unsigned maxBytes = numPages * PAGE_SIZE;
        unsigned usedBytes = 0;

        void *tuple = malloc(PAGE_SIZE);
        if (tuple == nullptr) {
            return QE_EOF;
        }

        while (usedBytes < maxBytes && leftIn->getNextTuple(tuple) != QE_EOF) {
            Attribute keyAttr;
            std::vector<char> keyValue;

            if (!getAttrValue(leftAttrs, tuple, condition.lhsAttr, keyAttr, keyValue)) {
                continue;
            }

            std::vector<char> tupleVec = copyTupleToVector(leftAttrs, tuple);
            usedBytes += tupleVec.size();

            std::string key = makeKeyString(keyAttr.type, keyValue);
            hashTable[key].push_back(tupleVec);

            if (usedBytes >= maxBytes) {
                break;
            }
        }

        free(tuple);

        if (hashTable.empty()) {
            leftExhausted = true;
            return QE_EOF;
        }

        if (rightIn != nullptr) {
            rightIn->setIterator();
        }

        blockLoaded = true;
        return 0;
    }

    RC BNLJoin::getNextTuple(void *data) {
        if (data == nullptr || leftIn == nullptr || rightIn == nullptr) {
            return QE_EOF;
        }

        while (true) {
            if (!outputQueue.empty()) {
                std::vector<char> result = outputQueue.front();
                outputQueue.pop();

                memcpy(data, result.data(), result.size());
                return 0;
            }

            if (!blockLoaded) {
                if (loadNextLeftBlock() == QE_EOF) {
                    return QE_EOF;
                }
            }

            void *rightTuple = malloc(PAGE_SIZE);
            if (rightTuple == nullptr) {
                return QE_EOF;
            }

            bool foundAnyRight = false;

            while (rightIn->getNextTuple(rightTuple) != QE_EOF) {
                foundAnyRight = true;

                Attribute rightKeyAttr;
                std::vector<char> rightKeyValue;

                if (!getAttrValue(rightAttrs, rightTuple, condition.rhsAttr, rightKeyAttr, rightKeyValue)) {
                    continue;
                }

                std::string key = makeKeyString(rightKeyAttr.type, rightKeyValue);
                auto it = hashTable.find(key);

                if (it == hashTable.end()) {
                    continue;
                }

                for (const std::vector<char> &leftTuple : it->second) {
                    std::vector<char> result = mergeTuples(leftAttrs,
                                                           leftTuple.data(),
                                                           rightAttrs,
                                                           rightTuple);
                    outputQueue.push(result);
                }

                if (!outputQueue.empty()) {
                    break;
                }
            }

            free(rightTuple);

            if (!outputQueue.empty()) {
                continue;
            }

            blockLoaded = false;

            if (!foundAnyRight) {
                return QE_EOF;
            }
        }
    }

    RC BNLJoin::getAttributes(std::vector<Attribute> &attrs) const {
        attrs.clear();
        attrs = this->outputAttrs;
        return 0;
    }


    INLJoin::INLJoin(Iterator *leftIn, IndexScan *rightIn, const Condition &condition) {
        this->leftIn = leftIn;
        this->rightIn = rightIn;
        this->condition = condition;
        this->hasCurrentLeft = false;
        this->currentLeftTuple.clear();

        if (this->leftIn != nullptr) {
            this->leftIn->getAttributes(this->leftAttrs);
        }

        if (this->rightIn != nullptr) {
            this->rightIn->getAttributes(this->rightAttrs);
        }

        this->outputAttrs.clear();
        this->outputAttrs.insert(this->outputAttrs.end(), this->leftAttrs.begin(), this->leftAttrs.end());
        this->outputAttrs.insert(this->outputAttrs.end(), this->rightAttrs.begin(), this->rightAttrs.end());
    }

    INLJoin::~INLJoin() = default;

    RC INLJoin::getNextTuple(void *data) {
        if (data == nullptr || leftIn == nullptr || rightIn == nullptr) {
            return QE_EOF;
        }

        void *rightTuple = malloc(PAGE_SIZE);
        if (rightTuple == nullptr) {
            return QE_EOF;
        }

        while (true) {
            if (!hasCurrentLeft) {
                void *leftTuple = malloc(PAGE_SIZE);
                if (leftTuple == nullptr) {
                    free(rightTuple);
                    return QE_EOF;
                }

                if (leftIn->getNextTuple(leftTuple) == QE_EOF) {
                    free(leftTuple);
                    free(rightTuple);
                    return QE_EOF;
                }

                Attribute keyAttr;
                std::vector<char> keyValue;

                if (!getAttrValue(leftAttrs, leftTuple, condition.lhsAttr, keyAttr, keyValue)) {
                    free(leftTuple);
                    continue;
                }

                currentLeftTuple = copyTupleToVector(leftAttrs, leftTuple);
                free(leftTuple);

                switch (condition.op) {
                    case EQ_OP:
                        rightIn->setIterator(keyValue.data(), keyValue.data(), true, true);
                        break;
                    case LT_OP:
                        rightIn->setIterator(keyValue.data(), nullptr, false, true);
                        break;
                    case LE_OP:
                        rightIn->setIterator(keyValue.data(), nullptr, true, true);
                        break;
                    case GT_OP:
                        rightIn->setIterator(nullptr, keyValue.data(), true, false);
                        break;
                    case GE_OP:
                        rightIn->setIterator(nullptr, keyValue.data(), true, true);
                        break;
                    case NE_OP:
                    case NO_OP:
                    default:
                        rightIn->setIterator(nullptr, nullptr, true, true);
                        break;
                }

                hasCurrentLeft = true;
            }

            while (rightIn->getNextTuple(rightTuple) != QE_EOF) {
                std::vector<char> result = mergeTuples(leftAttrs,
                                                       currentLeftTuple.data(),
                                                       rightAttrs,
                                                       rightTuple);

                if (!tupleSatisfiesCondition(outputAttrs, result.data(), condition)) {
                    continue;
                }

                memcpy(data, result.data(), result.size());
                free(rightTuple);
                return 0;
            }

            hasCurrentLeft = false;
        }
    }

    RC INLJoin::getAttributes(std::vector<Attribute> &attrs) const {
        attrs.clear();
        attrs = this->outputAttrs;
        return 0;
    }

    GHJoin::GHJoin(Iterator *leftIn, Iterator *rightIn, const Condition &condition, const unsigned int numPartitions) {
        this->leftIn = leftIn;
        this->rightIn = rightIn;
        this->condition = condition;
        this->numPartitions = numPartitions;
        this->built = false;

        if (this->leftIn != nullptr) {
            this->leftIn->getAttributes(this->leftAttrs);
        }

        if (this->rightIn != nullptr) {
            this->rightIn->getAttributes(this->rightAttrs);
        }

        this->outputAttrs.clear();
        this->outputAttrs.insert(this->outputAttrs.end(), this->leftAttrs.begin(), this->leftAttrs.end());
        this->outputAttrs.insert(this->outputAttrs.end(), this->rightAttrs.begin(), this->rightAttrs.end());

        createTempPartitionFiles(nextGHJoinId(), this->numPartitions, this->tempFileNames);
    }

    GHJoin::~GHJoin() {
        destroyTempPartitionFiles(this->tempFileNames);
    }

    RC GHJoin::getNextTuple(void *data) {
        if (data == nullptr) {
            return QE_EOF;
        }

        if (!built) {
            if (buildJoinResults() != 0) {
                built = true;
                return QE_EOF;
            }
            built = true;
        }

        if (outputQueue.empty()) {
            return QE_EOF;
        }

        std::vector<char> result = outputQueue.front();
        outputQueue.pop();
        memcpy(data, result.data(), result.size());
        return 0;
    }

    RC GHJoin::getAttributes(std::vector<Attribute> &attrs) const {
        attrs.clear();
        attrs = this->outputAttrs;
        return 0;
    }

    RC GHJoin::buildJoinResults() {
        if (leftIn == nullptr || rightIn == nullptr) {
            return QE_EOF;
        }

        std::unordered_map<std::string, std::vector<std::vector<char>>> hashTable;

        void *tuple = malloc(PAGE_SIZE);
        if (tuple == nullptr) {
            return -1;
        }

        while (leftIn->getNextTuple(tuple) != QE_EOF) {
            Attribute keyAttr;
            std::vector<char> keyValue;

            if (!getAttrValue(leftAttrs, tuple, condition.lhsAttr, keyAttr, keyValue)) {
                continue;
            }

            std::string key = makeKeyString(keyAttr.type, keyValue);
            hashTable[key].push_back(copyTupleToVector(leftAttrs, tuple));
        }

        while (rightIn->getNextTuple(tuple) != QE_EOF) {
            Attribute keyAttr;
            std::vector<char> keyValue;

            if (!getAttrValue(rightAttrs, tuple, condition.rhsAttr, keyAttr, keyValue)) {
                continue;
            }

            std::string key = makeKeyString(keyAttr.type, keyValue);
            auto it = hashTable.find(key);

            if (it == hashTable.end()) {
                continue;
            }

            for (const std::vector<char> &leftTuple : it->second) {
                outputQueue.push(mergeTuples(leftAttrs,
                                             leftTuple.data(),
                                             rightAttrs,
                                             tuple));
            }
        }

        free(tuple);
        return 0;
    }

    Aggregate::Aggregate(Iterator *input, const Attribute &aggAttr, AggregateOp op) {
        this->input = input;
        this->aggAttr = aggAttr;
        this->op = op;

        this->isGroup = false;
        this->computed = false;
        this->returned = false;
        this->basicResult = 0.0f;

        this->inputAttrs.clear();
        this->outputAttrs.clear();

        if (this->input != nullptr) {
            this->input->getAttributes(this->inputAttrs);
        }

        Attribute outAttr;
        outAttr.name = aggregateOpToString(op) + "(" + aggAttr.name + ")";
        outAttr.type = TypeReal;
        outAttr.length = 4;
        this->outputAttrs.push_back(outAttr);
    }

    Aggregate::Aggregate(Iterator *input, const Attribute &aggAttr, const Attribute &groupAttr, AggregateOp op) {
        this->input = input;
        this->aggAttr = aggAttr;
        this->groupAttr = groupAttr;
        this->op = op;

        this->isGroup = true;
        this->computed = false;
        this->returned = false;
        this->basicResult = 0.0f;

        this->inputAttrs.clear();
        this->outputAttrs.clear();

        if (this->input != nullptr) {
            this->input->getAttributes(this->inputAttrs);
        }

        this->outputAttrs.push_back(groupAttr);

        Attribute aggOutAttr;
        aggOutAttr.name = aggregateOpToString(op) + "(" + aggAttr.name + ")";
        aggOutAttr.type = TypeReal;
        aggOutAttr.length = 4;
        this->outputAttrs.push_back(aggOutAttr);
    }

    Aggregate::~Aggregate() = default;

    RC Aggregate::computeBasicAggregate() {
        if (input == nullptr) {
            basicResult = 0.0f;
            return 0;
        }

        void *tuple = malloc(PAGE_SIZE);
        if (tuple == nullptr) {
            return -1;
        }

        bool hasValue = false;
        float sum = 0.0f;
        float minValue = 0.0f;
        float maxValue = 0.0f;
        unsigned count = 0;

        while (input->getNextTuple(tuple) != QE_EOF) {
            Attribute foundAttr;
            std::vector<char> value;

            if (!getAttrValue(inputAttrs, tuple, aggAttr.name, foundAttr, value)) {
                continue;
            }

            float numericValue = getNumericValue(foundAttr.type, value.data());

            if (!hasValue) {
                minValue = numericValue;
                maxValue = numericValue;
                hasValue = true;
            }

            if (numericValue < minValue) {
                minValue = numericValue;
            }

            if (numericValue > maxValue) {
                maxValue = numericValue;
            }

            sum += numericValue;
            count++;
        }

        free(tuple);

        switch (op) {
            case MIN:
                basicResult = hasValue ? minValue : 0.0f;
                break;
            case MAX:
                basicResult = hasValue ? maxValue : 0.0f;
                break;
            case COUNT:
                basicResult = static_cast<float>(count);
                break;
            case SUM:
                basicResult = sum;
                break;
            case AVG:
                basicResult = count == 0 ? 0.0f : sum / static_cast<float>(count);
                break;
            default:
                basicResult = 0.0f;
                break;
        }

        return 0;
    }

    RC Aggregate::computeGroupAggregate() {
        if (input == nullptr) {
            return 0;
        }

        struct AggState {
            std::vector<char> groupValue;
            bool hasValue = false;
            float sum = 0.0f;
            float minValue = 0.0f;
            float maxValue = 0.0f;
            unsigned count = 0;
        };

        std::unordered_map<std::string, AggState> groups;

        void *tuple = malloc(PAGE_SIZE);
        if (tuple == nullptr) {
            return -1;
        }

        while (input->getNextTuple(tuple) != QE_EOF) {
            Attribute foundGroupAttr;
            Attribute foundAggAttr;
            std::vector<char> groupValue;
            std::vector<char> aggValue;

            if (!getAttrValue(inputAttrs, tuple, groupAttr.name, foundGroupAttr, groupValue)) {
                continue;
            }

            if (!getAttrValue(inputAttrs, tuple, aggAttr.name, foundAggAttr, aggValue)) {
                continue;
            }

            std::string key = makeKeyString(foundGroupAttr.type, groupValue);
            AggState &state = groups[key];

            if (state.groupValue.empty()) {
                state.groupValue = groupValue;
            }

            float numericValue = getNumericValue(foundAggAttr.type, aggValue.data());

            if (!state.hasValue) {
                state.minValue = numericValue;
                state.maxValue = numericValue;
                state.hasValue = true;
            }

            if (numericValue < state.minValue) {
                state.minValue = numericValue;
            }

            if (numericValue > state.maxValue) {
                state.maxValue = numericValue;
            }

            state.sum += numericValue;
            state.count++;
        }

        free(tuple);

        for (auto &entry : groups) {
            AggState &state = entry.second;

            float result = 0.0f;

            switch (op) {
                case MIN:
                    result = state.hasValue ? state.minValue : 0.0f;
                    break;
                case MAX:
                    result = state.hasValue ? state.maxValue : 0.0f;
                    break;
                case COUNT:
                    result = static_cast<float>(state.count);
                    break;
                case SUM:
                    result = state.sum;
                    break;
                case AVG:
                    result = state.count == 0 ? 0.0f : state.sum / static_cast<float>(state.count);
                    break;
                default:
                    result = 0.0f;
                    break;
            }

            groupResults.push(makeGroupAggregateTuple(state.groupValue, result));
        }

        return 0;
    }

    RC Aggregate::getNextTuple(void *data) {
        if (data == nullptr) {
            return QE_EOF;
        }

        if (!computed) {
            RC rc = isGroup ? computeGroupAggregate() : computeBasicAggregate();
            if (rc != 0) {
                return QE_EOF;
            }
            computed = true;
        }

        if (!isGroup) {
            if (returned) {
                return QE_EOF;
            }

            std::vector<char> tuple = makeAggregateTuple(basicResult);
            memcpy(data, tuple.data(), tuple.size());
            returned = true;
            return 0;
        }

        if (groupResults.empty()) {
            return QE_EOF;
        }

        std::vector<char> tuple = groupResults.front();
        groupResults.pop();

        memcpy(data, tuple.data(), tuple.size());
        return 0;
    }

    RC Aggregate::getAttributes(std::vector<Attribute> &attrs) const {
        attrs.clear();
        attrs = this->outputAttrs;
        return 0;
    }

} // namespace PeterDB
