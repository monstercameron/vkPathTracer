# Move-Only Decomposition Checklist

Use this checklist when splitting a large translation unit without changing behavior.

## Before editing

- Run `tools/source_size_report.ps1 -CheckGuardrails` and note the target file size.
- Identify the smallest domain slice to move.
- Confirm the move does not require a public API rename.
- Prefer focused transitional headers when direct header extraction would force broad call-site churn.

## During the move

- Move code first; do not change algorithms, defaults, output text, enum values, or serialization order.
- Add the new `.cpp` to the existing CMake source list in the same module group.
- Keep helper functions private to the new `.cpp` unless more than one split file needs them.
- Do include cleanup only after a green build.

## Validation

Run these commands after each compiled split:

```powershell
cmake --build --preset desktop-clang-debug
cmake --build --preset windows-clang-vulkan-debug
ctest --test-dir build/presets/desktop-clang-debug --output-on-failure
ctest --test-dir build/presets/windows-clang-vulkan-debug --output-on-failure
```

When D3D12 or Qt code changes, also run the matching preset used by `run.ps1`.

## Review

- Check `git diff --stat` and verify the source line reduction is in the intended file.
- Search for stale helpers left in the original file.
- Confirm new files have focused includes and no unrelated formatting churn.
- Update `docs/todos.md` only for tasks with completed acceptance evidence.
