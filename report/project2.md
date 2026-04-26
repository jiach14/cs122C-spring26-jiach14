## Project 2 Report


### 1. Basic information
 - Team #: 19
 - Github Repo Link: https://github.com/jiach14/cs122C-spring26-jiach14
 - Student 1 UCI NetID: songhag
 - Student 1 Name: songhao gao
 - Student 2 UCI NetID (if applicable):
 - Student 2 Name (if applicable):

### 2. Meta-data
- Show your meta-data design (Tables and Columns table) and information about each column.

| Column Name | Type        | Description                    |
| ----------- | ----------- | ------------------------------ |
| table-id    | int         | Unique identifier of the table |
| table-name  | varchar(50) | Name of the table              |
| file-name   | varchar(50) | Corresponding file name        |


| Column Name     | Type        | Description                                     |
| --------------- | ----------- | ----------------------------------------------- |
| table-id        | int         | Table identifier                                |
| column-name     | varchar(50) | Attribute name                                  |
| column-type     | int         | Attribute type (TypeInt, TypeReal, TypeVarChar) |
| column-length   | int         | Attribute length                                |
| column-position | int         | Position in schema                              |



### 3. Internal Record Format (in case you have changed from P1, please re-enter here)
- Show your record format design.

[ null-indicator ][ field-offset array ][ field data ]

- Describe how you store a null field.

Null fields are stored using a null-indicator bitmap located at the beginning of each record.

Each bit in the bitmap corresponds to one attribute:
A bit value of 1 indicates that the attribute is NULL
A bit value of 0 indicates that the attribute contains a valid value

- Describe how you store a VarChar field.

[ length (4 bytes) ][ actual string bytes ]

- Describe how your record design satisfies O(1) field access.

The record design achieves O(1) field access by maintaining an offset array in the record header.

For each attribute, the offset array stores the end position of that field within the record. Given a field index i:
The start position can be determined from the previous offset (or header size for the first field)
The end position is directly retrieved from the offset array

### 4. Page Format (in case you have changed from P1, please re-enter here)
- Show your page format design.

[ Page Header | Record Data Area | Free Space | Slot Directory ]

- Explain your slot directory design if applicable.

Slot Directory Design

Each slot entry contains:
offset: starting position of the record in the page
length: length of the record

Special values for length:
-1: deleted record
-2: tombstone (record has been moved, contains forwarding RID)


### 5. Page Management (in case you have changed from P1, please re-enter here)
- How many hidden pages are utilized in your design?
  no hidden pages are used


- Show your hidden page(s) format design if applicable
  Not applicable.


### 6. Describe the following operation logic.
- Delete a record
  Locate the record using its RID (page number and slot number).
  If the slot represents a tombstone, recursively delete the forwarded record.
  Mark the slot as deleted by setting its length to a special value (e.g., -1).
  Remove the record data from the page and compact the remaining records to eliminate gaps.
  Update the offsets of affected records in the slot directory.


- Update a record 
- - New size equals old size 
  - Overwrite the record in place. 
- - New size is smaller
  - Overwrite the record 
  - Shift subsequent records forward to compact space 
  - Update slot offsets accordingly 
- - New size is larger 
  - If there is enough free space in the page:
  - Relocate the record within the same page 
- - Otherwise:
  - Insert the updated record into another page 
  - Replace the original record with a tombstone storing a forwarding RID


- Scan on normal records
  
  - Sequentially scan all pages and slots
    Skip invalid slots
    Use readRecord() to retrieve record data
    Apply the filter condition (if any)
    Return only projected attributes


- Scan on deleted records

  - Deleted records are identified by a special slot marker (e.g., length = -1)
    These records are skipped during scan and not returned



- Scan on updated records
  - Updated records that have been moved are marked as tombstones (e.g., length = -2)
  - The original slot is skipped during scan
  - The actual record exists at the forwarded location and will be scanned normally
  - This ensures each record is returned exactly once without duplication


### 7. Implementation Detail
- Other implementation details goes here.
  - RM is implemented as a wrapper on top of RBFM
  - All catalog data (Tables, Columns) is stored using RBFM and persisted on disk
  - Each operation opens and closes files as required
  - Scan is implemented using iterators without caching results in memory
  - Memory is managed using malloc/free
  - Tombstone and page compaction are handled in RBFM
  - RID remains stable after updates and deletions


### 8. Member contribution (for team of two)
- Explain how you distribute the workload in team.
  - songhao: Implemented RBFM functions (deleteRecord, updateRecord, readAttribute, scan) and page management logic
  - jason: 
  - Both collaborated on debugging, testing, and final integration of the system.


### 9. Other (optional)
- Freely use this section to tell us about things that are related to the project 1, but not related to the other sections (optional)



- Feedback on the project to help improve the project. (optional)