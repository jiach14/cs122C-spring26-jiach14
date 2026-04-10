## Project 1 Report


### 1. Basic information
 - Team #: 19
 - Github Repo Link: https://github.com/jiach14/cs122C-spring26-jiach14.git
 - Student 1 UCI NetID: jiach14
 - Student 1 Name: Jason He
 - Student 2 UCI NetID (if applicable): songhag
 - Student 2 Name (if applicable): Songhao Gao


### 2. Internal Record Format
- Show your record format design.

Each record is stored as variable-length bytes. The first part of the record is the null-field indicator, whose size is ceil(number of fields / 8). After that, the record stores a field-offset directory with one 2-byte end offset for each field. The actual field data is stored after the offset directory. Int and Real fields are stored as 4-byte values. VarChar fields are stored as a 4-byte length followed by the actual character bytes. Null fields do not store value bytes inside the field data area.

The record itself does not store its page offset or length. That metadata is stored separately in the page slot directory.


- Describe how you store a null field.

Null fields are represented using the null-field indicator at the beginning of each record. The indicator uses ceil(number of fields / 8) bytes. Each bit corresponds to one field, where 1 means the field is NULL and 0 means the field is not NULL. The left-most bit of the first byte represents the first field. If a field is NULL, no value bytes are stored for that field in the record data area. 

In the field-offset directory, a null field has the same end offset as the previous field, or the beginning of the field data area if it is the first field, because it contributes zero bytes to the field data area.


- Describe how you store a VarChar field.

A VarChar field is stored as a 4-byte length followed by the actual character bytes. The length value tells how many characters are stored for that field. The implementation only stores the actual string length, not the maximum declared attribute length, so shorter strings use less space. If the VarChar field is NULL, then no length or character bytes are stored for that field.


- Describe how your record design satisfies O(1) field access.

The internal record format includes a field-offset directory after the null-field indicator. The directory stores a 2-byte end offset for each field. To access field i, the system directly reads the end offset for field i. For field 0, the start offset is the beginning of the field data area; for other fields, the start offset is the previous field’s end offset. Therefore, the byte range of a field can be found without scanning all previous fields. Null fields use the same end offset as the previous field, or the beginning of the field data area if it is the first field, because they store no value bytes.


### 3. Page Format
- Show your page format design.

Each page starts with a fixed-size page header. The page header contains two 2-byte values: `freeSpaceOffset` and `slotCount`. `freeSpaceOffset` points to the next free byte after the stored record data, and `slotCount` records how many slot entries are in the slot directory.

Record data is stored after the page header and grows toward higher byte offsets. The slot directory is stored at the end of the page and grows backward toward lower byte offsets. The free space is kept between the record data area and the slot directory.

The layout is:

`[PageHeader][Record Data ...][Free Space][Slot Directory]`

The slot directory stores metadata for records in the page, while the actual record bytes are stored in the record data area.


- Explain your slot directory design if applicable.

The slot directory is stored at the end of each page and grows backward. Each slot entry stores the offset and length of one record in the page. The RID contains the page number and slot number, so after reading the page, the system can use the slot number to directly locate the slot entry. The slot entry then gives the byte offset and length needed to access the record data.

Deleted slots are marked with a special length value and can be reused by later inserts. If a record is updated and moved to another page, the original slot can be marked as a tombstone that stores a forwarding RID to the new location.



### 4. Page Management
- Show your algorithm of finding next available-space page when inserting a record.

When inserting a record, the system first calculates the internal record size. It then checks the last page first. For each page, it checks whether there is a reusable deleted slot. If a deleted slot exists, the page only needs enough free space for the new record bytes. If no deleted slot exists, the page needs enough free space for both the new record bytes and a new slot entry.

If the last page has enough free space, the record is inserted there. Otherwise, the system scans from the beginning of the file and chooses the first page with enough free space. If no existing page can fit the record, a new page is initialized and appended to the file.


- How many hidden pages are utilized in your design?

One hidden page is used per file. It is stored as physical page 0 and is not counted as a user data page.


- Show your hidden page(s) format design if applicable

The hidden page stores persistent file metadata using the following format:

`[readCounter][writeCounter][appendCounter][dataPageCount][unused space]`

Each counter is stored as a 4-byte unsigned integer. `readCounter`, `writeCounter`, and `appendCounter` store the persistent FileHandle operation counters. `dataPageCount` stores the number of user-visible data pages in the file, excluding the hidden page. The remaining bytes in the hidden page are unused.


### 5. Implementation Detail
- Other implementation details goes here.

File operation counters are persistent. When a file is opened, the counters and data page count are loaded from the hidden page. When the file is closed, the updated metadata is written back to the hidden page.

The RecordBasedFileManager uses the PagedFileManager for file creation, deletion, opening, and closing. For records, `insertRecord` converts the API input format into the internal record format with a field-offset directory. `readRecord` converts the internal format back into the API format expected by the caller.

Deleted records are marked in the slot directory using special slot length values: -1 for deleted slots and -2 for tombstones. These deleted slots can be reused by later inserts. If an updated record grows and is moved to a new location, the original slot is marked as a tombstone and stores a forwarding RID. 


### 6. Member contribution (for team of two)
- Explain how you distribute the workload in team.
We distributed the implementation work across the PFM and RBFM components. Please check the commit history for detailed contribution records.

Songhao Gao provided more help with implementing the functions.
Jason He cleaned up the code and finished the report.


### 7. Other (optional)
- Freely use this section to tell us about things that are related to the project 1, but not related to the other sections (optional)



- Feedback on the project to help improve the project. (optional)
