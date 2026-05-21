## Project 3 Report


### 1. Basic information
 - Team #: 19
 - Github Repo Link: https://github.com/jiach14/cs122C-spring26-jiach14
 - Student 1 UCI NetID: songhag
 - Student 1 Name: Songhao Gao
 - Student 2 UCI NetID (if applicable): jiach14
 - Student 2 Name (if applicable): Jason He


### 2. Meta-data page in an index file
- Show your meta-data page of an index design if you have any.
  I use page 0 as the meta-data page. The meta page stores basic information about the B+ Tree.
  
- struct MetaPage {
  uint32_t magic;
  uint32_t initialized;
  PageNum rootPage;
  uint32_t height;
  };


### 3. Index Entry Format
- Show your index entry design (structure).
  - struct Node {
    bool isLeaf;
    PageNum nextLeaf;
    vector<vector<char>> keys;
    vector<PageNum> children;
    };

  - entries on internal nodes:  
  keys[0], keys[1], ..., keys[n-1]
  children[0], children[1], ..., children[n]
  
  - entries on leaf nodes:
    struct LeafEntry {
    vector<char> key;
    RID rid;
    };
  
  Leaf entries are sorted by key first, then by RID. Duplicate keys are allowed as long as the RID is different.


### 4. Page Format
- Show your internal-page (non-leaf node) design.
  - common node header:
    NODE_MAGIC      4 bytes
    isLeaf          1 byte
    keyCount        2 bytes
    nextLeaf        4 bytes
  - For internal pages, after the header I store:
    childCount      2 bytes
    children[]      childCount * 4 bytes
    keys[]          repeated variable-length keys
  - Each key is serialized as:
    keyLength       2 bytes
    keyBytes        keyLength bytes


- Show your leaf-page (leaf node) design.
  - common node header:
    NODE_MAGIC      4 bytes
    isLeaf          1 byte
    entryCount      2 bytes
    nextLeaf        4 bytes
  - each leaf entry is stored as:
    keyLength       2 bytes
    keyBytes        keyLength bytes
    rid.pageNum     4 bytes
    rid.slotNum     2 bytes


### 5. Describe the following operation logic.
- Split
  When inserting into a node, we first insert the new entry into the target leaf 
  and sort all entries. Then we check whether the serialized node still fits in 
  one page.
  - If the leaf node overflows:
    1. Split the leaf entries into two halves.
    2. Keep the first half in the original leaf page.
    3. Move the second half into a new right leaf page.
    4. Set right.nextLeaf to old left.nextLeaf.
    5. Set left.nextLeaf to the new right page.
    6. Promote the first key of the right leaf to the parent.
  - If an internal node overflows:
    1. Choose the middle key.
    2. Promote the middle key to the parent.
    3. Keep keys before the middle key in the left node.
    4. Move keys after the middle key into a new right internal node.
    5. Split the children accordingly.
  - If the root splits:
    1. Create a new internal root page.
    2. Put the promoted key into the new root.
    3. Set the old root and new right page as the two children.
    4. Update rootPage in the meta page.
    5. Increase tree height by 1.


- Rotation (if applicable)
  None


- Merge/non-lazy deletion (if applicable)
  The basic delete operation first finds the exact <key, RID> pair and removes
  it from the leaf page.

  For extra credit, I added a compact rebuild path for small trees after a
  deletion. If the index is small enough, the implementation scans the current
  leaf chain, collects the remaining entries, and rebuilds a compact B+ Tree
  from those entries. This removes underfilled pages from the active tree and
  can shrink the height of the tree. For example, if a height-2 tree can be
  represented as a height-1 tree after deletion, the metadata rootPage is
  updated to point to the new compact root. If only one entry remains, the new
  root is a leaf page and the height becomes 0.

  For large indexes, the implementation keeps the lazy-deletion behavior and
  does not rebuild after every deletion. This avoids making scan-delete tests
  too slow for large data sets.


- Duplicate key span in a page
  Leaf entries are sorted by:

  key first, then RID
  So multiple entries can have the same key as long as their RIDs are different,
  for example:
  5 : (1,1)
  5 : (1,2)
  5 : (2,1)
  When printing a leaf node, entries with the same key are grouped into one RID list:
  "5:[(1,1),(1,2),(2,1)]"
  If the exact same <key, RID> pair is inserted again, the insertion returns an error.


- Duplicate key span multiple pages (if applicable)
  Duplicate keys can span multiple leaf pages. The entries are still stored as
  individual <key, RID> pairs, and the leaf next pointers allow scans to keep
  walking through all pages that contain matching keys.

  The scan iterator keeps track of the last returned key/RID and the leaf page
  it came from. This lets the iterator continue correctly even if the caller
  deletes the entry that was just returned. If the entry is gone when
  getNextEntry() is called again, the iterator adjusts its page-local position
  and returns the next valid entry instead of skipping one.

### 6. Implementation Detail
- Have you added your own module or source file (.cc or .h)? 
  Clearly list the changes on files and CMakeLists.txt, if any.
  No new source files or CMake changes were added.


- Other implementation details:
  only modified the existing IX files:
  src/include/ix.h
  src/ix/ix.cc
  report/project3.md


### 7. Member contribution (for team of two)
- Explain how you distribute the workload in team.
  We worked together online throughout the project. Both members contributed to debugging, 
  implementing the IX component, running the test cases, and writing the report.


### 8. Other (optional)
- Freely use this section to tell us about things that are related to the project 3, but not related to the other sections (optional)



- Feedback on the project to help improve the project. (optional)
