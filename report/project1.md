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

Each record is stored as variable-length bytes. The first part of the record is the null-field indicator, whose size is ceil(number of fields / 8). After the null-field indicator, the record stores only the values of non-null fields. Int and Real fields are stored as 4-byte values. VarChar fields are stored as a 4-byte length followed by the actual character bytes. Null fields do not store value bytes inside the record.

The record itself does not store its page offset or length. That metadata is stored separately in the page slot directory.


- Describe how you store a null field.

Null fields are represented using the null-field indicator at the beginning of each record. The indicator uses ceil(number of fields / 8) bytes. Each bit corresponds to one field, where 1 means the field is NULL and 0 means the field is not NULL. The left-most bit of the first byte represents the first field. If a field is NULL, no value bytes are stored for that field in the record data area.


- Describe how you store a VarChar field.

A VarChar field is stored as a 4-byte length followed by the actual character bytes. The length value tells how many characters are stored for that field. The implementation only stores the actual string length, not the maximum declared attribute length, so shorter strings use less space. If the VarChar field is NULL, then no length or character bytes are stored for that field.


- Describe how your record design satisfies O(1) field access.

The page slot directory provides direct access to a record by RID. Given a page number and slot number, the system can locate the slot entry and use its offset to find the record bytes in the page.

However, the current internal record format does not fully provide O(1) access to individual fields. The record stores fields sequentially after the null-field indicator, and VarChar fields are variable-length. Therefore, to locate a later field, the implementation may need to scan earlier fields to compute the correct offset. A design with an internal field-offset directory would be needed to guarantee O(1) field access.


### 3. Page Format
- Show your page format design.
PageHeader stores freeSpaceOffset and slotCount
freeSpaceOffset is where the next record's bytes should be written
Slot entry is stored at the end of the page and grows backward.


- Explain your slot directory design if applicable.



### 4. Page Management
- Show your algorithm of finding next available-space page when inserting a record.



- How many hidden pages are utilized in your design?



- Show your hidden page(s) format design if applicable



### 5. Implementation Detail
- Other implementation details goes here.



### 6. Member contribution (for team of two)
- Explain how you distribute the workload in team.



### 7. Other (optional)
- Freely use this section to tell us about things that are related to the project 1, but not related to the other sections (optional)



- Feedback on the project to help improve the project. (optional)