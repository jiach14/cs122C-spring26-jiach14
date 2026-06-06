## Project 4 Report


### 1. Basic information
- Team #: 19
- Github Repo Link: https://github.com/jiach14/cs122C-spring26-jiach14
- Student 1 UCI NetID: songhag
- Student 1 Name: Songhao Gao
- Student 2 UCI NetID (if applicable): jiach14
- Student 2 Name (if applicable): Jason He


### 2. Catalog information about Index
- Show your catalog information about an index (tables, columns).

The RM keeps the same `Tables` and `Columns` catalogs from Project 2. Index files are named deterministically as:

`<table-name>_<attribute-name>.idx`

For example, an index on `left.B` is stored in `left_B.idx`. The table schema remains in `Columns`; index existence is reflected by the corresponding index file. Tuple-level RM operations check for an existing index file for each attribute and update it when present.


### 3. Filter
- Describe how your filter works (especially, how you check the condition.)

`Filter` reads tuples from its input iterator one at a time. For each tuple, it locates the condition's left-hand attribute using the input schema, extracts the raw API-format value, and compares it either with another tuple attribute or with the constant `rhsValue`.

The comparison helper supports `TypeInt`, `TypeReal`, and `TypeVarChar`, then applies the requested `CompOp`. Tuples with null values in the compared attributes are skipped.


### 4. Project
- Describe how your project works.

`Project` stores the input schema and builds an output schema in the exact order requested by `attrNames`. For every input tuple, it creates a new null-indicator for the projected attributes, copies only the selected attribute values into the output tuple, and sets output null bits for missing/null projected attributes.


### 5. Block Nested Loop Join
- Describe how your block nested loop join works (especially, how you manage the given buffers.)

`BNLJoin` uses the left iterator as the outer input and the right `TableScan` as the inner input. It loads up to `numPages * PAGE_SIZE` bytes of left tuples into an in-memory hash table keyed by the join attribute. Duplicate keys map to a vector of left tuples.

After a block is loaded, the right table scan is reset and scanned tuple by tuple. Matching right tuples probe the hash table, and each matching left tuple is merged with the right tuple and queued for output. When the right scan finishes, the next left block is loaded.


### 6. Index Nested Loop Join
- Describe how your index nested loop join works.

`INLJoin` reads one tuple from the left input, extracts its join key, and resets the right `IndexScan` to the matching key range. For equality joins, the right index scan is set to `[key, key]`. Other comparison operators are translated into the equivalent right-side range when possible; `NE_OP` and `NO_OP` use a full index scan and verify the merged tuple condition afterward.

For each right tuple returned by the index scan, the operator merges the current left tuple with the right tuple and returns the joined tuple.


### 7. Grace Hash Join (If you have implemented this feature)
- Describe how your grace hash join works (especially, in-memory structure).

`GHJoin` creates `left_join...` and `right_join...` temporary partition files using unique join ids, and deletes them in the destructor. The output implementation builds an in-memory hash table over the left input using the join attribute as the key; duplicate keys map to a vector of stored left tuples. It then scans the right input, probes the hash table, and queues all matching merged tuples.

The hash key includes the attribute type plus the raw value bytes, so `INT`, `REAL`, and `VARCHAR` join keys are handled uniformly.


### 8. Aggregation
- Describe how your basic aggregation works.

Basic aggregation scans the input once, extracts the aggregate attribute, and maintains `count`, `sum`, `min`, and `max` state. At EOF, it computes the requested operation (`MIN`, `MAX`, `COUNT`, `SUM`, or `AVG`) and returns a single tuple with one `REAL` value.

- Describe how your group-based aggregation works. (If you have implemented this feature)

Group-based aggregation uses an in-memory hash table keyed by the group-by attribute's raw value bytes plus type. Each group stores the original group value and aggregate state. After the input is consumed, the operator queues one output tuple per group containing the group value followed by the aggregate result.


### 9. Implementation Detail
- Have you added your own module or source file (.cc or .h)?
  Clearly list the changes on files and CMakeLists.txt, if any.

No new modules or source files were added. Project 4 logic is implemented in:

- `src/rm/rm.cc`
- `src/include/rm.h`
- `src/qe/qe.cc`
- `src/include/qe.h`

No CMakeLists changes were required.

- Other implementation details:

RM index maintenance is coordinated during tuple insert, delete, and update. Null attribute values are not inserted into indices. `deleteTable` also attempts to destroy all index files for the table's attributes.

QE tuple helpers parse and copy API-format tuples, including null indicators and variable-length strings. Join output schemas are the left schema followed by the right schema, with duplicate attribute names preserved as required.


### 10. Member contribution (for team of two)
- Explain how you distribute the workload in team.

Songhao Gao completed most of the base RM/QE implementation work. Jason He finished the missing Project 4 pieces, completed the extra credit operators, cleaned up behavior against the public tests, and completed this report.


### 11. Other (optional)
- Freely use this section to tell us about things that are related to the project 4, but not related to the other sections (optional)

All local public QE tests pass.

- Feedback on the project to help improve the project. (optional)

