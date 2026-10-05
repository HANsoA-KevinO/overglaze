# Security policy

## Reporting a vulnerability

Please report security problems privately through GitHub's private vulnerability reporting: on this repository's **Security** tab, choose **Report a vulnerability**. Don't open a public issue for a vulnerability.

Include:

- the Overglaze version;
- what you found, and steps to reproduce it;
- the impact you expect.

Don't attach NVIDIA files, game files or binaries. Describe them by name, version and SHA-256 instead.

This is a small project. We will acknowledge your report as soon as we can, and keep you informed while we work on a fix.

## Supported versions

Only the latest release gets security fixes.

## In scope

- **Installer and game manager:**
  - path traversal;
  - following reparse points (junctions, symbolic links, mount points) out of the intended folder;
  - writing outside the game folder or Overglaze's own subfolder;
  - overwriting game files;
  - uninstalling files Overglaze did not create;
  - races between the hash check and the copy;
  - malicious adapter packages or game-facts JSON.
- **Control pipe:**
  - access by another user or from a remote machine;
  - bypassing the access list or the single-writer rule;
  - malformed messages;
  - replaying or confusing requests across sessions.
- **DLL loading:**
  - search-order problems that could make the proxy, the host, the bridge or the model load from an unexpected location;
  - loading a model or module that doesn't pass its identity checks.
- **Late-load injection:** injecting into the wrong process, or into a process of another user or at another integrity level; races between the checks and the injection.
- **Launcher:** handling of the Steam launch-option command line.

## Out of scope

- **Detection by anti-cheat or anti-tamper software.** Overglaze is not meant for those games; see [POLICY.md](POLICY.md).
- **Antivirus heuristics flagging the proxy DLL.** This is a known false-positive pattern for proxy DLLs. Please report it as a normal issue.
- **Attacks that assume someone can already write to your game folder or Overglaze's folder with your account's permissions**, unless Overglaze itself makes such writes possible or easier.
- **Bugs in NVIDIA drivers, NVIDIA modules, the model, or games.** Report those to their vendors.
