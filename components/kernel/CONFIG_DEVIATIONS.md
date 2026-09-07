# Kernel build: deviations from the device's running config

Base config is `recon/config.gz` — pulled from the running device — and is otherwise **byte-identical**
(verified: 0 differing CONFIG lines before the change below). `out/.config.device-exact` preserves
the untouched original for comparison.

## The only deviation

```
-CONFIG_SYSTEM_TRUSTED_KEYS="verity.x509.pem"
+CONFIG_SYSTEM_TRUSTED_KEYS=""
```

**Why it is necessary:** Meta's GPLv2 source release does not ship `verity.x509.pem`, and it exists
nowhere in the tree. The build cannot proceed without either that file or an empty value.

**Why it is safe — this keyring has no consumer in this configuration:**

| dependency | state | conclusion |
|---|---|---|
| `CONFIG_MODULE_SIG` | **not set** | keyring not used to verify modules |
| modules loaded on device | **0** (`/proc/modules` empty) | nothing to verify regardless |
| `CONFIG_DM_VERITY_AVB` | **not set** | dm-verity does not consult this keyring |

Android's dm-verity takes its key from the verity metadata / fstab table, not
`SYSTEM_TRUSTED_KEYRING`; that keyring is used by `DM_VERITY_AVB` and signed modules, both disabled
here. `ro.boot.veritymode=enforcing` still applies to `/system`, and is unaffected by a boot-image
change.

**The alternative was worse.** Generating a self-signed placeholder would populate the trusted
keyring with *our* key while looking like Meta's — an empty keyring is the honest representation of
"we do not have their cert".
