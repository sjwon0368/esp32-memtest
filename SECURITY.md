# Security Policy

## Supported Versions

Security fixes are applied to the latest release. There is no long-term support
branch at present.

| Version | Supported |
| ------- | --------- |
| 1.0.x   | yes       |

## Reporting a Vulnerability

Please report suspected vulnerabilities privately rather than opening a public
issue. Use GitHub's private vulnerability reporting for this repository, or open
a confidential security advisory.

Please include:

- the affected version or commit,
- the ESP-IDF version and target chip,
- a description of the impact, and
- a reproducer if you have one.

You can expect an acknowledgement within a week. Once a fix is ready it will be
released as a new patch version and credited in `CHANGELOG.md` unless you prefer
otherwise.

## Scope

This project is a diagnostic firmware that runs on a device you own, talks to a
local serial port, and makes no network connections. Reports that are in scope
include memory corruption in the test engine, out-of-bounds access, buffer
overflows in the console parser, and anything that lets a crafted key sequence
read or write outside the intended buffer.

Reports about the hardware itself, or about third-party boards and modules, are
out of scope. Findings in ESP-IDF itself should go to Espressif.