#include "src/include/pfm.h"
#include <unordered_map>
#include <cstdint>

namespace {
    struct HiddenPage {
        uint32_t readCounter;
        uint32_t writeCounter;
        uint32_t appendCounter;
        uint32_t dataPageCount;   // excludes hidden page
        // rest of PAGE_SIZE bytes can stay unused
    };

    struct OpenFileInfo {
        FILE *filePtr;            
        HiddenPage meta;
        bool metaDirty;
        std::string fileName;
    };

    std::unordered_map<PeterDB::FileHandle *, OpenFileInfo> g_openFiles;
}

namespace PeterDB {
    PagedFileManager &PagedFileManager::instance() {
        static PagedFileManager _pf_manager = PagedFileManager();
        return _pf_manager;
    }

    PagedFileManager::PagedFileManager() = default;

    PagedFileManager::~PagedFileManager() = default;

    PagedFileManager::PagedFileManager(const PagedFileManager &) = default;

    PagedFileManager &PagedFileManager::operator=(const PagedFileManager &) = default;

    RC PagedFileManager::createFile(const std::string &fileName) {
        std::ifstream infile(fileName);
        if (infile.good()) {
            infile.close();
            return -1; // file already exists
        }
        infile.close(); // close stream

        FILE *filePtr = fopen(fileName.c_str(), "wb+");
        if (filePtr == nullptr) {
            return -1; // failed to create file
        }

        // hidden page (page 0)
        char page[PAGE_SIZE];
        memset(page, 0, PAGE_SIZE);

        HiddenPage meta{};
        memcpy(page, &meta, sizeof(HiddenPage));

        size_t written = fwrite(page, 1, PAGE_SIZE, filePtr);
        fflush(filePtr);
        fclose(filePtr);

        return (written == PAGE_SIZE) ? 0 : -1; // success if we wrote a full page
    }

    RC PagedFileManager::destroyFile(const std::string &fileName) {
        for (const auto &entry : g_openFiles) {
            if (entry.second.fileName == fileName) {
                return -1; // file is currently open
            }
        }

        return (std::remove(fileName.c_str()) == 0) ? 0 : -1; // success if file was removed
    }

    RC PagedFileManager::openFile(const std::string &fileName, FileHandle &fileHandle) {
        if (g_openFiles.count(&fileHandle) > 0) {
            return -1; // fileHandle is already in use
        }

        FILE *filePtr = fopen(fileName.c_str(), "rb+");
        if (filePtr == nullptr) {
            return -1; // failed to open file
        }

        // read hidden page (page 0)
        char page[PAGE_SIZE];
        if (fseek(filePtr, 0, SEEK_SET) != 0 || fread(page, 1, PAGE_SIZE, filePtr) != PAGE_SIZE) {
            fclose(filePtr);
            return -1; // failed to read hidden page
        }

        HiddenPage meta{};
        memcpy(&meta, page, sizeof(HiddenPage));

        // ssync FileHandle counters from persistent metadata
        fileHandle.readPageCounter = meta.readCounter;
        fileHandle.writePageCounter = meta.writeCounter;
        fileHandle.appendPageCounter = meta.appendCounter;

        g_openFiles[&fileHandle] = {filePtr, meta, false, fileName};
        return 0; // success
    }

    RC PagedFileManager::closeFile(FileHandle &fileHandle) {
        return -1;
    }

    FileHandle::FileHandle() {
        readPageCounter = 0;
        writePageCounter = 0;
        appendPageCounter = 0;
    }

    FileHandle::~FileHandle() = default;

    RC FileHandle::readPage(PageNum pageNum, void *data) {
        return -1;
    }

    RC FileHandle::writePage(PageNum pageNum, const void *data) {
        return -1;
    }

    RC FileHandle::appendPage(const void *data) {
        return -1;
    }

    unsigned FileHandle::getNumberOfPages() {
        return -1;
    }

    RC FileHandle::collectCounterValues(unsigned &readPageCount, unsigned &writePageCount, unsigned &appendPageCount) {
        return -1;
    }

} // namespace PeterDB