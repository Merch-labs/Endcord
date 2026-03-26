# Contributing

Thanks for contributing to Endcord.

## Local development

```bash
./scripts/setup-local-libcxx.sh
cmake --preset linux-clang-local-libcxx
cmake --build --preset build-local -j4
ctest --test-dir build-local --output-on-failure
```

## Pull request expectations

- Keep Linux `.so` builds working.
- Update `config/config.json.example` when config fields change.
- Update `config/config.schema.json` when config structure changes.
- Update `README.md` for user-visible behavior changes.
- Update `CHANGELOG.md` for release-worthy changes.
