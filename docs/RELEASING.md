# Releasing

The release is deliberately split into *build → gate → tag → publish* so that a
package is only published after it passed on real hardware, and so the bytes
users download are the bytes that were tested.

## 0. One-time setup

```bash
# GitHub CLI (MSI install; the binary lives in Program Files)
gh auth login

# Optional: sign SHA256SUMS.txt with minisign
minisign -G -W -p minisign.pub -s minisign.key      # -W = no passphrase
gh secret set MINISIGN_SECRET_KEY < minisign.key     # base64 not needed, raw file
git add minisign.pub && git commit -m "Add release signing key" && git push
```

Self-hosted runner for the hardware gate (Settings → Actions → Runners):

* Runs on the PC that has the device attached.
* Labels: `self-hosted`, `android-a22`.
* `adb`, `gh`, `unzip` and Git Bash must be on the runner's PATH; restart the
  runner service after installing them.
* Optional repository variable `ANDROID_SERIAL` if several devices are attached.

## 1. Build

```bash
cd /path/to/turnip-a22
./scripts/build_turnip_g2gen2.sh
ls -lh dist
```

Expected artifacts (all deterministic, see §6):

```
turnip-a22-26.3.0.adpkg.zip
turnip-a22-26.3.0.debug.zip
SHA256SUMS.txt
```

## 2. Hardware gate

Local (device attached, faster):

```bash
bash scripts/build_probes.sh
bash scripts/device_test.sh dist/turnip-a22-26.3.0.adpkg.zip
```

Expected tail:

```
compute dispatch: PASS (64/64 correct)
image clear+resolve: PASS (262144/262144 pixels correct)
PASS: driver gate satisfied
```

Or via CI once the package is on a release/tag:

```
Actions → device-test → Run workflow → tag: v26.3.0-a22.3
```

**Do not continue if the gate fails.** Record the output in the issue tracker and
fix the cause; a tag is cheap, a bad release is not.

## 3. Tag

```bash
git add -A
git commit -m "Turnip A22 26.3.0: deterministic packaging, debug symbols, device gate"
git push
git tag -a v26.3.0-a22.3 -m "Turnip A22 26.3.0 (packaging v2)"
git push origin v26.3.0-a22.3
```

Tags are immutable. If something is wrong after pushing, cut the next tag; never
move or delete a published one.

## 4. Publish

Workflow (rebuilds from the tag, then publishes):

```bash
gh workflow run release.yml -f tag=v26.3.0-a22.3
gh run watch
```

or attach the locally gated artifacts (what was actually tested):

```bash
gh release create v26.3.0-a22.3 \
  dist/turnip-a22-26.3.0.adpkg.zip \
  dist/turnip-a22-26.3.0.debug.zip \
  dist/SHA256SUMS.txt \
  --title "Turnip A22 26.3.0" \
  --generate-notes
```

If signing is configured, `SHA256SUMS.txt.minisig` is attached automatically by
the workflow; for a manual release add it explicitly.

## 5. Verify

```bash
gh release view v26.3.0-a22.3
# download one asset and compare
curl -L -o /tmp/a.zip "$(gh release view v26.3.0-a22.3 --json assets -q '.assets[] | select(.name|endswith("turnip-a22-26.3.0.adpkg.zip")) | .url')"
sha256sum /tmp/a.zip   # must match SHA256SUMS.txt
minisign -Vm SHA256SUMS.txt -p minisign.pub   # if signed
```

## 6. Determinism and debug symbols

* `scripts/package_driver.py` writes zips with fixed timestamps, sorted entries
  and fixed permissions. The same `.so` and `meta.json` always produce the same
  SHA256, so checksums are meaningful across machines.
* The driver `.so` inside the package is byte-identical to the binary validated
  on hardware; only the container and `meta.json` change between packaging
  releases. `sha256sum dist/stage-*/libvulkan_freedreno.so` proves it.
* Debug symbols ship separately (`*.debug.zip`) because AdrenoTools packages may
  only contain `meta.json` + the driver. Symbolize a tombstone address with:

  ```bash
  unzip turnip-a22-26.3.0.debug.zip
  llvm-symbolizer --obj=libvulkan_freedreno.so.debug 0xb90f3c
  ```

  Addresses in Android tombstones are module-relative (`#00 pc 0000000000b90f3c`),
  which is exactly what the debug file expects.

## 7. Release checklist

- [ ] `git status` clean, only the intended files staged
- [ ] `./scripts/build_turnip_g2gen2.sh` succeeds from a clean checkout
- [ ] device gate passes
- [ ] `dist/stage-*/libvulkan_freedreno.so` matches the previously validated hash
- [ ] `SHA256SUMS.txt` lists every attached asset
- [ ] README validation section reflects this run
- [ ] tag pushed, release published, checksums re-downloaded and verified
