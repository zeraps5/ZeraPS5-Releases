# ZeraPS5 privacy notice

This notice describes the public Windows b984 launcher and its optional diagnostic reporting, reviewed on 16 November 2026. It is a technical description of existing behavior; it does not change your settings or enable uploads.

## Local operation

Games, saves, settings, game-library paths and session logs are handled locally. Games, firmware, keys and console modules are not supplied with the application. Local session history can grow and is not automatically erased by exporting a report.

## Update checks

The launcher contacts GitHub to check for and download releases. Update checks do not upload a diagnostic report. GitHub receives ordinary connection information such as the requesting IP address and HTTP headers; its processing is covered by the [GitHub Privacy Statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement).

## Optional problem reports

Automatic report upload is disabled by default in this distribution. After a game session ends, the reporting interface lets you describe a problem, send a report to the private receiver, or save a local draft. Update downloads do not submit reports.

A report ZIP can contain filtered current/previous logs, the entered description, game and emulator build information, hardware/operating-system information, session status and report-quality summaries. Session IDs relate the diagnostic files; they are not persistent computer or account identifiers. Computer-name and persistent installation-ID upload headers are not sent.

Filtering removes recognized private patterns but cannot recognize every personal detail. Review the description and avoid entering passwords, keys, account details or personal information. Raw logs, environment variables, command lines and structured record files are not uploaded as a fallback.

The service uses Cloudflare infrastructure. The receiving infrastructure sees the source connection address, including for rate limiting; this does not mean the application includes a computer name or persistent device identifier. See the [Cloudflare Privacy Policy](https://www.cloudflare.com/privacypolicy/).

Reports are stored privately and are not publicly listed or downloadable. Report expiration is set to 14 days; deletion is asynchronous. The current limits are 10 MiB per report, 500 daily upload reservations overall and 10 per source address, using UTC day boundaries. Failed uploads after a reservation count toward those limits. A network/server failure preserves the local report ZIP.

## Disable report uploads

The packaged `docs/REPORTING.txt` describes the reporting controls. To disable the report upload destination entirely, set this in `user/config/zeraps5.ini`:

```ini
[relatorios]
servidor=
enviar_ao_fechar=0
```

Disabling report uploads does not delete local reports or disable the separate GitHub update check.

For a report inquiry, contact the project through [GitHub issues](https://github.com/zeraps5/ZeraPS5-Releases/issues) with only the report code and a non-sensitive summary. Do not attach personal logs, keys or proprietary game files to a public issue.
