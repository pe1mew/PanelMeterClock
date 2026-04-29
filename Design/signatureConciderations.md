# FOTA Signature Considerations
## PanelMeterClock Firmware

---

## 1. Why a Detached Signature?

### 1.1 The Core Problem

The device needs to verify that a firmware image was produced by a trusted party (the author) before writing it to flash. That requires a cryptographic signature. The question is where to put it.

### 1.2 What "Detached" Means

A **detached signature** is a separate file containing only the signature bytes — it does not wrap or modify the firmware binary in any way. The firmware binary remains a raw, valid `.bin` file that the ESP-IDF OTA API can consume directly. The signature covers the binary's content but lives alongside it.

### 1.3 Why the Current Design Uses Two Files

The OTA API (`esp_ota_write`) expects a raw firmware binary. It streams the binary directly to the flash partition in chunks — it has no concept of a wrapper format. If the signature were embedded in the binary (prepended, appended, or in a header), the OTA writer would either:

- write the signature bytes into flash as part of the firmware image (corrupting it), or
- require the firmware to parse and strip the wrapper before writing, which means the entire image must fit in RAM first — not feasible on a device with 512 KB SRAM and a binary that can be hundreds of kilobytes.

A detached signature avoids this: the web server receives the binary stream and the signature separately, buffers only the small signature (64 bytes for Ed25519), computes the SHA-256 digest of the binary *as it streams*, verifies the signature at the end, and only then commits the OTA partition as bootable. Nothing extra has to fit in RAM; nothing modifies the binary.

---

## 2. The Verification Process

### 2.1 Step-by-Step Flow

```
Developer machine                         Device (ESP32-S3)
─────────────────                         ─────────────────
1. Build firmware.bin
2. sha256(firmware.bin) → digest
3. Ed25519_sign(digest, private_key)
   → firmware.sig  (64 bytes)
4. Ship both files to browser

                    Browser uploads both ──►
                                          5. Receive firmware.sig (tiny, buffer it)
                                          6. Receive firmware.bin chunk by chunk:
                                             - feed each chunk to SHA-256 context
                                             - feed each chunk to esp_ota_write()
                                          7. Finalise SHA-256 → digest
                                          8. Ed25519_verify(digest, firmware.sig,
                                                            public_key_in_flash)
                                          9. SUCCESS → esp_ota_set_boot_partition()
                                             FAILURE  → esp_ota_abort(); erase slot
```

### 2.2 Key Properties

- The **private key never touches the device**. It lives only on the developer's signing machine.
- The **public key is baked into the firmware** at build time and excluded from OTA writes by the partition layout (FR-SEC-001, DC-006), so it cannot be replaced by a rogue update.
- The **binary is verified before it is declared bootable** — a failed signature leaves the running firmware unchanged (FR-WEB-042).

---

## 3. Alternatives Using a Single File

### 3.1 Appended Signature

The signature (64 bytes) is concatenated after the firmware binary. The OTA handler streams `file_size − 64` bytes to flash while hashing them, buffers the final 64 bytes as the signature, then verifies.

| Aspect | Detail |
|--------|--------|
| **Production** | `cat firmware.bin firmware.sig > firmware_signed.bin` |
| **Pro** | Single file; trivial to produce |
| **Con** | OTA handler must know total file length in advance from `Content-Length`; HTTP chunked transfer without `Content-Length` makes this awkward. On failure the OTA slot must be erased after the fact. |

### 3.2 Prepended Signature or Header

Signature bytes come first; the raw binary follows.

| Aspect | Detail |
|--------|--------|
| **Pro** | No need to know total length; buffer the small header first, then stream the rest directly to `esp_ota_write()` |
| **Con** | The binary no longer starts at offset 0; the OTA handler must skip the header before feeding bytes to the flash writer. Non-standard, but straightforward to implement. |

### 3.3 Container Format (e.g., custom envelope or ZIP)

A structured envelope wraps both the binary and the signature.

| Aspect | Detail |
|--------|--------|
| **Pro** | Self-describing, extensible, single file |
| **Con** | Device must parse the container before streaming to OTA. Formats like ZIP require random-access or decompression — impractical for direct flash streaming on a memory-constrained device. |

### 3.4 ESP-IDF Secure Boot v2 (Hardware-Enforced)

The ESP32-S3 supports hardware secure boot: the bootloader verifies a signature embedded in the firmware image itself before every boot, using a key burned into eFuses.

| Aspect | Detail |
|--------|--------|
| **Pro** | Verification at every boot, not just at OTA time; hardware enforcer cannot be bypassed in software; single signed image file using the standard ESP-IDF signed binary format |
| **Con** | Requires burning eFuses (irreversible); once enabled, unsigned firmware can never boot on that device again — a signing infrastructure failure permanently bricks the device. Requires `espsecure.py` toolchain and specific bootloader/partition configuration. |

---

## 4. Comparison Summary

| Approach | Files | RAM requirement | Complexity | Reversible |
|----------|-------|----------------|------------|------------|
| Detached signature (current) | 2 | Signature only (64 B) | Low | Yes |
| Appended signature | 1 | Signature only (64 B) | Low | Yes |
| Prepended signature | 1 | Header only (64 B) | Low | Yes |
| Container format | 1 | Container parser overhead | Medium | Yes |
| ESP-IDF Secure Boot v2 | 1 | None (hardware) | High | **No** |

---

## 5. Recommendation

The two-file detached signature approach specified in PMC-FRS-001 (FR-SEC-002) and PMC-STD-001 (§5.8) is appropriate for this project: it is simple to implement with standard ESP-IDF APIs, requires no additional RAM budget, and the signing toolchain is minimal (`openssl dgst -sign`).

If a single-file workflow is preferred, **appended signature** is the lowest-effort change: adjust the OTA handler to split the incoming stream at `Content-Length − 64`. The verification logic is identical; only the file packaging and stream-splitting change.

**ESP-IDF Secure Boot v2** is the most robust long-term option but carries significant operational risk (irreversible eFuse burning) that should be evaluated separately before adoption.
