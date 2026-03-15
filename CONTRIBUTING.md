# Contributing

Thanks for contributing to Bedrock Discord Bridge.

## Local development

Plugin build:

```bash
./scripts/setup-local-libcxx.sh
cmake --preset linux-clang-local-libcxx
cmake --build --preset build-local
```

Bot setup:

```bash
cd bot
python3 -m venv .venv
. .venv/bin/activate
pip install -e .
```

## Validation before opening a PR

Run these commands from the repository root unless noted otherwise:

```bash
cmake --build --preset build-local
python3 -m compileall bot/src
```

Optional bot packaging check:

```bash
cd bot
python3 -m build
```

## Pull request expectations

- Keep Endstone plugin changes compatible with Linux `.so` builds.
- Do not expose the bot bridge API publicly by default.
- Update JSON examples and schema files when config fields change.
- Update [CHANGELOG.md](CHANGELOG.md) for user-visible changes.
