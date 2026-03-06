# Wraith Changelog

#### v1.0.0
- Initial release
- Forces `kern.hv_vmm_present` to return a fixed value (0 or 1)
- Configured via `wrtvmm` boot-arg or NVRAM variable
- No per-process logic — all callers receive the same value
- Supports macOS Big Sur 11.3 through macOS Tahoe (26)
