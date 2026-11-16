# Code signing policy

ZeraPS5 is preparing an application to SignPath Foundation. No Foundation approval or production certificate is claimed. The existing b984 executables remain unsigned.

The proposed public maintainer, reviewer and release approver is **metallrockdev**. The application requests Foundation signing without a personal-name certificate. Provider acceptance and configuration remain pending.

The readiness workflow builds an unsigned review fixture from the hash-pinned, already-public b984 source materials and FFmpeg source. It adds Windows version-resource metadata and a local-review marker. These artifacts are for build-origin review, not a new release or a replacement for b984.

A production signing workflow will require reviewed source/dependencies, approved signing roles and MFA, verification of certificate identity and timestamps, checksums generated after signing, and an isolated launcher update test before publication. The signing provider's credentials will not be included in source or release assets.

If the project is accepted, this page will identify the confirmed provider and certificate publisher and include the required attribution. Until then, the project does not represent its downloads as signed or sponsored by SignPath.

See the [privacy notice](PRIVACY.md) for update and optional report behavior.
