# Security guidance

TraceVault Lite is an educational embedded-security prototype. Its local
RFID-plus-PIN gate is intentionally independent from Wi-Fi and Telegram.

## Credentials

- Keep real values only in the ignored `secrets.h` file.
- Never place tokens, passwords, PINs, RFID UIDs, or chat IDs in screenshots,
  serial-monitor captures, issues, or commit messages.
- Run a secret scan before every public push.
- Rotate a Telegram bot token immediately if it appears in a public commit.
- Change the Wi-Fi password and local access credentials if they are exposed.

## Prototype limitations

- The MFRC522 code compares a card UID. Many UID-based cards can be cloned, so
  this is not cryptographic smart-card authentication.
- `telegramClient.setInsecure()` disables TLS certificate verification. The
  connection is encrypted, but the peer identity is not authenticated.
- Credentials are compiled into device firmware and can be recovered by an
  attacker with sufficient physical access and tooling.
- The four-digit PIN has a small key space; the three-attempt lockdown slows
  online guessing but is not a substitute for tamper-resistant hardware.
- The system does not include a physical door-position sensor, secure element,
  encrypted audit store, or battery backup.

For real asset protection, add certificate validation, stronger card
authentication, protected credential storage, enclosure tamper detection,
power-loss handling, and a threat model appropriate to the deployment.
