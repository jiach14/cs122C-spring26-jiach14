#include "src/include/qe.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
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
        return QE_EOF;
    }

    RC BNLJoin::getNextTuple(void *data) {
        return QE_EOF;
    }

    RC BNLJoin::getAttributes(std::vector<Attribute> &attrs) const {
        attrs.clear();
        attrs = this->outputAttrs;
        return 0;
    }


    INLJoin::INLJoin(Iterator *leftIn, IndexScan *rightIn, const Condition &condition) {

    }

    INLJoin::~INLJoin() {

    }

    RC INLJoin::getNextTuple(void *data) {
        return -1;
    }

    RC INLJoin::getAttributes(std::vector<Attribute> &attrs) const {
        return -1;
    }

    GHJoin::GHJoin(Iterator *leftIn, Iterator *rightIn, const Condition &condition, const unsigned int numPartitions) {

    }

    GHJoin::~GHJoin() {

    }

    RC GHJoin::getNextTuple(void *data) {
        return -1;
    }

    RC GHJoin::getAttributes(std::vector<Attribute> &attrs) const {
        return -1;
    }

    Aggregate::Aggregate(Iterator *input, const Attribute &aggAttr, AggregateOp op) {

    }

    Aggregate::Aggregate(Iterator *input, const Attribute &aggAttr, const Attribute &groupAttr, AggregateOp op) {

    }

    Aggregate::~Aggregate() {

    }

    RC Aggregate::getNextTuple(void *data) {
        return -1;
    }

    RC Aggregate::getAttributes(std::vector<Attribute> &attrs) const {
        return -1;
    }
} // namespace PeterDB
