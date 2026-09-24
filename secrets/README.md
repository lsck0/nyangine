# secrets

Committed, and encrypted with [sops](https://github.com/getsops/sops) against a GPG key. The point is
that a secret lives with the code it belongs to, versioned like the code, reviewable like the code —
and unreadable to anyone without the key, including GitHub.

Nothing here is a real key yet, and there is deliberately no encrypted stand-in: producing one would
have meant generating a GPG key and committing it, and a key in a public repository is not a key.

| File                        | What it is                                                             |
| --------------------------- | ---------------------------------------------------------------------- |
| `signing.pfx.enc.example`   | Holds the path open, and the exact commands that replace it. Not a secret. |
| `signing.pfx.enc`           | The Authenticode certificate, once you make one. Does not exist yet.   |
| `.sops.yaml`                | At the repository root. Which key encrypts what.                       |

## Rules

- **Never commit a plaintext secret.** `.gitignore` refuses the obvious names, but the check that
  matters is yours. A secret that touched the working tree unencrypted is a secret that needs rotating.
- **Decrypt to a temporary file, and delete it.** Nothing in this repository reads a decrypted secret
  from `secrets/`; the build reads a path out of the environment. See below.
- **Rotate on exposure, not on a schedule.** A key that was in a CI log once is burnt.

## Making a key

Once, on a machine you control. The key never leaves it except as the export CI needs.

```sh
gpg --quick-generate-key "nyangine release <you@example.com>" rsa4096 sign,encrypt never
gpg --list-secret-keys --keyid-format=long
```

Put the 40 character fingerprint into `.sops.yaml` in place of the zeroes, and commit that.

## Adding or changing a secret

```sh
sops secrets/some-secret.yaml          # opens $EDITOR on the plaintext, re-encrypts on save
sops --encrypt --input-type binary --output-type binary \
     real-certificate.pfx > secrets/signing.pfx.enc
```

Then `git add secrets/ && git commit`. Review the diff first: for a yaml secret the keys stay readable,
so you can see *which* value changed without seeing what it changed to.

Adding a second person means adding their fingerprint to `.sops.yaml` and running
`sops updatekeys secrets/<file>` for every file, which re-encrypts to the new recipient list.

## How CI decrypts

CI needs the private key, so it gets an export of it as a repository secret. Two secrets, one key:

```sh
gpg --export-secret-keys --armor <FINGERPRINT> | base64 -w0     # -> GPG_PRIVATE_KEY
# the passphrase, if the key has one                            # -> GPG_PASSPHRASE
```

`gh secret set GPG_PRIVATE_KEY < key.txt`, and delete `key.txt`.

The release workflow imports it, decrypts the certificate to the runner's temporary directory — never
the workspace, which is archived — and points the build at it through the three `NYA_SIGNING_*`
overrides `flags.h` documents:

```yaml
- name: Decrypt the signing certificate
  env:
    GPG_PRIVATE_KEY: ${{ secrets.GPG_PRIVATE_KEY }}
    GPG_PASSPHRASE: ${{ secrets.GPG_PASSPHRASE }}
  run: |
    printf '%s' "${GPG_PRIVATE_KEY}" | base64 -d | gpg --batch --import
    sops --decrypt --input-type binary --output-type binary \
      secrets/signing.pfx.enc > "${RUNNER_TEMP}/signing.pfx"
    echo "NYA_SIGNING_PFX=${RUNNER_TEMP}/signing.pfx" >> "$GITHUB_ENV"
```

Without those secrets set the step is skipped, the build falls back to the sample certificate in
`.signing/`, and the release is unsigned rather than failing. A fork has no secrets and has to keep
building.

## Plugin signing keys

Separate from the Authenticode certificate above, and simpler. A plugin is signed with an Ed25519 key
so a build that requires it (the default; see `NYA_PLUGIN_REQUIRE_SIGNATURE` in
`src/nyangine/core/core_plugin_signature.h`) will load it and refuse an unsigned one.

```sh
./build plugin keygen --seed my.seed        # draws a key, writes the seed, prints the public key
./build plugin sign plugins/hello --seed my.seed
```

`keygen` prints only the public key — pin that in the program with `nya_plugin_trust_key`, the way
`src/gnyame/gnyame.c` pins the one the bundled `plugins/hello` is signed with. The **seed is the private
key**: keep it off the tree (`*.seed` is gitignored), keep it to yourself (`keygen` writes it `0600`),
and rotate it if it ever leaks, exactly as for any other key here. There is deliberately no way to
recover a lost seed: a new one means a new public key to pin and re-signing what it signed.

## Why sops and gpg, and not the alternatives

- **age** is a better key format and sops supports it. gpg wins here only because the Windows signing
  story already needs gpg on the machine, and one key management story beats two.
- **git-crypt** encrypts transparently on checkout, which means a working tree full of plaintext secrets
  and a `.gitattributes` mistake away from committing one.
- **A secret manager** is a network dependency and an account for something this repository can hold in
  eight kilobytes. The style rule is that everything runs locally; a build that cannot sign without a
  reachable vault does not.
