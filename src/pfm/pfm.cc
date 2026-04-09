#include "src/include/pfm.h"
#include <unordered_map>
#include <cstdint>
#include <cstring>

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
        auto it = g_openFiles.find(&fileHandle);
        if (it == g_openFiles.end()) {
            return -1;
        }

        OpenFileInfo &info = it->second;

        info.meta.readCounter = fileHandle.readPageCounter;
        info.meta.writeCounter = fileHandle.writePageCounter;
        info.meta.appendCounter = fileHandle.appendPageCounter;

        char page[PAGE_SIZE];
        memset(page, 0, PAGE_SIZE);
        memcpy(page, &info.meta, sizeof(HiddenPage));

        if (fseek(info.filePtr, 0, SEEK_SET) != 0) {
            fclose(info.filePtr);
            g_openFiles.erase(it);
            return -1;
        }

        if (fwrite(page, 1, PAGE_SIZE, info.filePtr) != PAGE_SIZE) {
            fclose(info.filePtr);
            g_openFiles.erase(it);
            return -1;
        }

        fflush(info.filePtr);
        fclose(info.filePtr);
        g_openFiles.erase(it);
        return 0;
    }

    FileHandle::FileHandle() {
        readPageCounter = 0;
        writePageCounter = 0;
        appendPageCounter = 0;
    }

    FileHandle::~FileHandle() = default;

    RC FileHandle::readPage(PageNum pageNum, void *data) {
        auto it = g_openFiles.find(this);
        if (it == g_openFiles.end() || data == nullptr) {
            return -1;
        }

        OpenFileInfo &info = it->second;

        if (pageNum >= info.meta.dataPageCount) {
            return -1; // requested page does not exist
        }

        long offset = static_cast<long>(pageNum + 1) * PAGE_SIZE; // +1 skips hidden page
        if (fseek(info.filePtr, offset, SEEK_SET) != 0) {
            return -1;
        }

        if (fread(data, 1, PAGE_SIZE, info.filePtr) != PAGE_SIZE) {
            return -1;
        }

        readPageCounter++;
        info.metaDirty = true;
        return 0;
    }

    RC FileHandle::writePage(PageNum pageNum, const void *data) {
        auto it = g_openFiles.find(this);
        if (it == g_openFiles.end() || data == nullptr) {
            return -1;
        }

        OpenFileInfo &info = it->second;

        if (pageNum >= info.meta.dataPageCount) {
            return -1; // requested page does not exist
        }

        long offset = static_cast<long>(pageNum + 1) * PAGE_SIZE; // +1 skips hidden page
        if (fseek(info.filePtr, offset, SEEK_SET) != 0) {
            return -1;
        }

        if (fwrite(data, 1, PAGE_SIZE, info.filePtr) != PAGE_SIZE) {
            return -1;
        }

        fflush(info.filePtr);
        writePageCounter++;
        info.metaDirty = true;
        return 0;
    }

    RC FileHandle::appendPage(const void *data) {
        auto it = g_openFiles.find(this);
        if (it == g_openFiles.end() || data == nullptr) {
            return -1;
        }

        OpenFileInfo &info = it->second;

        if (fseek(info.filePtr, 0, SEEK_END) != 0) {
            return -1;
        }

        if (fwrite(data, 1, PAGE_SIZE, info.filePtr) != PAGE_SIZE) {
            return -1;
        }

        fflush(info.filePtr);

        appendPageCounter++;
        info.meta.dataPageCount++;
        info.metaDirty = true;
        return 0;
    }

    unsigned FileHandle::getNumberOfPages() {
        auto it = g_openFiles.find(this);
        if (it == g_openFiles.end()) {
            return 0;
        }

        return it->second.meta.dataPageCount;
    }

    RC FileHandle::collectCounterValues(unsigned &readPageCount, unsigned &writePageCount, unsigned &appendPageCount) {
        auto it = g_openFiles.find(this);
        if (it == g_openFiles.end()) {
            return -1;
        }

        readPageCount = readPageCounter;
        writePageCount = writePageCounter;
        appendPageCount = appendPageCounter;
        return 0;
    }

} // namespace PeterDB