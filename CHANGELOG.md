# Change Log
All notable changes to Sylvan will be documented in this file.

## [1.1.0] - 2017-01-09
### Added
- This CHANGELOG file.
- Custom leaves can now implement custom callbacks for writing/reading to/from files.
- Implemented GMP leaf writing/reading to/from file.
- Method `mtbdd_eval_compose` for proper function composition (after partial evaluation).
- Method `mtbdd_enum_par_*` for parallel path enumeration.
- LDD methods `relprod` and `relprev` now support action labels (meta 5).
- Examples program `ldd2bdd` now converts LDD transition systems to BDDs transition systems.
- Methods `cache_get6` and `cache_put6` for operation cache entries that require two buckets.
- File `sylvan.pc` for pkg-config.

### Changed
- The API to register a custom MTBDD leaf now requires multiple calls, which is better design for future extensions.
- When rehashing during garbage collection fails (due to finite length probe sequences), Sylvan now increases the probe sequence length instead of aborting with an error message. However, Sylvan will probably still abort due to the table being full, since this error is typically triggered when garbage collection does not remove many dead nodes.

### Fixed
- Methods `mtbdd_enum_all_*` fixed and rewritten.

### Removed
- We no longer use both autoconf makefiles and CMake. Instead, we removed the autoconf files and rely solely on CMake now.
